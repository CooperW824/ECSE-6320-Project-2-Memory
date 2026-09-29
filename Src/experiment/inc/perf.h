#ifndef PERF_H
#define PERF_H

#include <stdint.h>
#include <time.h>

typedef struct {
	uint64_t nsec_start;
	uint64_t nsec_end;
} perf_Timer;

void perf_StartTimer(perf_Timer * timer);
void perf_EndTimer(perf_Timer * timer);
uint64_t perf_ComputeNSecDelta(const perf_Timer * timer);


#endif /* PERF_H */
