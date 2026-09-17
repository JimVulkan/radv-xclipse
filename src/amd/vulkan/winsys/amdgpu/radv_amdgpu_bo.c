/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based on amdgpu winsys.
 * Copyright © 2011 Marek Olšák <maraeo@gmail.com>
 * Copyright © 2015 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdio.h>
#include "ac_xclipse_log.h"

#include "tools/radv_debug.h"
#include "radv_amdgpu_bo.h"
#include "ac_cmdbuf.h"
#include "radv_logcat.h"
#include <android/log.h>

#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <xf86drm.h>
#include "drm-uapi/amdgpu_drm.h"
#include <sys/mman.h>
#include "ac_linux_drm.h"

#include "util/os_drm.h"
#include "util/os_time.h"
#include "util/u_atomic.h"
#include "util/u_math.h"
#include "util/u_memory.h"

#include <stdint.h>
#include <stdlib.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

static void radv_amdgpu_winsys_bo_destroy(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo);

/* BO churn accounting. No SDMA here, so every kernel TLB flush takes the raw MMIO ENG17 path; each
 * GART bind/unbind is one flush. Reported with debug.radv_xclipse_log 1. */

/* Size histogram of created BOs, powers of 4 from 16KB. */
static uint32_t radv_xclipse_bo_hist[8];
static const char *const radv_xclipse_bo_hist_name[8] = {"<16K", "<64K", "<256K", "<1M",
                                                         "<4M",  "<16M", "<64M",  ">=64M"};

static unsigned
radv_xclipse_size_bucket(uint64_t size)
{
   unsigned b = 0;
   uint64_t lim = 16 * 1024;
   while (b < 7 && size >= lim) {
      lim *= 4;
      b++;
   }
   return b;
}

static void
radv_xclipse_bo_churn(struct radv_amdgpu_winsys *ws, uint64_t size, bool created)
{
   static uint32_t n_create, n_destroy;
   static uint64_t bytes_created;
   /* Live footprint (bytes_created is cumulative). */
   static uint64_t live_bytes;
   static int64_t t_last;

   if (created) {
      p_atomic_inc(&radv_xclipse_bo_hist[radv_xclipse_size_bucket(size)]);
      p_atomic_inc(&n_create);
      p_atomic_add(&bytes_created, size);
      p_atomic_add(&live_bytes, size);
   } else {
      p_atomic_inc(&n_destroy);
      p_atomic_add(&live_bytes, -(int64_t)size);
   }

   /* Count create and destroy events: both GART bind and unbind flush the TLB, so events/s
    * approximates the flush rate. Report the first event, then every 64. */
   uint32_t nc = p_atomic_read(&n_create), nd = p_atomic_read(&n_destroy);
   uint32_t ev = nc + nd;
   if (ev != 1 && (ev & 0x3f))
      return;
   /* The report queries the kernel three times; only when logging. */
   if (ac_xclipse_log_level() < 1)
      return;

   int64_t now = os_time_get_nano();
   int64_t dt = t_last ? now - t_last : 0;
   t_last = now;

   /* Kernel counters:
    *   gtt_usage  - bytes resident in GTT (the whole budget, no VRAM)
    *   evictions  - BOs evicted (monotonic)
    *   moved      - bytes moved by evictions
    * Failed queries report 0. */
   uint64_t gtt_usage = 0, evictions = 0, moved = 0;
   if (ws && ws->dev) {
      struct amdgpu_heap_info heap = {0};
      if (ac_drm_query_heap_info(ws->dev, AMDGPU_GEM_DOMAIN_GTT, 0, &heap) == 0)
         gtt_usage = heap.heap_usage;
      ac_drm_query_info(ws->dev, AMDGPU_INFO_NUM_EVICTIONS, 8, &evictions);
      ac_drm_query_info(ws->dev, AMDGPU_INFO_NUM_BYTES_MOVED, 8, &moved);
   }
   /* allocMB is cumulative, liveMB is resident, gttMB is the kernel's number.
    * Filter: adb shell logcat -s RADV_CHURN:I */
   AC_XCLIPSE_LOGP(ANDROID_LOG_INFO, "RADV_CHURN",
                       "[BO_CHURN] ev=%u creates=%u destroys=%u live=%d allocMB=%" PRIu64 " liveMB=%" PRIu64
                       " gttMB=%" PRIu64 " evict=%" PRIu64 " movedMB=%" PRIu64
                       " last64_in=%.1fms (%.0f events/s ~= flushes/s)",
                       ev, nc, nd, (int)nc - (int)nd, bytes_created >> 20, live_bytes >> 20, gtt_usage >> 20,
                       evictions, moved >> 20, dt / 1e6, dt ? 64.0 / (dt / 1e9) : 0.0);
   AC_XCLIPSE_LOGP(ANDROID_LOG_INFO, "RADV_CHURN",
                       "[BO_SIZES] %s=%u %s=%u %s=%u %s=%u %s=%u %s=%u %s=%u %s=%u",
                       radv_xclipse_bo_hist_name[0], radv_xclipse_bo_hist[0],
                       radv_xclipse_bo_hist_name[1], radv_xclipse_bo_hist[1],
                       radv_xclipse_bo_hist_name[2], radv_xclipse_bo_hist[2],
                       radv_xclipse_bo_hist_name[3], radv_xclipse_bo_hist[3],
                       radv_xclipse_bo_hist_name[4], radv_xclipse_bo_hist[4],
                       radv_xclipse_bo_hist_name[5], radv_xclipse_bo_hist[5],
                       radv_xclipse_bo_hist_name[6], radv_xclipse_bo_hist[6],
                       radv_xclipse_bo_hist_name[7], radv_xclipse_bo_hist[7]);
}

/* Memory guard: the sgpu kernel hard-locks the phone when GPU-pinned GTT across all processes
 * reaches ~9-10GB (vendor ICD too). Allocations of 2 MB or more fail with OUT_OF_DEVICE_MEMORY
 * once the total would pass the advertised 8 GB heap. Usage comes from mem_info_gtt_used, or a
 * process-local total if that is unreadable. Heap sizes stay honest. */
#define XCLIPSE_MEMGUARD_SYSFS "/sys/devices/platform/16e00000.sgpu/mem_info_gtt_used"
#define XCLIPSE_MEMGUARD_LIMIT (8192ull << 20)

static bool
xclipse_memguard_reject(struct radv_amdgpu_winsys *ws, uint64_t size)
{
   if (!ws->info.gfx11_shader_core || size < (2ull << 20))
      return false;

   static int sysfs_ok = -1;
   uint64_t used = 0;
   if (sysfs_ok != 0) {
      FILE *f = fopen(XCLIPSE_MEMGUARD_SYSFS, "r");
      if (f) {
         sysfs_ok = fscanf(f, "%" PRIu64, &used) == 1;
         fclose(f);
      } else {
         sysfs_ok = 0;
      }
   }
   if (sysfs_ok != 1)
      used = p_atomic_read(&ws->alloc_tracker->allocated_gtt);

   if (used + size > XCLIPSE_MEMGUARD_LIMIT) {
      radv_log_msg("[MEMGUARD] REJECT alloc %" PRIu64 "MB: gtt_used=%" PRIu64 "MB limit=%" PRIu64 "MB (src=%s)",
                   size >> 20, used >> 20, XCLIPSE_MEMGUARD_LIMIT >> 20, sysfs_ok == 1 ? "sysfs" : "local");
      return true;
   }
   return false;
}

/* Winsys-local bo_flags bit (never leaves this file): PAL-style VA map, R|W|X and no MTYPE (0xe).
 * Imported BOs on 0x73a0 get it. */
#define RADV_XCLIPSE_VA_MTYPE_DEFAULT (1u << 31)

/* MTYPE for every VA map: 0..5 forces one, -1 = upstream. Default 0 (PAL) on 0x73a0 only.
 * Env first, then Android property. */
static int
radv_xclipse_mtype(struct radv_amdgpu_winsys *ws)
{
   static int mt = -2; /* not sampled yet */
   if (mt != -2)
      return mt;

   const char *e = getenv("RADV_XCLIPSE_MTYPE");
   const char *src = e ? "env" : "default";
   int v = e ? atoi(e) : -1;

#ifdef __ANDROID__
   if (!e) {
      char pbuf[PROP_VALUE_MAX] = {0};
      if (__system_property_get("debug.radv_xclipse_mtype", pbuf) > 0 && pbuf[0]) {
         v = atoi(pbuf);
         src = "prop";
      }
   }
#endif

   if (v < 0 && ws->info.pci_id == 0x73a0) {
      v = 0; /* PAL parity on our chip */
      if (!e)
         src = "default(0x73a0 PAL parity)";
   }
   if (v > 5)
      v = -1;
   mt = v;

   /* Log both arms. */
   static const char *const names[] = {"none/PAL", "NC", "WC", "CC", "UC", "RW"};
   AC_XCLIPSE_LOGP(ANDROID_LOG_INFO, "RADV_ARM", "[ARM] mtype=%d (%s) -- VA maps get %s",
                       mt, src, mt >= 0 ? names[mt] : "upstream (CC on GFX9+, UC on GL2_BYPASS)");
   return mt;
}

static int
radv_amdgpu_bo_va_op(struct radv_amdgpu_winsys *ws, uint32_t bo_handle, uint64_t offset, uint64_t size, uint64_t addr,
                     uint32_t bo_flags, uint64_t internal_flags, uint32_t ops)
{
   uint64_t flags = internal_flags;
   int r;

   if (bo_handle) {
      flags = AMDGPU_VM_PAGE_READABLE | AMDGPU_VM_PAGE_EXECUTABLE;

      /* PAL maps every buffer with flags 0xe (R|W|X, no MTYPE). MTYPE_CC made no difference on
       * any test (identical results and framebuffer hashes), so the default matches PAL.
       * Untested: concurrent CPU/GPU access to persistent mappings. CC on GFX9+ is upstream.
       *   RADV_XCLIPSE_MTYPE / debug.radv_xclipse_mtype
       *   0=none (default on 0x73a0)  1=NC  2=WC  3=CC (upstream)  4=UC  5=RW;  -1/unset elsewhere
       */
      const int mt = radv_xclipse_mtype(ws);
      if (mt >= 0) {
         static const uint64_t mtv[] = {0, AMDGPU_VM_MTYPE_NC, AMDGPU_VM_MTYPE_WC,
                                        AMDGPU_VM_MTYPE_CC, AMDGPU_VM_MTYPE_UC, AMDGPU_VM_MTYPE_RW};
         flags |= mtv[mt];
      } else if (bo_flags & RADV_XCLIPSE_VA_MTYPE_DEFAULT) {
         /* PAL maps imported gralloc/scanout buffers with no MTYPE bits (0x0e vs RADV's 0x6e). */
      } else if ((bo_flags & RADEON_FLAG_GL2_BYPASS) && ws->info.gfx_level >= GFX9) {
         flags |= AMDGPU_VM_MTYPE_UC;
      } else if (ws->info.gfx_level >= GFX9) {
         flags |= AMDGPU_VM_MTYPE_CC;
      }

      if (!(bo_flags & RADEON_FLAG_READ_ONLY))
         flags |= AMDGPU_VM_PAGE_WRITEABLE;
   }

   size = align64(size, getpagesize());

   if (bo_flags & RADEON_FLAG_VM_UPDATE_WAIT) {
      /* Wait for VM MAP updates when requested instead of delaying the updates at submit time.
       * This is a workaround to mitigate application bugs like use-before-alloc. Note that there is
       * still a very short period of time where the submit could start before the VM MAP updates
       * are actually done but this is deep UB territory. Also the BO VA will be only visible to the
       * application after VM updates are done, so it should be safe in most scenarios.
       */
      assert(ops == AMDGPU_VA_OP_MAP);

      simple_mtx_lock(&ws->vm_ioctl_lock);

      uint64_t vm_timeline_point = ++ws->vm_timeline_seq_num;

      r = ac_drm_bo_va_op_raw2(ws->dev, bo_handle, offset, size, addr, flags, ops, ws->vm_timeline_syncobj,
                               vm_timeline_point, 0, 0);

      simple_mtx_unlock(&ws->vm_ioctl_lock);

      if (r)
         return r;

      r = ac_drm_cs_syncobj_timeline_wait(ws->dev, &ws->vm_timeline_syncobj, &vm_timeline_point, 1, INT64_MAX,
                                          DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
                                          NULL);
   } else {
      r = ac_drm_bo_va_op_raw(ws->dev, bo_handle, offset, size, addr, flags, ops);
   }

   return r;
}

static uint64_t
radv_amdgpu_canonicalize_va(uint64_t va)
{
   /* Would be less hardcoded to use addr32_hi (0xffff8000) to generate a mask,
    * but there are confusing differences between page fault reports from kernel where
    * it seems to report the top 48 bits, where addr32_hi has 47-bits. */
   return va & ((1ull << 48) - 1);
}

static uint64_t
radv_amdgpu_bo_va_size(uint64_t bo_size, uint32_t flags)
{
   if (flags & RADEON_FLAG_VM_PAD_1PAGE) {
      const uint64_t va_padding = 4096;
      return align64(bo_size, 4096) + va_padding;
   }

   return bo_size;
}

static void
radv_amdgpu_log_va_op(struct radv_amdgpu_winsys *ws, struct radv_amdgpu_winsys_bo *bo, uint64_t offset, uint64_t size,
                      uint64_t virtual_va)
{
   const uint64_t timestamp = os_time_get_nano();
   uint64_t mapped_va = bo ? (bo->base.va + offset) : 0;

   if (ws->debug_log_bos) {
      struct radv_amdgpu_winsys_bo_log *bo_log = NULL;

      bo_log = calloc(1, sizeof(*bo_log));
      if (!bo_log)
         return;

      bo_log->va = virtual_va;
      bo_log->size = size;
      bo_log->timestamp = timestamp;
      bo_log->virtual_mapping = 1;
      bo_log->mapped_va = mapped_va;

      u_rwlock_wrlock(&ws->log_bo_list_lock);
      list_addtail(&bo_log->list, &ws->log_bo_list);
      u_rwlock_wrunlock(&ws->log_bo_list_lock);
   }

   if (ws->bo_history_logfile) {
      fprintf(ws->bo_history_logfile, "timestamp=%llu, VA=%.16llx-%.16llx, mapped_to=%.16llx\n", (long long)timestamp,
              (long long)radv_amdgpu_canonicalize_va(virtual_va),
              (long long)radv_amdgpu_canonicalize_va(virtual_va + size),
              (long long)radv_amdgpu_canonicalize_va(mapped_va));
      fflush(ws->bo_history_logfile);
   }
}

static uint64_t
radv_amdgpu_virtual_bo_get_low_addr(struct radv_amdgpu_winsys *ws, struct radv_amdgpu_winsys_bo *bo)
{
   return bo->base.va & ~(1ull << ws->info.address_prt_wa_control_bit);
}

static int
radv_amdgpu_virtual_bo_bind_low_null_prt(struct radv_amdgpu_winsys *ws, struct radv_amdgpu_winsys_bo *bo,
                                         uint64_t bo_offset, uint64_t bo_size, uint32_t ops)
{
   const uint64_t low_va = radv_amdgpu_virtual_bo_get_low_addr(ws, bo);
   uint64_t offset = 0;

   assert(util_is_aligned(bo_offset, 4096) && util_is_aligned(bo_size, 4096));

   while (bo_size > 0) {
      const uint64_t chunk_size = MIN2(bo_size, ws->null_prt_bug.bo->size);
      int r;

      r = radv_amdgpu_bo_va_op(ws, radv_amdgpu_winsys_bo(ws->null_prt_bug.bo)->bo_handle, 0, chunk_size,
                               low_va + bo_offset + offset, 0, 0, ops);
      if (r)
         return r;

      offset += chunk_size;
      bo_size -= chunk_size;
   }

   return 0;
}

static int
radv_amdgpu_virtual_bo_init_mapping(struct radv_amdgpu_winsys *ws, struct radv_amdgpu_winsys_bo *bo, uint64_t size)
{
   int r;

   r = radv_amdgpu_bo_va_op(ws, 0, 0, size, bo->base.va, 0, AMDGPU_VM_PAGE_PRT, AMDGPU_VA_OP_MAP);
   if (r)
      return r;

   if (bo->emulate_sparse_residency) {
      /* Bind the "LOW" address space to the zero-initialized BO when it's allocated to emulate
       * residency.
       */
      r = radv_amdgpu_virtual_bo_bind_low_null_prt(ws, bo, 0, size, AMDGPU_VA_OP_MAP);
   }

   return r;
}

static int
radv_amdgpu_virtual_bo_clear_mapping(struct radv_amdgpu_winsys *ws, struct radv_amdgpu_winsys_bo *bo)
{
   int r;

   r = radv_amdgpu_bo_va_op(ws, 0, 0, bo->base.size, bo->base.va, 0, 0, AMDGPU_VA_OP_CLEAR);
   if (r)
      return r;

   if (bo->emulate_sparse_residency) {
      /* Clear the "LOW" address space mapping when it's released. */
      const uint64_t low_va = radv_amdgpu_virtual_bo_get_low_addr(ws, bo);

      r = radv_amdgpu_bo_va_op(ws, 0, 0, bo->base.size, low_va, 0, 0, AMDGPU_VA_OP_CLEAR);
   }

   return r;
}

static int
radv_amdgpu_virtual_bo_map(struct radv_amdgpu_winsys *ws, struct radv_amdgpu_winsys_bo *parent, uint64_t offset,
                           uint64_t size, struct radv_amdgpu_winsys_bo *bo, uint64_t bo_offset)
{
   int r;

   r = radv_amdgpu_bo_va_op(ws, bo->bo_handle, bo_offset, size, parent->base.va + offset,
                            bo->flags & RADEON_FLAG_GL2_BYPASS, 0, AMDGPU_VA_OP_REPLACE);
   if (r)
      return r;

   if (parent->emulate_sparse_residency) {
      /* Bind the "LOW" address space to the same BO. */
      const uint64_t low_va = radv_amdgpu_virtual_bo_get_low_addr(ws, parent);

      r = radv_amdgpu_bo_va_op(ws, bo->bo_handle, bo_offset, size, low_va + offset, bo->flags & RADEON_FLAG_GL2_BYPASS,
                               0, AMDGPU_VA_OP_REPLACE);
      if (r)
         return r;
   }

   radv_amdgpu_log_va_op(ws, bo, bo_offset, size, parent->base.va + offset);
   return r;
}

static int
radv_amdgpu_virtual_bo_unmap(struct radv_amdgpu_winsys *ws, struct radv_amdgpu_winsys_bo *parent, uint64_t offset,
                             uint64_t size)
{
   int r;

   r = radv_amdgpu_bo_va_op(ws, 0, 0, size, parent->base.va + offset, 0, AMDGPU_VM_PAGE_PRT, AMDGPU_VA_OP_REPLACE);
   if (r)
      return r;

   if (parent->emulate_sparse_residency) {
      /* Re-bind the "LOW" address space to the zero-initialized BO when it's unmapped to emulate
       * residency.
       */
      r = radv_amdgpu_virtual_bo_bind_low_null_prt(ws, parent, offset, size, AMDGPU_VA_OP_REPLACE);
      if (r)
         return r;
   }

   radv_amdgpu_log_va_op(ws, NULL, 0, size, parent->base.va + offset);
   return r;
}

static VkResult
radv_amdgpu_winsys_bo_virtual_bind(struct radeon_winsys *_ws, struct radeon_winsys_bo *_parent, uint64_t offset,
                                   uint64_t size, struct radeon_winsys_bo *_bo, uint64_t bo_offset)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_winsys_bo *parent = (struct radv_amdgpu_winsys_bo *)_parent;
   struct radv_amdgpu_winsys_bo *bo = (struct radv_amdgpu_winsys_bo *)_bo;
   int r;

   assert(parent->base.is_virtual);
   assert(!bo || !bo->base.is_virtual);

   /* When the BO is NULL, AMDGPU will reset the PTE VA range to the initial state. Otherwise, it
    * will first unmap all existing VA that overlap the requested range and then map.
    */
   if (bo) {
      r = radv_amdgpu_virtual_bo_map(ws, parent, offset, size, bo, bo_offset);
   } else {
      r = radv_amdgpu_virtual_bo_unmap(ws, parent, offset, size);
   }

   if (r) {
      fprintf(stderr, "radv/amdgpu: Failed to replace a PRT VA region (%d).\n", r);
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;
   }

   return VK_SUCCESS;
}

static void
radv_amdgpu_log_bo(struct radv_amdgpu_winsys *ws, struct radv_amdgpu_winsys_bo *bo, bool destroyed)
{
   const uint64_t timestamp = os_time_get_nano();

   if (ws->debug_log_bos) {
      struct radv_amdgpu_winsys_bo_log *bo_log = NULL;

      bo_log = calloc(1, sizeof(*bo_log));
      if (!bo_log)
         return;

      bo_log->va = bo->base.va;
      bo_log->size = bo->base.size;
      bo_log->timestamp = timestamp;
      bo_log->is_virtual = bo->base.is_virtual;
      bo_log->destroyed = destroyed;

      u_rwlock_wrlock(&ws->log_bo_list_lock);
      list_addtail(&bo_log->list, &ws->log_bo_list);
      u_rwlock_wrunlock(&ws->log_bo_list_lock);
   }

   if (ws->bo_history_logfile) {
      fprintf(ws->bo_history_logfile, "timestamp=%llu, VA=%.16llx-%.16llx, destroyed=%d, is_virtual=%d\n",
              (long long)timestamp, (long long)radv_amdgpu_canonicalize_va(bo->base.va),
              (long long)radv_amdgpu_canonicalize_va(bo->base.va + bo->base.size), destroyed, bo->base.is_virtual);
      fflush(ws->bo_history_logfile);
   }
}

static int
radv_amdgpu_global_bo_list_add(struct radv_amdgpu_winsys *ws, struct radv_amdgpu_winsys_bo *bo)
{
   u_rwlock_wrlock(&ws->global_bo_list.lock);
   if (ws->global_bo_list.count == ws->global_bo_list.capacity) {
      unsigned capacity = MAX2(4, ws->global_bo_list.capacity * 2);
      void *data = realloc(ws->global_bo_list.bos, capacity * sizeof(struct radv_amdgpu_winsys_bo *));
      if (!data) {
         u_rwlock_wrunlock(&ws->global_bo_list.lock);
         return VK_ERROR_OUT_OF_HOST_MEMORY;
      }

      ws->global_bo_list.bos = (struct radv_amdgpu_winsys_bo **)data;
      ws->global_bo_list.capacity = capacity;
   }

   ws->global_bo_list.bos[ws->global_bo_list.count++] = bo;
   bo->base.use_global_list = true;
   u_rwlock_wrunlock(&ws->global_bo_list.lock);
   return VK_SUCCESS;
}

static void
radv_amdgpu_global_bo_list_del(struct radv_amdgpu_winsys *ws, struct radv_amdgpu_winsys_bo *bo)
{
   u_rwlock_wrlock(&ws->global_bo_list.lock);
   for (unsigned i = ws->global_bo_list.count; i-- > 0;) {
      if (ws->global_bo_list.bos[i] == bo) {
         ws->global_bo_list.bos[i] = ws->global_bo_list.bos[ws->global_bo_list.count - 1];
         --ws->global_bo_list.count;
         bo->base.use_global_list = false;
         break;
      }
   }
   u_rwlock_wrunlock(&ws->global_bo_list.lock);
}

static void
radv_amdgpu_winsys_virtual_bo_destroy(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(_bo);
   int r;

   r = radv_amdgpu_virtual_bo_clear_mapping(ws, bo);
   if (r) {
      fprintf(stderr, "radv/amdgpu: Failed to clear a PRT VA region (%d).\n", r);
   }

   ac_drm_va_range_free(bo->va_handle);
   FREE(bo);
}

/* IB / command BO reuse cache.
 *
 * Plain GTT BOs never reach amdgpu_gart_bind() (lpfn = 0), so they cost no TLB flush. IBs do:
 * amdgpu_cs_find_mapping() -> amdgpu_ttm_alloc_gart() binds them, and that bind (and the unbind on
 * destroy) runs the raw MMIO ENG17 flush. amdgpu_ttm_alloc_gart() returns early when the BO already
 * has a GART address, so a reused IB BO costs no flush.
 *
 * It creates nothing up front; it only declines to destroy.
 * Scope: RADEON_FLAG_XCLIPSE_CACHEABLE + NO_INTERPROCESS_SHARING only; never sparse, replayable,
 * imported or exported.
 */
#define RADV_XCLIPSE_IBCACHE_BUCKETS 64
#define RADV_XCLIPSE_IBCACHE_CAP (64ull << 20)

struct radv_xclipse_ibcache_entry {
   struct radv_amdgpu_winsys_bo *bo;
   struct radv_amdgpu_winsys *ws;
   uint64_t size;
   unsigned alignment;
   uint32_t flags;
   enum radeon_bo_domain domain;
};

static simple_mtx_t radv_xclipse_ibcache_lock = SIMPLE_MTX_INITIALIZER;
static struct radv_xclipse_ibcache_entry radv_xclipse_ibcache[RADV_XCLIPSE_IBCACHE_BUCKETS];
static unsigned radv_xclipse_ibcache_count;
static uint64_t radv_xclipse_ibcache_bytes;
/* Winsys being torn down, or NULL. park() refuses its BOs. */
static struct radv_amdgpu_winsys *radv_xclipse_ibcache_dying;

/* Is this BO safe to keep alive and hand back later? */
static bool
radv_xclipse_ibcache_cacheable(const struct radv_amdgpu_winsys_bo *bo)
{
   if (bo->base.is_virtual || bo->base.imported)
      return false;
   /* Shared BOs live in more than one VM. */
   if (!(bo->flags & RADEON_FLAG_NO_INTERPROCESS_SHARING))
      return false;
   /* Only the classes tagged as cacheable. */
   if (!(bo->flags & RADEON_FLAG_XCLIPSE_CACHEABLE))
      return false;
   if (bo->flags & (RADEON_FLAG_VIRTUAL | RADEON_FLAG_REPLAYABLE))
      return false;
   if (bo->emulate_sparse_residency)
      return false;
   return true;
}

/* Destroy path: park instead of freeing. Returns true if the cache took the BO.
 * The kernel BO, VA mapping and GART binding stay intact, so trackers are not decremented. */
static bool
radv_xclipse_ibcache_park(struct radv_amdgpu_winsys *ws, struct radv_amdgpu_winsys_bo *bo, unsigned alignment)
{
   if (!radv_xclipse_ibcache_cacheable(bo))
      return false;

   simple_mtx_lock(&radv_xclipse_ibcache_lock);
   if (ws == radv_xclipse_ibcache_dying) {
      simple_mtx_unlock(&radv_xclipse_ibcache_lock);
      return false;
   }
   if (radv_xclipse_ibcache_count >= RADV_XCLIPSE_IBCACHE_BUCKETS ||
       radv_xclipse_ibcache_bytes + bo->base.size > RADV_XCLIPSE_IBCACHE_CAP) {
      simple_mtx_unlock(&radv_xclipse_ibcache_lock);
      return false;
   }

   /* Drop the CPU mapping so buffer_map() sees a fresh BO. munmap does not touch the GART binding. */
   if (bo->cpu_map) {
      munmap(bo->cpu_map, bo->base.size);
      bo->cpu_map = NULL;
   }

   struct radv_xclipse_ibcache_entry *e = &radv_xclipse_ibcache[radv_xclipse_ibcache_count++];
   e->bo = bo;
   e->ws = ws;
   e->size = bo->base.size;
   e->alignment = alignment;
   e->flags = bo->flags;
   e->domain = bo->base.initial_domain;
   radv_xclipse_ibcache_bytes += bo->base.size;
   simple_mtx_unlock(&radv_xclipse_ibcache_lock);
   return true;
}

/* Create path: return an exact match (size, alignment, flags, domain) or NULL. */
static struct radv_amdgpu_winsys_bo *
radv_xclipse_ibcache_reuse(struct radv_amdgpu_winsys *ws, uint64_t size, unsigned alignment,
                           enum radeon_bo_domain domain, uint32_t flags)
{
   if (!(flags & RADEON_FLAG_XCLIPSE_CACHEABLE) || !(flags & RADEON_FLAG_NO_INTERPROCESS_SHARING))
      return NULL;

   /* Normalise the domain like bo_create (VRAM->GTT): park() records the post-downgrade domain. */
   if (ws->info.vram_size_kb == 0 && (domain & RADEON_DOMAIN_VRAM))
      domain = (domain & ~RADEON_DOMAIN_VRAM) | RADEON_DOMAIN_GTT;

   struct radv_amdgpu_winsys_bo *bo = NULL;
   simple_mtx_lock(&radv_xclipse_ibcache_lock);
   for (unsigned i = 0; i < radv_xclipse_ibcache_count; i++) {
      struct radv_xclipse_ibcache_entry *e = &radv_xclipse_ibcache[i];
      if (e->ws != ws || e->size != size || e->alignment != alignment || e->flags != flags || e->domain != domain)
         continue;
      bo = e->bo;
      radv_xclipse_ibcache_bytes -= e->size;
      radv_xclipse_ibcache[i] = radv_xclipse_ibcache[--radv_xclipse_ibcache_count];
      break;
   }
   simple_mtx_unlock(&radv_xclipse_ibcache_lock);
   return bo;
}

static void radv_amdgpu_winsys_bo_destroy(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo);

/* Free every BO a winsys parked when it is destroyed. Entries are keyed on the winsys pointer; a
 * new winsys at the same address would otherwise get BOs with stale GEM handles from a closed fd. */
static void
radv_xclipse_ibcache_purge_ws(struct radv_amdgpu_winsys *ws)
{
   for (;;) {
      struct radv_amdgpu_winsys_bo *bo = NULL;
      simple_mtx_lock(&radv_xclipse_ibcache_lock);
      for (unsigned i = 0; i < radv_xclipse_ibcache_count; i++) {
         if (radv_xclipse_ibcache[i].ws != ws)
            continue;
         bo = radv_xclipse_ibcache[i].bo;
         radv_xclipse_ibcache_bytes -= radv_xclipse_ibcache[i].size;
         radv_xclipse_ibcache[i] = radv_xclipse_ibcache[--radv_xclipse_ibcache_count];
         break;
      }
      simple_mtx_unlock(&radv_xclipse_ibcache_lock);
      if (!bo)
         return;
      radv_amdgpu_winsys_bo_destroy(&ws->base, &bo->base); /* park() refuses: ws is dying */
   }
}

/* First thing in radv_amdgpu_winsys_destroy(): stop parking this winsys' BOs and free them. */
void
radv_xclipse_ibcache_winsys_begin_destroy(struct radv_amdgpu_winsys *ws)
{
   simple_mtx_lock(&radv_xclipse_ibcache_lock);
   radv_xclipse_ibcache_dying = ws;
   simple_mtx_unlock(&radv_xclipse_ibcache_lock);
   radv_xclipse_ibcache_purge_ws(ws);
}

/* Before the winsys' device goes away: check nothing is left, then clear the marker. */
void
radv_xclipse_ibcache_winsys_end_destroy(struct radv_amdgpu_winsys *ws)
{
   radv_xclipse_ibcache_purge_ws(ws);
   simple_mtx_lock(&radv_xclipse_ibcache_lock);
   if (radv_xclipse_ibcache_dying == ws)
      radv_xclipse_ibcache_dying = NULL;
   simple_mtx_unlock(&radv_xclipse_ibcache_lock);
}

static void
radv_amdgpu_winsys_bo_destroy(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(_bo);

   /* Park an IB/command BO instead of destroying it. Before churn accounting, so [BO_CHURN] still
    * counts only real kernel create/destroy events. */
   if (radv_xclipse_ibcache_park(ws, bo, bo->xclipse_alignment))
      return;

   radv_amdgpu_log_bo(ws, bo, true);
   /* Only un-count what was counted: imports never passed through the allocate path. */
   if (bo->churn_counted)
      radv_xclipse_bo_churn(ws, bo->base.size, false);

   if (bo->base.is_virtual) {
      radv_amdgpu_winsys_virtual_bo_destroy(_ws, _bo);
      return;
   }

   if (bo->cpu_map)
      munmap(bo->cpu_map, bo->base.size);

   if (ws->debug_all_bos)
      radv_amdgpu_global_bo_list_del(ws, bo);

   const uint64_t va_size = radv_amdgpu_bo_va_size(bo->base.size, bo->flags);
   radv_amdgpu_bo_va_op(ws, bo->bo_handle, 0, va_size, bo->base.va, 0, 0, AMDGPU_VA_OP_UNMAP);
   ac_drm_bo_free(ws->dev, bo->bo);

   if (bo->base.initial_domain & RADEON_DOMAIN_VRAM) {
      if (bo->base.vram_no_cpu_access) {
         p_atomic_add(&ws->alloc_tracker->allocated_vram, -align64(bo->base.size, ws->info.gart_page_size));
      } else {
         p_atomic_add(&ws->alloc_tracker->allocated_vram_vis, -align64(bo->base.size, ws->info.gart_page_size));
      }
   }

   if (bo->base.initial_domain & RADEON_DOMAIN_GTT)
      p_atomic_add(&ws->alloc_tracker->allocated_gtt, -align64(bo->base.size, ws->info.gart_page_size));

   ac_drm_va_range_free(bo->va_handle);
   FREE(bo);
}

static VkResult
radv_amdgpu_init_null_prt_bo(struct radv_amdgpu_winsys *ws)
{
   VkResult result = VK_SUCCESS;

   if (p_atomic_read(&ws->null_prt_bug.bo))
      return result;

   simple_mtx_lock(&ws->null_prt_bug.lock);
   if (!ws->null_prt_bug.bo) {
      struct radeon_winsys_bo *bo;

      /* Create a zero-allocated 8MiB BO that will be used to map partially resident sparse buffers
       * at creation or when explicitly unmapped.
       */
      result = ws->base.buffer_create(&ws->base, 8 * 1024 * 1024 /* 8MiB */, 4096, RADEON_DOMAIN_VRAM,
                                      RADEON_FLAG_NO_CPU_ACCESS | RADEON_FLAG_ZERO_VRAM | RADEON_FLAG_READ_ONLY |
                                         RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_PREFER_LOCAL_BO,
                                      RADV_BO_PRIORITY_VIRTUAL, 0, &bo);
      if (result != VK_SUCCESS) {
         simple_mtx_unlock(&ws->null_prt_bug.lock);
         return result;
      }

      p_atomic_set(&ws->null_prt_bug.bo, bo);
   }
   simple_mtx_unlock(&ws->null_prt_bug.lock);

   return result;
}

static VkResult
radv_amdgpu_winsys_virtual_bo_create(struct radeon_winsys *_ws, uint64_t size, unsigned alignment,
                                     enum radeon_bo_flag flags, uint64_t replay_address,
                                     struct radeon_winsys_bo **out_bo)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_winsys_bo *bo;
   uint64_t va = 0;
   amdgpu_va_handle va_handle;
   int r;
   VkResult result = VK_SUCCESS;

   /* Just be robust for callers that might use NULL-ness for determining if things should be freed.
    */
   *out_bo = NULL;

   bo = CALLOC_STRUCT(radv_amdgpu_winsys_bo);
   if (!bo) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   unsigned virt_alignment = alignment;
   if (size >= ws->info.pte_fragment_size)
      virt_alignment = MAX2(virt_alignment, ws->info.pte_fragment_size);

   assert(!replay_address || (flags & RADEON_FLAG_REPLAYABLE));

   if (flags & RADEON_FLAG_EMULATE_SPARSE_RESIDENCY)
      replay_address &= ~(1ull << ws->info.address_prt_wa_control_bit);

   const uint64_t va_flags = AMDGPU_VA_RANGE_HIGH | (flags & RADEON_FLAG_32BIT ? AMDGPU_VA_RANGE_32_BIT : 0) |
                             (flags & RADEON_FLAG_REPLAYABLE ? AMDGPU_VA_RANGE_REPLAYABLE : 0);
   const uint64_t va_gap_size = ws->debug_vm ? MAX2(4 * virt_alignment, 64 * 1024) : 0;

   r = ac_drm_va_range_alloc(ws->dev, amdgpu_gpu_va_range_general, size + va_gap_size, virt_alignment, replay_address,
                             &va, &va_handle, va_flags);
   if (r) {
      result = replay_address ? VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_alloc;
   }

   /* RADEON_FLAG_32BIT BOs are reached through a 32-bit SH register completed with address32_hi.
    * libdrm silently falls back to the general range when the 4 GiB window is full; catch it here. */
   if ((flags & RADEON_FLAG_32BIT) && va && (uint32_t)(va >> 32) != ws->info.address32_hi)
      ac_xclipse_va32_violation(va, ws->info.address32_hi);
   bo->base.va = va;
   bo->base.size = size;
   bo->va_handle = va_handle;
   bo->emulate_sparse_residency = !!(flags & RADEON_FLAG_EMULATE_SPARSE_RESIDENCY);
   bo->base.is_virtual = true;

   if (bo->emulate_sparse_residency) {
      result = radv_amdgpu_init_null_prt_bo(ws);
      if (result != VK_SUCCESS) {
         fprintf(stderr, "radv/amdgpu: Failed to allocate the BO for the NULL PRT workaround.\n");
         goto error_ranges_alloc;
      }

      bo->base.va |= 1ull << ws->info.address_prt_wa_control_bit;
   }

   /* Reserve a PRT VA region. */
   r = radv_amdgpu_virtual_bo_init_mapping(ws, bo, size);
   if (r) {
      fprintf(stderr, "radv/amdgpu: Failed to reserve a PRT VA region (%d).\n", r);
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_ranges_alloc;
   }

   radv_amdgpu_log_bo(ws, bo, false);

   *out_bo = (struct radeon_winsys_bo *)bo;
   return VK_SUCCESS;

error_ranges_alloc:
   ac_drm_va_range_free(va_handle);

error_va_alloc:
   FREE(bo);
   return result;
}

static VkResult
radv_amdgpu_winsys_bo_create(struct radeon_winsys *_ws, uint64_t size, unsigned alignment,
                             enum radeon_bo_domain initial_domain, enum radeon_bo_flag flags, unsigned priority,
                             uint64_t replay_address, struct radeon_winsys_bo **out_bo)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_winsys_bo *bo;
   struct amdgpu_bo_alloc_request request = {0};
   ac_drm_bo buf_handle;
   uint64_t va = 0;
   amdgpu_va_handle va_handle;
   int r;
   VkResult result = VK_SUCCESS;

   if (flags & RADEON_FLAG_VIRTUAL)
      return radv_amdgpu_winsys_virtual_bo_create(_ws, size, alignment, flags, replay_address, out_bo);

   /* Just be robust for callers that might use NULL-ness for determining if things should be freed.
    */
   *out_bo = NULL;

   if (xclipse_memguard_reject(ws, size))
      return VK_ERROR_OUT_OF_DEVICE_MEMORY;

   /* Reuse a parked exact match: still GART-bound, so the next submit costs no flush. No churn
    * accounting: no kernel BO was created. */
   if (!replay_address) {
      struct radv_amdgpu_winsys_bo *reused = radv_xclipse_ibcache_reuse(ws, size, alignment, initial_domain, flags);
      if (reused) {
         reused->priority = priority;
         *out_bo = &reused->base;
         return VK_SUCCESS;
      }
   }

   bo = CALLOC_STRUCT(radv_amdgpu_winsys_bo);
   if (!bo) {
      return VK_ERROR_OUT_OF_HOST_MEMORY;
   }

   unsigned virt_alignment = alignment;
   if (size >= ws->info.pte_fragment_size)
      virt_alignment = MAX2(virt_alignment, ws->info.pte_fragment_size);

   assert(!replay_address || (flags & RADEON_FLAG_REPLAYABLE));

   const uint64_t va_size = radv_amdgpu_bo_va_size(size, flags);
   const uint64_t va_flags = AMDGPU_VA_RANGE_HIGH | (flags & RADEON_FLAG_32BIT ? AMDGPU_VA_RANGE_32_BIT : 0) |
                             (flags & RADEON_FLAG_REPLAYABLE ? AMDGPU_VA_RANGE_REPLAYABLE : 0);
   uint64_t va_gap_size = ws->debug_vm ? MAX2(4 * virt_alignment, 64 * 1024) : 0;

   r = ac_drm_va_range_alloc(ws->dev, amdgpu_gpu_va_range_general, va_size + va_gap_size, virt_alignment, replay_address,
                             &va, &va_handle, va_flags);
   if (r) {
      result = replay_address ? VK_ERROR_INVALID_OPAQUE_CAPTURE_ADDRESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_alloc;
   }

   /* Same check as the sparse path: a 32BIT BO served from the general range. */
   if ((flags & RADEON_FLAG_32BIT) && va && (uint32_t)(va >> 32) != ws->info.address32_hi)
      ac_xclipse_va32_violation(va, ws->info.address32_hi);
   bo->base.va = va;
   bo->base.size = size;
   bo->va_handle = va_handle;

   request.alloc_size = size;
   request.phys_alignment = alignment;

   if (ws->info.vram_size_kb == 0 &&
       initial_domain & RADEON_DOMAIN_VRAM) {
      /* Xclipse 920 reports dedicated VRAM for the heap topology but has none. Downgrade VRAM to
       * GTT, or shader code written via memcpy() sits in a mapping the GPU fetch can't see.
       */
      initial_domain = (initial_domain & ~RADEON_DOMAIN_VRAM) | RADEON_DOMAIN_GTT;
   }

   if (initial_domain & RADEON_DOMAIN_VRAM) {
      request.preferred_heap |= AMDGPU_GEM_DOMAIN_VRAM;

      /* Since VRAM and GTT have almost the same performance on
       * APUs, we could just set GTT. However, in order to decrease
       * GTT(RAM) usage, which is shared with the OS, allow VRAM
       * placements too. The idea is not to use VRAM usefully, but
       * to use it so that it's not unused and wasted.
       *
       * Furthermore, even on discrete GPUs this is beneficial. If
       * both GTT and VRAM are set then AMDGPU still prefers VRAM
       * for the initial placement, but it makes the buffers
       * spillable. Otherwise AMDGPU tries to place the buffers in
       * VRAM really hard to the extent that we are getting a lot
       * of unnecessary movement. This helps significantly when
       * e.g. Horizon Zero Dawn allocates more memory than we have
       * VRAM.
       */
      if (!(ws->perftest & RADV_PERFTEST_NO_GTT_SPILL))
         request.preferred_heap |= AMDGPU_GEM_DOMAIN_GTT;
   }

   if (initial_domain & RADEON_DOMAIN_GTT)
      request.preferred_heap |= AMDGPU_GEM_DOMAIN_GTT;
   if (initial_domain & RADEON_DOMAIN_GDS)
      request.preferred_heap |= AMDGPU_GEM_DOMAIN_GDS;
   if (initial_domain & RADEON_DOMAIN_OA)
      request.preferred_heap |= AMDGPU_GEM_DOMAIN_OA;

   if (flags & RADEON_FLAG_CPU_ACCESS)
      request.flags |= AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED;
   if (flags & RADEON_FLAG_NO_CPU_ACCESS) {
      bo->base.vram_no_cpu_access = initial_domain & RADEON_DOMAIN_VRAM;
      request.flags |= AMDGPU_GEM_CREATE_NO_CPU_ACCESS;
   }
   if (flags & RADEON_FLAG_GTT_WC)
      request.flags |= AMDGPU_GEM_CREATE_CPU_GTT_USWC;
   if (!(flags & RADEON_FLAG_IMPLICIT_SYNC))
      request.flags |= AMDGPU_GEM_CREATE_EXPLICIT_SYNC;
   if ((initial_domain & RADEON_DOMAIN_VRAM_GTT) && (flags & RADEON_FLAG_NO_INTERPROCESS_SHARING) &&
       ((ws->perftest & RADV_PERFTEST_LOCAL_BOS) || (flags & RADEON_FLAG_PREFER_LOCAL_BO))) {
      bo->base.is_local = true;
      request.flags |= AMDGPU_GEM_CREATE_VM_ALWAYS_VALID;
   }
   /* Set AMDGPU_GEM_CREATE_VIRTIO_SHARED if the driver didn't disable buffer sharing. */
   if (ws->info.is_virtio && (initial_domain & RADEON_DOMAIN_VRAM_GTT) &&
       (flags & RADEON_FLAG_NO_INTERPROCESS_SHARING) == 0)
      request.flags |= AMDGPU_GEM_CREATE_VIRTIO_SHARED;
   if (initial_domain & RADEON_DOMAIN_VRAM) {
      if (ws->zero_all_vram_allocs || (flags & RADEON_FLAG_ZERO_VRAM))
         request.flags |= AMDGPU_GEM_CREATE_VRAM_CLEARED;
   }

   /* AMDGPU_GEM_CREATE_DISCARDABLE needs DRM 3.47; older kernels reject unknown flags with -EINVAL.
    * Samsung's sgpu kernel is DRM 3.42. The gallium winsys has the same gate. */
   if (flags & RADEON_FLAG_DISCARDABLE && ws->info.drm_minor >= 47)
      request.flags |= AMDGPU_GEM_CREATE_DISCARDABLE;

   if (flags & RADEON_FLAG_GFX12_ALLOW_DCC && ws->info.drm_minor >= 58) {
      assert(ws->info.gfx_level >= GFX12 && (initial_domain & RADEON_DOMAIN_VRAM) &&
             (flags & RADEON_FLAG_NO_CPU_ACCESS));
      bo->base.gfx12_allow_dcc = true;
      request.flags |= AMDGPU_GEM_CREATE_GFX12_DCC;
   }

   if (flags & RADEON_FLAG_ENCRYPTED) {
      assert(ws->info.has_tmz_support);
      request.flags |= AMDGPU_GEM_CREATE_ENCRYPTED;
   }

   r = ac_drm_bo_alloc(ws->dev, &request, &buf_handle);
   if (r) {
      fprintf(stderr, "radv/amdgpu: Failed to allocate a buffer:\n");
      fprintf(stderr, "radv/amdgpu:    size      : %" PRIu64 " bytes\n", size);
      fprintf(stderr, "radv/amdgpu:    alignment : %u bytes\n", alignment);
      fprintf(stderr, "radv/amdgpu:    domains   : %u\n", initial_domain);
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_bo_alloc;
   }

   uint32_t kms_handle = 0;
   r = ac_drm_bo_export(ws->dev, buf_handle, amdgpu_bo_handle_type_kms, &kms_handle);
   assert(!r);

   r = radv_amdgpu_bo_va_op(ws, kms_handle, 0, size, va, flags, 0, AMDGPU_VA_OP_MAP);
   if (r) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_map;
   }

   if (flags & RADEON_FLAG_VM_PAD_1PAGE) {
      /* Map the first page of the same BO as read-only after the BO itself. */
      r = radv_amdgpu_bo_va_op(ws, kms_handle, 0, 4096, va + align64(size, 4096), flags | RADEON_FLAG_READ_ONLY, 0,
                               AMDGPU_VA_OP_MAP);
      if (r) {
         result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
         goto error_va_map;
      }
   }

   radv_xclipse_bo_churn(ws, size, true);
   bo->churn_counted = true;

   bo->bo = buf_handle;
   bo->bo_handle = kms_handle;
   bo->base.initial_domain = initial_domain;
   bo->base.use_global_list = false;
   bo->priority = priority;
   bo->cpu_map = NULL;
   bo->base.obj_id = (uintptr_t)(buf_handle.abo);
   bo->flags = flags;
   bo->xclipse_alignment = alignment; /* matched by the IB reuse cache */

   if (initial_domain & RADEON_DOMAIN_VRAM) {
      /* Buffers allocated in VRAM with the NO_CPU_ACCESS flag
       * aren't mappable and they are counted as part of the VRAM
       * counter.
       *
       * Otherwise, buffers with the CPU_ACCESS flag or without any
       * of both (imported buffers) are counted as part of the VRAM
       * visible counter because they can be mapped.
       */
      if (bo->base.vram_no_cpu_access) {
         p_atomic_add(&ws->alloc_tracker->allocated_vram, align64(bo->base.size, ws->info.gart_page_size));
      } else {
         p_atomic_add(&ws->alloc_tracker->allocated_vram_vis, align64(bo->base.size, ws->info.gart_page_size));
      }
   }

   if (initial_domain & RADEON_DOMAIN_GTT)
      p_atomic_add(&ws->alloc_tracker->allocated_gtt, align64(bo->base.size, ws->info.gart_page_size));

   if (ws->debug_all_bos)
      radv_amdgpu_global_bo_list_add(ws, bo);
   radv_amdgpu_log_bo(ws, bo, false);

   *out_bo = (struct radeon_winsys_bo *)bo;
   return VK_SUCCESS;
error_va_map:
   ac_drm_bo_free(ws->dev, buf_handle);

error_bo_alloc:
   ac_drm_va_range_free(va_handle);

error_va_alloc:
   FREE(bo);
   return result;
}

static void *
radv_amdgpu_winsys_bo_map(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo, bool use_fixed_addr,
                          void *fixed_addr)
{
   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(_bo);

   /* Safeguard for the Quantic Dream layer skipping unmaps. */
   if (bo->cpu_map && !use_fixed_addr)
      return bo->cpu_map;

   assert(!bo->cpu_map);

#if HAVE_AMDGPU_VIRTIO
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   if (ws->info.is_virtio) {
      /* We can't use DRM_AMDGPU_GEM_MMAP directly on virtio. Instead use bo_cpu_map since
       * the virtio version will map the buffer at the given address (if not NULL).
       */
      void *data = NULL;
      if (use_fixed_addr)
         data = fixed_addr;

      if (ac_drm_bo_cpu_map(ws->dev, bo->bo, &data))
         return NULL;
      return data;
   }
#endif

   union drm_amdgpu_gem_mmap args;
   memset(&args, 0, sizeof(args));
   args.in.handle = bo->bo_handle;

   int ret = drm_ioctl_write_read(radv_amdgpu_winsys(_ws)->fd, DRM_AMDGPU_GEM_MMAP, &args, sizeof(args));
   if (ret)
      return NULL;

   void *data = mmap(fixed_addr, bo->base.size, PROT_READ | PROT_WRITE, MAP_SHARED | (use_fixed_addr ? MAP_FIXED : 0),
                     radv_amdgpu_winsys(_ws)->fd, args.out.addr_ptr);
   if (data == MAP_FAILED)
      return NULL;

   bo->cpu_map = data;
   return data;
}

static void
radv_amdgpu_winsys_bo_unmap(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo, bool replace)
{
   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(_bo);

   /* Defense in depth against buggy apps. */
   if (!bo->cpu_map && !replace)
      return;

   assert(bo->cpu_map);
   if (replace) {
      (void)mmap(bo->cpu_map, bo->base.size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
   } else {
#if HAVE_AMDGPU_VIRTIO
      struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
      if (ws->info.is_virtio)
         ac_drm_bo_cpu_unmap(ws->dev, bo->bo);
      else
#endif
         munmap(bo->cpu_map, bo->base.size);
   }
   bo->cpu_map = NULL;
}

static uint64_t
radv_amdgpu_get_optimal_vm_alignment(struct radv_amdgpu_winsys *ws, uint64_t size, unsigned alignment)
{
   uint64_t vm_alignment = alignment;

   /* Increase the VM alignment for faster address translation. */
   if (size >= ws->info.pte_fragment_size)
      vm_alignment = MAX2(vm_alignment, ws->info.pte_fragment_size);

   /* Gfx9: Increase the VM alignment to the most significant bit set
    * in the size for faster address translation.
    */
   if (ws->info.gfx_level >= GFX9) {
      unsigned msb = util_last_bit64(size); /* 0 = no bit is set */
      uint64_t msb_alignment = msb ? 1ull << (msb - 1) : 0;

      vm_alignment = MAX2(vm_alignment, msb_alignment);
   }
   return vm_alignment;
}

static VkResult
radv_amdgpu_winsys_bo_from_ptr(struct radeon_winsys *_ws, void *pointer, uint64_t size, unsigned priority,
                               struct radeon_winsys_bo **out_bo)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   ac_drm_bo buf_handle;
   struct radv_amdgpu_winsys_bo *bo;
   uint64_t va;
   amdgpu_va_handle va_handle;
   uint64_t vm_alignment;
   VkResult result = VK_SUCCESS;
   int ret;

   /* Just be robust for callers that might use NULL-ness for determining if things should be freed.
    */
   *out_bo = NULL;

   bo = CALLOC_STRUCT(radv_amdgpu_winsys_bo);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   ret = ac_drm_create_bo_from_user_mem(ws->dev, pointer, size, &buf_handle);
   if (ret) {
      result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto error;
   }

   /* Using the optimal VM alignment also fixes GPU hangs for buffers that
    * are imported.
    */
   vm_alignment = radv_amdgpu_get_optimal_vm_alignment(ws, size, ws->info.gart_page_size);

   if (ac_drm_va_range_alloc(ws->dev, amdgpu_gpu_va_range_general, size, vm_alignment, 0, &va, &va_handle,
                             AMDGPU_VA_RANGE_HIGH)) {
      result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_alloc;
   }

   uint32_t kms_handle = 0;
   ASSERTED int r = ac_drm_bo_export(ws->dev, buf_handle, amdgpu_bo_handle_type_kms, &kms_handle);
   assert(!r);

   if (radv_amdgpu_bo_va_op(ws, kms_handle, 0, size, va, 0, 0, AMDGPU_VA_OP_MAP)) {
      result = VK_ERROR_UNKNOWN;
      goto error_va_map;
   }

   /* Initialize it */
   bo->base.va = va;
   bo->va_handle = va_handle;
   bo->base.size = size;
   bo->bo = buf_handle;
   bo->bo_handle = kms_handle;
   bo->base.initial_domain = RADEON_DOMAIN_GTT;
   bo->base.use_global_list = false;
   bo->base.imported = true;
   bo->priority = priority;
   bo->cpu_map = NULL;
   bo->base.obj_id = (uintptr_t)(buf_handle.abo);

   p_atomic_add(&ws->alloc_tracker->allocated_gtt, align64(bo->base.size, ws->info.gart_page_size));

   if (ws->debug_all_bos)
      radv_amdgpu_global_bo_list_add(ws, bo);
   radv_amdgpu_log_bo(ws, bo, false);

   *out_bo = (struct radeon_winsys_bo *)bo;
   return VK_SUCCESS;

error_va_map:
   ac_drm_va_range_free(va_handle);

error_va_alloc:
   ac_drm_bo_free(ws->dev, buf_handle);

error:
   FREE(bo);
   return result;
}

static VkResult
radv_amdgpu_winsys_bo_from_fd(struct radeon_winsys *_ws, int fd, unsigned priority, struct radeon_winsys_bo **out_bo,
                              uint64_t *alloc_size)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_winsys_bo *bo;
   uint64_t va;
   amdgpu_va_handle va_handle;
   enum amdgpu_bo_handle_type type = amdgpu_bo_handle_type_dma_buf_fd;
   struct ac_drm_bo_import_result result;
   struct amdgpu_bo_info info;
   enum radeon_bo_domain initial = 0;
   int r;
   VkResult vk_result = VK_SUCCESS;

   /* Just be robust for callers that might use NULL-ness for determining if things should be freed.
    */
   *out_bo = NULL;

   bo = CALLOC_STRUCT(radv_amdgpu_winsys_bo);
   if (!bo)
      return VK_ERROR_OUT_OF_HOST_MEMORY;

   r = ac_drm_bo_import(ws->dev, type, fd, &result);
   if (r) {
      vk_result = VK_ERROR_INVALID_EXTERNAL_HANDLE;
      goto error;
   }

   uint32_t kms_handle = 0;
   r = ac_drm_bo_export(ws->dev, result.bo, amdgpu_bo_handle_type_kms, &kms_handle);
   assert(!r);

   r = ac_drm_bo_query_info(ws->dev, kms_handle, &info);
   if (r) {
      vk_result = VK_ERROR_UNKNOWN;
      goto error_query;
   }

   if (alloc_size) {
      *alloc_size = info.alloc_size;
   }

   r = ac_drm_va_range_alloc(ws->dev, amdgpu_gpu_va_range_general, result.alloc_size, 1 << 20, 0, &va, &va_handle,
                             AMDGPU_VA_RANGE_HIGH);
   if (r) {
      vk_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_query;
   }

   const uint32_t import_va_flags = ws->info.pci_id == 0x73a0 ? RADV_XCLIPSE_VA_MTYPE_DEFAULT : 0;
   r = radv_amdgpu_bo_va_op(ws, kms_handle, 0, result.alloc_size, va, import_va_flags, 0, AMDGPU_VA_OP_MAP);
   if (r) {
      vk_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
      goto error_va_map;
   }

   if (info.preferred_heap & AMDGPU_GEM_DOMAIN_VRAM) {
      bo->base.vram_no_cpu_access = !!(info.alloc_flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS);
      initial |= RADEON_DOMAIN_VRAM;
   }
   if (info.preferred_heap & AMDGPU_GEM_DOMAIN_GTT)
      initial |= RADEON_DOMAIN_GTT;

   bo->bo = result.bo;
   bo->bo_handle = kms_handle;
   bo->base.va = va;
   bo->va_handle = va_handle;
   bo->base.initial_domain = initial;
   bo->base.use_global_list = false;
   bo->base.size = result.alloc_size;
   bo->base.imported = true;
   bo->priority = priority;

   /* Treat imported dma-bufs as resident and never list them per submit, like the vendor driver.
    * Listing them made the kernel bind them at submit, and the cold bind's ENG17 flush reset the
    * phone. is_local makes make_resident return early and radv_cs_add_buffer skip it. */
   if (ws->info.pci_id == 0x73a0)
      bo->base.is_local = true;

   bo->cpu_map = NULL;
   bo->base.obj_id = (uintptr_t)(result.bo.abo);

   if (bo->base.initial_domain & RADEON_DOMAIN_VRAM) {
      if (bo->base.vram_no_cpu_access) {
         p_atomic_add(&ws->alloc_tracker->allocated_vram, align64(bo->base.size, ws->info.gart_page_size));
      } else {
         p_atomic_add(&ws->alloc_tracker->allocated_vram_vis, align64(bo->base.size, ws->info.gart_page_size));
      }
   }

   if (bo->base.initial_domain & RADEON_DOMAIN_GTT)
      p_atomic_add(&ws->alloc_tracker->allocated_gtt, align64(bo->base.size, ws->info.gart_page_size));

   if (ws->debug_all_bos)
      radv_amdgpu_global_bo_list_add(ws, bo);
   radv_amdgpu_log_bo(ws, bo, false);

   *out_bo = (struct radeon_winsys_bo *)bo;
   return VK_SUCCESS;
error_va_map:
   ac_drm_va_range_free(va_handle);

error_query:
   ac_drm_bo_free(ws->dev, result.bo);

error:
   FREE(bo);
   return vk_result;
}

static bool
radv_amdgpu_winsys_get_fd(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo, int *fd)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(_bo);
   enum amdgpu_bo_handle_type type = amdgpu_bo_handle_type_dma_buf_fd;
   int r;
   unsigned handle;
   r = ac_drm_bo_export(ws->dev, bo->bo, type, &handle);
   if (r)
      return false;

   *fd = (int)handle;
   return true;
}

static bool
radv_amdgpu_bo_get_flags_from_fd(struct radeon_winsys *_ws, int fd, enum radeon_bo_domain *domains,
                                 enum radeon_bo_flag *flags)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct ac_drm_bo_import_result result = {0};
   struct amdgpu_bo_info info = {0};
   int r;

   *domains = 0;
   *flags = 0;

   r = ac_drm_bo_import(ws->dev, amdgpu_bo_handle_type_dma_buf_fd, fd, &result);
   if (r)
      return false;

   uint32_t kms_handle = 0;
   r = ac_drm_bo_export(ws->dev, result.bo, amdgpu_bo_handle_type_kms, &kms_handle);
   assert(!r);

   r = ac_drm_bo_query_info(ws->dev, kms_handle, &info);
   ac_drm_bo_free(ws->dev, result.bo);
   if (r)
      return false;

   if (info.preferred_heap & AMDGPU_GEM_DOMAIN_VRAM)
      *domains |= RADEON_DOMAIN_VRAM;
   if (info.preferred_heap & AMDGPU_GEM_DOMAIN_GTT)
      *domains |= RADEON_DOMAIN_GTT;
   if (info.preferred_heap & AMDGPU_GEM_DOMAIN_GDS)
      *domains |= RADEON_DOMAIN_GDS;
   if (info.preferred_heap & AMDGPU_GEM_DOMAIN_OA)
      *domains |= RADEON_DOMAIN_OA;

   if (info.alloc_flags & AMDGPU_GEM_CREATE_CPU_ACCESS_REQUIRED)
      *flags |= RADEON_FLAG_CPU_ACCESS;
   if (info.alloc_flags & AMDGPU_GEM_CREATE_NO_CPU_ACCESS)
      *flags |= RADEON_FLAG_NO_CPU_ACCESS;
   if (!(info.alloc_flags & AMDGPU_GEM_CREATE_EXPLICIT_SYNC))
      *flags |= RADEON_FLAG_IMPLICIT_SYNC;
   if (info.alloc_flags & AMDGPU_GEM_CREATE_CPU_GTT_USWC)
      *flags |= RADEON_FLAG_GTT_WC;
   if (info.alloc_flags & AMDGPU_GEM_CREATE_VM_ALWAYS_VALID)
      *flags |= RADEON_FLAG_NO_INTERPROCESS_SHARING | RADEON_FLAG_PREFER_LOCAL_BO;
   if (info.alloc_flags & AMDGPU_GEM_CREATE_VRAM_CLEARED)
      *flags |= RADEON_FLAG_ZERO_VRAM;
   if (info.alloc_flags & AMDGPU_GEM_CREATE_DISCARDABLE)
      *flags |= RADEON_FLAG_DISCARDABLE;
   if (info.alloc_flags & AMDGPU_GEM_CREATE_GFX12_DCC)
      *flags |= RADEON_FLAG_GFX12_ALLOW_DCC;
   if (info.alloc_flags & AMDGPU_GEM_CREATE_ENCRYPTED)
      *flags |= RADEON_FLAG_ENCRYPTED;
   return true;
}

static unsigned
eg_tile_split(unsigned tile_split)
{
   switch (tile_split) {
   case 0:
      tile_split = 64;
      break;
   case 1:
      tile_split = 128;
      break;
   case 2:
      tile_split = 256;
      break;
   case 3:
      tile_split = 512;
      break;
   default:
   case 4:
      tile_split = 1024;
      break;
   case 5:
      tile_split = 2048;
      break;
   case 6:
      tile_split = 4096;
      break;
   }
   return tile_split;
}

static unsigned
radv_eg_tile_split_rev(unsigned eg_tile_split)
{
   switch (eg_tile_split) {
   case 64:
      return 0;
   case 128:
      return 1;
   case 256:
      return 2;
   case 512:
      return 3;
   default:
   case 1024:
      return 4;
   case 2048:
      return 5;
   case 4096:
      return 6;
   }
}

#define AMDGPU_TILING_DCC_MAX_COMPRESSED_BLOCK_SIZE_SHIFT 45
#define AMDGPU_TILING_DCC_MAX_COMPRESSED_BLOCK_SIZE_MASK  0x3

static void
radv_amdgpu_winsys_bo_set_metadata(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo,
                                   struct radeon_bo_metadata *md)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(_bo);
   struct amdgpu_bo_metadata metadata = {0};
   uint64_t tiling_flags = 0;

   if (ws->info.gfx_level >= GFX12) {
      tiling_flags |= AMDGPU_TILING_SET(GFX12_SWIZZLE_MODE, md->u.gfx12.swizzle_mode);
      tiling_flags |= AMDGPU_TILING_SET(GFX12_DCC_MAX_COMPRESSED_BLOCK, md->u.gfx12.dcc_max_compressed_block);
      tiling_flags |= AMDGPU_TILING_SET(GFX12_DCC_NUMBER_TYPE, md->u.gfx12.dcc_number_type);
      tiling_flags |= AMDGPU_TILING_SET(GFX12_DCC_DATA_FORMAT, md->u.gfx12.dcc_data_format);
      tiling_flags |= AMDGPU_TILING_SET(GFX12_DCC_WRITE_COMPRESS_DISABLE, md->u.gfx12.dcc_write_compress_disable);
      tiling_flags |= AMDGPU_TILING_SET(GFX12_SCANOUT, md->u.gfx12.scanout);
   } else if (ws->info.gfx_level >= GFX9) {
      tiling_flags |= AMDGPU_TILING_SET(SWIZZLE_MODE, md->u.gfx9.swizzle_mode);
      tiling_flags |= AMDGPU_TILING_SET(DCC_OFFSET_256B, md->u.gfx9.dcc_offset_256b);
      tiling_flags |= AMDGPU_TILING_SET(DCC_PITCH_MAX, md->u.gfx9.dcc_pitch_max);
      tiling_flags |= AMDGPU_TILING_SET(DCC_INDEPENDENT_64B, md->u.gfx9.dcc_independent_64b_blocks);
      tiling_flags |= AMDGPU_TILING_SET(DCC_INDEPENDENT_128B, md->u.gfx9.dcc_independent_128b_blocks);
      tiling_flags |= AMDGPU_TILING_SET(DCC_MAX_COMPRESSED_BLOCK_SIZE, md->u.gfx9.dcc_max_compressed_block_size);
      tiling_flags |= AMDGPU_TILING_SET(SCANOUT, md->u.gfx9.scanout);
   } else {
      if (md->u.legacy.macrotile == RADEON_LAYOUT_TILED)
         tiling_flags |= AMDGPU_TILING_SET(ARRAY_MODE, 4); /* 2D_TILED_THIN1 */
      else if (md->u.legacy.microtile == RADEON_LAYOUT_TILED)
         tiling_flags |= AMDGPU_TILING_SET(ARRAY_MODE, 2); /* 1D_TILED_THIN1 */
      else
         tiling_flags |= AMDGPU_TILING_SET(ARRAY_MODE, 1); /* LINEAR_ALIGNED */

      tiling_flags |= AMDGPU_TILING_SET(PIPE_CONFIG, md->u.legacy.pipe_config);
      tiling_flags |= AMDGPU_TILING_SET(BANK_WIDTH, util_logbase2(md->u.legacy.bankw));
      tiling_flags |= AMDGPU_TILING_SET(BANK_HEIGHT, util_logbase2(md->u.legacy.bankh));
      if (md->u.legacy.tile_split)
         tiling_flags |= AMDGPU_TILING_SET(TILE_SPLIT, radv_eg_tile_split_rev(md->u.legacy.tile_split));
      tiling_flags |= AMDGPU_TILING_SET(MACRO_TILE_ASPECT, util_logbase2(md->u.legacy.mtilea));
      tiling_flags |= AMDGPU_TILING_SET(NUM_BANKS, util_logbase2(md->u.legacy.num_banks) - 1);

      if (md->u.legacy.scanout)
         tiling_flags |= AMDGPU_TILING_SET(MICRO_TILE_MODE, 0); /* DISPLAY_MICRO_TILING */
      else
         tiling_flags |= AMDGPU_TILING_SET(MICRO_TILE_MODE, 1); /* THIN_MICRO_TILING */
   }

   metadata.tiling_info = tiling_flags;
   metadata.size_metadata = md->size_metadata;
   memcpy(metadata.umd_metadata, md->metadata, sizeof(md->metadata));

   RADV_LOGI("[BO_SET_META] tiling_flags=0x%016llx size_meta=%u umd_meta[0]=0x%08x",
             (unsigned long long)tiling_flags, metadata.size_metadata, metadata.umd_metadata[0]);

   ac_drm_bo_set_metadata(ws->dev, bo->bo_handle, &metadata);
}

static void
radv_amdgpu_winsys_bo_get_metadata(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo,
                                   struct radeon_bo_metadata *md)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(_bo);
   struct amdgpu_bo_info info = {0};

   int r = ac_drm_bo_query_info(ws->dev, bo->bo_handle, &info);
   if (r)
      return;

   uint64_t tiling_flags = info.metadata.tiling_info;

   if (ws->info.gfx_level >= GFX12) {
      md->u.gfx12.swizzle_mode = AMDGPU_TILING_GET(tiling_flags, GFX12_SWIZZLE_MODE);
      md->u.gfx12.dcc_max_compressed_block = AMDGPU_TILING_GET(tiling_flags, GFX12_DCC_MAX_COMPRESSED_BLOCK);
      md->u.gfx12.dcc_data_format = AMDGPU_TILING_GET(tiling_flags, GFX12_DCC_DATA_FORMAT);
      md->u.gfx12.dcc_number_type = AMDGPU_TILING_GET(tiling_flags, GFX12_DCC_NUMBER_TYPE);
      md->u.gfx12.dcc_write_compress_disable = AMDGPU_TILING_GET(tiling_flags, GFX12_DCC_WRITE_COMPRESS_DISABLE);
      md->u.gfx12.scanout = AMDGPU_TILING_GET(tiling_flags, GFX12_SCANOUT);
   } else if (ws->info.gfx_level >= GFX9) {
      md->u.gfx9.swizzle_mode = AMDGPU_TILING_GET(tiling_flags, SWIZZLE_MODE);
      md->u.gfx9.scanout = AMDGPU_TILING_GET(tiling_flags, SCANOUT);
   } else {
      md->u.legacy.microtile = RADEON_LAYOUT_LINEAR;
      md->u.legacy.macrotile = RADEON_LAYOUT_LINEAR;

      if (AMDGPU_TILING_GET(tiling_flags, ARRAY_MODE) == 4) /* 2D_TILED_THIN1 */
         md->u.legacy.macrotile = RADEON_LAYOUT_TILED;
      else if (AMDGPU_TILING_GET(tiling_flags, ARRAY_MODE) == 2) /* 1D_TILED_THIN1 */
         md->u.legacy.microtile = RADEON_LAYOUT_TILED;

      md->u.legacy.pipe_config = AMDGPU_TILING_GET(tiling_flags, PIPE_CONFIG);
      md->u.legacy.bankw = 1 << AMDGPU_TILING_GET(tiling_flags, BANK_WIDTH);
      md->u.legacy.bankh = 1 << AMDGPU_TILING_GET(tiling_flags, BANK_HEIGHT);
      md->u.legacy.tile_split = eg_tile_split(AMDGPU_TILING_GET(tiling_flags, TILE_SPLIT));
      md->u.legacy.mtilea = 1 << AMDGPU_TILING_GET(tiling_flags, MACRO_TILE_ASPECT);
      md->u.legacy.num_banks = 2 << AMDGPU_TILING_GET(tiling_flags, NUM_BANKS);
      md->u.legacy.scanout = AMDGPU_TILING_GET(tiling_flags, MICRO_TILE_MODE) == 0; /* DISPLAY */
   }

   md->size_metadata = info.metadata.size_metadata;
   memcpy(md->metadata, info.metadata.umd_metadata, sizeof(md->metadata));
}

static VkResult
radv_amdgpu_winsys_bo_make_resident(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo, bool resident)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(_bo);
   VkResult result = VK_SUCCESS;

   /* Do not add the BO to the global list if it's a local BO because the
    * kernel maintains a list for us.
    */
   if (bo->base.is_local)
      return VK_SUCCESS;

   /* Do not add the BO twice to the global list if the allbos debug
    * option is enabled.
    */
   if (ws->debug_all_bos)
      return VK_SUCCESS;

   if (resident) {
      /* Imports are is_local (from_fd), so only non-resident BOs reach this path. */
      result = radv_amdgpu_global_bo_list_add(ws, bo);
   } else {
      radv_amdgpu_global_bo_list_del(ws, bo);
   }

   return result;
}

static int
radv_amdgpu_bo_va_compare(const void *a, const void *b)
{
   const struct radv_amdgpu_winsys_bo *bo_a = *(const struct radv_amdgpu_winsys_bo *const *)a;
   const struct radv_amdgpu_winsys_bo *bo_b = *(const struct radv_amdgpu_winsys_bo *const *)b;
   return bo_a->base.va < bo_b->base.va ? -1 : bo_a->base.va > bo_b->base.va ? 1 : 0;
}

static void
radv_amdgpu_dump_bo_log(struct radeon_winsys *_ws, FILE *file)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_winsys_bo_log *bo_log;

   if (!ws->debug_log_bos)
      return;

   u_rwlock_rdlock(&ws->log_bo_list_lock);
   LIST_FOR_EACH_ENTRY (bo_log, &ws->log_bo_list, list) {
      if (bo_log->virtual_mapping) {
         fprintf(file, "timestamp=%llu, VA=%.16llx-%.16llx, mapped_to=%.16llx\n", (long long)bo_log->timestamp,
                 (long long)radv_amdgpu_canonicalize_va(bo_log->va),
                 (long long)radv_amdgpu_canonicalize_va(bo_log->va + bo_log->size),
                 (long long)radv_amdgpu_canonicalize_va(bo_log->mapped_va));
      } else {
         fprintf(file, "timestamp=%llu, VA=%.16llx-%.16llx, destroyed=%d, is_virtual=%d\n",
                 (long long)bo_log->timestamp, (long long)radv_amdgpu_canonicalize_va(bo_log->va),
                 (long long)radv_amdgpu_canonicalize_va(bo_log->va + bo_log->size), bo_log->destroyed,
                 bo_log->is_virtual);
      }
   }
   u_rwlock_rdunlock(&ws->log_bo_list_lock);
}

static void
radv_amdgpu_dump_bo_ranges(struct radeon_winsys *_ws, FILE *file)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   if (ws->debug_all_bos) {
      struct radv_amdgpu_winsys_bo **bos = NULL;
      int i = 0;

      u_rwlock_rdlock(&ws->global_bo_list.lock);
      bos = malloc(sizeof(*bos) * ws->global_bo_list.count);
      if (!bos) {
         u_rwlock_rdunlock(&ws->global_bo_list.lock);
         fprintf(file, "  Failed to allocate memory to sort VA ranges for dumping\n");
         return;
      }

      for (i = 0; i < ws->global_bo_list.count; i++) {
         bos[i] = ws->global_bo_list.bos[i];
      }
      qsort(bos, ws->global_bo_list.count, sizeof(bos[0]), radv_amdgpu_bo_va_compare);

      for (i = 0; i < ws->global_bo_list.count; ++i) {
         fprintf(file, "  VA=%.16llx-%.16llx, handle=%d\n", (long long)radv_amdgpu_canonicalize_va(bos[i]->base.va),
                 (long long)radv_amdgpu_canonicalize_va(bos[i]->base.va + bos[i]->base.size), bos[i]->bo_handle);
      }
      free(bos);
      u_rwlock_rdunlock(&ws->global_bo_list.lock);
   } else
      fprintf(file, "  To get BO VA ranges, please specify RADV_DEBUG=allbos\n");
}

static bool
radv_amdgpu_bo_wait_for_idle(struct radeon_winsys *_ws, struct radeon_winsys_bo *_bo)
{
   struct radv_amdgpu_winsys *ws = radv_amdgpu_winsys(_ws);
   struct radv_amdgpu_winsys_bo *bo = radv_amdgpu_winsys_bo(_bo);
   bool buffer_busy = true;

   int r = ac_drm_bo_wait_for_idle(ws->dev, bo->bo, OS_TIMEOUT_INFINITE, &buffer_busy);
   if (r)
      fprintf(stderr, "radv/amdgpu: amdgpu_bo_wait_for_idle failed: %d\n", r);

   return !buffer_busy;
}

void
radv_amdgpu_bo_init_functions(struct radv_amdgpu_winsys *ws)
{
   ws->base.buffer_create = radv_amdgpu_winsys_bo_create;
   ws->base.buffer_destroy = radv_amdgpu_winsys_bo_destroy;
   ws->base.buffer_map = radv_amdgpu_winsys_bo_map;
   ws->base.buffer_unmap = radv_amdgpu_winsys_bo_unmap;
   ws->base.buffer_from_ptr = radv_amdgpu_winsys_bo_from_ptr;
   ws->base.buffer_from_fd = radv_amdgpu_winsys_bo_from_fd;
   ws->base.buffer_get_fd = radv_amdgpu_winsys_get_fd;
   ws->base.buffer_set_metadata = radv_amdgpu_winsys_bo_set_metadata;
   ws->base.buffer_get_metadata = radv_amdgpu_winsys_bo_get_metadata;
   ws->base.buffer_virtual_bind = radv_amdgpu_winsys_bo_virtual_bind;
   ws->base.buffer_get_flags_from_fd = radv_amdgpu_bo_get_flags_from_fd;
   ws->base.buffer_make_resident = radv_amdgpu_winsys_bo_make_resident;
   ws->base.dump_bo_ranges = radv_amdgpu_dump_bo_ranges;
   ws->base.dump_bo_log = radv_amdgpu_dump_bo_log;
   ws->base.bo_wait_for_idle = radv_amdgpu_bo_wait_for_idle;
}
