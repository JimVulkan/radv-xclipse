/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 *
 * [CPUSHARE]: CPU time spent in this driver as a share of wall clock, printed once a second.
 * simpleperf needs root or a debuggable app, so the driver times itself.
 *
 * Measures CPU time in our code, not the frame's critical path. Costs two clock_gettime() calls
 * (~50 ns) per timed call; not applied to the high-rate BO paths.
 */

#ifndef RADV_CPUSHARE_H
#define RADV_CPUSHARE_H

#ifdef __ANDROID__

#include <stdint.h>
#include <time.h>

enum {
   RADV_CPU_DECODE = 0, /* recording a BCn decode: pipeline, descriptors, dispatch, barrier */
   RADV_CPU_SUBMIT = 1,   /* radv_queue_submit -- the per-frame driver cost */
   /* Pipeline creation, a subset of DECODE (first use of each BC variant compiles lazily);
    * steady-state decode is the difference. */
   RADV_CPU_PIPELINE = 2,
   /* vk_meta_create_compute_pipeline alone (the ACO compile), a subset of PIPELINE. */
   RADV_CPU_COMPILE = 3,
   RADV_CPU_NBUCKET
};

void radv_cpushare_add(int bucket, uint64_t ns);

static inline uint64_t
radv_cpushare_now(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

#define RADV_CPU_T0()   const uint64_t _radv_cpu_t0 = radv_cpushare_now()
#define RADV_CPU_T1(b)  radv_cpushare_add((b), radv_cpushare_now() - _radv_cpu_t0)

#else /* !__ANDROID__ */

#define RADV_CPU_T0()   do {} while (0)
#define RADV_CPU_T1(b)  do {} while (0)

#endif
#endif /* RADV_CPUSHARE_H */
