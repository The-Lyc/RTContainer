#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <rtems.h>
#include <rtems/counter.h>
#include <rtems/score/container.h>
#include <rtems/score/threadimpl.h>
#include <tmacros.h>

#include <inttypes.h>
#include <sched.h>
#include <stdint.h>

const char rtems_test_name[] = "CONTAINER 06";

#define WORKER_COUNT 2u
#define WORKER_PRIORITY 20u
#define SAMPLE_COUNT 1000u
#define WARMUP_HANDOFFS 100u
#define TOTAL_HANDOFFS (WARMUP_HANDOFFS + SAMPLE_COUNT)
#define TEST_TIMEOUT_SECONDS 30u
#define BENCHMARK_CPU_INDEX 0u

/* Events sent to either worker (task events are private to each task). */
#define HANDOFF_EVENT RTEMS_EVENT_0
#define STOP_EVENT RTEMS_EVENT_1

/* Events sent by the workers to Init. */
#define READY_EVENT_0 RTEMS_EVENT_0
#define READY_EVENT_1 RTEMS_EVENT_1
#define DONE_EVENT_0 RTEMS_EVENT_2
#define DONE_EVENT_1 RTEMS_EVENT_3

typedef struct {
  rtems_id task_id;
  RtemsContainer *container;
} WorkerContext;

typedef struct {
  uint32_t sample_count;
  uint64_t total_ns;
  uint64_t min_ns;
  uint64_t max_ns;
} HandoffStatistics;

static WorkerContext workers[WORKER_COUNT];
static HandoffStatistics statistics;
static rtems_id init_task_id;

/*
 * There is exactly one ping-pong token.  The source worker writes these
 * values before waking the target, and the target reads them after its wait
 * returns.  Since both workers are pinned to one CPU, only one worker can
 * access the handoff state at a time.
 */
static volatile uint32_t handoff_sequence;
static volatile rtems_counter_ticks handoff_start_tick;

static rtems_event_set ready_event(uint32_t worker_index)
{
  return worker_index == 0u ? READY_EVENT_0 : READY_EVENT_1;
}

static rtems_event_set done_event(uint32_t worker_index)
{
  return worker_index == 0u ? DONE_EVENT_0 : DONE_EVENT_1;
}

static void pin_task_to_benchmark_cpu(rtems_id task_id)
{
#if defined(RTEMS_SMP)
  cpu_set_t set;
  rtems_status_code sc;

  CPU_ZERO(&set);
  CPU_SET(BENCHMARK_CPU_INDEX, &set);
  sc = rtems_task_set_affinity(task_id, sizeof(set), &set);
  rtems_test_assert(sc == RTEMS_SUCCESSFUL);
#else
  (void) task_id;
#endif
}

static void record_handoff(rtems_counter_ticks end_tick)
{
  rtems_counter_ticks delta;
  uint64_t latency_ns;

  delta = rtems_counter_difference(end_tick, handoff_start_tick);
  latency_ns = rtems_counter_ticks_to_nanoseconds(delta);

  ++statistics.sample_count;
  statistics.total_ns += latency_ns;

  if (latency_ns < statistics.min_ns) {
    statistics.min_ns = latency_ns;
  }

  if (latency_ns > statistics.max_ns) {
    statistics.max_ns = latency_ns;
  }
}

static rtems_task worker_task(rtems_task_argument arg)
{
  uint32_t worker_index = (uint32_t) arg;
  uint32_t peer_index = worker_index ^ 1u;
  WorkerContext *ctx = &workers[worker_index];
  Thread_Control *self = _Thread_Get_executing();
  rtems_status_code sc;

  sc = rtems_unified_container_enter(ctx->container, self);
  rtems_test_assert(sc == RTEMS_SUCCESSFUL);
  rtems_test_assert(
    self->cgroup == rtems_unified_container_get_core_cgroup(ctx->container)
  );
  rtems_test_assert(rtems_scheduler_get_processor() == BENCHMARK_CPU_INDEX);

  /*
   * Startup synchronization: signal only after entering the assigned
   * container.  RTEMS retains pending event bits, so the initial kick cannot
   * be lost even if Init sends it just before this worker starts to wait.
   */
  sc = rtems_event_send(init_task_id, ready_event(worker_index));
  rtems_test_assert(sc == RTEMS_SUCCESSFUL);

  while (true) {
    rtems_event_set events;
    rtems_counter_ticks end_tick;
    uint32_t sequence;

    sc = rtems_event_receive(
      HANDOFF_EVENT | STOP_EVENT,
      RTEMS_EVENT_ANY | RTEMS_WAIT,
      RTEMS_NO_TIMEOUT,
      &events
    );

    /*
     * Timing end: this is the first operation after the target worker returns
     * from its blocking wait and obtains the benchmark CPU.
     */
    end_tick = rtems_counter_read();
    rtems_test_assert(sc == RTEMS_SUCCESSFUL);

    if ((events & STOP_EVENT) != 0u) {
      break;
    }

    sequence = handoff_sequence;

    /*
     * sequence == 0 is the untimed startup kick.  The first handoffs warm up
     * the event, scheduler, cache, and counter paths without becoming samples.
     */
    if (sequence > WARMUP_HANDOFFS) {
      record_handoff(end_tick);
    }

    if (sequence == TOTAL_HANDOFFS) {
      /* The peer triggered the final sample and now only needs to be stopped. */
      sc = rtems_event_send(workers[peer_index].task_id, STOP_EVENT);
      rtems_test_assert(sc == RTEMS_SUCCESSFUL);
      break;
    }

    /*
     * Timing start: the currently running worker records the start while it
     * still owns the CPU, immediately before explicitly handing off to the
     * worker in the other container.
     */
    handoff_start_tick = rtems_counter_read();
    handoff_sequence = sequence + 1u;

    /*
     * Switching trigger: make the peer runnable, then loop back and block on
     * our own event.  If the wakeup itself preempts us, the switch happens in
     * rtems_event_send(); otherwise event_receive() forces the same handoff.
     */
    sc = rtems_event_send(workers[peer_index].task_id, HANDOFF_EVENT);
    rtems_test_assert(sc == RTEMS_SUCCESSFUL);
  }

  sc = rtems_unified_container_leave(ctx->container, self);
  rtems_test_assert(sc == RTEMS_SUCCESSFUL);

  sc = rtems_event_send(init_task_id, done_event(worker_index));
  rtems_test_assert(sc == RTEMS_SUCCESSFUL);
  rtems_task_exit();
}

static rtems_task Init(rtems_task_argument arg)
{
  RtemsContainerConfig config;
  RtemsContainer *container_a;
  RtemsContainer *container_b;
  rtems_event_set received;
  rtems_interval timeout;
  rtems_status_code sc;
  uint64_t quota_ticks;
  uint64_t average_ns;

  (void) arg;

  TEST_BEGIN();

  init_task_id = rtems_task_self();
  timeout = rtems_clock_get_ticks_per_second() * TEST_TIMEOUT_SECONDS;

  handoff_sequence = 0u;
  handoff_start_tick = 0;
  statistics.sample_count = 0u;
  statistics.total_ns = 0u;
  statistics.min_ns = UINT64_MAX;
  statistics.max_ns = 0u;

  rtems_unified_container_config_initialize(&config);
  config.flags = RTEMS_UNIFIED_CONTAINER_CPU;

  /*
   * Give each CPU container more quota than the complete test timeout.  This
   * keeps quota throttling and replenishment periods out of the samples.
   */
  quota_ticks =
    (uint64_t) rtems_clock_get_ticks_per_second() *
    TEST_TIMEOUT_SECONDS *
    2u;
  config.cgroup_config.cpu_quota = quota_ticks;
  config.cgroup_config.cpu_period = quota_ticks * 2u;
  config.cgroup_config.memory_limit = 0;
  config.cgroup_config.blkio_limit = 0;

  sc = rtems_unified_container_create(&config, &container_a);
  rtems_test_assert(sc == RTEMS_SUCCESSFUL);
  sc = rtems_unified_container_create(&config, &container_b);
  rtems_test_assert(sc == RTEMS_SUCCESSFUL);
  rtems_test_assert(container_a != container_b);
  rtems_test_assert(
    rtems_unified_container_get_core_cgroup(container_a) !=
      rtems_unified_container_get_core_cgroup(container_b)
  );

  workers[0].container = container_a;
  workers[1].container = container_b;

  for (uint32_t i = 0; i < WORKER_COUNT; ++i) {
    sc = rtems_task_create(
      rtems_build_name('W', 'K', '0', '0' + i),
      WORKER_PRIORITY,
      RTEMS_MINIMUM_STACK_SIZE,
      RTEMS_DEFAULT_MODES,
      RTEMS_DEFAULT_ATTRIBUTES,
      &workers[i].task_id
    );
    rtems_test_assert(sc == RTEMS_SUCCESSFUL);

    pin_task_to_benchmark_cpu(workers[i].task_id);

    sc = rtems_task_start(workers[i].task_id, worker_task, i);
    rtems_test_assert(sc == RTEMS_SUCCESSFUL);
  }

  /*
   * Both workers must be inside different containers before the single token
   * is injected.  The startup kick itself has no start timestamp and is not a
   * sample, which prevents task creation/startup from contaminating min/max.
   */
  sc = rtems_event_receive(
    READY_EVENT_0 | READY_EVENT_1,
    RTEMS_EVENT_ALL | RTEMS_WAIT,
    timeout,
    &received
  );
  rtems_test_assert(sc == RTEMS_SUCCESSFUL);

  printf(
    "cross-container ping-pong: cpu=%u, warm-up=%u, samples=%u\n",
    BENCHMARK_CPU_INDEX,
    WARMUP_HANDOFFS,
    SAMPLE_COUNT
  );

  sc = rtems_event_send(workers[0].task_id, HANDOFF_EVENT);
  rtems_test_assert(sc == RTEMS_SUCCESSFUL);

  /* A finite Init wait turns any broken handoff/termination path into failure. */
  sc = rtems_event_receive(
    DONE_EVENT_0 | DONE_EVENT_1,
    RTEMS_EVENT_ALL | RTEMS_WAIT,
    timeout,
    &received
  );
  rtems_test_assert(sc == RTEMS_SUCCESSFUL);
  rtems_test_assert(handoff_sequence == TOTAL_HANDOFFS);
  rtems_test_assert(statistics.sample_count == SAMPLE_COUNT);

  average_ns = statistics.total_ns / statistics.sample_count;

  printf(
    "cross-container handoff latency: samples=%" PRIu32
    ", average=%" PRIu64 " ns, min=%" PRIu64
    " ns, max=%" PRIu64 " ns\n",
    statistics.sample_count,
    average_ns,
    statistics.min_ns,
    statistics.max_ns
  );
  printf(
    "measurement includes event synchronization, scheduler dispatch, "
    "ordinary task context switch, and cross-container handoff overhead; "
    "it is not a pure container-internal state-switch cost\n"
  );

  sc = rtems_unified_container_delete(container_a);
  rtems_test_assert(sc == RTEMS_SUCCESSFUL);
  sc = rtems_unified_container_delete(container_b);
  rtems_test_assert(sc == RTEMS_SUCCESSFUL);

  TEST_END();
  rtems_test_exit(0);
}

#define CONFIGURE_APPLICATION_NEEDS_CLOCK_DRIVER
#define CONFIGURE_APPLICATION_NEEDS_SIMPLE_CONSOLE_DRIVER

#define CONFIGURE_MAXIMUM_TASKS 4
#define CONFIGURE_MAXIMUM_CGROUPS 2

/* Workers outrank Init, so both can send DONE and execute rtems_task_exit(). */
#define CONFIGURE_INIT_TASK_PRIORITY 30
#define CONFIGURE_RTEMS_INIT_TASKS_TABLE
#define CONFIGURE_INIT_TASK_ATTRIBUTES RTEMS_FLOATING_POINT
#define CONFIGURE_INITIAL_EXTENSIONS RTEMS_TEST_INITIAL_EXTENSION

#define CONFIGURE_INIT
#include <rtems/confdefs.h>
