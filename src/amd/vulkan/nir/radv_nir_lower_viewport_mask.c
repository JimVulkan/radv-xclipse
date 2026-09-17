/*
 * VK_NV_viewport_array2: lower gl_ViewportMask[] to gl_ViewportIndex.
 *
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 *
 * Writing ViewportIndex/Layer from vertex and tessellation stages already works in RADV. This pass
 * handles ViewportMaskNV, which AMD does not consume (the output was silently dropped). Maxwell
 * recompilers emit it even for a single viewport, so single-bit masks are the common case.
 *
 *   one bit  -> exact: gl_ViewportIndex = find_lsb(mask), constant or dynamic
 *   mask 0   -> should draw nothing; not handled
 *   2+ bits  -> should broadcast; needs primitive amplification, not implemented. Renders to the
 *               lowest set viewport, with a one-shot warning when provable statically.
 */

#include "nir.h"
#include "nir_builder.h"
#include "radv_nir.h"

struct lower_state {
   nir_variable *viewport; /* gl_ViewportIndex output; created on demand */
   bool multi_bit_seen;    /* a provably >1-bit constant mask was lowered approximately */
   bool progress;
};

static nir_variable *
get_viewport_output(nir_shader *nir, struct lower_state *s)
{
   if (s->viewport)
      return s->viewport;

   /* Writing both gl_ViewportIndex and gl_ViewportMask is ill-formed; reuse an existing index
    * variable (the last store wins). */
   s->viewport = nir_find_variable_with_location(nir, nir_var_shader_out, VARYING_SLOT_VIEWPORT);
   if (!s->viewport) {
      s->viewport = nir_variable_create(nir, nir_var_shader_out, glsl_int_type(), "gl_ViewportIndex");
      s->viewport->data.location = VARYING_SLOT_VIEWPORT;
      s->viewport->data.interpolation = INTERP_MODE_FLAT;
   }
   return s->viewport;
}

static bool
deref_is_viewport_mask(nir_deref_instr *deref)
{
   nir_variable *var = nir_deref_instr_get_variable(deref);
   return var && var->data.mode == nir_var_shader_out && var->data.location == VARYING_SLOT_VIEWPORT_MASK;
}

/* gl_ViewportMask is sized ceil(maxViewports/32); with 16 viewports only element 0 matters. Other
 * elements are skipped explicitly rather than folded into element 0. */
static bool
mask_element_is_zero(nir_deref_instr *deref)
{
   if (deref->deref_type != nir_deref_type_array)
      return true; /* scalar/whole-variable store */

   if (!nir_src_is_const(deref->arr.index))
      return false;

   return nir_src_as_uint(deref->arr.index) == 0;
}

static bool
lower_instr(nir_builder *b, nir_intrinsic_instr *intr, void *data)
{
   struct lower_state *s = data;

   if (intr->intrinsic != nir_intrinsic_store_deref)
      return false;

   nir_deref_instr *deref = nir_src_as_deref(intr->src[0]);
   if (!deref || !deref_is_viewport_mask(deref))
      return false;

   if (!mask_element_is_zero(deref))
      return false;

   nir_def *mask = intr->src[1].ssa;

   /* Static check, only to warn about the multi-bit approximation. */
   if (nir_src_is_const(intr->src[1])) {
      uint64_t v = nir_src_as_uint(intr->src[1]);
      if (util_bitcount64(v) > 1)
         s->multi_bit_seen = true;
   }

   b->cursor = nir_before_instr(&intr->instr);

   /* find_lsb(mask): exact for single-bit masks, lowest viewport otherwise; -1 for 0. */
   nir_def *index = nir_find_lsb(b, mask);

   nir_store_var(b, get_viewport_output(b->shader, s), index, 0x1);

   nir_instr_remove(&intr->instr);
   s->progress = true;
   return true;
}

bool
radv_nir_lower_viewport_mask(nir_shader *nir, bool *multi_bit)
{
   if (multi_bit)
      *multi_bit = false;

   if (!(nir->info.outputs_written & VARYING_BIT_VIEWPORT_MASK))
      return false;

   struct lower_state s = {0};

   nir_shader_intrinsics_pass(nir, lower_instr, nir_metadata_control_flow, &s);

   if (!s.progress)
      return false;

   /* Update outputs_written: skip_viewport_state_culling keys on VARYING_BIT_VIEWPORT_MASK and the
    * export path on VARYING_BIT_VIEWPORT. */
   nir->info.outputs_written &= ~VARYING_BIT_VIEWPORT_MASK;
   nir->info.outputs_written |= VARYING_BIT_VIEWPORT;

   if (multi_bit)
      *multi_bit = s.multi_bit_seen;

   return true;
}
