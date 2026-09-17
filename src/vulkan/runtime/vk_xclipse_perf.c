/*
 * "Is it us or the app?" instrument. See vk_xclipse_perf.h.
 *
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 *
 * Accounting is per thread: emulators present from several threads, compile on pools and wait on
 * the GPU from non-presenting threads. No verdict is printed unless the numbers support one.
 */

#include "vk_xclipse_perf.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/macros.h"
#include "util/os_time.h"
#include "util/u_atomic.h"

#ifdef __ANDROID__
#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
static inline int
xperf_tid(void)
{
   return (int)syscall(SYS_gettid);
}
#else
static inline int
xperf_tid(void)
{
   return 0;
}
#endif

static void xperf_log(const char *fmt, ...) PRINTFLIKE(1, 2);

static void
xperf_log(const char *fmt, ...)
{
   char buf[900];
   va_list ap;
   va_start(ap, fmt);
   vsnprintf(buf, sizeof(buf), fmt, ap);
   va_end(ap);
#ifdef __ANDROID__
   __android_log_print(ANDROID_LOG_INFO, "RADV_PERF", "%s", buf);
#else
   fprintf(stderr, "RADV_PERF: %s\n", buf);
#endif
}

static bool
xperf_prop_on(const char *prop, const char *env)
{
   const char *e = getenv(env);
   if (e)
      return e[0] == '1';
#ifdef __ANDROID__
   char v[PROP_VALUE_MAX] = {0};
   return __system_property_get(prop, v) > 0 && v[0] == '1';
#else
   (void)prop;
   return false;
#endif
}

bool
vk_xclipse_perf_enabled(void)
{
   static int cached = -1;
   if (cached < 0)
      cached = xperf_prop_on("debug.radv_xclipse_perf", "RADV_XCLIPSE_PERF");
   return cached;
}

static uint64_t
xperf_window_ns(void)
{
   static uint64_t cached;
   if (!cached) {
      uint64_t ms = 2000;
      const char *e = getenv("RADV_XCLIPSE_PERF_MS");
#ifdef __ANDROID__
      char v[PROP_VALUE_MAX] = {0};
      if (!e && __system_property_get("debug.radv_xclipse_perf_ms", v) > 0)
         e = v;
#endif
      if (e) {
         long got = strtol(e, NULL, 10);
         if (got >= 200 && got <= 60000)
            ms = (uint64_t)got;
      }
      cached = ms * 1000000ull;
   }
   return cached;
}

/* Per-thread accounting. Each slot's wait/compile figures are slices of that thread's wall clock
 * (never summed across threads). A thread that presents or submits is on the critical path. */
#define XPERF_MAX_THREADS 24

struct xperf_thread {
   int tid; /* 0 = free slot; claimed once with cmpxchg */
   uint64_t wait_ns;
   uint64_t pipe_ns;
   uint64_t submit_ns;
   uint32_t n_present;
   uint32_t n_wait;
   uint32_t n_submit;
   uint32_t n_pipe;
   uint32_t n_gs; /* pipelines containing a GEOMETRY stage -- the emulator-fallback probe */
};

static struct xperf_thread xperf_threads[XPERF_MAX_THREADS];
static uint32_t xperf_overflow; /* threads that did not fit; reported, never hidden */

static uint64_t t_window_start;
static int announced;

/* Cache the slot per thread (the lookup is a linear scan). */
static __thread struct xperf_thread *xperf_my_slot;

static struct xperf_thread *
xperf_slot(void)
{
   if (xperf_my_slot)
      return xperf_my_slot;

   const int me = xperf_tid();
   for (unsigned i = 0; i < XPERF_MAX_THREADS; i++) {
      struct xperf_thread *t = &xperf_threads[i];
      int cur = p_atomic_read(&t->tid);
      if (cur == me) {
         xperf_my_slot = t;
         return t;
      }
      if (cur == 0 && p_atomic_cmpxchg(&t->tid, 0, me) == 0) {
         xperf_my_slot = t;
         return t;
      }
   }
   p_atomic_inc(&xperf_overflow);
   return NULL;
}

static void xperf_maybe_report(void);

void
vk_xclipse_perf_submit(uint64_t ioctl_ns)
{
   if (!vk_xclipse_perf_enabled())
      return;
   struct xperf_thread *t = xperf_slot();
   if (t) {
      p_atomic_add(&t->submit_ns, ioctl_ns);
      p_atomic_inc(&t->n_submit);
   }
   /* Also a report trigger, so offscreen workloads produce output. */
   xperf_maybe_report();
}

void
vk_xclipse_perf_wait(uint64_t blocked_ns)
{
   if (!vk_xclipse_perf_enabled())
      return;
   struct xperf_thread *t = xperf_slot();
   if (t) {
      p_atomic_add(&t->wait_ns, blocked_ns);
      p_atomic_inc(&t->n_wait);
   }
}

void
vk_xclipse_perf_pipeline(uint64_t compile_ns, uint32_t count, uint32_t gs_count)
{
   if (!vk_xclipse_perf_enabled())
      return;
   struct xperf_thread *t = xperf_slot();
   if (t) {
      p_atomic_add(&t->pipe_ns, compile_ns);
      p_atomic_add(&t->n_pipe, count);
      p_atomic_add(&t->n_gs, gs_count);
   }
}

void
vk_xclipse_perf_present(void)
{
   if (!vk_xclipse_perf_enabled())
      return;
   struct xperf_thread *t = xperf_slot();
   if (t)
      p_atomic_inc(&t->n_present);
   xperf_maybe_report();
}

static void
xperf_maybe_report(void)
{
   uint64_t now = os_time_get_nano();
   uint64_t start = p_atomic_read(&t_window_start);
   if (start == 0) {
      p_atomic_cmpxchg(&t_window_start, 0, now);
      return;
   }

   uint64_t elapsed = now - start;
   if (elapsed < xperf_window_ns())
      return;
   if (p_atomic_cmpxchg(&t_window_start, start, now) != start)
      return; /* another thread is already reporting this window */

   /* Snapshot and clear. */
   struct xperf_thread snap[XPERF_MAX_THREADS];
   unsigned n = 0;
   uint32_t frames = 0, submits = 0, pipes = 0, waits = 0, gs_pipes = 0;
   uint64_t all_submit_ns = 0;

   for (unsigned i = 0; i < XPERF_MAX_THREADS; i++) {
      struct xperf_thread *t = &xperf_threads[i];
      int tid = p_atomic_read(&t->tid);
      if (tid == 0)
         continue;
      struct xperf_thread s = {
         .tid = tid,
         .wait_ns = p_atomic_xchg(&t->wait_ns, 0),
         .pipe_ns = p_atomic_xchg(&t->pipe_ns, 0),
         .submit_ns = p_atomic_xchg(&t->submit_ns, 0),
         .n_present = p_atomic_xchg(&t->n_present, 0),
         .n_wait = p_atomic_xchg(&t->n_wait, 0),
         .n_submit = p_atomic_xchg(&t->n_submit, 0),
         .n_pipe = p_atomic_xchg(&t->n_pipe, 0),
         .n_gs = p_atomic_xchg(&t->n_gs, 0),
      };
      frames += s.n_present;
      submits += s.n_submit;
      pipes += s.n_pipe;
      gs_pipes += s.n_gs;
      waits += s.n_wait;
      all_submit_ns += s.submit_ns;
      if (s.n_present || s.n_wait || s.n_submit || s.n_pipe || s.n_gs)
         snap[n++] = s;
   }

   const double win = (double)elapsed;

   if (p_atomic_inc_return(&announced) == 1)
      xperf_log("[PERF] ON (window %llums). Per-thread; crit = presents or submits. A verdict is "
                "printed only when one critical thread's own wall clock can support it.",
                (unsigned long long)(xperf_window_ns() / 1000000));

   if (n == 0) {
      xperf_log("[PERF] win=%llums IDLE (no driver activity)", (unsigned long long)(elapsed / 1000000));
      return;
   }

   /* Sort by wait+compile, descending -- the threads that explain where time went come first. */
   for (unsigned i = 1; i < n; i++) {
      struct xperf_thread key = snap[i];
      uint64_t k = key.wait_ns + key.pipe_ns;
      int j = (int)i - 1;
      while (j >= 0 && (snap[j].wait_ns + snap[j].pipe_ns) < k) {
         snap[j + 1] = snap[j];
         j--;
      }
      snap[j + 1] = key;
   }

   /* Verdict from the critical thread most blocked on the GPU. */
   double best_wait_pct = -1.0, best_pipe_pct = 0.0;
   int best_tid = 0;
   bool any_crit = false;
   for (unsigned i = 0; i < n; i++) {
      if (!snap[i].n_present && !snap[i].n_submit)
         continue; /* compile worker or similar: off the critical path */
      any_crit = true;
      double wp = 100.0 * (double)snap[i].wait_ns / win;
      if (wp > best_wait_pct) {
         best_wait_pct = wp;
         best_pipe_pct = 100.0 * (double)snap[i].pipe_ns / win;
         best_tid = snap[i].tid;
      }
   }

   char verdict[160];
   if (!any_crit) {
      snprintf(verdict, sizeof(verdict), "NONE (no thread presented or submitted)");
   } else if (frames == 0) {
      snprintf(verdict, sizeof(verdict), "NONE (offscreen: no presents, so no frame loop to limit)");
   } else if (best_pipe_pct >= 25.0) {
      snprintf(verdict, sizeof(verdict), "SHADER COMPILE (tid %d, %.0f%% of window)", best_tid, best_pipe_pct);
   } else if (best_wait_pct >= 50.0) {
      snprintf(verdict, sizeof(verdict), "GPU/US (tid %d blocked %.0f%% of window)", best_tid, best_wait_pct);
   } else if (best_wait_pct >= 20.0) {
      snprintf(verdict, sizeof(verdict), "MIXED (busiest crit tid %d blocked %.0f%%)", best_tid, best_wait_pct);
   } else {
      snprintf(verdict, sizeof(verdict), "APP CPU (no crit thread blocked >20%%; max %.0f%% on tid %d)",
               best_wait_pct < 0 ? 0.0 : best_wait_pct, best_tid);
   }

   char threads[640];
   size_t off = 0;
   threads[0] = '\0';
   for (unsigned i = 0; i < n && i < 6 && off < sizeof(threads) - 1; i++) {
      off += snprintf(threads + off, sizeof(threads) - off, " | t%d%s p=%u w=%.0f%%(%u) c=%.0f%% s=%u",
                      snap[i].tid, (snap[i].n_present || snap[i].n_submit) ? "*" : "",
                      snap[i].n_present, 100.0 * (double)snap[i].wait_ns / win, snap[i].n_wait,
                      100.0 * (double)snap[i].pipe_ns / win, snap[i].n_submit);
   }

   xperf_log("[PERF] win=%llums frames=%u fps=%.1f | LIMITER=%s | threads=%u%s%s%s | submits=%u "
             "ioctl=%.1fms avg=%.0fus pipelines=%u gs=%u waits=%u",
             (unsigned long long)(elapsed / 1000000), frames, 1e9 * frames / win, verdict, n, threads,
             xperf_overflow ? " | OVERFLOW=" : "", xperf_overflow ? "yes" : "", submits,
             all_submit_ns / 1e6, submits ? (all_submit_ns / 1e3) / (double)submits : 0.0, pipes,
             gs_pipes, waits);
}
