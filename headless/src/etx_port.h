// SPDX-License-Identifier: GPL-2.0-or-later
//
// Single-threaded OS port for the headless EdgeTX mixer.
//
// Force-included (-include) into every EdgeTX translation unit of this build.
// EdgeTX's os/task.h, os/timer.h and os/time.h pick the port from NATIVE_THREADS
// (std::thread) or FREE_RTOS; this build defines neither, so os/time.h falls back
// to its no-RTOS time_point_t and the handle types that os/task.h and os/timer.h
// expect from a port are declared here instead. The implementation is
// etx_port.cpp: no task ever starts, mutexes are uncontended, timers never fire,
// and time is the virtual clock etx_step() advances.

#pragma once

#if defined(__cplusplus)

#include <stdint.h>

#define TASK_DEFINE_STACK(name, size) void* name

struct task_handle_t {
  uint32_t _stack_size;
};

struct mutex_handle_t {
  uint8_t _locked;
};

bool task_running();

struct timer_handle_t;

struct timer_handle_t {
  void (*func)(timer_handle_t*);
  const char* name;
  unsigned period;
  bool repeat;
  bool created;
  bool active;
};

#define TIMER_INITIALIZER {nullptr, nullptr, 0, false, false, false}

#endif  // __cplusplus
