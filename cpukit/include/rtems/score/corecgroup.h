/**
 * @file
 *
 * @ingroup RTEMSScoreCgroup
 *
 * @brief Constants and Structures Associated with the Cgroup Handler.
 *
 * This include file contains all the constants and structures associated
 * with the Cgroup Handler.
 */

/*
 *  COPYRIGHT (c) 1989-2009.
 *  On-Line Applications Research Corporation (OAR).
 *
 *  The license and distribution terms for this file may be
 *  found in the file LICENSE in this distribution or at
 *  http://www.rtems.org/license/LICENSE.
 */

#ifndef _RTEMS_SCORE_CORECGROUP_H
#define _RTEMS_SCORE_CORECGROUP_H

#include <rtems/score/chain.h>
#include <rtems/score/isrlock.h>
#include <rtems/score/threadq.h>
#include <rtems/score/watchdog.h>
#include <rtems/score/statesimpl.h>
#include <rtems/score/isrlock.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @defgroup RTEMSScoreCgroup Cgroup Handler
 *
 * @ingroup RTEMSScore
 *
 * @brief Cgroup Handler
 *
 * This handler encapsulates functionality which provides the foundation
 * Cgroup services used in all of the APIs supported by RTEMS.
 *
 * @{
 */

typedef struct CORE_cgroup_Control CORE_cgroup_Control;

typedef struct CORE_cgroup_config
{
  uint64_t cpu_shares;
  uint64_t cpu_quota;
  uint64_t cpu_period;
  uint64_t memory_limit;
  uint64_t blkio_limit;
} CORE_cgroup_config;

/**
 * @brief Thread cgroup node for linking threads to cgroups.
 *
 * This structure represents the relationship between a thread and its cgroup.
 * It contains the necessary information for cgroup management and CPU usage tracking.
 */
typedef struct Thread_cgroup_node {
  /**
   * @brief Chain node for linking in the cgroup's thread list.
   */
  Chain_Node node;

  /**
   * @brief Pointer to the thread control block.
   */
  struct _Thread_Control *thread;

  /**
   * @brief Pointer to the cgroup that owns this thread.
   */
  struct CORE_cgroup_Control *cgroup;

  /**
   * @brief CPU time consumed by this thread in microseconds.
   */
  uint64_t cpu_usage;
} Thread_cgroup_node;

/** watchdog setup */
typedef struct {
  Watchdog_Control watchdog;
  CORE_cgroup_Control*  the_cgroup;
} cg_watchdog_t;

/**
 *  @brief Control block used to manage each cgroup.
 *
 *  The following defines the control block used to manage each
 *  cgroup and its associated threads.
 */
struct CORE_cgroup_Control {
  /** cgroup wait queue */
  Thread_queue_Control Wait_queue;

  /** the state of this cgroup */
  States_Control state;

  /** owned copy of the configuration parameters */
  CORE_cgroup_config config_storage;

  /** configuration parameters used by the quota paths */
  CORE_cgroup_config* config;

  /** list of threads in this cgroup */
  Chain_Control threads;

  /** current number of threads in this cgroup */
  uint32_t thread_count;

  /** maximum number of threads allowed in this cgroup */
  uint32_t max_threads;

  /** total CPU usage by all threads in this cgroup */
  uint64_t cpu_usage_total;

  /** CPU quota available in current period */
#if ISR_LOCK_NEEDS_OBJECT
  ISR_lock_Control quota_lock;
#endif
  uint64_t cpu_quota_available;

  /** current period arrival time */
  uint64_t cpu_period_arrival_time;

  /** watchdogs */
  cg_watchdog_t cpu_suspend_watchdog;
  cg_watchdog_t cpu_resume_watchdog;

  /** mem quota available  */
  uint64_t mem_quota_available;

  /**
   * @brief Memory shrink callback function.
   * @param arg User-provided context argument.
   * @param target The amount of memory (in bytes) that needs to be freed.
   * @return NULL
   */
  void (*shrink_callback)(void *arg, uintptr_t target);

  /**
   * @brief Argument to be passed to the shrink callback.
   */
  void *shrink_arg;

  /** flag to indicate whether next memory free should be charged*/
  bool should_charge_next_free;
};

/** @} */

#ifdef __cplusplus
}
#endif

#endif
/*  end of include file */
