/**
 *  @file
 *
 *  @brief RTEMS Create Cgroup
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
#include <rtems/rtems/status.h>
#include <rtems/rtems/attrimpl.h>
#include <rtems/rtems/options.h>
#include <rtems/rtems/support.h>
#include <rtems/score/sysstate.h>
#include <rtems/score/chain.h>
#include <rtems/score/isr.h>
#include <rtems/score/corecgroupimpl.h>
#include <rtems/score/thread.h>
#include <rtems/score/wkspace.h>
#include <rtems/sysinit.h>

rtems_status_code rtems_cgroup_create(
  rtems_name            name,
  rtems_id             *id,
  CORE_cgroup_config   *core_config
)
{
  Cgroup_Control      *the_cgroup;

  if ( !rtems_is_name_valid( name ) ) {
    return RTEMS_INVALID_NAME;
  }

  if ( !id ) {
    return RTEMS_INVALID_ADDRESS;
  }
  if ( !core_config ) {
    return RTEMS_INVALID_ADDRESS;
  }


  the_cgroup = _Cgroup_Allocate();

  if ( !the_cgroup ) {
    _Objects_Allocator_unlock();
    return RTEMS_TOO_MANY;
  }

  if ( ! _CORE_cgroup_Initialize(&the_cgroup->cgroup, core_config) ) {
    _Cgroup_Free( the_cgroup );
    _Objects_Allocator_unlock();
    return RTEMS_UNSATISFIED;
  }

  _Objects_Open_u32(
    &_Cgroup_Information,
    &the_cgroup->Object,
    (uint32_t) name
  );

  *id = the_cgroup->Object.id;

  _Objects_Allocator_unlock();
  return RTEMS_SUCCESSFUL;
}

static void _Cgroup_Manager_initialization( void )
{
  _Objects_Initialize_information( &_Cgroup_Information);
}

RTEMS_SYSINIT_ITEM(
  _Cgroup_Manager_initialization,
  RTEMS_SYSINIT_CLASSIC_CGROUP,
  RTEMS_SYSINIT_ORDER_MIDDLE
);
