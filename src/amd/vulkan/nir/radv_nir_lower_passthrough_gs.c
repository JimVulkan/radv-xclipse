/*
 * VK_NV_geometry_shader_passthrough: rebuild a passthrough GS as an ordinary one.
 *
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 *
 * A passthrough GS runs once per primitive with no EmitVertex/EndPrimitive: passthrough inputs are
 * copied to their outputs for each vertex, and the body only computes per-primitive outputs
 * (gl_Layer, gl_ViewportMask, gl_ViewportIndex). AMD has no passthrough mode, so append after the
 * body:
 *
 *     for each vertex v of the primitive:
 *         out_x = in_x[v]   (for every passthrough input)
 *         EmitVertex()
 *     EndPrimitive()
 *
 * Adjacency primitives emit only the non-adjacent vertices:
 *
 *     lines_adjacency     (4 in) -> vertices 1,2   as a line       (0 and 3 are the neighbours)
 *     triangles_adjacency (6 in) -> vertices 0,2,4 as a triangle   (1,3,5 are the neighbours)
 */

#include "nir.h"
#include "nir_builder.h"
#include "radv_nir.h"

struct pt_prim {
   unsigned count;
   unsigned idx[3];
   enum mesa_prim out_prim;
};

/* Vertex selection per input primitive. See the adjacency warning above. */
static bool
passthrough_prim_info(enum mesa_prim in_prim, struct pt_prim *out)
{
   switch (in_prim) {
   case MESA_PRIM_POINTS:
      *out = (struct pt_prim){1, {0}, MESA_PRIM_POINTS};
      return true;
   case MESA_PRIM_LINES:
      *out = (struct pt_prim){2, {0, 1}, MESA_PRIM_LINE_STRIP};
      return true;
   case MESA_PRIM_LINES_ADJACENCY:
      *out = (struct pt_prim){2, {1, 2}, MESA_PRIM_LINE_STRIP};
      return true;
   case MESA_PRIM_TRIANGLES:
      *out = (struct pt_prim){3, {0, 1, 2}, MESA_PRIM_TRIANGLE_STRIP};
      return true;
   case MESA_PRIM_TRIANGLES_ADJACENCY:
      *out = (struct pt_prim){3, {0, 2, 4}, MESA_PRIM_TRIANGLE_STRIP};
      return true;
   default:
      return false;
   }
}

#define PT_MAX_VARS 32

bool
radv_nir_lower_passthrough_gs(nir_shader *nir)
{
   if (nir->info.stage != MESA_SHADER_GEOMETRY)
      return false;

   nir_variable *in_vars[PT_MAX_VARS];
   unsigned n_vars = 0;
   bool overflow = false;

   nir_foreach_shader_in_variable (var, nir) {
      if (!var->data.passthrough)
         continue;
      if (n_vars < PT_MAX_VARS)
         in_vars[n_vars++] = var;
      else
         overflow = true;
   }

   if (!n_vars)
      return false;

   /* Refuse rather than silently drop varyings. */
   if (overflow) {
      fprintf(stderr, "radv: VK_NV_geometry_shader_passthrough - more than %d passthrough inputs; "
                      "not lowering.\n",
              PT_MAX_VARS);
      return false;
   }

   struct pt_prim prim;
   if (!passthrough_prim_info(nir->info.gs.input_primitive, &prim))
      return false;

   nir_function_impl *impl = nir_shader_get_entrypoint(nir);
   nir_builder b = nir_builder_at(nir_after_impl(impl));

   /* One output per passthrough input. A GS input is arrayed by vertex, so the output is the element
    * type. */
   nir_variable *out_vars[PT_MAX_VARS];
   for (unsigned i = 0; i < n_vars; i++) {
      nir_variable *in = in_vars[i];
      const struct glsl_type *out_type;

      /* Passthrough inputs may be declared non-arrayed; re-type them into arrays, since the
       * synthesised GS reads each vertex. Don't detect this with glsl_get_array_element(): it
       * returns FLOAT for a vec4. */
      if (glsl_type_is_array(in->type)) {
         out_type = glsl_get_array_element(in->type);
      } else {
         out_type = in->type;
         in->type = glsl_array_type(in->type, nir->info.gs.vertices_in, 0);
      }

      nir_variable *out = nir_variable_create(nir, nir_var_shader_out, out_type, in->name);
      out->data.location = in->data.location;
      out->data.location_frac = in->data.location_frac;
      out->data.interpolation = in->data.interpolation;
      out->data.driver_location = 0;
      out_vars[i] = out;

      nir->info.outputs_written |= BITFIELD64_BIT(in->data.location);
   }

   /* Fully unrolled: 1..3 vertices, and adjacency indices (0,2,4) need the table anyway. */
   for (unsigned v = 0; v < prim.count; v++) {
      for (unsigned i = 0; i < n_vars; i++) {
         nir_deref_instr *in_arr = nir_build_deref_var(&b, in_vars[i]);
         nir_deref_instr *in_elem = nir_build_deref_array_imm(&b, in_arr, prim.idx[v]);
         nir_deref_instr *out_deref = nir_build_deref_var(&b, out_vars[i]);
         nir_copy_deref(&b, out_deref, in_elem);
      }
      nir_emit_vertex(&b, .stream_id = 0);
   }
   nir_end_primitive(&b, .stream_id = 0);

   nir->info.gs.vertices_out = prim.count;
   nir->info.gs.output_primitive = prim.out_prim;
   if (nir->info.gs.invocations == 0)
      nir->info.gs.invocations = 1;
   nir->info.gs.active_stream_mask |= 1;

   /* Instructions and variables were added, so no metadata survives. */
   return nir_progress(true, impl, nir_metadata_none);
}
