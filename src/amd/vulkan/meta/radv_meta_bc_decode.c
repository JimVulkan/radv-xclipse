/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 *
 * GPU decode of the BC formats the texture unit has no decoder for (BC4, BC5, BC6H, BC7).
 *
 * The image gets a hidden second plane (see radv_image_get_plane_format). The app uploads the
 * compressed blocks into plane 0; a compute shader then reads plane 0 as a uint image (one texel
 * per 4x4 block) and writes plane 1. Sampled views use plane 1 (radv_image_view.c).
 *
 * Same structure as radv_meta_etc_decode.c (Copyright 2021 Google, MIT).
 */

#include "radv_formats.h"
#include "ac_xclipse_log.h"
#include "radv_meta.h"
#include "vk_format.h"
#ifdef __ANDROID__
#include <android/log.h>
#include <time.h>
#include "util/simple_mtx.h"
#include "util/u_atomic.h"
#include "radv_cpushare.h"
#endif

#ifdef __ANDROID__
/* [BCREPEAT]: is the app re-uploading the same textures? Counts decodes by two keys:
 *   SURFACE key -- image pointer + mip
 *   SHAPE key   -- format + width + height + mip
 * few surfaces, few shapes   -> same images re-uploaded; an image-keyed cache works.
 * many surfaces, few shapes  -> image churn; a cache needs a content hash.
 * many surfaces, many shapes -> distinct textures; no cache.
 * Upper bound only: same image + mip is not the same bytes, and pointers can be recycled.
 * One mutex and one hash probe per decode. */
#define BC_RPT_SLOTS 65536u /* power of two; open addressing, linear probing. Must not saturate. */

struct bc_rpt_tbl {
   uint64_t key[BC_RPT_SLOTS];
   uint32_t count[BC_RPT_SLOTS];
   uint32_t used;
   uint32_t full; /* keys dropped because the table filled -- printed, never hidden */
};

static simple_mtx_t bc_rpt_lock = SIMPLE_MTX_INITIALIZER;
static struct bc_rpt_tbl bc_rpt_surface, bc_rpt_shape;
static uint32_t bc_rpt_total;

static void
bc_rpt_add(struct bc_rpt_tbl *t, uint64_t key)
{
   /* key 0 would collide with an empty slot; fold it to something else. */
   if (!key)
      key = 1;
   uint64_t h = key * 0x9e3779b97f4a7c15ull;
   for (uint32_t i = 0; i < 64; i++) {
      const uint32_t s = (uint32_t)((h >> 32) + i) & (BC_RPT_SLOTS - 1);
      if (t->key[s] == key) {
         t->count[s]++;
         return;
      }
      if (!t->key[s]) {
         t->key[s] = key;
         t->count[s] = 1;
         t->used++;
         return;
      }
   }
   t->full++;
}

/* Share of decodes landing on the N hottest keys (concentration, not just a total). */
static void
bc_rpt_top(const struct bc_rpt_tbl *t, uint32_t *top1, uint32_t *top8_sum)
{
   uint32_t best[8] = {0};
   for (uint32_t s = 0; s < BC_RPT_SLOTS; s++) {
      uint32_t c = t->count[s];
      if (!c)
         continue;
      for (uint32_t j = 0; j < 8; j++) {
         if (c > best[j]) {
            const uint32_t tmp = best[j];
            best[j] = c;
            c = tmp;
         }
      }
   }
   *top1 = best[0];
   *top8_sum = 0;
   for (uint32_t j = 0; j < 8; j++)
      *top8_sum += best[j];
}

/* [BCCONTENT]: do the bytes repeat? DXVK recreates the VkImage per upload, so only content can key
 * a cache. The shader writes 64 sampled block hashes into a ring slot (bc_content_sample in
 * bc_decoder.glsl); the CPU reads a slot BC_HASH_LAG decodes later, with no fence or readback.
 * Torn reads can show false distinct keys, never false matches. The fill count is printed so a
 * saturated table is visible. */
#define BC_HASH_LAG 512u   /* decodes to wait before reading a slot back */
#define BC_HASH_SLOTS 1024u /* ring slots, each 64 dwords */
#define BC_HASH_BASE_DW 1024u /* the ring starts after the histogram's 1024 dwords */

static struct bc_rpt_tbl bc_rpt_content;
static uint32_t bc_hash_counter;
static uint32_t bc_hash_nblocks[BC_HASH_SLOTS]; /* ring entries that decode wrote */
static uint32_t bc_content_unreadable;

static void
bc_rpt_track(const struct radv_image *image, uint32_t mip, VkFormat format, uint32_t w, uint32_t h)
{
   simple_mtx_lock(&bc_rpt_lock);

   bc_rpt_total++;
   bc_rpt_add(&bc_rpt_surface, ((uint64_t)(uintptr_t)image << 4) ^ mip);
   bc_rpt_add(&bc_rpt_shape, ((uint64_t)format << 44) ^ ((uint64_t)w << 28) ^ ((uint64_t)h << 12) ^ mip);

   static uint64_t last_ms;
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   const uint64_t now = (uint64_t)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
   if (!last_ms)
      last_ms = now;

   if (now - last_ms >= 1000) {
      uint32_t s_top1, s_top8, p_top1, p_top8;
      bc_rpt_top(&bc_rpt_surface, &s_top1, &s_top8);
      bc_rpt_top(&bc_rpt_shape, &p_top1, &p_top8);
      const uint32_t tot = bc_rpt_total ? bc_rpt_total : 1;
      /* "repeat" = decodes beyond the first for a key. 0% means every decode was a distinct key. */
      AC_XCLIPSE_LOGP(
         ANDROID_LOG_INFO, "RADV_KILL",
         "[BCREPEAT] decodes=%u | SURFACE distinct=%u repeat=%.1f%% hottest=%u top8=%.1f%% | "
         "SHAPE distinct=%u repeat=%.1f%% hottest=%u top8=%.1f%%%s",
         bc_rpt_total, bc_rpt_surface.used, 100.0 * (double)(tot - bc_rpt_surface.used) / (double)tot, s_top1,
         100.0 * (double)s_top8 / (double)tot, bc_rpt_shape.used,
         100.0 * (double)(tot - bc_rpt_shape.used) / (double)tot, p_top1, 100.0 * (double)p_top8 / (double)tot,
         (bc_rpt_surface.full || bc_rpt_shape.full) ? " TABLE FULL, distinct UNDERCOUNTS" : "");

      /* The content repeat rate. 'used' next to the slot count shows saturation. */
      uint32_t c_top1, c_top8;
      bc_rpt_top(&bc_rpt_content, &c_top1, &c_top8);
      const uint32_t ctot = bc_hash_counter > BC_HASH_LAG ? bc_hash_counter - BC_HASH_LAG : 0;
      if (ctot)
         AC_XCLIPSE_LOGP(ANDROID_LOG_INFO, "RADV_KILL",
                             "[BCCONTENT] hashed=%u | distinct=%u/%u slots repeat=%.1f%% hottest=%u "
                             "top8=%.1f%% | unreadable=%u%s",
                             ctot, bc_rpt_content.used, BC_RPT_SLOTS,
                             100.0 * (double)(ctot - bc_rpt_content.used) / (double)ctot, c_top1,
                             100.0 * (double)c_top8 / (double)ctot, bc_content_unreadable,
                             bc_rpt_content.full ? "  TABLE FULL -- repeat%% IS ARITHMETIC, NOT SIGNAL" : "");
      last_ms = now;
   }

   simple_mtx_unlock(&bc_rpt_lock);
}
/* [CPUSHARE], see radv_cpushare.h. Atomics, not a mutex: submit is a hot path. The report is driven
 * by the first bucket past the one-second mark (no timer thread). */
static uint64_t bc_cpu_ns[RADV_CPU_NBUCKET];
static uint32_t bc_cpu_calls[RADV_CPU_NBUCKET];
static uint64_t bc_cpu_epoch;

static const char *const bc_cpu_name[RADV_CPU_NBUCKET] = {"decode", "submit", "pipeline", "compile"};

void
radv_cpushare_add(int bucket, uint64_t ns)
{
   if (bucket < 0 || bucket >= RADV_CPU_NBUCKET || ac_xclipse_log_level() < 1)
      return;
   p_atomic_add(&bc_cpu_ns[bucket], ns);
   p_atomic_inc(&bc_cpu_calls[bucket]);

   const uint64_t now = radv_cpushare_now();
   uint64_t epoch = p_atomic_read(&bc_cpu_epoch);
   if (!epoch) {
      p_atomic_cmpxchg(&bc_cpu_epoch, 0, now);
      return;
   }
   const uint64_t elapsed = now - epoch;
   if (elapsed < 1000000000ull)
      return;
   /* One winner takes the window; the losers just keep accumulating into the next one. */
   if (p_atomic_cmpxchg(&bc_cpu_epoch, epoch, now) != epoch)
      return;

   char line[320];
   int n = 0;
   double pct_of[RADV_CPU_NBUCKET] = {0};
   uint32_t all_calls = 0;
   for (int b = 0; b < RADV_CPU_NBUCKET; b++) {
      const uint64_t bns = p_atomic_xchg(&bc_cpu_ns[b], 0);
      const uint32_t bc = p_atomic_xchg(&bc_cpu_calls[b], 0);
      const double pct = 100.0 * (double)bns / (double)elapsed;
      pct_of[b] = pct;
      all_calls += bc;
      n += snprintf(line + n, sizeof line - (size_t)n, "%s%s=%.2f%% (%u calls, %.1f us ea)",
                    n ? " | " : "", bc_cpu_name[b], pct, bc,
                    bc ? (double)bns / (double)bc / 1000.0 : 0.0);
   }
   const double inst_pct = 100.0 * 50.0 * (double)all_calls / (double)elapsed;
   /* Share of one thread's wall time spent in our code, with the instrument's own cost.
    * Decode includes pipeline creation, so the net figure is printed too. */
   const double net_pct = pct_of[RADV_CPU_DECODE] - pct_of[RADV_CPU_PIPELINE];
   AC_XCLIPSE_LOGP(ANDROID_LOG_INFO, "RADV_KILL",
                       "[CPUSHARE] %s | decode NET of pipeline=%.3f%% | wall=%.2fs | instrument ~%.3f%%",
                       line, net_pct < 0.0 ? 0.0 : net_pct, (double)elapsed / 1e9, inst_pct);
}

#endif

static const uint32_t bc_decoder_unorm8_spv[] = {
#include "bc_decoder_unorm8_spv.h"
};
static const uint32_t bc_decoder_snorm8_spv[] = {
#include "bc_decoder_snorm8_spv.h"
};
static const uint32_t bc_decoder_f16_spv[] = {
#include "bc_decoder_f16_spv.h"
};
static const uint32_t bc_decoder_u32x4_spv[] = {
#include "bc_decoder_u32x4_spv.h"
};
static const uint32_t bc_decoder_eac_spv[] = {
#include "bc_decoder_eac_spv.h"
};
static const uint32_t bc_decoder_bc3_spv[] = {
#include "bc_decoder_bc3_spv.h"
};

/* Keep in sync with bc_decoder.glsl. */
enum bc_shader_format {
   BC_FMT_BC4_UNORM = 0,
   BC_FMT_BC4_SNORM = 1,
   BC_FMT_BC5_UNORM = 2,
   BC_FMT_BC5_SNORM = 3,
   BC_FMT_BC6H_UF16 = 4,
   BC_FMT_BC6H_SF16 = 5,
   BC_FMT_BC7 = 6,
};

enum bc_variant {
   BC_VARIANT_UNORM8 = 0,
   BC_VARIANT_SNORM8 = 1,
   BC_VARIANT_F16 = 2,
   /* Not a decode: BC4_UNORM transcoded into a BC3 plane for the texture unit to decode. */
   BC_VARIANT_U32X4 = 3,
   /* Not a decode either: BC5_UNORM re-encoded into an EAC_R11G11 plane. */
   BC_VARIANT_EAC = 4,
   /* Decode, then re-encode into a BC3 plane (the BC7 carrier, no endpoint search). */
   BC_VARIANT_BC3 = 5,
   BC_VARIANT_COUNT,
};

static enum bc_shader_format
bc_shader_format(VkFormat format)
{
   switch (format) {
   case VK_FORMAT_BC4_UNORM_BLOCK:
      return BC_FMT_BC4_UNORM;
   case VK_FORMAT_BC4_SNORM_BLOCK:
      return BC_FMT_BC4_SNORM;
   case VK_FORMAT_BC5_UNORM_BLOCK:
      return BC_FMT_BC5_UNORM;
   case VK_FORMAT_BC5_SNORM_BLOCK:
      return BC_FMT_BC5_SNORM;
   case VK_FORMAT_BC6H_UFLOAT_BLOCK:
      return BC_FMT_BC6H_UF16;
   case VK_FORMAT_BC6H_SFLOAT_BLOCK:
      return BC_FMT_BC6H_SF16;
   default:
      return BC_FMT_BC7;
   }
}

static enum bc_variant
bc_variant(VkFormat format)
{
   switch (radv_bc_emulation_format(format)) {
   case VK_FORMAT_R8G8B8A8_SNORM:
   case VK_FORMAT_R8G8_SNORM:
   case VK_FORMAT_R8_SNORM:
      return BC_VARIANT_SNORM8;
   case VK_FORMAT_R16G16B16A16_SFLOAT:
      return BC_VARIANT_F16;
   /* The hidden plane is itself compressed: transcode, do not decode. */
   case VK_FORMAT_BC3_UNORM_BLOCK:
      /* BC3 planes come from BC4 (byte shuffle) or BC7 (decode + re-encode): dispatch on the source
       * format. */
      return (format == VK_FORMAT_BC7_UNORM_BLOCK || format == VK_FORMAT_BC7_SRGB_BLOCK)
                ? BC_VARIANT_BC3 : BC_VARIANT_U32X4;
   case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
      return BC_VARIANT_EAC;
   default:
      return BC_VARIANT_UNORM8;
   }
}

/* Plane 0 holds the compressed blocks. View it as one uint texel per block. */
static VkFormat
bc_load_format(VkFormat format)
{
   return vk_format_get_blocksize(format) == 8 ? VK_FORMAT_R32G32_UINT : VK_FORMAT_R32G32B32A32_UINT;
}

/* Plane 1 is written through a storage image, which cannot be sRGB. */
static VkFormat
bc_store_format(VkFormat format)
{
   VkFormat emulation = radv_bc_emulation_format(format);
   /* A compressed destination plane is viewed as one uint texel per block (16 bytes).
    * Every compressed carrier must be listed here: otherwise the compressed format is returned, it
    * is not a legal storage format, and imageStore is silently discarded. */
   if (emulation == VK_FORMAT_BC3_UNORM_BLOCK || emulation == VK_FORMAT_EAC_R11G11_UNORM_BLOCK)
      return VK_FORMAT_R32G32B32A32_UINT;
   return emulation == VK_FORMAT_R8G8B8A8_SRGB ? VK_FORMAT_R8G8B8A8_UNORM : emulation;
}

struct radv_bc_decode_key {
   enum radv_meta_object_key_type type;
   uint32_t variant;
};

static VkResult
get_pipeline_layout(struct radv_device *device, VkPipelineLayout *layout_out)
{
   enum radv_meta_object_key_type key = RADV_META_OBJECT_KEY_BC_DECODE;

   const VkDescriptorSetLayoutBinding bindings[] = {
      {
         .binding = 0,
         .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         .binding = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         /* Endpoint-spread histogram; see bc_record_spread() in bc_decoder.glsl. */
         .binding = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
      {
         /* BC5 -> EAC_R11G11 lookup table (bc_eac_encode() in bc_decoder.glsl). Declared for every
          * variant so one pipeline layout serves all; only the EAC shader reads it. */
         .binding = 3,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
         .descriptorCount = 1,
         .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      },
   };

   const VkDescriptorSetLayoutCreateInfo desc_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT,
      .bindingCount = 4,
      .pBindings = bindings,
   };

   const VkPushConstantRange pc_range = {
      .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
      .size = 32,
   };

   return vk_meta_get_pipeline_layout(&device->vk, &device->meta_state.device, &desc_info, &pc_range, &key, sizeof(key),
                                      layout_out);
}

static VkResult
get_pipeline(struct radv_device *device, enum bc_variant variant, VkPipeline *pipeline_out,
             VkPipelineLayout *layout_out)
{
   struct radv_bc_decode_key key;
   VkResult result;

   result = get_pipeline_layout(device, layout_out);
   if (result != VK_SUCCESS)
      return result;

   memset(&key, 0, sizeof(key));
   key.type = RADV_META_OBJECT_KEY_BC_DECODE;
   key.variant = variant;

   VkPipeline pipeline_from_cache = vk_meta_lookup_pipeline(&device->meta_state.device, &key, sizeof(key));
   if (pipeline_from_cache != VK_NULL_HANDLE) {
      *pipeline_out = pipeline_from_cache;
      return VK_SUCCESS;
   }

   const uint32_t *spv;
   size_t spv_size;

   switch (variant) {
   case BC_VARIANT_SNORM8:
      spv = bc_decoder_snorm8_spv;
      spv_size = sizeof(bc_decoder_snorm8_spv);
      break;
   case BC_VARIANT_F16:
      spv = bc_decoder_f16_spv;
      spv_size = sizeof(bc_decoder_f16_spv);
      break;
   case BC_VARIANT_U32X4:
      spv = bc_decoder_u32x4_spv;
      spv_size = sizeof(bc_decoder_u32x4_spv);
      break;
   case BC_VARIANT_EAC:
      spv = bc_decoder_eac_spv;
      spv_size = sizeof(bc_decoder_eac_spv);
      break;
   case BC_VARIANT_BC3:
      spv = bc_decoder_bc3_spv;
      spv_size = sizeof(bc_decoder_bc3_spv);
      break;
   default:
      spv = bc_decoder_unorm8_spv;
      spv_size = sizeof(bc_decoder_unorm8_spv);
      break;
   }

   const VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = spv_size,
      .pCode = spv,
   };

   const struct vk_device_dispatch_table *disp = &device->vk.dispatch_table;
   VkShaderModule module;
   result = disp->CreateShaderModule(radv_device_to_handle(device), &module_info, &device->meta_state.alloc, &module);
   if (result != VK_SUCCESS)
      return result;

   const VkPipelineShaderStageCreateInfo stage_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
      .module = module,
      .pName = "main",
   };

   const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = stage_info,
      .flags = 0,
      .layout = *layout_out,
   };

   const uint64_t _cc_t0 = radv_cpushare_now();
   result = vk_meta_create_compute_pipeline(&device->vk, &device->meta_state.device, &pipeline_info, &key, sizeof(key),
                                            pipeline_out);
   const uint64_t _cc_ns = radv_cpushare_now() - _cc_t0;
   radv_cpushare_add(RADV_CPU_COMPILE, _cc_ns);
#ifdef __ANDROID__
   /* Log compile time per variant (each compiles at most once per process). */
   {
      static const char *const vname[BC_VARIANT_COUNT] = {
         "unorm8", "snorm8", "f16", "u32x4", "eac", "bc3"};
      AC_XCLIPSE_LOGP(ANDROID_LOG_INFO, "RADV_KILL", "[BCCOMPILE] variant=%s %.0f ms",
                          (variant >= 0 && variant < BC_VARIANT_COUNT) ? vname[variant] : "?",
                          (double)_cc_ns / 1e6);
   }
#endif

   disp->DestroyShaderModule(radv_device_to_handle(device), module, &device->meta_state.alloc);

   return result;
}

/* The spread histogram and census buffer live in device->meta_state.xclipse_bc (per device, see
 * radv_device.h). Unsynchronised: the shader writes, the CPU reads a slightly stale copy. */

/* The BC5 -> EAC_R11G11 lookup table, uploaded once: 65536 x 16 bits,
 * base_codeword<<8 | multiplier<<4 | table_index, indexed by the BC4 endpoint pair (eac_lut.h). */
#include "eac_lut.h"
/* The configuration table (65536 uints) followed by the nearest-index remap table (131072 uints,
 * two configurations per endpoint pair), in one buffer and binding. 768 KB. */
#define BC_EAC_CFG_WORDS   65536u
#define BC_EAC_REMAP_WORDS 131072u
#define BC_EAC_LUT_BYTES   ((BC_EAC_CFG_WORDS + BC_EAC_REMAP_WORDS) * 4u)

/* Lives in device->meta_state.xclipse_bc, like the histogram. */

static bool
bc_eac_lut_init(struct radv_device *device)
{
   if (device->meta_state.xclipse_bc.eac_lut_bo)
      return true;
   if (radv_bo_create(device, NULL, BC_EAC_LUT_BYTES, 4096, RADEON_DOMAIN_GTT,
                      RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING,
                      RADV_BO_PRIORITY_UPLOAD_BUFFER, 0, true, &device->meta_state.xclipse_bc.eac_lut_bo) != VK_SUCCESS)
      return false;
   void *p = radv_buffer_map(device->ws, device->meta_state.xclipse_bc.eac_lut_bo);
   if (!p) {
      radv_bo_destroy(device, NULL, device->meta_state.xclipse_bc.eac_lut_bo);
      device->meta_state.xclipse_bc.eac_lut_bo = NULL;
      return false;
   }
   memcpy(p, eac_lut, BC_EAC_CFG_WORDS * 4u);
   memcpy((char *)p + BC_EAC_CFG_WORDS * 4u, eac_remap, BC_EAC_REMAP_WORDS * 4u);
   device->meta_state.xclipse_bc.eac_lut_va = radv_buffer_get_va(device->meta_state.xclipse_bc.eac_lut_bo);
   return true;
}

/* The histogram is 1024 dwords; the [BCCONTENT] ring (1024 slots x 64 dwords) follows in the same
 * buffer. */
#ifndef BC_HASH_SLOTS   /* non-Android builds do not carry the census */
#define BC_HASH_SLOTS 1024u
#endif
#define BC_HIST_BYTES (4096u + BC_HASH_SLOTS * 64u * 4u)

static bool
bc_hist_init(struct radv_device *device)
{
   if (device->meta_state.xclipse_bc.hist_bo)
      return true;
   if (radv_bo_create(device, NULL, BC_HIST_BYTES, 4096, RADEON_DOMAIN_GTT,
                      RADEON_FLAG_CPU_ACCESS | RADEON_FLAG_NO_INTERPROCESS_SHARING, RADV_BO_PRIORITY_UPLOAD_BUFFER,
                      0, true, &device->meta_state.xclipse_bc.hist_bo) != VK_SUCCESS)
      return false;
   device->meta_state.xclipse_bc.hist_ptr = radv_buffer_map(device->ws, device->meta_state.xclipse_bc.hist_bo);
   if (!device->meta_state.xclipse_bc.hist_ptr) {
      radv_bo_destroy(device, NULL, device->meta_state.xclipse_bc.hist_bo);
      device->meta_state.xclipse_bc.hist_bo = NULL;
      return false;
   }
   memset(device->meta_state.xclipse_bc.hist_ptr, 0, BC_HIST_BYTES);
   device->meta_state.xclipse_bc.hist_va = radv_buffer_get_va(device->meta_state.xclipse_bc.hist_bo);
   return true;
}

/* Called from radv_device_finish_meta(). The BOs die with their device. */
void
radv_device_finish_meta_xclipse_bc(struct radv_device *device)
{
   if (device->meta_state.xclipse_bc.hist_bo)
      radv_bo_destroy(device, NULL, device->meta_state.xclipse_bc.hist_bo);
   if (device->meta_state.xclipse_bc.eac_lut_bo)
      radv_bo_destroy(device, NULL, device->meta_state.xclipse_bc.eac_lut_bo);
   memset(&device->meta_state.xclipse_bc, 0, sizeof(device->meta_state.xclipse_bc));
}

#ifdef __ANDROID__
/* Claim a ring slot for this dispatch and fold in one that is BC_HASH_LAG decodes old. */
static int
bc_content_slot(struct radv_device *device, uint32_t w, uint32_t h)
{
   /* The content census is a diagnostic. Slot -1 also disables the shader's ring write. */
   if (!device->meta_state.xclipse_bc.hist_ptr || ac_xclipse_log_level() < 1)
      return -1;

   const uint32_t blocks = ((w + 3) / 4) * ((h + 3) / 4);
   const uint32_t n = blocks < 64 ? blocks : 64;
   if (!n)
      return -1;

   simple_mtx_lock(&bc_rpt_lock);
   const uint32_t slot = bc_hash_counter++ & (BC_HASH_SLOTS - 1);
   bc_hash_nblocks[slot] = n;

   /* Fold back a slot old enough that its dispatch has certainly retired. */
   if (bc_hash_counter > BC_HASH_LAG) {
      const uint32_t old = (bc_hash_counter - BC_HASH_LAG) & (BC_HASH_SLOTS - 1);
      const uint32_t cnt = bc_hash_nblocks[old];
      if (cnt) {
         const uint32_t *ring = (const uint32_t *)device->meta_state.xclipse_bc.hist_ptr + BC_HASH_BASE_DW + old * 64u;
         uint64_t key = 0x9e3779b97f4a7c15ull ^ ((uint64_t)cnt << 56);
         for (uint32_t i = 0; i < cnt; i++)
            key = (key ^ ring[i]) * 0x100000001b3ull;
         bc_rpt_add(&bc_rpt_content, key);
      } else {
         bc_content_unreadable++;
      }
   }
   simple_mtx_unlock(&bc_rpt_lock);
   return (int)slot;
}

#endif

void
radv_meta_decode_bc(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, VkImageLayout layout,
                    const VkImageSubresourceLayers *subresource, VkOffset3D offset, VkExtent3D extent)
{
   RADV_CPU_T0();
   struct radv_device *device = radv_cmd_buffer_device(cmd_buffer);
   const VkFormat format = image->vk.format;
   VkPipeline pipeline;
   VkPipelineLayout p_layout;

   const uint64_t _pipe_t0 = radv_cpushare_now();
   const VkResult _pipe_r = get_pipeline(device, bc_variant(format), &pipeline, &p_layout);
   radv_cpushare_add(RADV_CPU_PIPELINE, radv_cpushare_now() - _pipe_t0);
   if (_pipe_r != VK_SUCCESS) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }

   if (!bc_hist_init(device) || !bc_eac_lut_init(device)) {
      vk_command_buffer_set_error(&cmd_buffer->vk, VK_ERROR_OUT_OF_HOST_MEMORY);
      return;
   }
   radv_cs_add_buffer(device->ws, cmd_buffer->cs->b, device->meta_state.xclipse_bc.hist_bo);
   radv_cs_add_buffer(device->ws, cmd_buffer->cs->b, device->meta_state.xclipse_bc.eac_lut_bo);

   const bool is_3d = image->vk.image_type == VK_IMAGE_TYPE_3D;
   const uint32_t base_slice = is_3d ? offset.z : subresource->baseArrayLayer;
   const uint32_t slice_count = is_3d ? extent.depth : vk_image_subresource_layer_count(&image->vk, subresource);
   const uint32_t layer_end = subresource->baseArrayLayer + vk_image_subresource_layer_count(&image->vk, subresource);
   const VkImageViewType view_type = is_3d ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;

   extent = vk_image_sanitize_extent(&image->vk, extent);
   offset = vk_image_sanitize_offset(&image->vk, offset);

#ifdef __ANDROID__
   /* Tracked after sanitising, so the SHAPE key sees the real extent. */
   if (ac_xclipse_log_level() >= 1) /* census, diagnostics only */
      bc_rpt_track(image, subresource->mipLevel, format, extent.width, extent.height);
#endif

   /* Source: the compressed plane, viewed as uint texels, one per 4x4 block. */
   const VkImageViewUsage2CreateInfoKHR src_usage = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_2_CREATE_INFO_KHR,
      .usage = VK_IMAGE_USAGE_2_SAMPLED_BIT_KHR,
   };
   struct radv_image_view src_iview;
   radv_image_view_init(&src_iview, device,
                        &(VkImageViewCreateInfo){
                           .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                           .pNext = &src_usage,
                           .flags = VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA,
                           .image = radv_image_to_handle(image),
                           .viewType = view_type,
                           .format = bc_load_format(format),
                           .subresourceRange =
                              {
                                 .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                 .baseMipLevel = subresource->mipLevel,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = layer_end,
                              },
                        },
                        NULL);

   /* Destination: the hidden decoded plane. */
   const VkImageViewUsage2CreateInfoKHR dst_usage = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_2_CREATE_INFO_KHR,
      .usage = VK_IMAGE_USAGE_2_STORAGE_BIT_KHR,
   };
   struct radv_image_view dst_iview;
   radv_image_view_init(&dst_iview, device,
                        &(VkImageViewCreateInfo){
                           .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                           .pNext = &dst_usage,
                           .flags = VK_IMAGE_VIEW_CREATE_DRIVER_INTERNAL_BIT_MESA,
                           .image = radv_image_to_handle(image),
                           .viewType = view_type,
                           .format = bc_store_format(format),
                           .subresourceRange =
                              {
                                 .aspectMask = VK_IMAGE_ASPECT_PLANE_1_BIT,
                                 .baseMipLevel = subresource->mipLevel,
                                 .levelCount = 1,
                                 .baseArrayLayer = 0,
                                 .layerCount = layer_end,
                              },
                        },
                        NULL);

   radv_meta_bind_descriptors(
      cmd_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, p_layout, 4,
      (VkDescriptorGetInfoEXT[]){{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
                                  .type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                  .data.pSampledImage =
                                     (VkDescriptorImageInfo[]){
                                        {.sampler = VK_NULL_HANDLE,
                                         .imageView = radv_image_view_to_handle(&src_iview),
                                         .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
                                     }},
                                 {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
                                  .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                  .data.pStorageImage = (VkDescriptorImageInfo[]){
                                     {.sampler = VK_NULL_HANDLE,
                                      .imageView = radv_image_view_to_handle(&dst_iview),
                                      .imageLayout = VK_IMAGE_LAYOUT_GENERAL},
                                  }},
                                 {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
                                  .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .data.pStorageBuffer = (VkDescriptorAddressInfoEXT[]){
                                     {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
                                      .address = device->meta_state.xclipse_bc.hist_va,
                                      .range = BC_HIST_BYTES,
                                      .format = VK_FORMAT_UNDEFINED},
                                  }},
                                 {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_GET_INFO_EXT,
                                  .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .data.pStorageBuffer = (VkDescriptorAddressInfoEXT[]){
                                     {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_ADDRESS_INFO_EXT,
                                      .address = device->meta_state.xclipse_bc.eac_lut_va,
                                      .range = BC_EAC_LUT_BYTES,
                                      .format = VK_FORMAT_UNDEFINED},
                                  }}});

   radv_meta_bind_compute_pipeline(cmd_buffer, pipeline);

#ifdef __ANDROID__
   const int hash_slot = bc_content_slot(device, extent.width, extent.height);
#else
   const int hash_slot = -1;
#endif
   const unsigned push_constants[8] = {
      offset.x, offset.y, base_slice, bc_shader_format(format), image->vk.image_type,
      extent.width, extent.height, (unsigned)hash_slot,
   };
   radv_meta_push_constants(cmd_buffer, p_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants),
                            push_constants);

   /* Dispatch whole 8x8 workgroups and let the shader's bounds check mask the surplus: the
    * partial-workgroup path of radv_unaligned_dispatch() writes nothing on the Xclipse 920.
    * One invocation per 4x4 block, so the dispatch is ceil(texels/4) / 8. Change together with
    * bc_decoder.glsl.
    */
   const struct radv_dispatch_info info = {
      .blocks = {DIV_ROUND_UP(DIV_ROUND_UP(extent.width, 4), 8),
                 DIV_ROUND_UP(DIV_ROUND_UP(extent.height, 4), 8), slice_count},
   };
   /* Mark this dispatch as a decode so radv_emit_cache_flush() does not drain the post-decode flush
    * a previous decode left pending. Covers only the dispatch emission. */
   cmd_buffer->state.bc_decode_active++;
   radv_compute_dispatch(cmd_buffer, &info);
   cmd_buffer->state.bc_decode_active--;

#ifdef __ANDROID__
   /* Count decodes and report the rate, summarised (logging each would perturb the storm).
    * BC emulation cannot be switched off for an A/B: D3D11 titles require textureCompressionBC. */
   if (ac_xclipse_log_level() >= 1) { /* census, diagnostics only */
      static uint32_t n_decode;
      static uint64_t window_start_ms, last_report_ms;
      static uint32_t window_n;
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      const uint64_t now = (uint64_t)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;
      const uint32_t total = p_atomic_inc_return(&n_decode);

      /* [BCFMT]: per-format census since process start (the fmt= field above is a 1 Hz sample).
       * Counted in decodes (presence) and blocks (weight). */
      static uint32_t fmt_decodes[7], fmt_blocks[7];
      {
         const unsigned fi = (unsigned)bc_shader_format(format);
         const uint32_t nblk = (((uint32_t)extent.width + 3) >> 2) * (((uint32_t)extent.height + 3) >> 2);
         p_atomic_inc(&fmt_decodes[fi]);
         p_atomic_add(&fmt_blocks[fi], nblk);
      }

      if (!window_start_ms) {
         window_start_ms = last_report_ms = now;
         window_n = 0;
      }
      window_n++;
      if (now - last_report_ms >= 1000) {
         const uint64_t span = now - last_report_ms;
         AC_XCLIPSE_LOGP(ANDROID_LOG_INFO, "RADV_KILL",
                             "[BCDECODE] %u decodes in last %llu ms (%.0f/s) | total=%u | "
                             "last %ux%u fmt=%u",
                             window_n, (unsigned long long)span,
                             span ? (double)window_n * 1000.0 / (double)span : 0.0, total,
                             extent.width, extent.height, (unsigned)format);
         last_report_ms = now;
         window_n = 0;

         /* Every emulated format, every decode. BC4/BC5_UNORM transcode through the same dispatch
          * and are counted too. */
         {
            static const char *const fname[7] = {"BC4U", "BC4S", "BC5U", "BC5S", "BC6HU", "BC6HS", "BC7"};
            char line[320];
            char *p = line;
            uint64_t tot_blk = 0;
            for (int i = 0; i < 7; i++)
               tot_blk += fmt_blocks[i];
            for (int i = 0; i < 7; i++) {
               if (!fmt_decodes[i])
                  continue;
               p += sprintf(p, " %s:%u/%.1f%%", fname[i], fmt_decodes[i],
                            tot_blk ? 100.0 * (double)fmt_blocks[i] / (double)tot_blk : 0.0);
            }
            if (p != line)
               AC_XCLIPSE_LOGP(ANDROID_LOG_INFO, "RADV_KILL",
                                   "[BCFMT] decodes/share-of-blocks:%s | total_blocks=%llu", line,
                                   (unsigned long long)tot_blk);
         }

         /* Share of real BC4/BC5 blocks with endpoint spread <= 32 (exactly decodable by the texture
          * unit). Quiet until some BC4/BC5 was decoded. */
         if (device->meta_state.xclipse_bc.hist_ptr && device->meta_state.xclipse_bc.hist_ptr[6]) {
            const uint32_t *h = device->meta_state.xclipse_bc.hist_ptr;
            const uint32_t tot = h[6];
            const uint32_t narrow = h[0] + h[1] + h[2];   /* spread <= 32 */
            /* h[8] (both filters) is the real number: the r0 <= r1 mode is off-line at any spread. */
            AC_XCLIPSE_LOGP(ANDROID_LOG_INFO, "RADV_KILL",
                                "[BCSPREAD] sampled=%u narrow=%u (%.1f%%) mode8=%u (%.1f%%) "
                                "offline=%u (%.1f%%) DECODABLE=%u (%.1f%%) | "
                                "<=8:%u 9-16:%u 17-32:%u 33-64:%u 65-128:%u >128:%u",
                                tot, narrow, 100.0 * (double)narrow / (double)tot,
                                h[7], 100.0 * (double)h[7] / (double)tot,
                                h[9], 100.0 * (double)h[9] / (double)tot,
                                h[8], 100.0 * (double)h[8] / (double)tot,
                                h[0], h[1], h[2], h[3], h[4], h[5]);
         }

         /* BC5 census (bc_record_rg in bc_decoder.glsl): VIABLE = R and G spans agree within 2 LSB
          * (ASTC dual-plane CEM 0 shares one endpoint pair). Reported as a distribution. BC5 only. */
         if (device->meta_state.xclipse_bc.hist_ptr && device->meta_state.xclipse_bc.hist_ptr[17]) {
            const uint32_t *h = device->meta_state.xclipse_bc.hist_ptr;
            const uint32_t tot = h[17];
            const uint32_t viable = h[10] + h[11];
            AC_XCLIPSE_LOGP(ANDROID_LOG_INFO, "RADV_KILL",
                                "[BCRG] blocks=%u VIABLE(<=2)=%u (%.1f%%) | "
                                "0:%u 1-2:%u 3-4:%u 5-8:%u 9-16:%u 17-32:%u >32:%u",
                                tot, viable, 100.0 * (double)viable / (double)tot,
                                h[10], h[11], h[12], h[13], h[14], h[15], h[16]);
         }

         /* BC7 census (bc_record_bc7 in bc_decoder.glsl). ASTC reproduces only 30/64 two-subset and
          * 11/64 three-subset partitions.
          * FREE = modes 4/5/6 (single subset). OK/BAD = multi-subset on a pattern ASTC can/cannot
          * generate. TRANSCODABLE = FREE + OK. */
         if (device->meta_state.xclipse_bc.hist_ptr && device->meta_state.xclipse_bc.hist_ptr[30]) {
            const uint32_t *h = device->meta_state.xclipse_bc.hist_ptr;
            const uint32_t tot = h[30];
            const uint32_t freeblk = h[18 + 4] + h[18 + 5] + h[18 + 6];
            const uint32_t ok = h[26] + h[28], bad = h[27] + h[29];
            AC_XCLIPSE_LOGP(ANDROID_LOG_INFO, "RADV_KILL",
                                "[BC7MODE] blocks=%u | m0:%u m1:%u m2:%u m3:%u m4:%u m5:%u m6:%u m7:%u"
                                " | FREE(1-subset)=%u (%.1f%%) partOK=%u partBAD=%u"
                                " | TRANSCODABLE=%.1f%%",
                                tot, h[18], h[19], h[20], h[21], h[22], h[23], h[24], h[25],
                                freeblk, 100.0 * (double)freeblk / (double)tot, ok, bad,
                                100.0 * (double)(freeblk + ok) / (double)tot);
         }
      }
   }
#endif

   radv_image_view_finish(&src_iview);
   radv_image_view_finish(&dst_iview);
   RADV_CPU_T1(RADV_CPU_DECODE);
}

/* Order the decode dispatches against everything that follows. Call once after a run of
 * radv_meta_decode_bc() calls, not after each one.
 *
 * The app's barrier only covers the transfer it asked for, not our compute dispatch, so a later
 * read of plane 1 would race it. The L2 caches must also be written back and invalidated, or reads
 * are intermittently stale. Decodes in a run cannot alias (Vulkan forbids overlapping destination
 * regions), so nothing is flushed between them. */
void
radv_meta_decode_bc_flush(struct radv_cmd_buffer *cmd_buffer, struct radv_image *image, uint32_t n_regions)
{
   /* WB_L2|INV_L2 are kept (removing them cost 2-3 fps in-game: dirty decode output evicts render
    * data), but deferred across commands via state.bc_decode_pending: radv_emit_cache_flush()
    * materialises them at the next draw, dispatch, barrier or end of command buffer.
    * The image-specific part resolves now, since `image` is not captured. */
   cmd_buffer->state.bc_decode_pending = true;
   cmd_buffer->state.flush_bits |= radv_src_access_flush(cmd_buffer, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                                         VK_ACCESS_2_SHADER_WRITE_BIT, 0, image, NULL);

#ifdef __ANDROID__
   /* Regions per copy command, reported as a distribution once per second. One copy per mip means
    * only cross-command flush deferral helps. */
   if (ac_xclipse_log_level() >= 1) { /* census, diagnostics only */
      static uint32_t n_copies, n_regions_total, n_multi;
      static uint32_t bucket[5]; /* 1 | 2-3 | 4-7 | 8-15 | 16+ */
      static uint64_t last_report_ms;
      static uint32_t worst;
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      const uint64_t now = (uint64_t)ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull;

      p_atomic_inc(&n_copies);
      p_atomic_add(&n_regions_total, n_regions);
      if (n_regions > 1)
         p_atomic_inc(&n_multi);
      if (n_regions > worst)
         worst = n_regions;
      p_atomic_inc(&bucket[n_regions <= 1 ? 0 : n_regions <= 3 ? 1 : n_regions <= 7 ? 2 : n_regions <= 15 ? 3 : 4]);

      if (!last_report_ms)
         last_report_ms = now;
      if (now - last_report_ms >= 1000) {
         const uint32_t c = n_copies;
         /* skipped/drained prove the cross-command deferral engaged. */
         extern uint32_t radv_xclipse_bc_defer_skipped, radv_xclipse_bc_defer_drained;
         AC_XCLIPSE_LOGP(ANDROID_LOG_INFO, "RADV_KILL",
                             "[BCREGIONS] copies=%u regions=%u multi=%u (%.0f%%) max=%u | "
                             "hist 1:%u 2-3:%u 4-7:%u 8-15:%u 16+:%u | defer skipped=%u drained=%u",
                             c, n_regions_total, n_multi, c ? 100.0 * (double)n_multi / (double)c : 0.0, worst,
                             bucket[0], bucket[1], bucket[2], bucket[3], bucket[4],
                             radv_xclipse_bc_defer_skipped, radv_xclipse_bc_defer_drained);
         last_report_ms = now;
      }
   }
#endif
}
