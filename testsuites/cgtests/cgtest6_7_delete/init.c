#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#define CONFIGURE_INIT
#include "system.h"

#include <inttypes.h>
#include <stdlib.h>

#include <rtems/test.h>
#include <rtems/libcsupport.h>
#include <rtems/score/heapinfo.h>
#include <rtems/score/watchdogimpl.h>
#include <rtems/score/corecgroup.h>
#include <rtems/score/isrlock.h>
#include <rtems/score/thread.h>
#include <rtems/score/threadimpl.h>

#include <rtems/rtems/cgroupimpl.h>
#include <rtems/rtems/event.h>
#include <rtems/rtems/tasks.h>

const char rtems_test_name[] = "CGTEST6-7 DELETE";

#define QUOTA_TASK_COUNT    4
#define QUOTA_CGROUP_COUNT  2
#define SHARED_BUFFER_SIZE  ( 2 * 1024 * 1024 )
#define RELEASE_EVENT       RTEMS_EVENT_31
#define PHYSICAL_DONE_EVENT RTEMS_EVENT_8
#define HUGE_MEMORY_LIMIT   ( (uint64_t) 1024 * 1024 * 1024 * 1024 )

typedef struct {
  const char *name;
  rtems_id    id;
  void       *shared_buffer;
  bool        shrink_called;
} Quota_Context;

static rtems_id Init_task_id;
static Quota_Context Quota_groups[ QUOTA_CGROUP_COUNT ] = {
  { .name = "CG1" },
  { .name = "CG2" }
};
static void *Quota_task_buffer[ QUOTA_TASK_COUNT ];
static const uint32_t Quota_task_to_group[ QUOTA_TASK_COUNT ] = {
  0, 0, 1, 1
};
static const uint32_t Quota_shared_owner[ QUOTA_CGROUP_COUNT ] = {
  0, 2
};
static const size_t Quota_task_alloc_size[ QUOTA_TASK_COUNT ] = {
  2 * 1024 * 1024,
  3 * 1024 * 1024,
  3 * 1024 * 1024,
  1 * 1024 * 1024
};

static void *Reclaimable_buffer;
static size_t Reclaimable_size;
static bool Physical_shrink_called;
static rtems_id Physical_cgroup_id;

static rtems_event_set quota_allocation_mask( void )
{
  rtems_event_set mask = 0;

  for ( uint32_t i = 0; i < QUOTA_TASK_COUNT; ++i ) {
    mask |= RTEMS_EVENT_0 << i;
  }

  return mask;
}

static rtems_event_set quota_cleanup_mask( void )
{
  rtems_event_set mask = 0;

  for ( uint32_t i = 0; i < QUOTA_TASK_COUNT; ++i ) {
    mask |= RTEMS_EVENT_4 << i;
  }

  return mask;
}

static void quota_shrinker( void *arg, uintptr_t target )
{
  Quota_Context *ctx = arg;

  ctx->shrink_called = true;
  printf(
    "[quota:%s] shrink requested for %lu bytes\n",
    ctx->name,
    (unsigned long) target
  );

  if ( ctx->shared_buffer != NULL ) {
    free( ctx->shared_buffer );
    ctx->shared_buffer = NULL;
    printf( "[quota:%s] released shared 2MB buffer\n", ctx->name );
  }
}

static rtems_task quota_worker( rtems_task_argument arg )
{
  uint32_t task_index = (uint32_t) arg;
  uint32_t group_index = Quota_task_to_group[ task_index ];
  Quota_Context *ctx = &Quota_groups[ group_index ];
  rtems_status_code status;
  rtems_event_set received;

  if ( task_index == Quota_shared_owner[ group_index ] ) {
    ctx->shared_buffer = malloc( SHARED_BUFFER_SIZE );
    rtems_test_assert( ctx->shared_buffer != NULL );
  }

  Quota_task_buffer[ task_index ] = malloc(
    Quota_task_alloc_size[ task_index ]
  );
  rtems_test_assert( Quota_task_buffer[ task_index ] != NULL );

  status = rtems_event_send(
    Init_task_id,
    RTEMS_EVENT_0 << task_index
  );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );

  status = rtems_event_receive(
    RELEASE_EVENT,
    RTEMS_EVENT_ALL | RTEMS_WAIT,
    RTEMS_NO_TIMEOUT,
    &received
  );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );

  free( Quota_task_buffer[ task_index ] );
  Quota_task_buffer[ task_index ] = NULL;

  status = rtems_cgroup_remove_task( ctx->id, rtems_task_self() );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );

  status = rtems_event_send(
    Init_task_id,
    RTEMS_EVENT_4 << task_index
  );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );

  rtems_task_exit();
}

static void run_quota_phase( void )
{
  CORE_cgroup_config configs[ QUOTA_CGROUP_COUNT ] = {
    {
      .cpu_shares = 1,
      .cpu_quota = _Watchdog_Ticks_per_second * 60,
      .cpu_period = _Watchdog_Ticks_per_second * 60,
      .memory_limit = 5 * 1024 * 1024,
      .blkio_limit = 0
    },
    {
      .cpu_shares = 1,
      .cpu_quota = _Watchdog_Ticks_per_second * 60,
      .cpu_period = _Watchdog_Ticks_per_second * 60,
      .memory_limit = 5 * 1024 * 1024,
      .blkio_limit = 0
    }
  };
  rtems_event_set received;
  rtems_status_code status;

  printf( "[merge] running CGTEST6-5 quota/shrink phase\n" );

  for ( uint32_t i = 0; i < QUOTA_CGROUP_COUNT; ++i ) {
    status = rtems_cgroup_create(
      rtems_build_name( 'Q', 'G', '1' + i, ' ' ),
      &Quota_groups[ i ].id,
      &configs[ i ]
    );
    rtems_test_assert( status == RTEMS_SUCCESSFUL );

    ISR_lock_Context lock_context;
    Cgroup_Control *the_cgroup = _Cgroup_Get(
      Quota_groups[ i ].id,
      &lock_context
    );

    rtems_test_assert( the_cgroup != NULL );
    the_cgroup->cgroup.shrink_callback = quota_shrinker;
    the_cgroup->cgroup.shrink_arg = &Quota_groups[ i ];
    _ISR_lock_ISR_enable( &lock_context );
  }

  for ( uint32_t i = 0; i < QUOTA_TASK_COUNT; ++i ) {
    uint32_t group_index = Quota_task_to_group[ i ];

    status = rtems_task_create(
      rtems_build_name( '6', '7', 'Q', '0' + i ),
      9 - i,
      RTEMS_MINIMUM_STACK_SIZE,
      RTEMS_DEFAULT_MODES,
      RTEMS_DEFAULT_ATTRIBUTES,
      &Task_id[ i ]
    );
    rtems_test_assert( status == RTEMS_SUCCESSFUL );

    status = rtems_cgroup_add_task(
      Quota_groups[ group_index ].id,
      Task_id[ i ]
    );
    rtems_test_assert( status == RTEMS_SUCCESSFUL );

    status = rtems_task_start( Task_id[ i ], quota_worker, i );
    rtems_test_assert( status == RTEMS_SUCCESSFUL );
  }

  status = rtems_event_receive(
    quota_allocation_mask(),
    RTEMS_EVENT_ALL | RTEMS_WAIT,
    RTEMS_NO_TIMEOUT,
    &received
  );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );
  rtems_test_assert( Quota_groups[ 0 ].shrink_called );
  rtems_test_assert( Quota_groups[ 1 ].shrink_called );

  for ( uint32_t i = 0; i < QUOTA_CGROUP_COUNT; ++i ) {
    status = rtems_cgroup_delete( Quota_groups[ i ].id );
    rtems_test_assert( status == RTEMS_RESOURCE_IN_USE );
  }
  printf( "[delete] active cgroups correctly rejected as resource-in-use\n" );

  for ( uint32_t i = 0; i < QUOTA_TASK_COUNT; ++i ) {
    status = rtems_event_send( Task_id[ i ], RELEASE_EVENT );
    rtems_test_assert( status == RTEMS_SUCCESSFUL );
  }

  status = rtems_event_receive(
    quota_cleanup_mask(),
    RTEMS_EVENT_ALL | RTEMS_WAIT,
    RTEMS_NO_TIMEOUT,
    &received
  );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );

  for ( uint32_t i = 0; i < QUOTA_CGROUP_COUNT; ++i ) {
    uint32_t count;
    rtems_cgroup_state state;
    rtems_id old_id = Quota_groups[ i ].id;

    status = rtems_cgroup_get_task_count( old_id, &count );
    rtems_test_assert( status == RTEMS_SUCCESSFUL );
    rtems_test_assert( count == 0 );

    status = rtems_cgroup_delete( old_id );
    rtems_test_assert( status == RTEMS_SUCCESSFUL );

    status = rtems_cgroup_get_state( old_id, &state );
    rtems_test_assert( status == RTEMS_INVALID_ID );
  }

  printf( "[delete] quota-phase cgroups removed and old IDs invalidated\n" );
}

static void report_memory( const char *tag )
{
  Heap_Information_block info;

  rtems_test_assert( malloc_info( &info ) == 0 );
  printf(
    "[physical] %s: occupied=%" PRIuPTR ", free=%" PRIuPTR "\n",
    tag,
    (uintptr_t) ( info.Stats.size - info.Stats.free_size ),
    (uintptr_t) info.Stats.free_size
  );
}

static uintptr_t heap_largest_free( void )
{
  Heap_Information_block info;

  rtems_test_assert( malloc_info( &info ) == 0 );
  return info.Free.largest;
}

static uintptr_t heap_total_size( void )
{
  Heap_Information_block info;

  rtems_test_assert( malloc_info( &info ) == 0 );
  return info.Stats.size;
}

static uint32_t heap_failed_allocs( void )
{
  Heap_Information_block info;

  rtems_test_assert( malloc_info( &info ) == 0 );
  return info.Stats.failed_allocs;
}

static void physical_shrinker( void *arg, uintptr_t target )
{
  (void) arg;

  Physical_shrink_called = true;
  printf(
    "[physical] shrink requested for %lu bytes\n",
    (unsigned long) target
  );

  if ( Reclaimable_buffer != NULL ) {
    free( Reclaimable_buffer );
    Reclaimable_buffer = NULL;
    Reclaimable_size = 0;
  }
}

static void physical_shrink_rescue( void )
{
  uintptr_t largest = heap_largest_free();
  uintptr_t reclaim_size = largest / 2 + largest / 4;
  uintptr_t probe_size = largest / 2;
  void *probe;

  Reclaimable_buffer = malloc( reclaim_size );
  rtems_test_assert( Reclaimable_buffer != NULL );
  Reclaimable_size = reclaim_size;
  rtems_test_assert( probe_size > heap_largest_free() );

  Physical_shrink_called = false;
  report_memory( "before recoverable OOM" );
  probe = malloc( probe_size );
  report_memory( "after recoverable OOM" );

  rtems_test_assert( Physical_shrink_called );
  rtems_test_assert( probe != NULL );
  free( probe );
}

static void physical_unrecoverable_oom( void )
{
  uintptr_t reclaim_size = heap_largest_free() / 2;
  uintptr_t impossible = heap_total_size() * 2;
  uint32_t failed_before;
  void *huge;

  Reclaimable_buffer = malloc( reclaim_size );
  rtems_test_assert( Reclaimable_buffer != NULL );
  Reclaimable_size = reclaim_size;

  Physical_shrink_called = false;
  failed_before = heap_failed_allocs();
  huge = malloc( impossible );

  rtems_test_assert( Physical_shrink_called );
  rtems_test_assert( huge == NULL );
  rtems_test_assert( Reclaimable_buffer == NULL );
  rtems_test_assert( Reclaimable_size == 0 );
  rtems_test_assert( heap_failed_allocs() == failed_before + 1 );
}

static rtems_task physical_worker( rtems_task_argument arg )
{
  const Thread_Control *self = _Thread_Get_executing();
  rtems_status_code status;

  (void) arg;
  rtems_test_assert( self != NULL );
  rtems_test_assert( self->is_added_to_cgroup );
  rtems_test_assert( self->cgroup != NULL );

  physical_shrink_rescue();
  physical_unrecoverable_oom();

  status = rtems_cgroup_remove_task(
    Physical_cgroup_id,
    rtems_task_self()
  );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );

  status = rtems_event_send( Init_task_id, PHYSICAL_DONE_EVENT );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );
  rtems_task_exit();
}

static void run_physical_phase( void )
{
  CORE_cgroup_config config = {
    .cpu_shares = 1,
    .cpu_quota = _Watchdog_Ticks_per_second * 60,
    .cpu_period = _Watchdog_Ticks_per_second * 60,
    .memory_limit = HUGE_MEMORY_LIMIT,
    .blkio_limit = 0
  };
  ISR_lock_Context lock_context;
  Cgroup_Control *the_cgroup;
  rtems_event_set received;
  rtems_status_code status;

  printf( "[merge] running CGTEST6-6 physical-OOM phase\n" );

  status = rtems_cgroup_create(
    rtems_build_name( 'P', 'H', 'Y', 'S' ),
    &Physical_cgroup_id,
    &config
  );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );

  the_cgroup = _Cgroup_Get( Physical_cgroup_id, &lock_context );
  rtems_test_assert( the_cgroup != NULL );
  the_cgroup->cgroup.shrink_callback = physical_shrinker;
  the_cgroup->cgroup.shrink_arg = NULL;
  _ISR_lock_ISR_enable( &lock_context );

  status = rtems_task_create(
    rtems_build_name( '6', '7', 'P', '0' ),
    8,
    RTEMS_MINIMUM_STACK_SIZE,
    RTEMS_DEFAULT_MODES,
    RTEMS_DEFAULT_ATTRIBUTES,
    &Task_id[ 0 ]
  );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );

  status = rtems_cgroup_add_task( Physical_cgroup_id, Task_id[ 0 ] );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );

  status = rtems_task_start( Task_id[ 0 ], physical_worker, 0 );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );

  status = rtems_event_receive(
    PHYSICAL_DONE_EVENT,
    RTEMS_EVENT_ALL | RTEMS_WAIT,
    RTEMS_NO_TIMEOUT,
    &received
  );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );

  status = rtems_cgroup_delete( Physical_cgroup_id );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );
  printf( "[delete] physical-OOM cgroup deleted after worker cleanup\n" );
}

static void verify_object_reuse( void )
{
  CORE_cgroup_config configs[ 2 ];
  rtems_id ids[ 2 ];
  rtems_id extra_id;
  rtems_cgroup_state state;
  rtems_status_code status;

  printf( "[delete] verifying object-pool and watchdog reuse\n" );

  for ( uint32_t i = 0; i < 2; ++i ) {
    configs[ i ].cpu_shares = 1;
    configs[ i ].cpu_quota = 1;
    configs[ i ].cpu_period = 1;
    configs[ i ].memory_limit = 1024 * 1024;
    configs[ i ].blkio_limit = 0;

    status = rtems_cgroup_create(
      rtems_build_name( 'R', 'E', '0' + i, ' ' ),
      &ids[ i ],
      &configs[ i ]
    );
    rtems_test_assert( status == RTEMS_SUCCESSFUL );
  }

  status = rtems_cgroup_create(
    rtems_build_name( 'F', 'U', 'L', 'L' ),
    &extra_id,
    &configs[ 0 ]
  );
  rtems_test_assert( status == RTEMS_TOO_MANY );

  for ( uint32_t i = 0; i < 2; ++i ) {
    rtems_id old_id = ids[ i ];

    status = rtems_cgroup_delete( old_id );
    rtems_test_assert( status == RTEMS_SUCCESSFUL );

    status = rtems_cgroup_get_state( old_id, &state );
    rtems_test_assert( status == RTEMS_INVALID_ID );
  }

  status = rtems_task_wake_after( 2 );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );

  status = rtems_cgroup_create(
    rtems_build_name( 'R', 'E', 'U', 'S' ),
    &extra_id,
    &configs[ 0 ]
  );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );

  status = rtems_cgroup_delete( extra_id );
  rtems_test_assert( status == RTEMS_SUCCESSFUL );
  status = rtems_cgroup_delete( extra_id );
  rtems_test_assert( status == RTEMS_INVALID_ID );

  printf( "[delete] object slots reused; stale ID rejected; watchdogs stayed cancelled\n" );
}

rtems_task Init( rtems_task_argument ignored )
{
  (void) ignored;

  rtems_print_printer_fprintf_putc( &rtems_test_printer );
  TEST_BEGIN();

  Init_task_id = rtems_task_self();

  run_quota_phase();
  run_physical_phase();
  verify_object_reuse();

  printf( "CGTEST6-7 completed: merged memory tests and cgroup deletion passed\n" );
  TEST_END();
  rtems_test_exit( 0 );
}
