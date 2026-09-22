/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 */

/* GPU side of the Xclipse field profiler for RADV (util/u_xclipse_prof.c does the CPU side and
 * writes the report). Off unless MESA_XCLIPSE_PROF / debug.mesa_xclipse_prof is set. */
#ifndef RADV_XCLIPSE_PROF_H
#define RADV_XCLIPSE_PROF_H

#include <stdbool.h>
#include <stdint.h>

#include "vulkan/vulkan_core.h"

struct radv_cmd_buffer;
struct radv_device;

void radv_xprof_device_init(struct radv_device *device);
void radv_xprof_device_finish(struct radv_device *device);

/* Primary command buffers on the general queue only; everything else is ignored. */
void radv_xprof_begin_cmdbuf(struct radv_cmd_buffer *cmd_buffer);
void radv_xprof_end_cmdbuf(struct radv_cmd_buffer *cmd_buffer);

/* Pass boundaries: a render pass begins, a dispatch, and (from radv_xprof_draw) a new pixel shader. */
void radv_xprof_begin_rendering(struct radv_cmd_buffer *cmd_buffer, const VkRenderingInfo *info);
void radv_xprof_dispatch(struct radv_cmd_buffer *cmd_buffer, const uint32_t blocks[3]);
void radv_xprof_draw_slow(struct radv_cmd_buffer *cmd_buffer, uint32_t draw_count);

#endif
