/*
 * Tiku Operating System v0.05
 * Simple. Ubiquitous. Intelligence, Everywhere.
 * http://tiku-os.org
 *
 * Authors: Ambuj Varshney <ambuj@tiku-os.org>
 *
 * tiku_timer.c - Unified software timer implementation
 *
 * Single process, single deadline-sorted linked list, handles both callback
 * and event timers.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*---------------------------------------------------------------------------*/
/* INCLUDES                                                                  */
/*---------------------------------------------------------------------------*/

#include "tiku_timer.h"
#include "tiku_crit.h"
#include "tiku.h"
#include <hal/tiku_compiler.h>
#include <stddef.h>

/*---------------------------------------------------------------------------*/
/* MODULE STATE                                                              */
/*---------------------------------------------------------------------------*/

/** Head of the active timer singly-linked list, sorted by nearest deadline. */
static struct tiku_timer *timer_list = NULL;

/** Running count of timer expirations (wraps at 65535). */
static uint16_t timer_fire_count;

/*---------------------------------------------------------------------------*/
/* ARCH HOOKS                                                                */
/*---------------------------------------------------------------------------*/

/**
 * @brief Weak default for platform deadline reprogramming.
 *
 * Event-driven ports override this to arm/disarm a hardware compare for the
 * nearest software-timer deadline. The default keeps periodic-tick ports
 * unchanged.
 */
TIKU_WEAK void
tiku_timer_arch_rearm(tiku_clock_time_t next, uint8_t armed)
{
  (void)next;
  (void)armed;
}

/*---------------------------------------------------------------------------*/
/* INTERNAL HELPERS                                                          */
/*---------------------------------------------------------------------------*/

/**
 * @brief Check if clock time @p now is past the timer's expiration.
 */
static inline int
timer_is_due(struct tiku_timer *t, tiku_clock_time_t now)
{
  return (tiku_clock_time_t)(now - t->start) >= t->interval;
}

/**
 * @brief Return a timer's absolute expiration tick.
 */
static inline tiku_clock_time_t
timer_expiration(struct tiku_timer *t)
{
  return (tiku_clock_time_t)(t->start + t->interval);
}

/**
 * @brief Re-arm the platform backend at the current list head.
 */
static void
timer_rearm_head(void)
{
  if (timer_list != NULL) {
    tiku_timer_arch_rearm(timer_expiration(timer_list), 1);
  } else {
    tiku_timer_arch_rearm(0, 0);
  }
}

/**
 * @brief Remove a timer from the active list without rearming.
 */
static int
timer_unlink(struct tiku_timer *t)
{
  struct tiku_timer **pp;

  for (pp = &timer_list; *pp != NULL; pp = &(*pp)->next) {
    if (*pp == t) {
      *pp = t->next;
      t->next = NULL;
      t->active = 0;
      return 1;
    }
  }

  return 0;
}

/**
 * @brief Remove a timer from the active list.
 */
static void
timer_remove(struct tiku_timer *t)
{
  tiku_atomic_enter();
  if (timer_unlink(t)) {
    timer_rearm_head();
  }
  tiku_atomic_exit();
}

/**
 * @brief Insert a timer into the active list by nearest deadline.
 *
 * Due timers sort before future timers so the head-only work-pending fast path
 * remains correct even for immediate timers and late periodic resets.
 */
static void
timer_insert(struct tiku_timer *t)
{
  struct tiku_timer **pp;
  tiku_clock_time_t now;
  tiku_clock_time_t dist;
  int due;

  tiku_atomic_enter();

  if (t->active) {
    (void)timer_unlink(t);
  }

  now = tiku_clock_time();
  dist = (tiku_clock_time_t)(timer_expiration(t) - now);
  due = timer_is_due(t, now);

  for (pp = &timer_list; *pp != NULL; pp = &(*pp)->next) {
    int other_due = timer_is_due(*pp, now);

    if (due && !other_due) {
      break;
    }
    if (!due && other_due) {
      continue;
    }

    if (dist < (tiku_clock_time_t)(timer_expiration(*pp) - now)) {
      break;
    }
  }

  t->next = *pp;
  *pp = t;
  t->active = 1;

  timer_rearm_head();
  tiku_atomic_exit();

  /* Wake the timer process to catch immediate/due insertions. */
  tiku_process_poll(&tiku_timer_process);
}

/**
 * @brief Pop the head timer from the active list.
 */
static struct tiku_timer *
timer_pop_head(void)
{
  struct tiku_timer *t = timer_list;

  if (t != NULL) {
    timer_list = t->next;
    t->next = NULL;
    t->active = 0;
  }

  return t;
}

/*---------------------------------------------------------------------------*/
/* TIMER MANAGEMENT PROCESS                                                  */
/*---------------------------------------------------------------------------*/

TIKU_PROCESS(tiku_timer_process, "Timer");

TIKU_PROCESS_THREAD(tiku_timer_process, ev, data)
{
  struct tiku_timer *t;

  TIKU_PROCESS_BEGIN();

  while (1) {
    TIKU_PROCESS_YIELD();

    /*
     * Handle process exit: remove all timers belonging to the exited process.
     */
    if (ev == TIKU_EVENT_EXITED) {
      struct tiku_process *dead = tiku_event_proc(ev, data);
      struct tiku_timer **pp = &timer_list;
      uint8_t changed = 0;

      tiku_atomic_enter();
      while (*pp != NULL) {
        if ((*pp)->p == dead) {
          struct tiku_timer *victim = *pp;
          TIMER_PRINTF("Cleanup: removed timer for exited process\n");
          *pp = victim->next;
          victim->next = NULL;
          victim->active = 0;
          changed = 1;
        } else {
          pp = &(*pp)->next;
        }
      }
      if (changed) {
        timer_rearm_head();
      }
      tiku_atomic_exit();
      continue;
    }

    if (ev != TIKU_EVENT_POLL) {
      continue;
    }

    /*
     * If a critical-execution window is held, defer the scan. The poll
     * re-issued from tiku_crit_end() will pick up any expirations that came due
     * while the window was held.
     */
    if (tiku_crit_active()) {
      continue;
    }

    while (timer_list != NULL && timer_is_due(timer_list, tiku_clock_time())) {
      tiku_atomic_enter();
      if (timer_list == NULL || !timer_is_due(timer_list, tiku_clock_time())) {
        tiku_atomic_exit();
        break;
      }
      t = timer_pop_head();
      timer_rearm_head();
      tiku_atomic_exit();

      timer_fire_count++;

      /* Dispatch outside the atomic section: callbacks may reset timers. */
      if (t->mode == TIKU_TIMER_MODE_CALLBACK && t->func != NULL) {
        TIMER_PRINTF("Expired: callback dispatched\n");
        TIKU_PROCESS_CONTEXT_BEGIN(t->p);
        t->func(t->ptr);
        TIKU_PROCESS_CONTEXT_END(t->p);
      } else if (t->mode == TIKU_TIMER_MODE_EVENT && t->p != NULL) {
        TIMER_PRINTF("Expired: event posted to %s\n", t->p->name);
        tiku_process_post(t->p, TIKU_EVENT_TIMER, t);
      }
    }

    tiku_atomic_enter();
    timer_rearm_head();
    tiku_atomic_exit();
  }

  TIKU_PROCESS_END();
}

/*---------------------------------------------------------------------------*/
/* PUBLIC API                                                                */
/*---------------------------------------------------------------------------*/

void
tiku_timer_init(void)
{
  timer_list = NULL;
  timer_fire_count = 0;
  tiku_timer_arch_rearm(0, 0);
  tiku_process_start(&tiku_timer_process, NULL);
  TIMER_PRINTF("Init complete\n");
}

/*---------------------------------------------------------------------------*/

void
tiku_timer_set_callback(struct tiku_timer *t, tiku_clock_time_t ticks,
                        tiku_timer_callback_t func, void *ptr)
{
  tiku_atomic_enter();
  t->start = tiku_clock_time();
  t->interval = ticks;
  t->mode = TIKU_TIMER_MODE_CALLBACK;
  t->func = func;
  t->ptr = ptr;
  t->p = TIKU_PROCESS_CURRENT();

  TIMER_PRINTF("Set callback: interval=%u ticks\n", ticks);
  timer_insert(t);
  tiku_atomic_exit();
}

/*---------------------------------------------------------------------------*/

void
tiku_timer_set_event(struct tiku_timer *t, tiku_clock_time_t ticks)
{
  tiku_atomic_enter();
  t->start = tiku_clock_time();
  t->interval = ticks;
  t->mode = TIKU_TIMER_MODE_EVENT;
  t->func = NULL;
  t->ptr = NULL;
  t->p = TIKU_PROCESS_CURRENT();

  TIMER_PRINTF("Set event: interval=%u ticks\n", ticks);
  timer_insert(t);
  tiku_atomic_exit();
}

/*---------------------------------------------------------------------------*/

void
tiku_timer_reset(struct tiku_timer *t)
{
  tiku_atomic_enter();
  /* Drift-free: advance start by one interval from last start. */
  t->start += t->interval;
  timer_insert(t);
  tiku_atomic_exit();
}

/*---------------------------------------------------------------------------*/

void
tiku_timer_restart(struct tiku_timer *t)
{
  tiku_atomic_enter();
  t->start = tiku_clock_time();
  timer_insert(t);
  tiku_atomic_exit();
}

/*---------------------------------------------------------------------------*/

void
tiku_timer_stop(struct tiku_timer *t)
{
  TIMER_PRINTF("Stopped timer\n");
  timer_remove(t);
}

/*---------------------------------------------------------------------------*/

int
tiku_timer_expired(struct tiku_timer *t)
{
  return !t->active;
}

/*---------------------------------------------------------------------------*/

tiku_clock_time_t
tiku_timer_remaining(struct tiku_timer *t)
{
  tiku_clock_time_t elapsed;

  if (!t->active) {
    return 0;
  }

  elapsed = tiku_clock_time() - t->start;
  if (elapsed >= t->interval) {
    return 0;
  }
  return t->interval - elapsed;
}

/*---------------------------------------------------------------------------*/

tiku_clock_time_t
tiku_timer_expiration_time(struct tiku_timer *t)
{
  return timer_expiration(t);
}

/*---------------------------------------------------------------------------*/

void
tiku_timer_request_poll(void)
{
  tiku_process_poll(&tiku_timer_process);
}

/*---------------------------------------------------------------------------*/

int
tiku_timer_any_pending(void)
{
  return timer_list != NULL;
}

/*---------------------------------------------------------------------------*/

int
tiku_timer_work_pending(void)
{
  return (timer_list != NULL && timer_is_due(timer_list, tiku_clock_time()));
}

/*---------------------------------------------------------------------------*/

int
tiku_timer_owner_armed(const struct tiku_process *p)
{
  struct tiku_timer *t;

  for (t = timer_list; t != NULL; t = t->next) {
    if (t->p == p) {
      return 1;
    }
  }
  return 0;
}

/*---------------------------------------------------------------------------*/

uint8_t
tiku_timer_count(void)
{
  struct tiku_timer *t;
  uint8_t n = 0;

  for (t = timer_list; t != NULL; t = t->next) {
    n++;
  }
  return n;
}

uint16_t
tiku_timer_fired(void)
{
  return timer_fire_count;
}

struct tiku_timer *
tiku_timer_get(uint8_t idx)
{
  struct tiku_timer *t;
  uint8_t n = 0;

  for (t = timer_list; t != NULL; t = t->next) {
    if (n == idx) {
      return t;
    }
    n++;
  }
  return (struct tiku_timer *)0;
}

/*---------------------------------------------------------------------------*/

tiku_clock_time_t
tiku_timer_next_expiration(void)
{
  if (timer_list == NULL) {
    return 0;
  }

  return timer_expiration(timer_list);
}

/*---------------------------------------------------------------------------*/

tiku_clock_time_t
tiku_timer_next_delay(void)
{
  tiku_clock_time_t now;

  if (timer_list == NULL) {
    return 0;
  }

  now = tiku_clock_time();
  if (timer_is_due(timer_list, now)) {
    return 0;
  }

  return (tiku_clock_time_t)(timer_expiration(timer_list) - now);
}

/*---------------------------------------------------------------------------*/
