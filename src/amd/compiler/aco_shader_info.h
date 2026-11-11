/*
 * Copyright © 2016 Red Hat.
 * Copyright © 2016 Bas Nieuwenhuizen
 *
 * based in part on anv driver which is:
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ACO_SHADER_INFO_H
#define ACO_SHADER_INFO_H

#include "ac_gpu_info.h"
#include "ac_hw_stage.h"
#include "ac_shader_args.h"
#include "amd_family.h"
#include "shader_enums.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACO_MAX_SO_OUTPUTS     128
#define ACO_MAX_SO_BUFFERS     4
#define ACO_MAX_VERTEX_ATTRIBS 32
#define ACO_MAX_VBS            32

#define ACO_SPI_SHADER_Z_FORMAT_UNKNOWN 0xff

struct aco_vs_prolog_info {
   struct ac_arg inputs;

   uint32_t instance_rate_inputs;
   uint32_t nontrivial_divisors;
   uint32_t zero_divisors;
   uint32_t post_shuffle;
   /* Having two separate fields instead of a single uint64_t makes it easier to remove attributes
    * using bitwise arithmetic.
    */
   uint32_t alpha_adjust_lo;
   uint32_t alpha_adjust_hi;

   uint8_t formats[ACO_MAX_VERTEX_ATTRIBS];

   unsigned num_attributes;
   uint32_t misaligned_mask;
   uint32_t unaligned_mask;
   bool is_ngg;
   mesa_shader_stage next_stage;
};

struct aco_ps_epilog_info {
   struct ac_arg colors[MAX_DRAW_BUFFERS];
   uint8_t color_map[MAX_DRAW_BUFFERS];

   uint32_t spi_shader_col_format;

   /* Bitmasks, each bit represents one of the 8 MRTs. */
   uint8_t color_is_int8;
   uint8_t color_is_int10;

   bool mrt0_is_dual_src;

   bool alpha_to_coverage_via_mrtz;
   bool alpha_to_one;

   /* OpenGL only */
   uint16_t color_types;
   bool clamp_color;
   bool skip_null_export;
   bool writes_all_cbufs;
   enum compare_func alpha_func;
   /* Depth/stencil/samplemask are always passed via VGPRs, and the epilog key can choose
    * not to export them using these flags, which can be dynamic states.
    */
   bool kill_depth;
   bool kill_stencil;
   bool kill_samplemask;
   bool lower_1bit_sample_mask_to_discard;
   uint8_t spi_shader_z_format;

   struct ac_arg alpha_reference;
   struct ac_arg depth;
   struct ac_arg stencil;
   struct ac_arg samplemask;
};

struct aco_ps_prolog_info {
   bool poly_stipple;
   unsigned poly_stipple_buf_offset;

   bool bc_optimize_for_persp;
   bool bc_optimize_for_linear;
   bool force_persp_sample_interp;
   bool force_linear_sample_interp;
   bool force_persp_center_interp;
   bool force_linear_center_interp;
   bool uses_persp_centroid;
   bool uses_linear_centroid;

   unsigned samplemask_log_ps_iter;
   bool force_samplemask_to_helper_invocation;
   unsigned num_interp_inputs;
   unsigned colors_read;
   uint8_t color_attr_index[2];
   enum ac_color_interp color_interp[2];
   bool color_two_side;
   bool needs_wqm;

   struct ac_arg internal_bindings;
};

struct aco_shader_info {
   enum ac_hw_stage hw_stage;
   uint8_t wave_size;
   bool schedule_ngg_pos_exports; /* Whether we should schedule position exports up or not. */
   bool image_2d_view_of_3d;
   unsigned workgroup_size;
   unsigned lds_size;
   bool merged_shader_compiled_separately; /* GFX9+ */
   bool descriptor_heap;
   struct ac_arg next_stage_pc;
   struct ac_arg epilog_pc; /* Vulkan only */
   struct {
      bool tcs_in_out_eq;
      bool any_tcs_inputs_via_lds;
      bool has_prolog;
      bool preserve_s2;
   } vs;
   struct {
      uint32_t num_inputs;
      unsigned spi_ps_input_ena;
      unsigned spi_ps_input_addr;
      bool has_prolog;
      bool has_epilog;

      /* OpenGL only */
      struct ac_arg alpha_reference;
   } ps;
   struct {
      bool uses_full_subgroups;
   } cs;
};

enum aco_compiler_debug_level {
   ACO_COMPILER_DEBUG_LEVEL_ERROR,
};

struct aco_compiler_options {
   const struct ac_compiler_info* compiler_info;
   bool dump_ir;
   bool dump_preoptir;
   bool record_asm;
   bool record_ir;
   bool record_stats;
   bool optimisations_disabled;
   uint8_t enable_mrt_output_nan_fixup;
   bool wgp_mode;
   bool is_opengl;
   enum radeon_family family;
   /* The ISA to emit. On Xclipse 920 this is GFX11 while the hardware is RDNA2, so it does not by
    * itself imply any RDNA3 hardware capability; those get their own flags below. */
   enum amd_gfx_level gfx_level;
   /* Dual-issue VALU (v_dual_*): RDNA3 hardware only; an RDNA2 core decoding GFX11 faults on it. */
   bool has_vopd;
   /* Do not emit s_delay_alu: an RDNA3 hardware feature. The vendor compiler emits none here. */
   bool no_delay_alu;
   /* No v_permlane64_b32 (RDNA3 hardware): on the Xclipse it reads 0, so a wave64 shuffle or
    * rotate across the two 32-lane halves returned 0 (sgprobe 64; flicker in Eden). Cross-half
    * wave64 permutes use the per-lane readlane emulation instead. */
   bool no_permlane64;
   /* LDS_PARAM_LOAD waits for every outstanding VALU (wait_vdst 0), as the vendor compiler does.
    * An older VALU write to the load's destination VGPR can land after the load even when the
    * GFX11 va_vdst count has passed, and the fragment input then reads the stale value
    * (paulowesll/mesa a0191be5f8: a flat input returned a v_min3_f32 result issued 7 VALUs
    * earlier). */
   bool ldsdir_valu_drain;
   /* Global/scratch immediate offsets are 12-bit signed (-2048..2047) as on RDNA2, although the
    * GFX11 encoding has a 13-bit field. Measured on the Xclipse 920 (probe/gfx/rt/goffprobe): a
    * global access at +2800 or +4092 lands 4 KiB low. */
   bool gfx10_global_offsets;
   /* Swizzled buffer access without IDXEN, as on GFX10_3, although the ISA is GFX11 (where ACO
    * sets IDXEN for it). With IDXEN the Xclipse does not add the lane (ADD_TID), so every lane of a
    * legacy GS wrote lane 0's GSVS slot (transform feedback on the 920). The vendor's GSVS stores
    * use neither IDXEN nor OFFEN. */
   bool swizzle_no_idxen;
   /* Device-scope loads also set DLC. The ISA is GFX11, where GLC alone means device scope, but
    * the Xclipse caches keep GFX10_3's scopes: GLC alone is shader-array scope, and only GLC + DLC
    * bypasses GL1. With more than one shader array (940: 2, 950: 4) a wave spinning on another
    * array's write never saw it (the BVH build hung from 512 triangles on the 950). */
   bool device_scope_dlc;
   /* Load FLAT_SCRATCH in the shader from the scratch ring address in s[0:1] and the wave's
    * scratch offset, as on GFX10.3, although the ISA is GFX11 (where the hardware sets the base
    * from COMPUTE_DISPATCH_SCRATCH_BASE). The Xclipse's command processor is GFX10.3 and never
    * does, so without this every scratch access faults until the kernel's 5 s job timeout
    * (probe/gfx/rt/scratchprobe). */
   bool gfx10_scratch_init;
   uint32_t address32_hi;
};

enum aco_statistic {
   aco_statistic_hash,
   aco_statistic_instructions,
   aco_statistic_copies,
   aco_statistic_branches,
   aco_statistic_latency,
   aco_statistic_inv_throughput,
   aco_statistic_vmem_clauses,
   aco_statistic_smem_clauses,
   aco_statistic_sgpr_presched,
   aco_statistic_vgpr_presched,
   aco_statistic_valu,
   aco_statistic_salu,
   aco_statistic_vmem,
   aco_statistic_smem,
   aco_statistic_vopd,
   aco_num_statistics
};

enum aco_symbol_id {
   aco_symbol_invalid,
   aco_symbol_scratch_addr_lo,
   aco_symbol_scratch_addr_hi,
   aco_symbol_const_data_addr,
};

struct aco_symbol {
   enum aco_symbol_id id;
   unsigned offset;
};

#define MAX_SGPRS       108
#define MAX_VGPRS       256
#define MAX_LDS_SIZE    65536 /* 64 KiB */
#define NUM_SAVED_VGPRS 2

struct aco_trap_handler_layout {
   uint32_t saved_vgprs[NUM_SAVED_VGPRS * 64];

   uint32_t ttmp0;
   uint32_t ttmp1;

   union {
      struct {
         uint32_t state_priv;
         uint32_t mode;
         uint32_t status;
         uint32_t gpr_alloc;
         uint32_t lds_alloc;
         uint32_t ib_sts;
         uint32_t excp_flag_priv;
         uint32_t excp_flag_user;
         uint32_t trap_ctrl;
         uint32_t hw_id1;
      } gfx12;

      struct {
         uint32_t status;
         uint32_t mode;
         uint32_t trap_sts;
         uint32_t hw_id1;
         uint32_t gpr_alloc;
         uint32_t lds_alloc;
         uint32_t ib_sts;
         uint32_t reserved0;
         uint32_t reserved1;
         uint32_t reserved2;
      } gfx8;
   } sq_wave_regs;

   uint32_t m0;
   uint32_t exec_lo;
   uint32_t exec_hi;
   uint32_t sgprs[MAX_SGPRS];
   uint32_t vgprs[MAX_VGPRS * 64];
   uint32_t lds[MAX_LDS_SIZE / 4];
};

#ifdef __cplusplus
}
#endif
#endif
