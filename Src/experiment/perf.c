#define _POSIX_C_SOURCE 199309L
#include "inc/perf.h"
#include <assert.h>
#include <time.h>

static uint64_t perf_NowNSec(void) {
  struct timespec ts;
  int rc = clock_gettime(CLOCK_MONOTONIC, &ts);
  assert(rc == 0);
  (void)rc;
  return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

void perf_StartTimer(perf_Timer *timer) { timer->nsec_start = perf_NowNSec(); }
void perf_EndTimer(perf_Timer *timer)   { timer->nsec_end   = perf_NowNSec(); }

uint64_t perf_ComputeNSecDelta(const perf_Timer *timer) {
  return timer->nsec_end - timer->nsec_start;
}