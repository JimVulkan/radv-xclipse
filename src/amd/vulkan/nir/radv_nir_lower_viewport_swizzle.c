/*
 * Copyright 2026 JimVulkan
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * VK_NV_viewport_swizzle for AMD hardware.
 *
 * The clip coordinate components are swapped/negated per viewport before clipping. AMD's PA has no
 * such block, so every gl_Position store in the last VGT stage is rewritten; clipping, culling and
 * the viewport transform see the result. Clip/cull distances are unaffected, per the spec.
 *
 * Per-viewport swizzles use the gl_ViewportIndex store when it dominates the position store, else
 * viewport 0's swizzle; the pass reports that case through *incomplete.
 */

#include <string.h>
#include "nir.h"
#include "nir_builder.h"
#include "radv_nir.h"

/* VkViewportCoordinateSwizzleNV: value >> 1 selects x/y/z/w, value & 1 negates. */
#define SWIZZLE_CHAN(v)   ((v) >> 1)
#define SWIZZLE_NEGATE(v) ((v) & 1)

static bool
swizzle_is_identity(const uint8_t sw[4])
{
   return sw[0] == 0 && sw[1] == 2 && sw[2] == 4 && sw[3] == 6;
}

bool
radv_nir_viewport_swizzle_is_identity(const uint8_t (*swizzles)[4], unsigned count)
{
   for (unsigned i = 0; i < count; i++) {
      if (!swizzle_is_identity(swizzles[i]))
         return false;
   }
   return true;
}

static bool
swizzles_are_uniform(const uint8_t (*swizzles)[4], unsigned count)
{
   for (unsigned i = 1; i < count; i++) {
      if (memcmp(swizzles[i], swizzles[0], 4))
         return false;
   }
   return true;
}

static nir_def *
apply_swizzle(nir_builder *b, nir_def *pos, const uint8_t sw[4])
{
   nir_def *comps[4];

   for (unsigned i = 0; i < 4; i++) {
      nir_def *c = nir_channel(b, pos, SWIZZLE_CHAN(sw[i]));
      comps[i] = SWIZZLE_NEGATE(sw[i]) ? nir_fneg(b, c) : c;
   }

   return nir_vec(b, comps, 4);
}

static bool
is_output_store(const nir_intrinsic_instr *intr, gl_varying_slot slot)
{
   if (intr->intrinsic != nir_intrinsic_store_deref)
      return false;

   const nir_variable *var = nir_intrinsic_get_var((nir_intrinsic_instr *)intr, 0);
   return var && var->data.mode == nir_var_shader_out && var->data.location == slot;
}

/* True if the value stored by `def_instr` is usable at `use_instr`. Requires
 * dominance metadata and instruction indices to be up to date.
 */
static bool
store_dominates(const nir_instr *def_instr, const nir_instr *use_instr)
{
   if (def_instr->block == use_instr->block)
      return def_instr->index < use_instr->index;

   return nir_block_dominates(def_instr->block, use_instr->block);
}

struct lower_state {
   const uint8_t (*swizzles)[4];
   unsigned count;
   bool uniform;
   /* The single store to gl_ViewportIndex, if there is exactly one. */
   nir_intrinsic_instr *vp_store;
   bool fell_back;
};

static nir_def *
build_swizzled_pos(nir_builder *b, struct lower_state *s, nir_def *pos, nir_instr *pos_store)
{
   if (s->uniform)
      return apply_swizzle(b, pos, s->swizzles[0]);

   /* Per-viewport swizzles: select on the viewport index the shader wrote. */
   if (s->vp_store && store_dominates(&s->vp_store->instr, pos_store)) {
      nir_def *vp_idx = s->vp_store->src[1].ssa;

      /* gl_ViewportIndex is a scalar int output; a partial/vector store here
       * would mean something unexpected, so be conservative.
       */
      if (vp_idx->num_components == 1 && vp_idx->bit_size == 32) {
         nir_def *acc = apply_swizzle(b, pos, s->swizzles[0]);

         for (unsigned i = 1; i < s->count; i++) {
            if (!memcmp(s->swizzles[i], s->swizzles[0], 4))
               continue;
            acc = nir_bcsel(b, nir_ieq_imm(b, vp_idx, i), apply_swizzle(b, pos, s->swizzles[i]), acc);
         }

         return acc;
      }
   }

   /* No usable viewport index. Viewport 0's swizzle is the best guess. */
   s->fell_back = true;
   return apply_swizzle(b, pos, s->swizzles[0]);
}

bool
radv_nir_lower_viewport_swizzle(nir_shader *nir, const uint8_t (*swizzles)[4], unsigned count, bool *incomplete)
{
   if (incomplete)
      *incomplete = false;

   if (!count || radv_nir_viewport_swizzle_is_identity(swizzles, count))
      return false;

   /* nir_lower_io can't see through an array deref of a vector, and neither can
    * we -- flatten first so every gl_Position store is a whole vec4.
    */
   NIR_PASS(_, nir, nir_lower_array_deref_of_vec, nir_var_shader_out, NULL,
            nir_lower_direct_array_deref_of_vec_load | nir_lower_indirect_array_deref_of_vec_load |
               nir_lower_direct_array_deref_of_vec_store | nir_lower_indirect_array_deref_of_vec_store);

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   struct lower_state s = {
      .swizzles = swizzles,
      .count = count,
      .uniform = swizzles_are_uniform(swizzles, count),
   };

   nir_metadata_require(impl, nir_metadata_dominance);
   nir_index_instrs(impl);

   /* Locate the viewport index store. More than one and we can't tell which
    * value is live at a given position store, so don't guess.
    */
   if (!s.uniform) {
      unsigned num_vp_stores = 0;

      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (is_output_store(intr, VARYING_SLOT_VIEWPORT)) {
               s.vp_store = intr;
               num_vp_stores++;
            }
         }
      }

      if (num_vp_stores != 1)
         s.vp_store = NULL;

      /* Mesh shaders write per-primitive outputs into arrays, so a dominating
       * store tells us nothing about which primitive this position belongs to.
       */
      if (nir->info.stage == MESA_SHADER_MESH)
         s.vp_store = NULL;
   }

   nir_builder b = nir_builder_create(impl);
   bool progress = false;

   nir_foreach_block (block, impl) {
      nir_foreach_instr_safe (instr, block) {
         if (instr->type != nir_instr_type_intrinsic)
            continue;

         nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
         if (!is_output_store(intr, VARYING_SLOT_POS))
            continue;

         nir_def *pos = intr->src[1].ssa;
         if (pos->num_components != 4) {
            /* A partial write leaves components we can't source from. */
            s.fell_back = true;
            continue;
         }

         b.cursor = nir_before_instr(instr);
         nir_src_rewrite(&intr->src[1], build_swizzled_pos(&b, &s, pos, instr));
         progress = true;
      }
   }

   if (incomplete)
      *incomplete = s.fell_back;

   if (progress) {
      nir_progress(true, impl, nir_metadata_control_flow);
   } else {
      nir_no_progress(impl);
   }

   return progress;
}
