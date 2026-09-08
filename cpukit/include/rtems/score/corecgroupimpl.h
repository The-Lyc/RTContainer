/**
 * @file
 *
 * @ingroup RTEMSScoreCgroup
 *
 * @brief Inlined Routines in the Core Cgroup Handler
 *
 * This include file contains the static inline implementation of all
 * inlined routines in the Core Cgroup Handler.
 */

/*
 *  COPYRIGHT (c) 1989-2009.
 *  On-Line Applications Research Corporation (OAR).
 *
 *  The license and distribution terms for this file may be
 *  found in the file LICENSE in this distribution or at
 *  http://www.rtems.org/license/LICENSE.
 */

#ifndef _RTEMS_SCORE_CORECGROUPIMPL_H
#define _RTEMS_SCORE_CORECGROUPIMPL_H

#include <rtems/score/corecgroup.h>
#include <rtems/score/status.h>
#include <rtems/score/chainimpl.h>
#include <rtems/score/threaddispatch.h>
#include <rtems/score/threadqimpl.h>
#include <rtems/score/isrlevel.h>
#include <rtems/score/percpu.h>
#include <rtems/score/watchdogimpl.h>

#include <rtems/rtems/status.h>
#include <limits.h>
#include <string.h>
#include <assert.h>

#ifdef __cplusplus
extern "C" {
#endif


static void set_cpu_resume_watchdog( CORE_cgroup_Control *cgroup );

/**
 * @brief Suspend the cgroup.
 *
 * @param[in, out] the_cgroup The cgroup to suspend.
 */
rtems_status_code _CORE_cgroup_Suspend(
  CORE_cgroup_Control *the_cgroup,
  States_Control      wait_state
);

/**
 * @brief Resume the cgroup.
 *
 * @param[in, out] the_cgroup The cgroup to resume.
 */
rtems_status_code _CORE_cgroup_Resume(
  CORE_cgroup_Control *the_cgroup,
  States_Control      wait_state
);


static void cpu_suspend_routine(
  Watchdog_Control *the_watchdog
)
{
  cg_watchdog_t* wd_ctx = (cg_watchdog_t*) the_watchdog;
  CORE_cgroup_Control* cgroup = wd_ctx->the_cgroup;

  _CORE_cgroup_Suspend( cgroup, STATES_WAITING_FOR_CGROUP_CPU_QUOTA );
}

static void cpu_resume_routine(
  Watchdog_Control *the_watchdog
)
{
  cg_watchdog_t* wd_ctx = (cg_watchdog_t*) the_watchdog;
  CORE_cgroup_Control* cgroup = wd_ctx->the_cgroup;

  _CORE_cgroup_Resume( cgroup, STATES_WAITING_FOR_CGROUP_CPU_QUOTA );

  /** launch resume watchdog immediately */
  set_cpu_resume_watchdog(cgroup);
}

static void cg_watchdog_setup(
  cg_watchdog_t *wd_ctx,
  CORE_cgroup_Control *cgroup,
  Watchdog_Service_routine_entry routine
)
{
  Per_CPU_Control *cpu = _Per_CPU_Get_by_index(0);
  _Watchdog_Preinitialize(&wd_ctx->watchdog, cpu);
  _Watchdog_Initialize(&wd_ctx->watchdog, routine);
  wd_ctx->the_cgroup = cgroup;
}

static void set_cpu_suspend_watchdog( CORE_cgroup_Control *cgroup )
{
  Per_CPU_Control *cpu = _Watchdog_Get_CPU( &cgroup->cpu_suspend_watchdog.watchdog );
  ISR_lock_Context lock_context;

  _ISR_lock_ISR_disable_and_acquire( &cgroup->quota_lock, &lock_context );
  _Watchdog_Per_CPU_insert_ticks(
    &cgroup->cpu_suspend_watchdog.watchdog,
    cpu,
    cgroup->cpu_quota_available
  );
  _ISR_lock_Release_and_ISR_enable( &cgroup->quota_lock, &lock_context );

  assert( _Watchdog_Is_scheduled( &cgroup->cpu_suspend_watchdog.watchdog ) );
}

static void cancel_cpu_suspend_watchdog( CORE_cgroup_Control *cgroup )
{
  _Watchdog_Per_CPU_remove_ticks( &cgroup->cpu_suspend_watchdog.watchdog );
}

static void set_cpu_resume_watchdog( CORE_cgroup_Control *cgroup )
{
  ISR_lock_Context lock_context;

  _ISR_lock_ISR_disable_and_acquire( &cgroup->quota_lock, &lock_context );
  cgroup->cpu_quota_available = cgroup->config->cpu_quota;
  _ISR_lock_Release_and_ISR_enable( &cgroup->quota_lock, &lock_context );

  Per_CPU_Control *cpu = _Watchdog_Get_CPU( &cgroup->cpu_resume_watchdog.watchdog );
  _Watchdog_Per_CPU_insert_ticks(
    &cgroup->cpu_resume_watchdog.watchdog,
    cpu,
    cgroup->config->cpu_period
  );
}

static void cancel_cpu_resume_watchdog( CORE_cgroup_Control *cgroup )
{
  _Watchdog_Per_CPU_remove_ticks( &cgroup->cpu_resume_watchdog.watchdog );
}


/**
 * @brief Initializes a cgroup.
 *
 * This routine initializes the cgroup control structure based on the
 * parameters passed.
 *
 * @param[out] the_cgroup The cgroup to initialize.
 * @param[in] config The cgroup configuration.
 *
 * @retval true The cgroup can be initialized.
 * @retval false Memory for the cgroup cannot be allocated.
 */
bool _CORE_cgroup_Initialize(
  CORE_cgroup_Control *the_cgroup,
  CORE_cgroup_config  *config
);

/**
 * @brief Destroys an empty cgroup and releases its internal resources.
 *
 * The caller must ensure that no threads remain in the cgroup and that no new
 * users can obtain the enclosing API object.
 *
 * @param[in, out] the_cgroup The cgroup to destroy.
 */
void _CORE_cgroup_Destroy(
  CORE_cgroup_Control *the_cgroup
);

/**
 * @brief Add a thread to the cgroup.
 *
 * @param[in,out] cgroup The cgroup to add the thread to.
 * @param[in] thread The thread to add.
 *
 * @retval RTEMS_SUCCESSFUL Successful operation.
 * @retval RTEMS_TOO_MANY Maximum number of threads exceeded.
 * @retval RTEMS_NO_MEMORY Could not allocate memory for cgroup node.
 */
bool _CORE_cgroup_Add_thread(
  CORE_cgroup_Control *cgroup,
  Thread_Control      *thread
);

/**
 * @brief Remove a thread from the cgroup.
 *
 * @param[in,out] cgroup The cgroup to remove the thread from.
 * @param[in] thread The thread to remove.
 *
 * @retval RTEMS_SUCCESSFUL Successful operation.
 * @retval RTEMS_INVALID_ID Thread not found in cgroup.
 */
bool _CORE_cgroup_Remove_thread(
  CORE_cgroup_Control *cgroup,
  Thread_Control      *thread
);


/**
 * @brief Find a thread in the cgroup.
 *
 * @param[in] cgroup The cgroup to search in.
 * @param[in] thread The thread to find.
 *
 * @return Pointer to the cgroup node if found, NULL otherwise.
 */
Thread_cgroup_node *_CORE_cgroup_Find_thread(
  CORE_cgroup_Control *cgroup,
  Thread_Control      *thread
);

/**
 * @brief Consume CPU quota for a cgroup.
 *
 * @param[in] cgroup The cgroup to update.
 * @param[in] cpu_time_delta The CPU time consumed since last update.
 */
void _CORE_cgroup_Consume_cpu_quota(
  CORE_cgroup_Control *cgroup,
  uint64_t            cpu_time_delta
);

/**
 * @brief Get total CPU usage for the cgroup.
 *
 * @param[in] cgroup The cgroup to get usage for.
 *
 * @return Total CPU usage in microseconds.
 */
uint64_t _CORE_cgroup_Get_total_cpu_usage(
  CORE_cgroup_Control *cgroup
);

/**
 * @brief Check if more threads can be added to the cgroup.
 *
 * @param[in] cgroup The cgroup to check.
 *
 * @retval true More threads can be added.
 * @retval false Maximum thread limit reached.
 */
static inline bool _CORE_cgroup_Can_add_thread(
  CORE_cgroup_Control *cgroup
)
{
  return cgroup->thread_count < cgroup->max_threads;
}

/**
 * @brief Get the number of threads in the cgroup.
 *
 * @param[in] cgroup The cgroup to get thread count for.
 *
 * @return Number of threads in the cgroup.
 */
static inline uint32_t _CORE_cgroup_Get_thread_count(
  CORE_cgroup_Control *cgroup
)
{
  return cgroup->thread_count;
}

/**
 * @brief Close the cgroup.
 *
 * @param[in, out] the_cgroup The cgroup to close.
 * @param[in, out] queue_context The thread queue context.
 */
void _CORE_cgroup_Close(
  CORE_cgroup_Control *the_cgroup,
  Thread_queue_Context *queue_context
);

void _CORE_cgroup_set_cpu_quota(
  CORE_cgroup_Control *cgroup,
  uint32_t            cpu_quota,
  uint32_t            cpu_period
);

void _CORE_cgroup_set_shrink_callback(
  CORE_cgroup_Control *cgroup,
  void (*shrink_callback)(void *arg, uintptr_t target),
  void *shrink_arg
);

/** @} */

#ifdef __cplusplus
}
#endif

#endif
/* end of include file */
