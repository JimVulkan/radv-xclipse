/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 */

/*
 * Xclipse field profiler: where does a real app's CPU time go, per thread, with the driver's
 * functions named. Off unless MESA_XCLIPSE_PROF (or the property debug.mesa_xclipse_prof) is set
 * to "<delay>[,<seconds>]": <delay> seconds after the screen is created, every thread in the
 * process is sampled at each tick (250 Hz) of its own CPU time for <seconds> (default 20), then a
 * helper thread writes mesa_prof_<pid>.txt into MESA_XCLIPSE_PROF_DIR, else $TMPDIR, else the app's
 * /sdcard/Android/data/<package>/files (which an app can always write and adb can read). Each sample
 * is the program counter plus the return addresses from the frame-pointer chain, bounded by the
 * thread's own stack mapping; the header gives frames presented and per-thread CPU time over the
 * window. Symbolize with tools/profsym.py.
 *
 * Nothing here runs on a GPU-waited thread: the signal handler only stores to memory, and all
 * file I/O happens on the helper thread.
 */
#if defined(__ANDROID__) && defined(__aarch64__)

#include "u_xclipse_prof.h"

#include <dirent.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#define PROF_SAMPLES (1u << 18)
#define PROF_DEPTH 6
#define PROF_THREADS 512

struct prof_sample {
   uintptr_t pc, ret[PROF_DEPTH];
   int tid, cpu;
};

struct prof_stack {
   int tid;
   uintptr_t lo, hi;
};

struct prof_thread {
   int tid;
   timer_t timer;
   bool has_timer;
   long ticks0;
   long long run0, runq0; /* schedstat: on-CPU and runnable-but-waiting ns */
   char comm[32];
};

static int prof_delay = -1, prof_seconds;
static struct prof_sample *prof_buf;
static atomic_uint prof_n;
static atomic_int prof_on;
static atomic_uint prof_frames;
static struct prof_stack prof_stacks[PROF_THREADS];
static atomic_int prof_nstacks;
static struct prof_thread prof_threads[PROF_THREADS];
static int prof_nthreads;
static atomic_llong prof_wait_ns[U_XCLIPSE_WAIT_COUNT];
static atomic_uint prof_wait_n[U_XCLIPSE_WAIT_COUNT];
static const char *const prof_wait_names[U_XCLIPSE_WAIT_COUNT] = {
   "dequeueBuffer", "swap_flush_and_fence", "client_fence", "bo_idle", "tc_sync", "queue_submit",
};

static void
prof_config(void)
{
   static atomic_int done;
   if (atomic_load(&done))
      return;
   const char *e = getenv("MESA_XCLIPSE_PROF");
   char v[PROP_VALUE_MAX] = {0};
   if (!(e && e[0]) && __system_property_get("debug.mesa_xclipse_prof", v) > 0)
      e = v;
   if (e && e[0]) {
      int d = -1, s = 20;
      if (sscanf(e, "%d,%d", &d, &s) >= 1 && d >= 0 && s > 0) {
         prof_delay = d;
         prof_seconds = s;
      }
   }
   atomic_store(&done, 1);
}

bool
u_xclipse_prof_armed(void)
{
   prof_config();
   return prof_delay >= 0;
}

__attribute__((visibility("default"))) bool
u_xclipse_prof_active(void)
{
   return atomic_load_explicit(&prof_on, memory_order_relaxed);
}

__attribute__((visibility("default"))) void
u_xclipse_prof_wait(enum u_xclipse_wait kind, int64_t ns)
{
   if (kind >= U_XCLIPSE_WAIT_COUNT || !atomic_load_explicit(&prof_on, memory_order_relaxed))
      return;
   atomic_fetch_add_explicit(&prof_wait_ns[kind], ns, memory_order_relaxed);
   atomic_fetch_add_explicit(&prof_wait_n[kind], 1, memory_order_relaxed);
}

static pthread_mutex_t prof_gpu_lock = PTHREAD_MUTEX_INITIALIZER;
static u_xclipse_prof_gpu_fn prof_gpu_fn;
static void *prof_gpu_data;

void
u_xclipse_prof_set_gpu_reporter(u_xclipse_prof_gpu_fn fn, void *data)
{
   pthread_mutex_lock(&prof_gpu_lock);
   prof_gpu_fn = fn;
   prof_gpu_data = data;
   pthread_mutex_unlock(&prof_gpu_lock);
}

void
u_xclipse_prof_frame(void)
{
   atomic_fetch_add_explicit(&prof_frames, 1, memory_order_relaxed);
}

unsigned
u_xclipse_prof_frames(void)
{
   return atomic_load_explicit(&prof_frames, memory_order_relaxed);
}

static void
prof_handler(int sig, siginfo_t *si, void *uc_)
{
   (void)sig;
   (void)si;
   if (!atomic_load_explicit(&prof_on, memory_order_relaxed))
      return;
   const unsigned i = atomic_fetch_add_explicit(&prof_n, 1, memory_order_relaxed);
   if (i >= PROF_SAMPLES)
      return;
   const ucontext_t *uc = uc_;
   struct prof_sample *s = &prof_buf[i];
   const int tid = (int)syscall(SYS_gettid);
   s->pc = uc->uc_mcontext.pc;
   s->tid = tid;
   s->cpu = sched_getcpu();

   uintptr_t lo = 0, hi = 0;
   const int ns = atomic_load_explicit(&prof_nstacks, memory_order_acquire);
   for (int k = 0; k < ns; k++) {
      if (prof_stacks[k].tid == tid) {
         lo = prof_stacks[k].lo;
         hi = prof_stacks[k].hi;
         break;
      }
   }
   uintptr_t fp = uc->uc_mcontext.regs[29], floor = uc->uc_mcontext.sp;
   if (floor < lo)
      floor = lo;
   for (int d = 0; d < PROF_DEPTH; d++) {
      s->ret[d] = 0;
      /* Only ever read inside this thread's own stack mapping, above the current frame. JIT code
       * leaves garbage in x29: compare against hi - 16 so a pointer near the top of the address
       * space can't wrap past the check. */
      if (!hi || hi < 16 || (fp & 7) || fp < floor || fp > hi - 16)
         continue;
      const uintptr_t *fr = (const uintptr_t *)fp;
      s->ret[d] = fr[1];
      floor = fp + 16;
      fp = fr[0];
   }
}

/* Thread stacks from /proc/self/maps: bionic names them "[anon:stack_and_tls:<tid>]", the main
 * thread's is "[stack]". */
static void
prof_scan_stacks(void)
{
   FILE *f = fopen("/proc/self/maps", "r");
   if (!f)
      return;
   char line[512];
   const int pid = getpid();
   while (fgets(line, sizeof(line), f)) {
      unsigned long lo, hi;
      if (sscanf(line, "%lx-%lx", &lo, &hi) != 2)
         continue;
      int tid = 0;
      const char *p = strstr(line, "[anon:stack_and_tls:");
      if (p)
         tid = atoi(p + strlen("[anon:stack_and_tls:"));
      else if (strstr(line, "[stack]"))
         tid = pid;
      if (tid <= 0)
         continue;
      const int ns = atomic_load(&prof_nstacks);
      bool known = false;
      for (int k = 0; k < ns; k++)
         known |= prof_stacks[k].tid == tid;
      if (known || ns == PROF_THREADS)
         continue;
      prof_stacks[ns].tid = tid;
      prof_stacks[ns].lo = lo;
      prof_stacks[ns].hi = hi;
      atomic_store_explicit(&prof_nstacks, ns + 1, memory_order_release);
   }
   fclose(f);
}

static long
prof_thread_ticks(int tid, char *comm, size_t comm_size)
{
   char path[64], buf[512];
   snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
   FILE *f = fopen(path, "r");
   if (!f)
      return -1;
   const size_t len = fread(buf, 1, sizeof(buf) - 1, f);
   fclose(f);
   buf[len] = 0;
   char *open = strchr(buf, '('), *close = strrchr(buf, ')');
   if (!open || !close)
      return -1;
   if (comm) {
      size_t n = close - open - 1;
      if (n >= comm_size)
         n = comm_size - 1;
      memcpy(comm, open + 1, n);
      comm[n] = 0;
      for (char *q = comm; *q; q++)
         if (*q == ' ')
            *q = '_';
   }
   /* Fields after ')': state is field 3, utime 14, stime 15. */
   char *save = NULL, *tok = strtok_r(close + 1, " ", &save);
   long ticks = 0;
   for (int k = 0; tok && k <= 12; k++, tok = strtok_r(NULL, " ", &save))
      if (k == 11 || k == 12)
         ticks += strtol(tok, NULL, 10);
   return ticks;
}

static bool
prof_schedstat(int tid, long long *run, long long *runq)
{
   char path[64];
   snprintf(path, sizeof(path), "/proc/self/task/%d/schedstat", tid);
   FILE *f = fopen(path, "r");
   if (!f)
      return false;
   const bool ok = fscanf(f, "%lld %lld", run, runq) == 2;
   fclose(f);
   return ok;
}

static void
prof_scan_threads(void)
{
   DIR *d = opendir("/proc/self/task");
   if (!d)
      return;
   struct dirent *e;
   while ((e = readdir(d))) {
      const int tid = atoi(e->d_name);
      if (tid <= 0 || prof_nthreads == PROF_THREADS)
         continue;
      bool known = false;
      for (int k = 0; k < prof_nthreads; k++)
         known |= prof_threads[k].tid == tid;
      if (known)
         continue;
      struct prof_thread *t = &prof_threads[prof_nthreads++];
      t->tid = tid;
      t->ticks0 = prof_thread_ticks(tid, NULL, 0);
      if (!prof_schedstat(tid, &t->run0, &t->runq0))
         t->run0 = t->runq0 = -1;
      /* MAKE_THREAD_CPUCLOCK(tid, CPUCLOCK_SCHED) */
      const clockid_t clk = (clockid_t)((~(unsigned)tid << 3) | 6);
      struct sigevent ev;
      memset(&ev, 0, sizeof(ev));
      ev.sigev_notify = SIGEV_THREAD_ID;
      ev.sigev_signo = SIGPROF;
      ev._sigev_un._tid = tid;
      if (timer_create(clk, &ev, &t->timer) == 0) {
         struct itimerspec it = {{0, 4000000}, {0, 4000000}};
         timer_settime(t->timer, 0, &it, NULL);
         t->has_timer = true;
      }
   }
   closedir(d);
}

static FILE *
prof_open(void)
{
   char path[512], pkg[256] = {0};
   const char *dirs[4] = {getenv("MESA_XCLIPSE_PROF_DIR"), getenv("TMPDIR"), NULL,
                          "/data/local/tmp"};
   /* An app launched without an environment (Eden, any APK) still has its own external files
    * directory: the process name is the package, up to a ':' for secondary processes. */
   FILE *c = fopen("/proc/self/cmdline", "r");
   if (c) {
      if (fgets(pkg, sizeof(pkg), c)) {
         char *colon = strchr(pkg, ':');
         if (colon)
            *colon = 0;
      }
      fclose(c);
   }
   char appdir[320];
   if (pkg[0] && strchr(pkg, '.')) {
      snprintf(appdir, sizeof(appdir), "/sdcard/Android/data/%s/files", pkg);
      dirs[2] = appdir;
   }
   for (int i = 0; i < 4; i++) {
      if (!dirs[i] || !dirs[i][0])
         continue;
      snprintf(path, sizeof(path), "%s/mesa_prof_%d.txt", dirs[i], getpid());
      FILE *f = fopen(path, "w");
      if (f)
         return f;
   }
   return NULL;
}

static void
prof_write(double seconds, unsigned frames)
{
   FILE *f = prof_open();
   if (!f)
      return;
   fprintf(f, "# seconds %.2f frames %u fps %.1f clk_tck %ld\n", seconds, frames,
           frames / seconds, sysconf(_SC_CLK_TCK));
   for (int k = 0; k < prof_nthreads; k++) {
      struct prof_thread *t = &prof_threads[k];
      strcpy(t->comm, "?");
      const long t1 = prof_thread_ticks(t->tid, t->comm, sizeof(t->comm));
      long long run1 = -1, runq1 = -1;
      prof_schedstat(t->tid, &run1, &runq1);
      if (t1 >= 0 && t->ticks0 >= 0 && t1 > t->ticks0)
         fprintf(f, "# thread %d %s cpu_ticks %ld run_ms %lld runq_ms %lld\n", t->tid, t->comm,
                 t1 - t->ticks0, t->run0 >= 0 && run1 >= 0 ? (run1 - t->run0) / 1000000 : -1,
                 t->runq0 >= 0 && runq1 >= 0 ? (runq1 - t->runq0) / 1000000 : -1);
   }
   /* Give the last IBs of the window time to retire, then ask the driver. */
   usleep(200000);
   pthread_mutex_lock(&prof_gpu_lock);
   if (prof_gpu_fn)
      prof_gpu_fn(prof_gpu_data, f);
   pthread_mutex_unlock(&prof_gpu_lock);
   for (int k = 0; k < U_XCLIPSE_WAIT_COUNT; k++)
      fprintf(f, "# wait %s ms %lld count %u\n", prof_wait_names[k],
              (long long)atomic_load(&prof_wait_ns[k]) / 1000000, atomic_load(&prof_wait_n[k]));
   unsigned n = atomic_load(&prof_n);
   if (n > PROF_SAMPLES)
      n = PROF_SAMPLES;
   for (unsigned i = 0; i < n; i++) {
      const struct prof_sample *s = &prof_buf[i];
      const char *comm = "?";
      for (int k = 0; k < prof_nthreads; k++)
         if (prof_threads[k].tid == s->tid) {
            comm = prof_threads[k].comm;
            break;
         }
      fprintf(f, "%d %d %s", s->tid, s->cpu, comm);
      for (int d = -1; d < PROF_DEPTH; d++) {
         if (d >= 0 && !s->ret[d])
            break;
         const uintptr_t a = d < 0 ? s->pc : s->ret[d] - 4; /* the call site */
         Dl_info di;
         if (dladdr((void *)a, &di) && di.dli_fname)
            fprintf(f, " %s 0x%lx", di.dli_fname, (unsigned long)(a - (uintptr_t)di.dli_fbase));
         else
            fprintf(f, " ? 0x%lx", (unsigned long)a);
      }
      fputc('\n', f);
   }
   fclose(f);
}

static double
prof_now(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec + t.tv_nsec / 1e9;
}

static void *
prof_thread_main(void *arg)
{
   (void)arg;
   sleep(prof_delay);
   prof_scan_stacks();
   const unsigned f0 = atomic_load(&prof_frames);
   const double t0 = prof_now();
   atomic_store(&prof_on, 1);
   while (prof_now() - t0 < prof_seconds) {
      prof_scan_stacks();
      prof_scan_threads();
      usleep(250000);
   }
   atomic_store(&prof_on, 0);
   const double seconds = prof_now() - t0;
   const unsigned frames = atomic_load(&prof_frames) - f0;
   for (int k = 0; k < prof_nthreads; k++)
      if (prof_threads[k].has_timer)
         timer_delete(prof_threads[k].timer);
   prof_write(seconds, frames);
   return NULL;
}

void
u_xclipse_prof_start(void)
{
   static atomic_int started;
   if (!u_xclipse_prof_armed() || atomic_exchange(&started, 1))
      return;

   /* Never take SIGPROF from someone else (a profiler already attached). */
   struct sigaction old;
   if (sigaction(SIGPROF, NULL, &old) || (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN))
      return;

   prof_buf = calloc(PROF_SAMPLES, sizeof(*prof_buf));
   if (!prof_buf)
      return;
   struct sigaction sa;
   memset(&sa, 0, sizeof(sa));
   sa.sa_sigaction = prof_handler;
   sa.sa_flags = SA_SIGINFO | SA_RESTART;
   sigemptyset(&sa.sa_mask);
   sigaction(SIGPROF, &sa, NULL);

   /* The helper samples nothing itself. */
   sigset_t all, saved;
   sigfillset(&all);
   pthread_sigmask(SIG_BLOCK, &all, &saved);
   pthread_t th;
   pthread_attr_t attr;
   pthread_attr_init(&attr);
   pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
   pthread_create(&th, &attr, prof_thread_main, NULL);
   pthread_attr_destroy(&attr);
   pthread_sigmask(SIG_SETMASK, &saved, NULL);
}

#endif
