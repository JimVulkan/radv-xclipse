/*
 * "Is it us or the app?" instrument: is a slow game limited by the GPU or by the emulator's CPU?
 *
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 *
 * Per thread, over a fixed window:
 *
 *   - blocked in vkWaitForFences  -> it is waiting for the GPU        -> WE are the limiter
 *   - inside vkCreate*Pipelines   -> it is compiling shaders          -> compile stall
 *   - everything else             -> it is running emulator CPU code  -> the APP is the limiter
 *
 * Costs two vDSO clock_gettime() calls and a few atomics per event; no syscalls, I/O or threads.
 * Off by default; the gate is announced on the first report.
 *
 *   setprop debug.radv_xclipse_perf 1     (or RADV_XCLIPSE_PERF=1)
 *   setprop debug.radv_xclipse_perf_ms N  (report window, default 2000)
 */

#ifndef VK_XCLIPSE_PERF_H
#define VK_XCLIPSE_PERF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Cached after the first call; safe on any thread. */
bool vk_xclipse_perf_enabled(void);

/* Submit ioctl duration. Called from the winsys, on whatever thread submitted. */
void vk_xclipse_perf_submit(uint64_t ioctl_ns);

/* Time blocked inside vkWaitForFences. Attributed to the calling thread. */
void vk_xclipse_perf_wait(uint64_t blocked_ns);

/* Time inside vkCreateGraphicsPipelines / vkCreateComputePipelines. `gs_count` counts pipelines
 * with a geometry stage. */
void vk_xclipse_perf_pipeline(uint64_t compile_ns, uint32_t count, uint32_t gs_count);

/* Frame boundary. Also the report trigger, and what identifies the render thread. */
void vk_xclipse_perf_present(void);

#ifdef __cplusplus
}
#endif

#endif /* VK_XCLIPSE_PERF_H */
