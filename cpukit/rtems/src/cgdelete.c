/**
 *  @file
 *
 *  @brief RTEMS Delete Cgroup
 *  @ingroup ClassicCgroup
 */

/*
 *  COPYRIGHT (c) 1989-2014.
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
#include <rtems/rtems/attrimpl.h>

rtems_status_code rtems_cgroup_delete(
  rtems_id id
)
{
  Cgroup_Control *the_cgroup;
  ISR_lock_Context   lock_context;

  _Objects_Allocator_lock();
  the_cgroup = _Cgroup_Get( id, &lock_context );

  if ( the_cgroup == NULL ) {
    _Objects_Allocator_unlock();
    return RTEMS_INVALID_ID;
  }

  if (
    the_cgroup->cgroup.thread_count != 0
      || !_Chain_Is_empty( &the_cgroup->cgroup.threads )
  ) {
    _ISR_lock_ISR_enable( &lock_context );
    _Objects_Allocator_unlock();
    return RTEMS_RESOURCE_IN_USE;
  }

  _Objects_Close( &_Cgroup_Information, &the_cgroup->Object );
  _CORE_cgroup_Destroy( &the_cgroup->cgroup );
  _ISR_lock_ISR_enable( &lock_context );
  _Cgroup_Free( the_cgroup );
  _Objects_Allocator_unlock();
  return RTEMS_SUCCESSFUL;
}
