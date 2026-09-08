/**
 *  @file
 *
 *  @brief Manipulate a Cgroup
 *  @ingroup RTEMSScoreCgroup
 */

/*
 *  COPYRIGHT (c) 1989-2009.
 *  On-Line Applications Research Corporation (OAR).
 *
 *  The license and distribution terms for this file may be
 *  found in the file LICENSE in this distribution or at
 *  http://www.rtems.org/license/LICENSE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <rtems/score/corecgroupimpl.h>
#include <rtems/score/wkspace.h>
#include <rtems/score/chainimpl.h>
#include <rtems/score/isrlevel.h>
#include <rtems/score/thread.h>
#include <rtems/score/threadimpl.h>

rtems_status_code _CORE_cgroup_Suspend(
  CORE_cgroup_Control *the_cgroup,
  States_Control    wait_state
)
{
  Per_CPU_Control *cpu_self;

  if(the_cgroup->state != STATES_READY) {
      the_cgroup->state |= wait_state;
  } else {
    the_cgroup->state |= wait_state;

    Chain_Node *node;
    Chain_Node *next_node;
    Thread_cgroup_node *thread_node;
    Thread_Control *the_thread;

    cpu_self = _Thread_Dispatch_disable();

    node = _Chain_First(&the_cgroup->threads);
    while (!_Chain_Is_tail(&the_cgroup->threads, node)) {
      next_node = _Chain_Next(node);
      thread_node = RTEMS_CONTAINER_OF(node, Thread_cgroup_node, node);
      the_thread = thread_node->thread;
      _Thread_Set_state(the_thread, wait_state);
      node = next_node;
    }

    _Thread_Dispatch_enable(cpu_self);
  }
  return RTEMS_SUCCESSFUL;
}

bool _CORE_cgroup_Initialize(
  CORE_cgroup_Control *the_cgroup,
  CORE_cgroup_config  *config
)
{
  _Thread_queue_Object_initialize( &the_cgroup->Wait_queue );
  _Chain_Initialize_empty(&the_cgroup->threads);

  the_cgroup->state = STATES_READY;
  the_cgroup->config_storage = *config;
  the_cgroup->config = &the_cgroup->config_storage;
  the_cgroup->thread_count = 0;
  the_cgroup->max_threads = 10;
  the_cgroup->cpu_usage_total = 0;
  the_cgroup->cpu_period_arrival_time = 0;
  the_cgroup->shrink_callback = NULL;
  the_cgroup->shrink_arg = NULL;

  _ISR_lock_Initialize(&the_cgroup->quota_lock, "cgroup_quota_lock");

  cg_watchdog_setup(&the_cgroup->cpu_suspend_watchdog, the_cgroup, cpu_suspend_routine);
  cg_watchdog_setup(&the_cgroup->cpu_resume_watchdog, the_cgroup, cpu_resume_routine);

  set_cpu_resume_watchdog(the_cgroup);

  the_cgroup->mem_quota_available = the_cgroup->config->memory_limit;
  the_cgroup->should_charge_next_free = true;

  return true;
}

void _CORE_cgroup_Destroy(
  CORE_cgroup_Control *the_cgroup
)
{
  _Assert( _Chain_Is_empty( &the_cgroup->threads ) );
  _Assert( the_cgroup->thread_count == 0 );

  cancel_cpu_suspend_watchdog( the_cgroup );
  cancel_cpu_resume_watchdog( the_cgroup );
  _ISR_lock_Destroy( &the_cgroup->quota_lock );
  _Thread_queue_Destroy( &the_cgroup->Wait_queue );

  the_cgroup->config = NULL;
  memset( &the_cgroup->config_storage, 0, sizeof( the_cgroup->config_storage ) );
  the_cgroup->state = STATES_READY;
  the_cgroup->max_threads = 0;
  the_cgroup->cpu_usage_total = 0;
  the_cgroup->cpu_quota_available = 0;
  the_cgroup->cpu_period_arrival_time = 0;
  the_cgroup->mem_quota_available = 0;
  the_cgroup->shrink_callback = NULL;
  the_cgroup->shrink_arg = NULL;
  the_cgroup->should_charge_next_free = false;
}

rtems_status_code _CORE_cgroup_Resume(
  CORE_cgroup_Control *the_cgroup,
  States_Control      wait_state
)
{
  Per_CPU_Control *cpu_self;

  if(the_cgroup->state && (wait_state & STATES_BLOCKED) == 0) {
      return RTEMS_INCORRECT_STATE;
  }

  the_cgroup->state &= ~wait_state;

  if((the_cgroup->state & STATES_BLOCKED) == 0) {
      Chain_Node *node;
      Chain_Node *next_node;
      Thread_cgroup_node *thread_node;
      Thread_Control *the_thread;

      cpu_self = _Thread_Dispatch_disable();

      node = _Chain_First(&the_cgroup->threads);
      while (!_Chain_Is_tail(&the_cgroup->threads, node)) {
          next_node = _Chain_Next(node);
          thread_node = RTEMS_CONTAINER_OF(node, Thread_cgroup_node, node);
          the_thread = thread_node->thread;
          _Thread_Clear_state(the_thread, wait_state);
          node = next_node;
      }

      _Thread_Dispatch_enable(cpu_self);
  }
  return RTEMS_SUCCESSFUL;
}

bool _CORE_cgroup_Add_thread(
  CORE_cgroup_Control *cgroup,
  Thread_Control      *thread
)
{
  Thread_cgroup_node *cgroup_node;
  ISR_Level level;

  cgroup_node = _Workspace_Allocate(sizeof(Thread_cgroup_node));
  if (cgroup_node == NULL) {
    return false;
  }

  _Chain_Initialize_node(&cgroup_node->node);
  cgroup_node->thread = thread;
  cgroup_node->cgroup = cgroup;
  cgroup_node->cpu_usage = 0;

  _ISR_Local_disable(level);

  _Chain_Append_unprotected(&cgroup->threads, &cgroup_node->node);
  cgroup->thread_count++;

  thread->cgroup = cgroup;
  thread->is_added_to_cgroup = true;

  _ISR_Local_enable(level);

  return true;
}

bool _CORE_cgroup_Remove_thread(
  CORE_cgroup_Control *cgroup,
  Thread_Control      *thread
)
{
  Thread_cgroup_node *cgroup_node;
  ISR_Level level;

  cgroup_node = _CORE_cgroup_Find_thread(cgroup, thread);
  if (cgroup_node == NULL) {
    return false;
  }

  _ISR_Local_disable(level);

  thread->cgroup = NULL;
  thread->is_added_to_cgroup = false;
  thread->last_cpu_start_timestamp = 0;
  _Chain_Extract_unprotected(&cgroup_node->node);
  cgroup->thread_count--;

  _ISR_Local_enable(level);

  _Workspace_Free(cgroup_node);

  return true;
}

Thread_cgroup_node *_CORE_cgroup_Find_thread(
  CORE_cgroup_Control *cgroup,
  Thread_Control      *thread
)
{
  Chain_Node *node;
  Thread_cgroup_node *cgroup_node;
  ISR_Level level;
  Thread_cgroup_node *result = NULL;

  _ISR_Local_disable(level);

  for (node = _Chain_First(&cgroup->threads);
       !_Chain_Is_tail(&cgroup->threads, node);
       node = _Chain_Next(node)) {

    cgroup_node = RTEMS_CONTAINER_OF(node, Thread_cgroup_node, node);

    if (cgroup_node->thread == thread) {
      result = cgroup_node;
      break;
    }
  }

  _ISR_Local_enable(level);
  return result;
}

void _CORE_cgroup_Consume_cpu_quota(
  CORE_cgroup_Control *cgroup,
  uint64_t            cpu_ticks_delta
)
{
  ISR_lock_Context lock_context;

  _ISR_lock_ISR_disable_and_acquire( &cgroup->quota_lock, &lock_context );

  if(cgroup->cpu_quota_available <= cpu_ticks_delta) {
    cgroup->cpu_quota_available = 0;
    _ISR_lock_Release_and_ISR_enable( &cgroup->quota_lock, &lock_context );
    _CORE_cgroup_Suspend(cgroup, STATES_WAITING_FOR_CGROUP_CPU_QUOTA);
  }
  else {
    cgroup->cpu_quota_available -= cpu_ticks_delta;
    _ISR_lock_Release_and_ISR_enable( &cgroup->quota_lock, &lock_context );
  }
}

uint64_t _CORE_cgroup_Get_total_cpu_usage(CORE_cgroup_Control *cgroup)
{
  Chain_Node *node;
  Thread_cgroup_node *cgroup_node;
  uint64_t total;
  ISR_Level level;

  _ISR_Local_disable(level);

  total = cgroup->cpu_usage_total;

  for (node = _Chain_First(&cgroup->threads);
       !_Chain_Is_tail(&cgroup->threads, node);
       node = _Chain_Next(node)) {

    cgroup_node = RTEMS_CONTAINER_OF(node, Thread_cgroup_node, node);
    total += cgroup_node->cpu_usage;
  }

  _ISR_Local_enable(level);
  return total;
}

void _CORE_cgroup_set_cpu_quota(
  CORE_cgroup_Control *cgroup,
  uint32_t            cpu_quota,
  uint32_t            cpu_period
)
{
  cgroup->config->cpu_quota = cpu_quota;
  cgroup->config->cpu_period = cpu_period;
}

void _CORE_cgroup_set_shrink_callback(
  CORE_cgroup_Control *cgroup,
  void (*shrink_callback)(void *arg, uintptr_t target),
  void *shrink_arg
)
{
  cgroup->shrink_callback = shrink_callback;
  cgroup->shrink_arg = shrink_arg;
}
