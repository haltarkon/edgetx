// SPDX-License-Identifier: GPL-2.0-or-later
//
// Single-threaded implementation of EdgeTX's os/ abstraction (task.h, timer.h,
// time.h, sleep.h, async.h) for the headless mixer. See etx_port.h.
//
// Nothing here runs concurrently with anything else: the host calls one export
// at a time, and every EdgeTX function the API uses is called from that call.
// So a task is never started, a mutex is never contended, and time is not the
// wall clock but the virtual clock that etx_step() advances in 10 ms ticks.

#include "etx_port_impl.h"

#include "os/async.h"
#include "os/sleep.h"
#include "os/task.h"
#include "os/time.h"
#include "os/timer.h"

// ---- virtual time ---------------------------------------------------------

static uint32_t s_virtual_ms = 0;

uint32_t etxVirtualMs() { return s_virtual_ms; }
void etxAdvanceVirtualMs(uint32_t ms) { s_virtual_ms += ms; }
void etxSetVirtualMs(uint32_t ms) { s_virtual_ms = ms; }

uint32_t time_get_ms() { return s_virtual_ms; }
time_point_t time_point_now() { return s_virtual_ms; }

// Sleeping would only ever wait for another task, and there is none.
void sleep_ms(uint32_t) {}
void sleep_until(time_point_t* tp, uint32_t ts_ms) { *tp += ts_ms; }

// ---- tasks & mutexes ------------------------------------------------------

void task_create(task_handle_t* h, task_func_t, const char*, void*,
                 unsigned stack_size, unsigned)
{
  // Tasks are never started: the API calls what the tasks would.
  if (h) h->_stack_size = stack_size;
}

bool task_running() { return false; }
unsigned task_get_stack_usage(task_handle_t*) { return 0; }
unsigned task_get_stack_size(task_handle_t* h) { return h ? h->_stack_size * 4 : 0; }
bool scheduler_is_running() { return false; }

void mutex_create(mutex_handle_t* h)
{
  if (h) h->_locked = 0;
}

bool mutex_lock(mutex_handle_t* h)
{
  if (!h) return false;
  h->_locked = 1;
  return true;
}

void mutex_unlock(mutex_handle_t* h)
{
  if (h) h->_locked = 0;
}

bool mutex_trylock(mutex_handle_t* h)
{
  if (!h) return false;
  h->_locked = 1;
  return true;
}

// ---- software timers ------------------------------------------------------
// Created and started as EdgeTX asks, never fired: the only periodic work the
// mixer needs (the 10 ms tick, the mixer pass, the 50 ms UI pass) is driven by
// etx_step() explicitly, in the order the radio's tasks run it.

int timer_create(timer_handle_t* h, timer_func_t func, const char* name,
                 unsigned period, bool repeat)
{
  if (!h) return -1;
  h->func = func;
  h->name = name;
  h->period = period;
  h->repeat = repeat;
  h->created = true;
  h->active = false;
  return 0;
}

bool timer_is_created(timer_handle_t* h) { return h && h->created; }
bool timer_is_active(timer_handle_t* h) { return h && h->active; }

int timer_start(timer_handle_t* h)
{
  if (!h || !h->created) return -1;
  h->active = true;
  return 0;
}

int timer_stop(timer_handle_t* h)
{
  if (!h || !h->created) return -1;
  h->active = false;
  return 0;
}

int timer_set_period(timer_handle_t* h, unsigned period)
{
  if (!h || !h->created) return -1;
  h->period = period;
  return 0;
}

// ---- deferred calls -------------------------------------------------------
// There is no timer task to defer to; run the call now, as the next thing the
// radio would have done.

bool async_call(async_func_t func, volatile bool* excl_flag, void* param1,
                uint32_t param2)
{
  if (excl_flag && *excl_flag) return false;
  if (func) func(param1, param2);
  return true;
}

bool async_call_isr(async_func_t func, volatile bool* excl_flag, void* param1,
                    uint32_t param2)
{
  return async_call(func, excl_flag, param1, param2);
}
