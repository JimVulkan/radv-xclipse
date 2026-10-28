/*
 * Mesa 3-D graphics library
 *
 * Copyright © 2021, Google Inc.
 * SPDX-License-Identifier: MIT
 */

#include "u_gralloc_internal.h"

#include <hardware/gralloc.h>

#include "drm-uapi/drm_fourcc.h"
#include "util/log.h"
#include <unistd.h>

#include "util/macros.h"
#include "util/u_memory.h"
#include "util/u_xclipse_log.h"

#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <string.h>

struct fallback_gralloc {
   struct u_gralloc base;
   gralloc_module_t *gralloc_module;
};

/* returns # of fds, and by reference the actual fds */
static unsigned
get_native_buffer_fds(const native_handle_t *handle, int fds[3])
{
   if (!handle)
      return 0;

   /*
    * Various gralloc implementations exist, but the dma-buf fd tends
    * to be first. Access it directly to avoid a dependency on specific
    * gralloc versions.
    */
   for (int i = 0; i < handle->numFds; i++)
      fds[i] = handle->data[i];

   return handle->numFds;
}

/* The fd of a handle that holds the buffer: the largest dma-buf among its fds. Allocators do not
 * agree on where it is: MediaTek's ARM gralloc puts -1 first and the buffer second, and a small
 * shared-attribute region may follow it. Returns -1 when no fd has a size. */
static int
largest_handle_fd(const native_handle_t *handle)
{
   int fd = -1;
   off_t best = 0;
   for (int i = 0; i < handle->numFds; i++) {
      if (handle->data[i] < 0)
         continue;
      const off_t sz = lseek(handle->data[i], 0, SEEK_END);
      lseek(handle->data[i], 0, SEEK_SET);
      if (sz > best) {
         best = sz;
         fd = handle->data[i];
      }
   }
   return fd;
}

#ifdef HAS_SAMSUNG_GRALLOC
/* Samsung Exynos gralloc private handle (Xclipse devices), measured on SM-S908B. Indices are into
 * native_handle_t::data[]: the struct has a fixed fd array, so numFds does not shift them. 64-bit fields are stored as low/high int pairs.
 */
#define SEC_GRALLOC_MAGIC    0x59700795
#define SEC_HND_MAGIC        5
#define SEC_HND_WIDTH        30
#define SEC_HND_HEIGHT       31
#define SEC_HND_STRIDE       35 /* pixels */
#define SEC_HND_ALLOC_FORMAT 37 /* HAL format actually allocated */
#define SEC_HND_LAYOUT       38 /* 1 linear, 2 compressed tiling (probe/gfx/grhandle, ahbx) */
#define SEC_HND_NUM_PLANES   39
#define SEC_HND_NUM_BUFS     40 /* dma-bufs: plane i is in data[i] when this is > 1, else all in data[0] */
#define SEC_HND_PLANE0       41 /* per plane: size, offset, byte stride, alloc width, alloc height */
#define SEC_HND_PLANE_INTS   10

/* Allocated formats of the YUV buffers, measured on SM-S908B with probe/gfx/grhandle.c. A
 * YCbCr_420_888 request is allocated as Exynos NV12M: Y and CbCr in two dma-bufs, each plane at
 * offset 0 of its own. P010 and YV12 keep all planes in one dma-buf at the handle's offsets. */
#define SEC_HAL_YCbCr_420_SP_M 0x105

static int
sec_yuv_fourcc(int format, int num_planes)
{
   switch (format) {
   case SEC_HAL_YCbCr_420_SP_M:
      return num_planes == 2 ? DRM_FORMAT_NV12 : -1;
   case HAL_PIXEL_FORMAT_YCBCR_P010:
      return num_planes == 2 ? DRM_FORMAT_P010 : -1;
   case HAL_PIXEL_FORMAT_YV12:
      return num_planes == 3 ? DRM_FORMAT_YVU420 : -1;
   default:
      return -1;
   }
}

static uint64_t
sec_hnd_u64(const native_handle_t *handle, int idx)
{
   return (uint64_t)(uint32_t)handle->data[idx] |
          ((uint64_t)(uint32_t)handle->data[idx + 1] << 32);
}

static int
samsung_gralloc_get_yuv_info(const native_handle_t *handle, int format, int num_planes,
                             struct u_gralloc_buffer_basic_info *out)
{
   const int drm_fourcc = sec_yuv_fourcc(format, num_planes);
   const int num_bufs = handle->data[SEC_HND_NUM_BUFS];
   const uint64_t height = (uint32_t)handle->data[SEC_HND_HEIGHT];

   if (drm_fourcc == -1 || num_bufs < 1 || num_bufs > handle->numFds ||
       (num_bufs > 1 && num_bufs != num_planes) ||
       handle->numFds + handle->numInts < SEC_HND_PLANE0 + num_planes * SEC_HND_PLANE_INTS) {
      mesa_loge("Samsung gralloc: unsupported format 0x%x (%d planes, %d buffers)", format,
                num_planes, num_bufs);
      return -EINVAL;
   }

   for (int i = 0; i < num_planes; i++) {
      const int p = SEC_HND_PLANE0 + i * SEC_HND_PLANE_INTS;
      const uint64_t size = sec_hnd_u64(handle, p);
      const uint64_t offset = sec_hnd_u64(handle, p + 2);
      const uint64_t stride = sec_hnd_u64(handle, p + 4);
      /* 4:2:0 chroma planes have half the rows. */
      const uint64_t rows = i ? (height + 1) / 2 : height;

      if (stride == 0 || stride > INT32_MAX || offset > INT32_MAX || size < stride * rows) {
         mesa_loge("Samsung gralloc: inconsistent plane %d (format 0x%x, stride %" PRIu64
                   ", size %" PRIu64 ")", i, format, stride, size);
         return -EINVAL;
      }
      out->fds[i] = handle->data[num_bufs > 1 ? i : 0];
      out->offsets[i] = offset;
      out->strides[i] = stride;
   }

   out->drm_fourcc = drm_fourcc;
   out->modifier = DRM_FORMAT_MOD_LINEAR;
   out->num_planes = num_planes;

   return 0;
}

/* Returns -ENOENT when the handle is not a Samsung gralloc handle. */
static int
samsung_gralloc_get_buffer_info(struct u_gralloc_buffer_handle *hnd,
                                struct u_gralloc_buffer_basic_info *out)
{
   const native_handle_t *handle = hnd->handle;

   if (handle->numFds + handle->numInts < SEC_HND_PLANE0 + SEC_HND_PLANE_INTS ||
       (uint32_t)handle->data[SEC_HND_MAGIC] != SEC_GRALLOC_MAGIC)
      return -ENOENT;

   const int format = handle->data[SEC_HND_ALLOC_FORMAT];
   const int num_planes = handle->data[SEC_HND_NUM_PLANES];

   /* data[38] is the layout: 1 linear, 2 the vendor's compressed tiling (GPU-only usage on the 530
    * and 940, composer buffers everywhere). Nothing here can describe the latter, so it is still
    * imported as linear, and its contents will not match the vendor's: say so, once, with
    * debug.radv_xclipse_log >= 1. */
   if (handle->data[SEC_HND_LAYOUT] != 1) {
      static bool warned;
      if (!warned) {
         warned = true;
         if (u_xclipse_log_on())
            mesa_logw("Samsung gralloc: buffer %dx%d with non-linear layout %d imported as linear; "
                      "contents will not match other users of it",
                      handle->data[SEC_HND_WIDTH], handle->data[SEC_HND_HEIGHT], handle->data[SEC_HND_LAYOUT]);
      }
   }
   if (num_planes != 1 || is_hal_format_yuv(format))
      return samsung_gralloc_get_yuv_info(handle, format, num_planes, out);

   const int drm_fourcc = get_fourcc_from_hal_format(format);
   if (drm_fourcc == -1)
      return -EINVAL;

   const uint64_t width = (uint32_t)handle->data[SEC_HND_WIDTH];
   const uint64_t height = (uint32_t)handle->data[SEC_HND_HEIGHT];
   const uint64_t pixel_stride = (uint32_t)handle->data[SEC_HND_STRIDE];
   const uint64_t plane_size = sec_hnd_u64(handle, SEC_HND_PLANE0);
   const uint64_t offset = sec_hnd_u64(handle, SEC_HND_PLANE0 + 2);
   const uint64_t alloc_w = sec_hnd_u64(handle, SEC_HND_PLANE0 + 6);
   const uint64_t alloc_h = sec_hnd_u64(handle, SEC_HND_PLANE0 + 8);
   uint64_t stride = sec_hnd_u64(handle, SEC_HND_PLANE0 + 4);

   /* gralloc leaves the byte stride at 0 for some buffers (e.g. scanout): derive it from the
    * pixel stride and the bytes per pixel. */
   if (stride == 0) {
      uint64_t bpp = get_hal_format_bpp(format);
      if (!bpp && alloc_w && alloc_h && plane_size % (alloc_w * alloc_h) == 0)
         bpp = plane_size / (alloc_w * alloc_h);
      stride = pixel_stride * bpp;
   }

   /* Reject a layout that does not fit, so an unexpected handle revision fails cleanly. */
   if (!width || pixel_stride < width || stride == 0 || stride > INT32_MAX ||
       offset > INT32_MAX || plane_size < stride * height) {
      mesa_loge("Samsung gralloc: inconsistent handle (format 0x%x, %" PRIu64 "x%" PRIu64
                ", stride %" PRIu64 ", plane size %" PRIu64 ")",
                format, width, height, stride, plane_size);
      return -EINVAL;
   }

   out->drm_fourcc = drm_fourcc;
   out->modifier = DRM_FORMAT_MOD_LINEAR;
   out->num_planes = 1;
   out->fds[0] = handle->data[0];
   out->offsets[0] = offset;
   out->strides[0] = stride;

   return 0;
}
#endif /* HAS_SAMSUNG_GRALLOC */

static int
fallback_gralloc_get_yuv_info(struct u_gralloc *gralloc,
                              struct u_gralloc_buffer_handle *hnd,
                              struct u_gralloc_buffer_basic_info *out)
{
   struct fallback_gralloc *gr = (struct fallback_gralloc *)gralloc;
   gralloc_module_t *gr_mod = gr->gralloc_module;
   struct android_ycbcr ycbcr;
   int num_fds = 0;
   int fds[3];
   int ret;

   num_fds = get_native_buffer_fds(hnd->handle, fds);
   if (num_fds == 0)
      return -EINVAL;

   if (!gr_mod || !gr_mod->lock_ycbcr) {
      return -EINVAL;
   }

   memset(&ycbcr, 0, sizeof(ycbcr));
   ret = gr_mod->lock_ycbcr(gr_mod, hnd->handle, 0, 0, 0, 0, 0, &ycbcr);
   if (ret) {
      /* HACK: See native_window_buffer_get_buffer_info() and
       * https://issuetracker.google.com/32077885.*/
      if (hnd->hal_format == HAL_PIXEL_FORMAT_IMPLEMENTATION_DEFINED)
         return -EAGAIN;

      mesa_logw("gralloc->lock_ycbcr failed: %d", ret);
      return -EINVAL;
   }
   gr_mod->unlock(gr_mod, hnd->handle);

   ret = bufferinfo_from_ycbcr(&ycbcr, hnd, out);
   if (ret)
      return ret;

   /*
    * Since this is EGL_NATIVE_BUFFER_ANDROID don't assume that
    * the single-fd case cannot happen.  So handle eithe single
    * fd or fd-per-plane case:
    */
   if (num_fds == 1) {
      out->fds[1] = out->fds[0] = fds[0];
      if (out->num_planes == 3)
         out->fds[2] = fds[0];
   } else {
      assert(num_fds == out->num_planes);
      out->fds[0] = fds[0];
      out->fds[1] = fds[1];
      out->fds[2] = fds[2];
   }

   return 0;
}

/*
 * ARM's mali gralloc private handle, as the Exynos 9820 (Android 12) and 9611 (Android 13)
 * allocators hand it out behind IMapper 2.x, which offers no layout query. Measured with
 * probe/mali/ahbdump.c: indices are into native_handle_t::data[], fds included, 64-bit fields
 * as low/high int pairs. The byte stride is the allocator's own (a 65x33 buffer is 80x48), and
 * a linear buffer starts at offset 0 of the dma-buf, both confirmed by the vendor driver
 * rendering into such a buffer and the pixels read back through the dma-buf (probe/mali/vkanb.c).
 */
#define ARM_GRALLOC_MAGIC        0x03141592
/* Offsets from the magic, which is data[5] on the Exynos handles. MediaTek's allocator (Galaxy
 * A31, MT6768, Android 12: 3 fds and 18 ints of its own first) puts the same ARM handle behind a
 * prefix, magic at data[21], and has 6 more ints before plane_info. So the magic is searched for,
 * and the plane-0 {byte stride, width, height} triple is taken from the first known offset where
 * it is consistent with the buffer. */
#define ARM_HND_WIDTH            2
#define ARM_HND_HEIGHT           3
#define ARM_HND_REQ_FORMAT       4
static const int arm_hnd_plane0[] = {15 /* Exynos */, 21 /* MediaTek */};
/* The u64 alloc format (HAL format low, AFBC and other layout bits from 32) sits 5 ints before
 * plane_info on both: magic+10 on Exynos, magic+16 on MediaTek. */
#define ARM_HND_ALLOC_FORMAT_BACK 5
#define ARM_HND_PLANE_INTS       3  /* plane_info[3] of {byte stride, width, height} */
#define ARM_HND_MIN_INTS         36
#define ARM_ALLOC_FORMAT_AFBC    (1ull << 32)
/* MediaTek's allocator gives GPU-only buffers (sampled + framebuffer, no CPU or composer usage)
 * alloc format 0x311_<hal format>, whatever the size: AFBC with 16x16 superblocks, headers tiled
 * 8x8 (so 128x128-pixel aligned), YUV transform and sparse bodies, the header at offset 0 and the
 * body right after it on a 4 KB boundary. That is the layout Mesa computes for the equivalent DRM
 * modifier, and the plane stride is the uncompressed byte stride of the aligned width, which is
 * what the AFBC import takes as its row pitch. Read off probe/mali/ahbdump.c at 16x16 to
 * 1920x1080 and checked pixel for pixel in both directions against the vendor GLES driver
 * (probe/mali/vkafbc.c). Only this combination, and only RGBA/RGBX 8888, is taken as AFBC.
 * A BGRA 8888 request is stored as RGBA AFBC (alloc format 0x311_00000001), so it is reported
 * as RGBA, which is also the Vulkan format the vendor driver gives such a buffer. */
#define ARM_MTK_AFBC_ALLOC_BITS  0x311u
/* Exynos's allocator (Galaxy S10e, Android 12) makes the same GPU-only buffers AFBC with alloc
 * format 0x1_<hal format>: 16x16 superblocks, untiled headers, no YUV transform, the plane stride
 * that of the 16-aligned width. Its body starts at the header size rounded up to 4 KB where Mesa
 * uses 128 bytes; readers follow the header's offsets and Mesa's body ends inside the buffer, so
 * that does not matter. Checked like the MediaTek layout (probe/mali/vkafbc.c): 16x16 + sparse
 * passes both directions, with YUV transform every pixel is wrong. A BGRA 8888 buffer is stored in
 * RGBA component order here too (alloc format 0x1_00000005), as the vendor driver reports it. */
#define ARM_EXYNOS_AFBC_ALLOC_BITS 0x1u

/* Returns -ENOENT when the handle is not an ARM gralloc handle. */
static int
arm_gralloc_get_buffer_info(struct u_gralloc_buffer_handle *hnd,
                            struct u_gralloc_buffer_basic_info *out)
{
   const native_handle_t *handle = hnd->handle;

   const int total = handle->numFds + handle->numInts;
   if (total < ARM_HND_MIN_INTS)
      return -ENOENT;

   int m = handle->numFds;
   while (m < total && (uint32_t)handle->data[m] != ARM_GRALLOC_MAGIC)
      m++;
   if (m + ARM_HND_MIN_INTS - 5 > total)
      return -ENOENT;

   const int *h = &handle->data[m];
   const int format = h[ARM_HND_REQ_FORMAT];
   const uint32_t width = h[ARM_HND_WIDTH];
   const uint32_t height = h[ARM_HND_HEIGHT];
   const uint32_t bpp = get_hal_format_bpp(format);

   int plane0 = -1;
   for (unsigned i = 0; i < ARRAY_SIZE(arm_hnd_plane0) && plane0 < 0; i++) {
      const int o = arm_hnd_plane0[i];
      if (m + o + 2 * ARM_HND_PLANE_INTS > total)
         continue;
      const uint32_t ps = h[o], pw = h[o + 1], ph = h[o + 2];
      if (bpp && width && ps >= width * bpp && ps <= INT32_MAX && pw >= width && ph >= height &&
          ps >= pw * bpp)
         plane0 = o;
   }
   if (plane0 < 0) {
      /* An ARM handle of a layout not measured here (another allocator version: a Helio G90's
       * Mali-G76 did not match either). Refusing it made every AHardwareBuffer import fail, and
       * with it the swapchain of every Winlator-style wrapper. Take the buffer as linear, with
       * the stride the allocator reported to the app, and log the handle once so its layout can
       * be added. ARM's allocator only compresses GPU-only buffers, and the wrappers' and the
       * swapchain's buffers carry CPU or composer usage, so they are linear. */
      static bool dumped;
      if (!dumped) {
         dumped = true;
         char ints[512] = "";
         for (int i = handle->numFds; i < total && strlen(ints) < sizeof(ints) - 12; i++)
            snprintf(ints + strlen(ints), sizeof(ints) - strlen(ints), " %x",
                     (unsigned)handle->data[i]);
         mesa_logw("ARM gralloc: unknown handle layout (%d fds, %d ints, magic at %d, request "
                   "format 0x%x stride %d); taken as linear. ints:%s",
                   handle->numFds, handle->numInts, m, hnd->hal_format, hnd->pixel_stride, ints);
      }
      const uint32_t req_bpp = get_hal_format_bpp(hnd->hal_format);
      const int req_fourcc = get_fourcc_from_hal_format(hnd->hal_format);
      const int req_fd = largest_handle_fd(handle);
      if (!req_bpp || req_fourcc == -1 || hnd->pixel_stride <= 0 || req_fd < 0 ||
          is_hal_format_yuv(hnd->hal_format))
         return -EINVAL;
      out->drm_fourcc = req_fourcc;
      out->modifier = DRM_FORMAT_MOD_LINEAR;
      out->num_planes = 1;
      out->fds[0] = req_fd;
      out->offsets[0] = 0;
      out->strides[0] = hnd->pixel_stride * req_bpp;
      return 0;
   }
   const uint32_t stride = h[plane0];
   const int af = plane0 - ARM_HND_ALLOC_FORMAT_BACK;
   const uint64_t alloc_format =
      (uint64_t)(uint32_t)h[af] | ((uint64_t)(uint32_t)h[af + 1] << 32);

   if (is_hal_format_yuv(format) || h[plane0 + ARM_HND_PLANE_INTS]) {
      mesa_loge("ARM gralloc: multi-planar format 0x%x is not handled", format);
      return -EINVAL;
   }

   /* Other AFBC layouts are the allocator's, and nothing here describes them, so such a buffer
    * is refused rather than read as linear. Swapchain images and the driver's own
    * AHardwareBuffers avoid AFBC by asking for MALI_GRALLOC_USAGE_NO_AFBC and composer usage. */
   uint64_t modifier = DRM_FORMAT_MOD_LINEAR;
   int stored_format = format;
   if (alloc_format & ARM_ALLOC_FORMAT_AFBC) {
      const uint32_t alloc_hal = (uint32_t)alloc_format;
      const bool mtk_afbc = plane0 == arm_hnd_plane0[1] &&
                            (alloc_format >> 32) == ARM_MTK_AFBC_ALLOC_BITS &&
                            ((alloc_hal == (uint32_t)format &&
                              (format == HAL_PIXEL_FORMAT_RGBA_8888 ||
                               format == HAL_PIXEL_FORMAT_RGBX_8888)) ||
                             (alloc_hal == HAL_PIXEL_FORMAT_RGBA_8888 &&
                              format == HAL_PIXEL_FORMAT_BGRA_8888));
      const bool exy_afbc = plane0 == arm_hnd_plane0[0] &&
                            (alloc_format >> 32) == ARM_EXYNOS_AFBC_ALLOC_BITS &&
                            alloc_hal == (uint32_t)format &&
                            (format == HAL_PIXEL_FORMAT_RGBA_8888 ||
                             format == HAL_PIXEL_FORMAT_RGBX_8888 ||
                             format == HAL_PIXEL_FORMAT_BGRA_8888);
      if (!mtk_afbc && !exy_afbc) {
         mesa_loge("ARM gralloc: AFBC buffer (alloc format 0x%" PRIx64 ") is not handled",
                   alloc_format);
         return -EINVAL;
      }
      if (mtk_afbc) {
         modifier = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                                            AFBC_FORMAT_MOD_TILED | AFBC_FORMAT_MOD_YTR |
                                            AFBC_FORMAT_MOD_SPARSE);
         stored_format = alloc_hal;
      } else {
         modifier = DRM_FORMAT_MOD_ARM_AFBC(AFBC_FORMAT_MOD_BLOCK_SIZE_16x16 |
                                            AFBC_FORMAT_MOD_SPARSE);
         stored_format = format == HAL_PIXEL_FORMAT_BGRA_8888 ? HAL_PIXEL_FORMAT_RGBA_8888
                                                              : format;
      }
   }

   const int drm_fourcc = get_fourcc_from_hal_format(stored_format);
   if (drm_fourcc == -1 || !bpp || !width || stride < width * bpp || stride > INT32_MAX) {
      mesa_loge("ARM gralloc: inconsistent handle (format 0x%x, %ux%u, stride %u)", format,
                width, height, stride);
      return -EINVAL;
   }

   /* The buffer's dma-buf: the first fd big enough to hold it. Exynos puts it first; MediaTek's
    * handle carries 3 fds and the first is not one (lseek gives no size). */
   int fd = -1;
   for (int i = 0; i < handle->numFds && fd < 0; i++) {
      const off_t sz = lseek(handle->data[i], 0, SEEK_END);
      lseek(handle->data[i], 0, SEEK_SET);
      if (sz > 0 && (uint64_t)sz >= (uint64_t)stride * height)
         fd = handle->data[i];
   }
   if (fd < 0) {
      mesa_loge("ARM gralloc: no fd of the handle holds %u bytes", stride * height);
      return -EINVAL;
   }

   out->drm_fourcc = drm_fourcc;
   out->modifier = modifier;
   out->num_planes = 1;
   out->fds[0] = fd;
   out->offsets[0] = 0;
   out->strides[0] = stride;

   return 0;
}

static int
fallback_gralloc_get_buffer_info(struct u_gralloc *gralloc,
                                 struct u_gralloc_buffer_handle *hnd,
                                 struct u_gralloc_buffer_basic_info *out)
{
   int num_planes = 0;
   int drm_fourcc = 0;
   int stride = 0;

   if (hnd->handle->numFds == 0)
      return -EINVAL;

#ifdef HAS_SAMSUNG_GRALLOC
   int sec_ret = samsung_gralloc_get_buffer_info(hnd, out);
   if (sec_ret != -ENOENT)
      return sec_ret;
#endif
   int arm_ret = arm_gralloc_get_buffer_info(hnd, out);
   if (arm_ret != -ENOENT)
      return arm_ret;

   if (is_hal_format_yuv(hnd->hal_format)) {
      int ret = fallback_gralloc_get_yuv_info(gralloc, hnd, out);
      /*
       * HACK: https://issuetracker.google.com/32077885
       * There is no API available to properly query the
       * IMPLEMENTATION_DEFINED format. As a workaround we rely here on
       * gralloc allocating either an arbitrary YCbCr 4:2:0 or RGBX_8888, with
       * the latter being recognized by lock_ycbcr failing.
       */
      if (ret != -EAGAIN)
         return ret;
   }

   /*
    * Non-YUV formats could *also* have multiple planes, such as ancillary
    * color compression state buffer, but the rest of the code isn't ready
    * yet to deal with modifiers:
    */
   num_planes = 1;

   drm_fourcc = get_fourcc_from_hal_format(hnd->hal_format);
   if (drm_fourcc == -1) {
      mesa_loge("Failed to get drm_fourcc");
      return -EINVAL;
   }

   stride = hnd->pixel_stride * get_hal_format_bpp(hnd->hal_format);
   if (stride == 0) {
      mesa_loge("Failed to calcuulate stride");
      return -EINVAL;
   }

   out->drm_fourcc = drm_fourcc;
   out->modifier = DRM_FORMAT_MOD_INVALID;
   out->num_planes = num_planes;
   out->fds[0] = largest_handle_fd(hnd->handle);
   if (out->fds[0] < 0)
      out->fds[0] = hnd->handle->data[0];
   out->strides[0] = stride;

#ifdef HAS_FREEDRENO
   uint32_t gmsm = ('g' << 24) | ('m' << 16) | ('s' << 8) | 'm';
   if (hnd->handle->numInts >= 2 && hnd->handle->data[hnd->handle->numFds] == gmsm) {
      /* This UBWC flag was introduced in a5xx. */
      bool ubwc = hnd->handle->data[hnd->handle->numFds + 1] & 0x08000000;
      out->modifier = ubwc ? DRM_FORMAT_MOD_QCOM_COMPRESSED : DRM_FORMAT_MOD_LINEAR;
   }
#endif

   return 0;
}

static int
destroy(struct u_gralloc *gralloc)
{
   struct fallback_gralloc *gr = (struct fallback_gralloc *)gralloc;
   if (gr->gralloc_module) {
      dlclose(gr->gralloc_module->common.dso);
   }

   FREE(gr);

   return 0;
}

struct u_gralloc *
u_gralloc_fallback_create()
{
   struct fallback_gralloc *gr = CALLOC_STRUCT(fallback_gralloc);
   int err = 0;

   err = hw_get_module(GRALLOC_HARDWARE_MODULE_ID,
                       (const hw_module_t **)&gr->gralloc_module);

   if (err) {
      mesa_logw(
         "No gralloc hwmodule detected (video buffers won't be supported)");
   } else if (!gr->gralloc_module->lock_ycbcr) {
      mesa_logw("Gralloc doesn't support lock_ycbcr (video buffers won't be "
                "supported)");
   }

   gr->base.ops.get_buffer_basic_info = fallback_gralloc_get_buffer_info;
   gr->base.ops.destroy = destroy;

   mesa_logi("Using fallback gralloc implementation");

   return &gr->base;
}
