/*
 * Copyright © 2023 Valve Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef RADV_NIR_H
#define RADV_NIR_H

#include <stdbool.h>
#include <stdint.h>
#include "amd_family.h"
#include "nir_defines.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nir_shader nir_shader;
struct radv_shader_stage;
struct radv_shader_info;
struct radv_shader_args;
struct radv_shader_layout;
struct radv_graphics_state_key;
struct radv_ps_epilog_key;
struct radv_debug_nir;
struct radv_compiler_info;
struct vk_sampler_state_array;

bool radv_nir_lower_descriptors(nir_shader *shader, const struct radv_compiler_info *compiler_info,
                                const struct radv_shader_stage *stage);

bool radv_nir_lower_abi(nir_shader *shader, enum amd_gfx_level gfx_level, const struct radv_shader_stage *stage,
                        const struct radv_graphics_state_key *gfx_state, uint32_t address32_hi);

bool radv_nir_lower_hit_attrib_derefs(nir_shader *shader);

bool radv_nir_lower_ray_payload_derefs(nir_shader *shader, uint32_t offset);

bool radv_nir_lower_ray_queries(nir_shader *shader, const struct radv_compiler_info *compiler_info);

bool radv_nir_lower_vs_inputs(nir_shader *shader, const struct radv_compiler_info *compiler_info,
                              const struct radv_shader_stage *vs_stage,
                              const struct radv_graphics_state_key *gfx_state);

bool radv_nir_optimize_vs_inputs_to_const(nir_shader *shader, const struct radv_graphics_state_key *gfx_state);

bool radv_nir_lower_primitive_shading_rate(nir_shader *nir, enum amd_gfx_level gfx_level);

/* VK_NV_viewport_swizzle. `swizzles` holds VkViewportCoordinateSwizzleNV values, 4 per viewport.
 * `incomplete` is set when a position store had to fall back to viewport 0's swizzle.
 */
bool radv_nir_lower_viewport_swizzle(nir_shader *nir, const uint8_t (*swizzles)[4], unsigned count,
                                     bool *incomplete);

bool radv_nir_viewport_swizzle_is_identity(const uint8_t (*swizzles)[4], unsigned count);

/* VK_NV_viewport_array2: lower gl_ViewportMask[] to gl_ViewportIndex. Exact for single-bit masks
 * (the Maxwell-native case); sets *multi_bit when a provably multi-bit constant mask was lowered
 * approximately, because real broadcast needs primitive amplification we do not implement. */
bool radv_nir_lower_viewport_mask(nir_shader *nir, bool *multi_bit);

/* VK_NV_geometry_shader_passthrough: no AMD hardware has a passthrough geometry stage, so rebuild
 * the equivalent ordinary GS (copy each passthrough input to its output per vertex, then
 * EmitVertex/EndPrimitive). Adjacency primitives emit only the non-adjacent vertices. */
bool radv_nir_lower_passthrough_gs(nir_shader *nir);

bool radv_nir_lower_fs_intrinsics(nir_shader *nir, const struct radv_shader_stage *fs_stage,
                                  const struct radv_graphics_state_key *gfx_state);

bool radv_nir_lower_fs_input_attachment(nir_shader *nir);

bool radv_nir_lower_fs_barycentric(nir_shader *shader, const struct radv_graphics_state_key *gfx_state,
                                   unsigned num_raster_vertices_per_prim);

bool radv_nir_lower_intrinsics_early(nir_shader *nir, bool lower_view_index_to_zero);

bool radv_nir_export_multiview(nir_shader *nir);

unsigned radv_map_io_driver_location(unsigned semantic);

void radv_nir_lower_io(nir_shader *nir);

bool radv_nir_lower_io_to_mem(const struct radv_compiler_info *compiler_info, struct radv_shader_stage *stage);

bool radv_nir_lower_cooperative_matrix(nir_shader *shader, enum amd_gfx_level gfx_level, unsigned wave_size);

bool radv_nir_opt_cooperative_matrix(nir_shader *shader, enum amd_gfx_level gfx_level);

bool radv_nir_lower_draw_id_to_zero(nir_shader *shader);

bool radv_nir_remap_color_attachment(nir_shader *shader, const struct radv_graphics_state_key *gfx_state);

bool radv_nir_trim_fs_color_exports(nir_shader *shader, const struct radv_ps_epilog_key *epilog_key,
                                    bool mrt0_alpha_is_dead);

bool radv_nir_lower_printf(nir_shader *shader, struct radv_debug_nir *debug_nir);

typedef struct radv_nir_opt_tid_function_options {
   bool use_masked_swizzle_amd : 1;
   bool use_dpp16_shift_amd : 1;
   bool use_shuffle_xor : 1;
   bool use_quad_swap_broadcast : 1;
   bool use_clustered_rotate : 1;
   bool use_permute16_amd : 1;
   bool use_dpp8_swizzle_amd : 1;
   /* These can be smaller than the api ballot size
    * if some invocations are always inactive.
    */
   uint8_t hw_ballot_bit_size;
   uint8_t hw_ballot_num_comp;
} radv_nir_opt_tid_function_options;

bool radv_nir_opt_tid_function(nir_shader *shader, const radv_nir_opt_tid_function_options *options);

bool radv_nir_opt_fs_builtins(nir_shader *shader, const struct radv_graphics_state_key *gfx_state,
                              unsigned num_raster_vertices_per_prim);

bool radv_nir_lower_opt_fs_frag_pos(nir_shader *shader, bool vrs_may_be_enabled, bool sample_shading);

bool radv_nir_lower_immediate_samplers(nir_shader *shader, const struct radv_compiler_info *compiler_info,
                                       const struct radv_shader_stage *stage,
                                       const struct vk_sampler_state_array *embedded_samplers);

void radv_nir_lower_callee_signature(nir_function *function);

bool radv_nir_lower_call_abi(nir_shader *shader, unsigned wave_size);

#ifdef __cplusplus
}
#endif

#endif /* RADV_NIR_H */
