/*
 * Copyright (c) 2015, Charlie Curtsinger and Emery Berger,
 *                     University of Massachusetts Amherst
 * This file is part of the Coz project. See LICENSE.md file at the top-level
 * directory of this distribution and at http://github.com/plasma-umass/coz.
 */

#include "profiler.h"

#include <asm/unistd.h>
#include <execinfo.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>

#include <atomic>
#include <cstdint>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "inspect.h"
#include "perf.h"
#include "progress_point.h"
#include "util.h"

#include "ccutil/log.h"
#include "ccutil/spinlock.h"
#include "ccutil/timer.h"

using namespace std;

/**
 * Start the profiler
 */
void profiler::startup(const string& outfile,
                       line* fixed_line,
                       int fixed_speedup,
                       bool end_to_end) {
  // Set up the sampling signal handler
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = profiler::samples_ready;
  sa.sa_flags = SA_SIGINFO;
  real::sigaction(SampleSignal, &sa, nullptr);

  // Set up handlers for errors
  memset(&sa, 0, sizeof(sa));
  sa.sa_sigaction = on_error;
  sa.sa_flags = SA_SIGINFO;
  
  real::sigaction(SIGSEGV, &sa, nullptr);
  real::sigaction(SIGABRT, &sa, nullptr);

  // Save the output file name
  _output_filename = outfile;

  // If a non-empty fixed line was provided, set it
  if(fixed_line) _fixed_line = fixed_line;

  // If the speedup amount is in bounds, set a fixed delay size
  if(fixed_speedup >= 0 && fixed_speedup <= 100)
    _fixed_delay_size = SamplePeriod * fixed_speedup / 100;

  // Should end-to-end mode be enabled?
  _enable_end_to_end = end_to_end;

  // Use a spinlock to wait for the profiler thread to finish intialization
  spinlock l;
  l.lock();

  // Create the profiler thread
  INFO << "Starting profiler thread";
  int rc = real::pthread_create(&_profiler_thread, nullptr, profiler::start_profiler_thread, (void*)&l);
  REQUIRE(rc == 0) << "Failed to start profiler thread";

  // Double-lock l. This blocks until the profiler thread unlocks l
  l.lock();

  // Begin sampling in the main thread
  thread_state* state = add_thread();
  REQUIRE(state) << "Failed to add thread";
  begin_sampling(state);
}

/**
 * Body of the main profiler thread
 */
void profiler::profiler_thread(spinlock& l) {
  // Open the output file
  ofstream output;
  output.open(_output_filename, ios_base::app);
  output.rdbuf()->pubsetbuf(0, 0);
  output.setf(ios::fixed, ios::floatfield);
  output.precision(2);

  // Initialize the delay size RNG
  default_random_engine generator(get_time());
  uniform_int_distribution<size_t> delay_dist(0, ZeroSpeedupWeight + SpeedupDivisions);

  // Initialize the experiment duration
  size_t experiment_length = ExperimentMinTime;

  // Get the starting time for the profiler
  size_t start_time = get_time();

  // Log the start of this execution
  output << "startup\t"
         << "time=" << start_time << "\n";

  // Unblock the main thread
  l.unlock();

  // Wait until there is at least one progress point
  _throughput_points_lock.lock();
  _latency_points_lock.lock();
  while(_throughput_points.size() == 0 && _latency_points.size() == 0 && _running) {
    _throughput_points_lock.unlock();
    _latency_points_lock.unlock();
    wait(ExperimentCoolOffTime);
    _throughput_points_lock.lock();
    _latency_points_lock.lock();
  }
  _throughput_points_lock.unlock();
  _latency_points_lock.unlock();

  // Log sample counts after this many experiments (doubles each time)
  size_t sample_log_interval = 32;
  size_t sample_log_countdown = sample_log_interval;

  // Main experiment loop
  while(_running) {
    // Select a line
    line* selected;
    if(_fixed_line) {   // If this run has a fixed line, use it
      selected = _fixed_line;
    } else {            // Otherwise, wait for the next line to be selected
      selected = _next_line.load();
      while(_running && selected == nullptr) {
        wait(SamplePeriod * SampleBatchSize);
        selected = _next_line.load();
      }

      // If we're no longer running, exit the experiment loop
      if(!_running) break;
    }

    // Store the globally-visible selected line
    _selected_line.store(selected);

    // Choose a delay size
    size_t delay_size;
    if(_fixed_delay_size >= 0) {
      delay_size = _fixed_delay_size;
    } else {
      size_t r = delay_dist(generator);
      if(r <= ZeroSpeedupWeight) {
        delay_size = 0;
      } else {
        delay_size = (r - ZeroSpeedupWeight) * SamplePeriod / SpeedupDivisions;
      }
    }

    _delay_size.store(delay_size);

    // Save the starting time and sample count
    size_t start_time = get_time();
    size_t starting_samples = selected->get_samples();
    size_t starting_delay_time = _global_delay.load();

    // Save throughput point values at the start of the experiment
    vector<unique_ptr<throughput_point::saved>> saved_throughput_points;
    _throughput_points_lock.lock();
    for(pair<const std::string, throughput_point*>& p : _throughput_points) {
      saved_throughput_points.emplace_back(p.second->save());
    }
    _throughput_points_lock.unlock();
    
    // Save latency point values at the start of the experiment
    vector<unique_ptr<latency_point::saved>> saved_latency_points;
    _latency_points_lock.lock();
    for(pair<const std::string, latency_point*>& p : _latency_points) {
      saved_latency_points.emplace_back(p.second->save());
    }
    _latency_points_lock.unlock();

    // Tell threads to start the experiment
    _experiment_active.store(true);

    // Wait until the experiment ends, or until shutdown if in end-to-end mode
    if(_enable_end_to_end) {
      while(_running) {
        wait(SamplePeriod * SampleBatchSize);
      }
    } else {
      wait(experiment_length);
    }

    // Compute experiment parameters
    float speedup = (float)delay_size / (float)SamplePeriod;
    size_t experiment_delay = _global_delay.load() - starting_delay_time;
    size_t duration = get_time() - start_time - experiment_delay;
    size_t selected_samples = selected->get_samples() - starting_samples;

    // Log the experiment parameters
    output << "experiment\t"
           << "selected=" << selected << "\t"
           << "speedup=" << speedup << "\t"
           << "duration=" << duration << "\t"
           << "selected-samples=" << selected_samples << "\n";

    // Keep a running count of the minimum delta over all progress points
    size_t min_delta = std::numeric_limits<size_t>::max();
    
    // Log throughput point measurements and update the minimum delta
    for(const auto& s : saved_throughput_points) {
      size_t delta = s->get_delta();
      if(delta < min_delta) min_delta = delta;
      s->log(output);
    }
    
    // Log latency point measurements and update the minimum delta
    for(const auto& s : saved_latency_points) {
      size_t begin_delta = s->get_begin_delta();
      size_t end_delta = s->get_end_delta();
      if(begin_delta < min_delta) min_delta = begin_delta;
      if(end_delta < min_delta) min_delta = end_delta;
      s->log(output);
    }

    // Lengthen the experiment if the min_delta is too small
    if(min_delta < ExperimentTargetDelta) {
      experiment_length *= 2;
    } else if(min_delta > ExperimentTargetDelta*2 && experiment_length >= ExperimentMinTime*2) {
      experiment_length /= 2;
    }

    output.flush();

    // Clear the next line, so threads will select one
    _next_line.store(nullptr);

    // End the experiment
    _experiment_active.store(false);

    // Log samples after a while, then double the countdown
    if(--sample_log_countdown == 0) {
      log_samples(output, start_time);
      if(sample_log_interval < 20) {
        sample_log_interval *= 2;
      }
      sample_log_countdown = sample_log_interval;
    }

    // Cool off before starting a new experiment, unless the program is exiting
    if(_running) wait(ExperimentCoolOffTime);
  }

  // Log the sample counts on exit
  log_samples(output, start_time);

  output.flush();
  output.close();
}

void profiler::log_samples(ofstream& output, size_t start_time) {
  // Log total runtime for phase correction
  output << "runtime\t"
         << "time=" << (get_time() - start_time) << "\n";

  // Log sample counts for all observed lines
  for(const auto& file_entry : memory_map::get_instance().files()) {
    for(const auto& line_entry : file_entry.second->lines()) {
      shared_ptr<line> l = line_entry.second;
      if(l->get_samples() > 0) {
        output << "samples\t"
               << "location=" << l << "\t"
               << "count=" << l->get_samples() << "\n";
      }
    }
  }
}

/**
 * Terminate the profiler thread, then exit
 */
void profiler::shutdown() {
  if(_shutdown_run.test_and_set() == false) {
    // Stop sampling in the main thread
    end_sampling();

    // "Signal" the profiler thread to stop
    _running.store(false);

    // Join with the profiler thread
    real::pthread_join(_profiler_thread, nullptr);
  }
}

thread_state* profiler::add_thread() {
  thread_state* inserted = _thread_states.insert(gettid());
  if (inserted != nullptr) {
    _num_threads_running += 1;
  }
  return inserted;
}

thread_state* profiler::get_thread_state() {
  return _thread_states.find(gettid());
}

void profiler::remove_thread() {
  _thread_states.remove(gettid());
  _num_threads_running -= 1;
}

/**
 * Entry point for wrapped threads
 */
void* profiler::start_thread(void* p) {
  thread_start_arg* arg = reinterpret_cast<thread_start_arg*>(p);

  thread_state* state = get_instance().add_thread();
  REQUIRE(state) << "Failed to add thread";

  state->local_delay = arg->_parent_delay_time;

  // Make local copies of the function and argument before freeing the arg wrapper
  thread_fn_t real_fn = arg->_fn;
  void* real_arg = arg->_arg;
  delete arg;

  // Start the sampler for this thread
  profiler::get_instance().begin_sampling(state);

  // Run the real thread function
  void* result = real_fn(real_arg);

  // Always exit via pthread_exit
  pthread_exit(result);
}

void profiler::begin_sampling(thread_state* state) {
  // Set the perf_event sampler configuration
  struct perf_event_attr pe;
  memset(&pe, 0, sizeof(pe));
  pe.type = PERF_TYPE_SOFTWARE;
  pe.config = PERF_COUNT_SW_TASK_CLOCK;
  pe.sample_type = PERF_SAMPLE_IP | PERF_SAMPLE_CALLCHAIN;
  pe.sample_period = SamplePeriod;
  pe.wakeup_events = SampleBatchSize; // This is ignored on linux 3.13 (why?)
  pe.exclude_idle = 1;
  pe.exclude_kernel = 1;
  pe.disabled = 1;

  // Create this thread's perf_event sampler and start sampling
  state->sampler = perf_event(pe);
  state->process_timer = timer(SampleSignal);
  state->process_timer.start_interval(SamplePeriod * SampleBatchSize);
  state->sampler.start();
}

void profiler::end_sampling() {
  thread_state* state = get_thread_state();
  if(state) {
    state->set_in_use(true);

    process_samples(state);

    state->sampler.stop();
    state->sampler.close();

    remove_thread();
  }
}

std::pair<line*,bool> profiler::match_line(perf_event::record& sample) {
  // bool -> true: hit selected_line
  std::pair<line*, bool> match_res(nullptr, false);
  // flag use to increase the sample only for the first line in the source scope. could it be last line in callchain?
  bool first_hit = false;
  if(!sample.is_sample())
    return match_res;
  // Check if the sample occurred in known code
  line* l = memory_map::get_instance().find_line(sample.get_ip()).get();
  if(l){
    match_res.first = l;
    first_hit = true;
    if(_selected_line == l){
      match_res.second = true;
      return match_res;
    }
  }
  // Walk the callchain
  for(uint64_t pc : sample.get_callchain()) {
    // Need to subtract one. PC is the return address, but we're looking for the callsite.
    l = memory_map::get_instance().find_line(pc-1).get();
    if(l){
      if(!first_hit){
        first_hit = true;
        match_res.first = l;
      }
      if(_selected_line == l){
        match_res.first = l;
	match_res.second = true;
        return match_res;
      }
    }
  }

  // No hits. Return null
  return match_res;
}

void profiler::add_delays(thread_state* state) {
  // Add delays if there is an experiment running
  if(_experiment_active.load()) {
    // Take a snapshot of the global and local delays
    size_t global_delay = _global_delay;
    size_t delay_size = _delay_size;

    // Is this thread ahead or behind on delays?
    if(state->local_delay > global_delay) {
      // Thread is ahead: increase the global delay time to make other threads pause
      _global_delay.fetch_add(state->local_delay - global_delay);

    } else if(state->local_delay < global_delay) {
      // Thread is behind: Pause this thread to catch up
      
      // Pause and record the exact amount of time this thread paused
      state->sampler.stop();
      state->local_delay += wait(global_delay - state->local_delay);
      state->sampler.start();
    }

  } else {
    // Just skip ahead on delays if there isn't an experiment running
    state->local_delay = _global_delay;
  }
}

void profiler::process_samples(thread_state* state) {
  for(perf_event::record r : state->sampler) {
    if(r.is_sample()) {
      // Find and matches the line that contains this sample
      std::pair<line*, bool> sampled_line = match_line(r);
      if(sampled_line.first) {
        sampled_line.first->add_sample();
      }

      if(_experiment_active) {
        // Add a delay if the sample is in the selected line
        if(sampled_line.second)
          state->local_delay += _delay_size;

      } else if(sampled_line.first != nullptr && _next_line.load() == nullptr) {
        _next_line.store(sampled_line.first);
      }
    }
  }

  add_delays(state);
}

/**
 * Entry point for the profiler thread
 */
void* profiler::start_profiler_thread(void* arg) {
  spinlock* l = (spinlock*)arg;
  profiler::get_instance().profiler_thread(*l);
  real::pthread_exit(nullptr);
  // Unreachable return silences compiler warning
  return nullptr;
}

void profiler::samples_ready(int signum, siginfo_t* info, void* p) {
  thread_state* state = get_instance().get_thread_state();
  if(state && !state->check_in_use()) {
    // Process all available samples
    profiler::get_instance().process_samples(state);
  }
}

void profiler::on_error(int signum, siginfo_t* info, void* p) {
  if(signum == SIGSEGV) {
    fprintf(stderr, "Segmentation fault at %p\n", info->si_addr);
  } else if(signum == SIGABRT) {
    fprintf(stderr, "Aborted!\n");
  } else {
    fprintf(stderr, "Signal %d at %p\n", signum, info->si_addr);
  }

  void* buf[256];
  int frames = backtrace(buf, 256);
  char** syms = backtrace_symbols(buf, frames);

  for(int i=0; i<frames; i++) {
    fprintf(stderr, "  %d: %s\n", i, syms[i]);
  }

  real::_exit(2);
}
