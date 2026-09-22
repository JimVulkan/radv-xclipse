/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 */

/* Xclipse field profiler (Android, off by default). See u_xclipse_prof.c. */
#ifndef U_XCLIPSE_PROF_H
#define U_XCLIPSE_PROF_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where a thread blocked, summed over the sampling window. */
enum u_xclipse_wait {
   U_XCLIPSE_WAIT_DEQUEUE,      /* ANativeWindow_dequeueBuffer (EGL) */
   U_XCLIPSE_WAIT_SWAP,         /* whole eglSwapBuffers (EGL) */
   U_XCLIPSE_WAIT_CLIENT_FENCE, /* glClientWaitSync / glFinish style fence waits */
   U_XCLIPSE_WAIT_BO,           /* waiting for a buffer to go idle (maps) */
   U_XCLIPSE_WAIT_TC_SYNC,      /* the app thread waiting for the driver thread */
   U_XCLIPSE_WAIT_SUBMIT,       /* inside the driver's queue submit (Vulkan), ioctl included */
   U_XCLIPSE_WAIT_COUNT,
};

#if defined(__ANDROID__) && defined(__aarch64__)
void u_xclipse_prof_start(void);
void u_xclipse_prof_frame(void);
/* Frames presented so far (for drivers that tag GPU work with a frame). */
unsigned u_xclipse_prof_frames(void);
bool u_xclipse_prof_armed(void);
/* True only while sampling; time a wait only then. */
bool u_xclipse_prof_active(void);
void u_xclipse_prof_wait(enum u_xclipse_wait kind, int64_t ns);
/* The driver writes its GPU lines ("# gpu ...", "# pass ...") into the report, from the helper
 * thread after the window. */
typedef void (*u_xclipse_prof_gpu_fn)(void *data, FILE *f);
void u_xclipse_prof_set_gpu_reporter(u_xclipse_prof_gpu_fn fn, void *data);
#else
static inline void u_xclipse_prof_start(void) {}
static inline void u_xclipse_prof_frame(void) {}
static inline unsigned u_xclipse_prof_frames(void) { return 0; }
static inline bool u_xclipse_prof_armed(void) { return false; }
static inline bool u_xclipse_prof_active(void) { return false; }
static inline void u_xclipse_prof_wait(enum u_xclipse_wait kind, int64_t ns) {}
#endif

#ifdef __cplusplus
}
#endif

#endif
