/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 */

/* GPU side of the Xclipse field profiler for RADV; radeonsi's twin is si_xclipse_prof.c and the
 * report format is the same. No register reads: only timestamps the CP writes, as for queries.
 *  - Busy time: every primary command buffer on the general queue writes a top-of-pipe timestamp
 *    where the CP starts it and a bottom-of-pipe one where its work retires; the union of the
 *    spans is the time the GPU spent on this app.
 *  - Passes: a bottom-of-pipe timestamp at every render pass begin and end, pixel-shader change
 *    and dispatch marks the end of everything before it. A pass runs from its boundary to the
 *    next one IN THE SAME COMMAND BUFFER (apps record on several threads, so slot order is not
 *    execution order), and is grouped by kind, shader (first word of its BLAKE3), VGPRs and
 *    target.
 * Slots are taken at record time, so a command buffer submitted twice reports its last run. */

#include "radv_xclipse_prof.h"

#include "radv_buffer.h"
#include "radv_cmd_buffer.h"
#include "radv_cs.h"
#include "radv_device.h"
#include "radv_image_view.h"
#include "radv_physical_device.h"
#include "radv_query.h"
#include "radv_shader.h"

#include "util/format/u_format.h"
#include "util/u_xclipse_prof.h"
#include "vk_format.h"

#if defined(__ANDROID__) && defined(__aarch64__)

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

#define XPROF_SLOTS 16384     /* command buffer busy spans, [top, bottom] */
#define XPASS_SLOTS (1 << 18) /* pass boundaries */

enum { XPASS_FB, XPASS_CS, XPASS_OTHER, XPASS_END };

struct radv_xprof_pass {
   uint32_t frame, shader, draws, groups;
   uint32_t end;    /* slot + 1 of the boundary that closes this pass, 0 = never closed */
   uint32_t cb0, zs; /* VkFormat */
   uint16_t w, h, vgprs;
   uint8_t ncb, kind;
   uint8_t meta; /* 1: some of its work was RADV's own (clears, blits, resolves: radv_meta) */
   uint8_t pad[3];
};

static pthread_mutex_t xp_lock = PTHREAD_MUTEX_INITIALIZER;
static struct radv_device *xp_device;
static bool xp_failed;
static struct radeon_winsys_bo *xp_bo;
static uint64_t xp_va;
static uint64_t *xp_map; /* XPROF_SLOTS pairs, then XPASS_SLOTS boundaries */
static struct radv_xprof_pass *xp_meta;
static atomic_uint xp_ts_next, xp_pass_next;

static const char *const xp_kind[2][3] = {{"FB", "CS", "--"}, {"FB*", "CS*", "--*"}};

static int
xp_cmp_u64(const void *a, const void *b)
{
   const uint64_t *x = a, *y = b;
   return x[0] < y[0] ? -1 : x[0] > y[0];
}

struct xp_group {
   struct radv_xprof_pass key;
   double ms;
   unsigned n;
   uint64_t draws, groups;
};

static int
xp_group_cmp(const void *a, const void *b)
{
   const struct xp_group *x = a, *y = b;
   return x->ms < y->ms ? 1 : x->ms > y->ms ? -1 : 0;
}

static const char *
xp_format(uint32_t vk_format)
{
   if (!vk_format)
      return "-";
   return util_format_short_name(vk_format_to_pipe_format((VkFormat)vk_format));
}

static void
xp_report(void *data, FILE *f)
{
   struct radv_device *device = data;
   const struct radv_physical_device *pdev = radv_device_physical(device);
   const double khz = pdev->info.clock_crystal_freq;

   /* Command buffer busy time */
   const unsigned n = MIN2(atomic_load(&xp_ts_next), XPROF_SLOTS);
   uint64_t(*pairs)[2] = malloc(sizeof(*pairs) * (n ? n : 1));
   unsigned m = 0;
   for (unsigned i = 0; i < n; i++) {
      const uint64_t s = xp_map[2 * i], e = xp_map[2 * i + 1];
      if (s && e && e >= s) {
         pairs[m][0] = s;
         pairs[m][1] = e;
         m++;
      }
   }
   qsort(pairs, m, sizeof(*pairs), xp_cmp_u64);
   uint64_t busy = 0, cur_s = 0, cur_e = 0;
   for (unsigned i = 0; i < m; i++) {
      if (!cur_e || pairs[i][0] > cur_e) {
         busy += cur_e - cur_s;
         cur_s = pairs[i][0];
         cur_e = pairs[i][1];
      } else if (pairs[i][1] > cur_e) {
         cur_e = pairs[i][1];
      }
   }
   busy += cur_e - cur_s;
   const double span = m ? (pairs[m - 1][1] - pairs[0][0]) / khz : 0;
   fprintf(f, "# gpu busy_ms %.1f span_ms %.1f ibs %u busy_pct %.1f\n", busy / khz, span, m,
           span > 0 ? 100.0 * busy / khz / span : 0.0);
   free(pairs);

   /* Passes */
   const uint64_t *pts = xp_map + 2 * XPROF_SLOTS;
   const unsigned np = MIN2(atomic_load(&xp_pass_next), XPASS_SLOTS);
   struct xp_group *g = calloc(np ? np : 1, sizeof(*g));
   unsigned ng = 0, counted = 0;
   uint32_t f0 = UINT32_MAX, f1 = 0;
   double total = 0;
   for (unsigned i = 0; i < np; i++) {
      const struct radv_xprof_pass *p = &xp_meta[i];
      if (p->kind == XPASS_END || !p->end || p->end > np)
         continue;
      const uint64_t t0 = pts[i], t1 = pts[p->end - 1];
      if (!t0 || !t1 || t1 < t0)
         continue;
      const double ms = (t1 - t0) / khz;
      if (ms > 1000)
         continue;
      f0 = MIN2(f0, p->frame);
      f1 = MAX2(f1, p->frame);
      struct radv_xprof_pass k = *p;
      k.frame = 0;
      k.draws = 0;
      k.groups = 0;
      k.end = 0;
      unsigned j;
      for (j = 0; j < ng; j++)
         if (!memcmp(&g[j].key, &k, sizeof(k)))
            break;
      if (j == ng) {
         g[ng].key = k;
         ng++;
      }
      g[j].ms += ms;
      g[j].n++;
      g[j].draws += p->draws;
      g[j].groups += p->groups;
      total += ms;
      counted++;
   }
   qsort(g, ng, sizeof(*g), xp_group_cmp);
   const unsigned frames = counted ? MAX2(f1 - f0, 1) : 1;
   fprintf(f, "# passes %u groups %u frames %u gpu_ms_per_frame %.2f  (* = RADV's own work: clears, blits, resolves)\n",
           counted, ng, frames, total / frames);
   for (unsigned j = 0; j < ng && j < 40; j++) {
      const struct radv_xprof_pass *k = &g[j].key;
      fprintf(f,
              "# pass %s shader %08x vgprs %u %ux%u cb %u:%s zs %s  ms/frame %.2f (%.1f%%)  "
              "per-pass %.3f ms  x%u  draws/pass %.1f  groups/pass %.0f\n",
              xp_kind[k->meta][k->kind], k->shader, k->vgprs, k->w, k->h, k->ncb, xp_format(k->cb0),
              xp_format(k->zs), g[j].ms / frames, total > 0 ? 100.0 * g[j].ms / total : 0,
              g[j].ms / g[j].n, g[j].n, (double)g[j].draws / g[j].n, (double)g[j].groups / g[j].n);
   }
   free(g);
}

/* The first device that records while sampling is the one measured. */
static bool
xp_ready(struct radv_device *device)
{
   if (xp_device == device)
      return true;

   pthread_mutex_lock(&xp_lock);
   if (!xp_device && !xp_failed) {
      const uint64_t size = XPROF_SLOTS * 16 + XPASS_SLOTS * 8;
      VkResult r = radv_bo_create(device, NULL, size, 4096, RADEON_DOMAIN_GTT,
                                  RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING,
                                  RADV_BO_PRIORITY_QUERY_POOL, 0, true, &xp_bo);
      if (r == VK_SUCCESS)
         xp_map = device->ws->buffer_map(device->ws, xp_bo, false, NULL);
      xp_meta = calloc(XPASS_SLOTS, sizeof(*xp_meta));
      if (r != VK_SUCCESS || !xp_map || !xp_meta) {
         if (xp_bo)
            radv_bo_destroy(device, NULL, xp_bo);
         xp_bo = NULL;
         free(xp_meta);
         xp_meta = NULL;
         xp_failed = true;
      } else {
         memset(xp_map, 0, size);
         xp_va = radv_buffer_get_va(xp_bo);
         xp_device = device;
         u_xclipse_prof_set_gpu_reporter(xp_report, device);
      }
   }
   pthread_mutex_unlock(&xp_lock);
   return xp_device == device;
}

static void
xp_timestamp(struct radv_cmd_buffer *cmd_buffer, uint64_t va, VkPipelineStageFlags2 stage)
{
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   radeon_check_space(device->ws, cmd_buffer->cs->b, 16);
   radv_write_timestamp(cmd_buffer, va, stage);
}

/* Close the open pass at boundary slot `n` and open one of `kind` (none for XPASS_END). */
static struct radv_xprof_pass *
xp_boundary(struct radv_cmd_buffer *cmd_buffer, unsigned kind)
{
   if (!cmd_buffer->xprof_slot)
      return NULL;
   const uint32_t n = atomic_fetch_add(&xp_pass_next, 1);
   if (n >= XPASS_SLOTS)
      return NULL;

   if (cmd_buffer->xprof_pass) {
      struct radv_xprof_pass *prev = &xp_meta[cmd_buffer->xprof_pass - 1];
      prev->end = n + 1;
      prev->draws = cmd_buffer->xprof_draws;
      if (prev->kind == XPASS_FB && cmd_buffer->xprof_ps) {
         memcpy(&prev->shader, cmd_buffer->xprof_ps->hash, 4);
         prev->vgprs = cmd_buffer->xprof_ps->config.num_vgprs;
      }
   }

   xp_timestamp(cmd_buffer, xp_va + XPROF_SLOTS * 16 + n * 8ull, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);

   struct radv_xprof_pass *p = &xp_meta[n];
   memset(p, 0, sizeof(*p));
   p->frame = u_xclipse_prof_frames();
   p->kind = kind;
   p->meta = cmd_buffer->state.meta.inside_meta_op;
   cmd_buffer->xprof_pass = kind == XPASS_END ? 0 : n + 1;
   cmd_buffer->xprof_draws = 0;
   return p;
}

void
radv_xprof_device_init(struct radv_device *device)
{
   (void)device;
   u_xclipse_prof_start();
}

void
radv_xprof_device_finish(struct radv_device *device)
{
   pthread_mutex_lock(&xp_lock);
   if (xp_device == device) {
      u_xclipse_prof_set_gpu_reporter(NULL, NULL);
      radv_bo_destroy(device, NULL, xp_bo);
      xp_bo = NULL;
      xp_map = NULL;
      free(xp_meta);
      xp_meta = NULL;
      xp_device = NULL;
   }
   pthread_mutex_unlock(&xp_lock);
}

void
radv_xprof_begin_cmdbuf(struct radv_cmd_buffer *cmd_buffer)
{
   cmd_buffer->xprof_slot = 0;
   cmd_buffer->xprof_pass = 0;
   cmd_buffer->xprof_draws = 0;
   cmd_buffer->xprof_ps = NULL;

   if (likely(!u_xclipse_prof_active()) || cmd_buffer->vk.level != VK_COMMAND_BUFFER_LEVEL_PRIMARY ||
       cmd_buffer->qf != RADV_QUEUE_GENERAL)
      return;

   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   if (!xp_ready(device))
      return;
   const uint32_t slot = atomic_fetch_add(&xp_ts_next, 1);
   if (slot >= XPROF_SLOTS)
      return;

   radv_cs_add_buffer(device->ws, cmd_buffer->cs->b, xp_bo);
   xp_timestamp(cmd_buffer, xp_va + slot * 16ull, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT);
   cmd_buffer->xprof_slot = slot + 1;
   xp_boundary(cmd_buffer, XPASS_OTHER);
}

void
radv_xprof_end_cmdbuf(struct radv_cmd_buffer *cmd_buffer)
{
   if (!cmd_buffer->xprof_slot)
      return;
   xp_boundary(cmd_buffer, XPASS_END);
   xp_timestamp(cmd_buffer, xp_va + (cmd_buffer->xprof_slot - 1) * 16ull + 8,
                VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT);
   cmd_buffer->xprof_slot = 0;
   cmd_buffer->xprof_pass = 0;
}

void
radv_xprof_begin_rendering(struct radv_cmd_buffer *cmd_buffer, const VkRenderingInfo *info)
{
   if (!cmd_buffer->xprof_slot)
      return;
   struct radv_xprof_pass *p = xp_boundary(cmd_buffer, info ? XPASS_FB : XPASS_OTHER);
   cmd_buffer->xprof_ps = NULL;
   if (!p || !info)
      return;
   p->w = info->renderArea.extent.width;
   p->h = info->renderArea.extent.height;
   p->ncb = info->colorAttachmentCount;
   for (uint32_t i = 0; i < info->colorAttachmentCount; i++) {
      if (info->pColorAttachments[i].imageView) {
         VK_FROM_HANDLE(radv_image_view, iview, info->pColorAttachments[i].imageView);
         p->cb0 = iview->vk.format;
         break;
      }
   }
   const VkRenderingAttachmentInfo *ds = info->pDepthAttachment && info->pDepthAttachment->imageView
                                            ? info->pDepthAttachment
                                            : info->pStencilAttachment;
   if (ds && ds->imageView) {
      VK_FROM_HANDLE(radv_image_view, iview, ds->imageView);
      p->zs = iview->vk.format;
   }
}

void
radv_xprof_dispatch(struct radv_cmd_buffer *cmd_buffer, const uint32_t blocks[3])
{
   if (!cmd_buffer->xprof_slot)
      return;
   struct radv_xprof_pass *p = xp_boundary(cmd_buffer, XPASS_CS);
   if (!p)
      return;
   const struct radv_shader *cs = cmd_buffer->state.shaders[MESA_SHADER_COMPUTE];
   if (cs) {
      memcpy(&p->shader, cs->hash, 4);
      p->vgprs = cs->config.num_vgprs;
   }
   p->groups = blocks[0] * blocks[1] * blocks[2];
}

/* Only called while a pass is open (radv_before_draw checks xprof_pass). */
void
radv_xprof_draw_slow(struct radv_cmd_buffer *cmd_buffer, uint32_t draw_count)
{
   const struct radv_shader *ps = cmd_buffer->state.shaders[MESA_SHADER_FRAGMENT];
   const struct radv_xprof_pass cur = xp_meta[cmd_buffer->xprof_pass - 1];

   if (cur.kind == XPASS_FB && cmd_buffer->xprof_draws && ps != cmd_buffer->xprof_ps) {
      struct radv_xprof_pass *p = xp_boundary(cmd_buffer, XPASS_FB);
      if (p) {
         p->w = cur.w;
         p->h = cur.h;
         p->ncb = cur.ncb;
         p->cb0 = cur.cb0;
         p->zs = cur.zs;
      }
   }
   if (cmd_buffer->state.meta.inside_meta_op)
      xp_meta[cmd_buffer->xprof_pass - 1].meta = 1;
   cmd_buffer->xprof_ps = ps;
   cmd_buffer->xprof_draws += draw_count;
}

#else

void radv_xprof_device_init(struct radv_device *device) {}
void radv_xprof_device_finish(struct radv_device *device) {}
void radv_xprof_begin_cmdbuf(struct radv_cmd_buffer *cmd_buffer) {}
void radv_xprof_end_cmdbuf(struct radv_cmd_buffer *cmd_buffer) {}
void radv_xprof_begin_rendering(struct radv_cmd_buffer *cmd_buffer, const VkRenderingInfo *info) {}
void radv_xprof_dispatch(struct radv_cmd_buffer *cmd_buffer, const uint32_t blocks[3]) {}
void radv_xprof_draw_slow(struct radv_cmd_buffer *cmd_buffer, uint32_t draw_count) {}

#endif
