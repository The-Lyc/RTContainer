/**
 * @file
 *
 * @ingroup RTEMSClassicCgroup
 *
 * @brief RTEMS Remove Task from Cgroup
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

#include <rtems/rtems/cgroupimpl.h>
#include <rtems/rtems/tasks.h>
#include <rtems/score/threadimpl.h>

rtems_status_code rtems_cgroup_remove_task(
  rtems_id cgroup_id,
  rtems_id task_id
)
{
  Cgroup_Control    *the_cgroup;
  Thread_Control    *the_thread;
  ISR_lock_Context   lock_context;
  bool              is_removed;

  the_cgroup = _Cgroup_Get( cgroup_id, &lock_context );
  _ISR_lock_ISR_enable( &lock_context );
  if ( the_cgroup == NULL ) {
    return RTEMS_INVALID_ID;
  }


  the_thread = _Thread_Get( task_id, &lock_context );
  _ISR_lock_ISR_enable( &lock_context );
  if ( the_thread == NULL ) {
    return RTEMS_INVALID_ID;
  }


  is_removed = _CORE_cgroup_Remove_thread(
    &the_cgroup->cgroup,
    the_thread
  );

  if(is_removed) {
    return RTEMS_SUCCESSFUL;
  } else {
    return RTEMS_RESOURCE_IN_USE;
  }
}
