/*
 * Copyright 2023 Advanced Micro Devices, Inc.
 *
 * SPDX-License-Identifier: MIT
 */

#include "ac_nir.h"
#include "sid.h"
#include "nir_builder.h"
#include "nir_builtin_builder.h"

typedef struct {
   const ac_nir_lower_ps_options *options;

   nir_variable *persp_center;
   nir_variable *persp_centroid;
   nir_variable *persp_sample;
   nir_variable *linear_center;
   nir_variable *linear_centroid;
   nir_variable *linear_sample;
   bool lower_load_barycentric;

   nir_def *color[MAX_DRAW_BUFFERS][4];
   nir_def *depth;
   nir_def *stencil;
   nir_def *sample_mask;

   uint8_t colors_written;
   nir_alu_type color_type[MAX_DRAW_BUFFERS];
   bool has_dual_src_blending;

   /* MAX_DRAW_BUFFERS for MRT export, 1 for MRTZ export */
   nir_intrinsic_instr *exp[MAX_DRAW_BUFFERS + 1];
   unsigned exp_num;

   unsigned compacted_mrt_index;
   unsigned spi_shader_col_format;
} lower_ps_state;

static void
create_interp_param(nir_builder *b, lower_ps_state *s)
{
   if (s->options->force_persp_sample_interp) {
      s->persp_center =
         nir_local_variable_create(b->impl, glsl_vec_type(2), "persp_center");
   }

   if (s->options->bc_optimize_for_persp ||
       s->options->force_persp_sample_interp ||
       s->options->force_persp_center_interp) {
      s->persp_centroid =
         nir_local_variable_create(b->impl, glsl_vec_type(2), "persp_centroid");
   }

   if (s->options->force_persp_center_interp) {
      s->persp_sample =
         nir_local_variable_create(b->impl, glsl_vec_type(2), "persp_sample");
   }

   if (s->options->force_linear_sample_interp) {
      s->linear_center =
         nir_local_variable_create(b->impl, glsl_vec_type(2), "linear_center");
   }

   if (s->options->bc_optimize_for_linear ||
       s->options->force_linear_sample_interp ||
       s->options->force_linear_center_interp) {
      s->linear_centroid =
         nir_local_variable_create(b->impl, glsl_vec_type(2), "linear_centroid");
   }

   if (s->options->force_linear_center_interp) {
      s->linear_sample =
         nir_local_variable_create(b->impl, glsl_vec_type(2), "linear_sample");
   }

   s->lower_load_barycentric =
      s->persp_center || s->persp_centroid || s->persp_sample ||
      s->linear_center || s->linear_centroid || s->linear_sample;
}

static void
init_interp_param(nir_builder *b, lower_ps_state *s)
{
   b->cursor = nir_before_cf_list(&b->impl->body);

   /* The shader should do: if (PRIM_MASK[31]) CENTROID = CENTER;
    * The hw doesn't compute CENTROID if the whole wave only
    * contains fully-covered quads.
    */
   if (s->options->bc_optimize_for_persp || s->options->bc_optimize_for_linear) {
      nir_def *bc_optimize = nir_load_barycentric_optimize_amd(b);

      if (s->options->bc_optimize_for_persp) {
         nir_def *center =
            nir_load_barycentric_pixel(b, 32, .interp_mode = INTERP_MODE_SMOOTH);
         nir_def *centroid =
            nir_load_barycentric_centroid(b, 32, .interp_mode = INTERP_MODE_SMOOTH);

         nir_def *value = nir_bcsel(b, bc_optimize, center, centroid);
         nir_store_var(b, s->persp_centroid, value, 0x3);
      }

      if (s->options->bc_optimize_for_linear) {
         nir_def *center =
            nir_load_barycentric_pixel(b, 32, .interp_mode = INTERP_MODE_NOPERSPECTIVE);
         nir_def *centroid =
            nir_load_barycentric_centroid(b, 32, .interp_mode = INTERP_MODE_NOPERSPECTIVE);

         nir_def *value = nir_bcsel(b, bc_optimize, center, centroid);
         nir_store_var(b, s->linear_centroid, value, 0x3);
      }
   }

   if (s->options->force_persp_sample_interp) {
      nir_def *sample =
         nir_load_barycentric_sample(b, 32, .interp_mode = INTERP_MODE_SMOOTH);
      nir_store_var(b, s->persp_center, sample, 0x3);
      nir_store_var(b, s->persp_centroid, sample, 0x3);
   }

   if (s->options->force_linear_sample_interp) {
      nir_def *sample =
         nir_load_barycentric_sample(b, 32, .interp_mode = INTERP_MODE_NOPERSPECTIVE);
      nir_store_var(b, s->linear_center, sample, 0x3);
      nir_store_var(b, s->linear_centroid, sample, 0x3);
   }

   if (s->options->force_persp_center_interp) {
      nir_def *center =
         nir_load_barycentric_pixel(b, 32, .interp_mode = INTERP_MODE_SMOOTH);
      nir_store_var(b, s->persp_sample, center, 0x3);
      nir_store_var(b, s->persp_centroid, center, 0x3);
   }

   if (s->options->force_linear_center_interp) {
      nir_def *center =
         nir_load_barycentric_pixel(b, 32, .interp_mode = INTERP_MODE_NOPERSPECTIVE);
      nir_store_var(b, s->linear_sample, center, 0x3);
      nir_store_var(b, s->linear_centroid, center, 0x3);
   }
}

static bool
lower_ps_load_barycentric(nir_builder *b, nir_intrinsic_instr *intrin, lower_ps_state *s)
{
   enum glsl_interp_mode mode = nir_intrinsic_interp_mode(intrin);
   nir_variable *var = NULL;

   switch (mode) {
   case INTERP_MODE_NONE:
   case INTERP_MODE_SMOOTH:
      switch (intrin->intrinsic) {
      case nir_intrinsic_load_barycentric_pixel:
         var = s->persp_center;
         break;
      case nir_intrinsic_load_barycentric_centroid:
         var = s->persp_centroid;
         break;
      case nir_intrinsic_load_barycentric_sample:
         var = s->persp_sample;
         break;
      default:
         break;
      }
      break;

   case INTERP_MODE_NOPERSPECTIVE:
      switch (intrin->intrinsic) {
      case nir_intrinsic_load_barycentric_pixel:
         var = s->linear_center;
         break;
      case nir_intrinsic_load_barycentric_centroid:
         var = s->linear_centroid;
         break;
      case nir_intrinsic_load_barycentric_sample:
         var = s->linear_sample;
         break;
      default:
         break;
      }
      break;

   default:
      break;
   }

   if (!var)
      return false;

   b->cursor = nir_before_instr(&intrin->instr);

   nir_def *replacement = nir_load_var(b, var);
   nir_def_replace(&intrin->def, replacement);
   return true;
}

static bool
gather_ps_store_output(nir_builder *b, nir_intrinsic_instr *intrin, lower_ps_state *s)
{
   unsigned slot = nir_intrinsic_io_semantics(intrin).location;
   unsigned dual_src_blend_index = nir_intrinsic_io_semantics(intrin).dual_source_blend_index;
   unsigned write_mask = nir_intrinsic_write_mask(intrin);
   unsigned component = nir_intrinsic_component(intrin);
   unsigned color_index = (slot >= FRAG_RESULT_DATA0 ? slot - FRAG_RESULT_DATA0 : 0) +
                          dual_src_blend_index;
   nir_def *store_val = intrin->src[0].ssa;

   b->cursor = nir_before_instr(&intrin->instr);

   u_foreach_bit (i, write_mask) {
      nir_def *chan = nir_channel(b, store_val, i);
      unsigned comp = component + i;

      switch (slot) {
      case FRAG_RESULT_DEPTH:
         assert(comp == 0);
         s->depth = chan;
         break;
      case FRAG_RESULT_STENCIL:
         assert(comp == 0);
         s->stencil = chan;
         break;
      case FRAG_RESULT_SAMPLE_MASK:
         assert(comp == 0);
         if (!s->options->kill_samplemask)
            s->sample_mask = chan;
         break;
      case FRAG_RESULT_COLOR:
         s->color[color_index][comp] = chan;
         break;
      default:
         assert(slot >= FRAG_RESULT_DATA0 && slot <= FRAG_RESULT_DATA7);
         s->color[color_index][comp] = chan;
         break;
      }
   }

   if ((slot == FRAG_RESULT_COLOR || (slot >= FRAG_RESULT_DATA0 && slot <= FRAG_RESULT_DATA7)) &&
       write_mask) {
      s->colors_written |= BITFIELD_BIT(color_index);
      s->color_type[color_index] = nir_intrinsic_src_type(intrin);
      s->has_dual_src_blending |= dual_src_blend_index == 1;
   }

   /* Keep output instruction if not exported in nir. */
   if (!s->options->no_color_export && !s->options->no_depth_export) {
      nir_instr_remove(&intrin->instr);
   } else {
      if (slot >= FRAG_RESULT_DATA0 && !s->options->no_color_export) {
         nir_instr_remove(&intrin->instr);
      } else if ((slot == FRAG_RESULT_DEPTH || slot == FRAG_RESULT_STENCIL ||
                  slot == FRAG_RESULT_SAMPLE_MASK) && !s->options->no_depth_export) {
         nir_instr_remove(&intrin->instr);
      }
   }

   return true;
}

static bool
lower_ps_load_sample_mask_in(nir_builder *b, nir_intrinsic_instr *intrin, lower_ps_state *s)
{
   /* Section 15.2.2 (Shader Inputs) of the OpenGL 4.5 (Core Profile) spec
    * says:
    *
    *    "When per-sample shading is active due to the use of a fragment
    *     input qualified by sample or due to the use of the gl_SampleID
    *     or gl_SamplePosition variables, only the bit for the current
    *     sample is set in gl_SampleMaskIn. When state specifies multiple
    *     fragment shader invocations for a given fragment, the sample
    *     mask for any single fragment shader invocation may specify a
    *     subset of the covered samples for the fragment. In this case,
    *     the bit corresponding to each covered sample will be set in
    *     exactly one fragment shader invocation."
    *
    * The samplemask loaded by hardware is always the coverage of the
    * entire pixel/fragment, so mask bits out based on the sample ID.
    */

   b->cursor = nir_before_instr(&intrin->instr);

   uint32_t ps_iter_mask = ac_get_ps_iter_mask(s->options->ps_iter_samples);
   nir_def *sampleid = nir_load_sample_id(b);
   nir_def *submask = nir_ishl(b, nir_imm_int(b, ps_iter_mask), sampleid);

   nir_def *sample_mask = nir_load_sample_mask_in(b);
   nir_def *replacement = nir_iand(b, sample_mask, submask);

   nir_def_replace(&intrin->def, replacement);
   return true;
}

static bool
lower_ps_intrinsic(nir_builder *b, nir_instr *instr, void *state)
{
   lower_ps_state *s = (lower_ps_state *)state;

   if (instr->type != nir_instr_type_intrinsic)
      return false;

   nir_intrinsic_instr *intrin = nir_instr_as_intrinsic(instr);

   switch (intrin->intrinsic) {
   case nir_intrinsic_store_output:
      return gather_ps_store_output(b, intrin, s);
   case nir_intrinsic_load_barycentric_pixel:
   case nir_intrinsic_load_barycentric_centroid:
   case nir_intrinsic_load_barycentric_sample:
      if (s->lower_load_barycentric)
         return lower_ps_load_barycentric(b, intrin, s);
      break;
   case nir_intrinsic_load_sample_mask_in:
      if (s->options->ps_iter_samples > 1)
         return lower_ps_load_sample_mask_in(b, intrin, s);
      break;
   default:
      break;
   }

   return false;
}

static void
emit_ps_color_clamp_and_alpha_test(nir_builder *b, lower_ps_state *s)
{
   u_foreach_bit (slot, s->colors_written) {
      if (s->options->clamp_color) {
         for (int i = 0; i < 4; i++) {
            if (s->color[slot][i])
               s->color[slot][i] = nir_fsat(b, s->color[slot][i]);
         }
      }

      if (s->options->alpha_to_one)
         s->color[slot][3] = nir_imm_floatN_t(b, 1, nir_alu_type_get_type_size(s->color_type[slot]));

      if (slot == 0) {
         if (s->options->alpha_func == COMPARE_FUNC_ALWAYS) {
            /* always pass, do nothing */
         } else if (s->options->alpha_func == COMPARE_FUNC_NEVER) {
            nir_discard(b);
         } else if (s->color[slot][3]) {
            nir_def *ref = nir_load_alpha_reference_amd(b);
            nir_def *cond =
               nir_compare_func(b, s->options->alpha_func, s->color[slot][3], ref);
            nir_discard_if(b, nir_inot(b, cond));
         }
      }
   }
}

static void
emit_ps_mrtz_export(nir_builder *b, lower_ps_state *s)
{
   nir_def *mrtz_alpha = s->options->alpha_to_coverage_via_mrtz ? s->color[0][3] : NULL;

   /* skip mrtz export if no one has written to any of them */
   if (!s->depth && !s->stencil && !s->sample_mask && !mrtz_alpha)
      return;

   unsigned format =
      ac_get_spi_shader_z_format(s->depth, s->stencil, s->sample_mask,
                                 s->options->alpha_to_coverage_via_mrtz);

   nir_def *undef = nir_undef(b, 1, 32);
   nir_def *outputs[4] = {undef, undef, undef, undef};
   unsigned write_mask = 0;
   unsigned flags = 0;

   if (format == V_028710_SPI_SHADER_UINT16_ABGR) {
      assert(!s->depth && !mrtz_alpha);

      if (s->options->gfx_level < GFX11)
         flags |= AC_EXP_FLAG_COMPRESSED;

      if (s->stencil) {
         outputs[0] = nir_ishl_imm(b, s->stencil, 16);
         write_mask |= s->options->gfx_level >= GFX11 ? 0x1 : 0x3;
      }

      if (s->sample_mask) {
         outputs[1] = s->sample_mask;
         write_mask |= s->options->gfx_level >= GFX11 ? 0x2 : 0xc;
      }
   } else {
      if (s->depth) {
         outputs[0] = s->depth;
         write_mask |= 0x1;
      }

      if (s->stencil) {
         outputs[1] = s->stencil;
         write_mask |= 0x2;
      }

      if (s->sample_mask) {
         outputs[2] = s->sample_mask;
         write_mask |= 0x4;
      }

      if (mrtz_alpha) {
         outputs[3] = mrtz_alpha;
         write_mask |= 0x8;
      }
   }

   /* GFX6 (except OLAND and HAINAN) has a bug that it only looks at the
    * X writemask component.
    */
   if (s->options->gfx_level == GFX6 &&
       s->options->family != CHIP_OLAND &&
       s->options->family != CHIP_HAINAN) {
      write_mask |= 0x1;
   }

   s->exp[s->exp_num++] = nir_export_amd(b, nir_vec(b, outputs, 4),
                                         .base = V_008DFC_SQ_EXP_MRTZ,
                                         .write_mask = write_mask,
                                         .flags = flags);
}

static unsigned
get_ps_color_export_target(lower_ps_state *s)
{
   unsigned target = V_008DFC_SQ_EXP_MRT + s->compacted_mrt_index;

   if (s->options->dual_src_blend_swizzle && s->compacted_mrt_index < 2)
      target += 21;

   s->compacted_mrt_index++;

   return target;
}

static bool
emit_ps_color_export(nir_builder *b, lower_ps_state *s, unsigned output_index, unsigned mrt_index)
{
   assert(output_index < 8 && mrt_index < 8);

   unsigned spi_shader_col_format = (s->spi_shader_col_format >> (mrt_index * 4)) & 0xf;
   if (spi_shader_col_format == V_028714_SPI_SHADER_ZERO)
      return false;

   /* get target after checking spi_shader_col_format as we need to increase
    * compacted_mrt_index anyway regardless of whether the export is built
    */
   unsigned target = get_ps_color_export_target(s);

   /* no one has written to this slot */
   if (!(s->colors_written & BITFIELD_BIT(output_index)))
      return false;

   bool is_int8 = s->options->color_is_int8 & BITFIELD_BIT(mrt_index);
   bool is_int10 = s->options->color_is_int10 & BITFIELD_BIT(mrt_index);
   bool enable_mrt_output_nan_fixup =
      s->options->enable_mrt_output_nan_fixup & BITFIELD_BIT(mrt_index);

   nir_def *undef = nir_undef(b, 1, 32);
   nir_def *outputs[4] = {undef, undef, undef, undef};
   unsigned write_mask = 0;
   unsigned flags = 0;

   nir_alu_type type = s->color_type[output_index];
   nir_alu_type base_type = nir_alu_type_get_base_type(type);
   unsigned type_size = nir_alu_type_get_type_size(type);

   nir_def *data[4];
   memcpy(data, s->color[output_index], sizeof(data));

   /* Replace NaN by zero (for 32-bit float formats) to fix game bugs if requested. */
   if (enable_mrt_output_nan_fixup && type == nir_type_float32) {
      for (int i = 0; i < 4; i++) {
         if (data[i]) {
            nir_def *isnan = nir_fisnan(b, data[i]);
            data[i] = nir_bcsel(b, isnan, nir_imm_float(b, 0), data[i]);
         }
      }
   }

   switch (spi_shader_col_format) {
   case V_028714_SPI_SHADER_32_R:
      if (data[0]) {
         outputs[0] = nir_convert_to_bit_size(b, data[0], base_type, 32);
         write_mask = 0x1;
      }
      break;

   case V_028714_SPI_SHADER_32_GR:
      if (data[0]) {
         outputs[0] = nir_convert_to_bit_size(b, data[0], base_type, 32);
         write_mask |= 0x1;
      }

      if (data[1]) {
         outputs[1] = nir_convert_to_bit_size(b, data[1], base_type, 32);
         write_mask |= 0x2;
      }
      break;

   case V_028714_SPI_SHADER_32_AR:
      if (data[0]) {
         outputs[0] = nir_convert_to_bit_size(b, data[0], base_type, 32);
         write_mask |= 0x1;
      }

      if (data[3]) {
         unsigned index = s->options->gfx_level >= GFX10 ? 1 : 3;
         outputs[index] = nir_convert_to_bit_size(b, data[3], base_type, 32);
         write_mask |= BITFIELD_BIT(index);
      }
      break;

   case V_028714_SPI_SHADER_32_ABGR:
      for (int i = 0; i < 4; i++) {
         if (data[i]) {
            outputs[i] = nir_convert_to_bit_size(b, data[i], base_type, 32);
            write_mask |= BITFIELD_BIT(i);
         }
      }
      break;

   default: {
      nir_op pack_op = nir_op_pack_32_2x16;

      switch (spi_shader_col_format) {
      case V_028714_SPI_SHADER_FP16_ABGR:
         if (type_size == 32)
            pack_op = nir_op_pack_half_2x16_rtz_split;
         break;
      case V_028714_SPI_SHADER_UINT16_ABGR:
         if (type_size == 32) {
            pack_op = nir_op_pack_uint_2x16;
            if (is_int8 || is_int10) {
               /* clamp 32bit output for 8/10 bit color component */
               uint32_t max_rgb = is_int8 ? 255 : 1023;

               for (int i = 0; i < 4; i++) {
                  if (!data[i])
                     continue;

                  uint32_t max_value = i == 3 && is_int10 ? 3 : max_rgb;
                  data[i] = nir_umin(b, data[i], nir_imm_int(b, max_value));
               }
            }
         }
         break;
      case V_028714_SPI_SHADER_SINT16_ABGR:
         if (type_size == 32) {
            pack_op = nir_op_pack_sint_2x16;
            if (is_int8 || is_int10) {
               /* clamp 32bit output for 8/10 bit color component */
               uint32_t max_rgb = is_int8 ? 127 : 511;
               uint32_t min_rgb = is_int8 ? -128 : -512;

               for (int i = 0; i < 4; i++) {
                  if (!data[i])
                     continue;

                  uint32_t max_value = i == 3 && is_int10 ? 1 : max_rgb;
                  uint32_t min_value = i == 3 && is_int10 ? -2u : min_rgb;

                  data[i] = nir_imin(b, data[i], nir_imm_int(b, max_value));
                  data[i] = nir_imax(b, data[i], nir_imm_int(b, min_value));
               }
            }
         }
         break;
      case V_028714_SPI_SHADER_UNORM16_ABGR:
         pack_op = nir_op_pack_unorm_2x16;
         break;
      case V_028714_SPI_SHADER_SNORM16_ABGR:
         pack_op = nir_op_pack_snorm_2x16;
         break;
      default:
         unreachable("unsupported color export format");
         break;
      }

      for (int i = 0; i < 2; i++) {
         nir_def *lo = data[i * 2];
         nir_def *hi = data[i * 2 + 1];
         if (!lo && !hi)
            continue;

         lo = lo ? lo : nir_undef(b, 1, type_size);
         hi = hi ? hi : nir_undef(b, 1, type_size);

         if (nir_op_infos[pack_op].num_inputs == 2) {
            outputs[i] = nir_build_alu2(b, pack_op, lo, hi);
         } else {
            nir_def *vec = nir_vec2(b, lo, hi);
            outputs[i] = nir_build_alu1(b, pack_op, vec);
         }

         if (s->options->gfx_level >= GFX11)
            write_mask |= BITFIELD_BIT(i);
         else
            write_mask |= 0x3 << (i * 2);
      }

      if (s->options->gfx_level < GFX11)
         flags |= AC_EXP_FLAG_COMPRESSED;
   }
   }

   s->exp[s->exp_num++] = nir_export_amd(b, nir_vec(b, outputs, 4),
                                         .base = target,
                                         .write_mask = write_mask,
                                         .flags = flags);
   return true;
}

static void
emit_ps_dual_src_blend_swizzle(nir_builder *b, lower_ps_state *s, unsigned first_color_export)
{
   assert(s->exp_num > first_color_export + 1);

   nir_intrinsic_instr *mrt0_exp = s->exp[first_color_export];
   nir_intrinsic_instr *mrt1_exp = s->exp[first_color_export + 1];

   /* There are some instructions which operate mrt1_exp's argument
    * between mrt0_exp and mrt1_exp. Move mrt0_exp next to mrt1_exp,
    * so that we can swizzle their arguments.
    */
   unsigned target0 = nir_intrinsic_base(mrt0_exp);
   unsigned target1 = nir_intrinsic_base(mrt1_exp);
   if (target0 > target1) {
      /* mrt0 export is after mrt1 export, this happens when src0 is missing,
       * so we emit mrt1 first then emit an empty mrt0.
       *
       * swap the pointer
       */
      nir_intrinsic_instr *tmp = mrt0_exp;
      mrt0_exp = mrt1_exp;
      mrt1_exp = tmp;

      /* move mrt1_exp down to after mrt0_exp */
      nir_instr_move(nir_after_instr(&mrt0_exp->instr), &mrt1_exp->instr);
   } else {
      /* move mrt0_exp down to before mrt1_exp */
      nir_instr_move(nir_before_instr(&mrt1_exp->instr), &mrt0_exp->instr);
   }

   uint32_t mrt0_write_mask = nir_intrinsic_write_mask(mrt0_exp);
   uint32_t mrt1_write_mask = nir_intrinsic_write_mask(mrt1_exp);
   uint32_t write_mask = mrt0_write_mask & mrt1_write_mask;

   nir_def *mrt0_arg = mrt0_exp->src[0].ssa;
   nir_def *mrt1_arg = mrt1_exp->src[0].ssa;

   /* Swizzle code is right before mrt0_exp. */
   b->cursor = nir_before_instr(&mrt0_exp->instr);

   /* ACO need to emit the swizzle code by a pseudo instruction. */
   if (s->options->use_aco) {
      nir_export_dual_src_blend_amd(b, mrt0_arg, mrt1_arg, .write_mask = write_mask);
      nir_instr_remove(&mrt0_exp->instr);
      nir_instr_remove(&mrt1_exp->instr);
      return;
   }

   nir_def *undef = nir_undef(b, 1, 32);
   nir_def *arg0_vec[4] = {undef, undef, undef, undef};
   nir_def *arg1_vec[4] = {undef, undef, undef, undef};

   /* For illustration, originally
    *   lane0 export arg00 and arg01
    *   lane1 export arg10 and arg11.
    *
    * After the following operation
    *   lane0 export arg00 and arg10
    *   lane1 export arg01 and arg11.
    */
   u_foreach_bit (i, write_mask) {
      nir_def *arg0 = nir_channel(b, mrt0_arg, i);
      nir_def *arg1 = nir_channel(b, mrt1_arg, i);

      /* swap odd,even lanes of arg0 */
      arg0 = nir_quad_swizzle_amd(b, arg0, .swizzle_mask = 0b10110001, .fetch_inactive = true);

      /* swap even lanes between arg0 and arg1 */
      nir_def *tid = nir_load_subgroup_invocation(b);
      nir_def *is_even = nir_ieq_imm(b, nir_iand_imm(b, tid, 1), 0);

      nir_def *tmp = arg0;
      arg0 = nir_bcsel(b, is_even, arg1, arg0);
      arg1 = nir_bcsel(b, is_even, tmp, arg1);

      /* swap odd,even lanes again for arg0 */
      arg0 = nir_quad_swizzle_amd(b, arg0, .swizzle_mask = 0b10110001, .fetch_inactive = true);

      arg0_vec[i] = arg0;
      arg1_vec[i] = arg1;
   }

   nir_src_rewrite(&mrt0_exp->src[0], nir_vec(b, arg0_vec, 4));
   nir_src_rewrite(&mrt1_exp->src[0], nir_vec(b, arg1_vec, 4));

   nir_intrinsic_set_write_mask(mrt0_exp, write_mask);
   nir_intrinsic_set_write_mask(mrt1_exp, write_mask);
}

static void
emit_ps_null_export(nir_builder *b, lower_ps_state *s)
{
   const bool pops = b->shader->info.fs.sample_interlock_ordered ||
                     b->shader->info.fs.sample_interlock_unordered ||
                     b->shader->info.fs.pixel_interlock_ordered ||
                     b->shader->info.fs.pixel_interlock_unordered;

   /* Gfx10+ doesn't need to export anything if we don't need to export the EXEC mask
    * for discard.
    * In Primitive Ordered Pixel Shading, however, GFX11+ explicitly uses the `done` export to exit
    * the ordered section, and before GFX11, shaders with POPS also need an export.
    */
   if (s->options->gfx_level >= GFX10 && !s->options->uses_discard && !pops)
      return;

   /* The `done` export exits the POPS ordered section on GFX11+, make sure UniformMemory and
    * ImageMemory (in SPIR-V terms) accesses from the ordered section may not be reordered below it.
    */
   if (s->options->gfx_level >= GFX11 && pops)
      nir_scoped_memory_barrier(b, SCOPE_QUEUE_FAMILY, NIR_MEMORY_RELEASE,
                                nir_var_image | nir_var_mem_ubo | nir_var_mem_ssbo |
                                nir_var_mem_global);

   /* Gfx11 doesn't support null exports, and mrt0 should be exported instead. */
   unsigned target = s->options->gfx_level >= GFX11 ?
      V_008DFC_SQ_EXP_MRT : V_008DFC_SQ_EXP_NULL;

   nir_intrinsic_instr *intrin =
      nir_export_amd(b, nir_undef(b, 4, 32),
                     .base = target,
                     .flags = AC_EXP_FLAG_VALID_MASK | AC_EXP_FLAG_DONE);
   /* To avoid builder set write mask to 0xf. */
   nir_intrinsic_set_write_mask(intrin, 0);
}

static void
export_ps_outputs(nir_builder *b, lower_ps_state *s)
{
   b->cursor = nir_after_impl(b->impl);

   emit_ps_color_clamp_and_alpha_test(b, s);

   if (!s->options->no_depth_export)
      emit_ps_mrtz_export(b, s);

   /* When non-monolithic shader, RADV export mrtz in main part (except on
    * RDNA3 for alpha to coverage) and export color in epilog.
    */
   if (s->options->no_color_export)
      return;

   unsigned first_color_export = s->exp_num;

   /* Add exports for dual source blending manually if they are missing.
    * It will automatically generate exports with undef.
    */
   if (s->has_dual_src_blending) {
      switch (s->colors_written) {
      case BITFIELD_BIT(0):
         s->colors_written |= BITFIELD_BIT(1);
         s->color_type[1] = s->color_type[0];
         s->spi_shader_col_format |= (s->spi_shader_col_format & 0xf) << 4;
         break;

      case BITFIELD_BIT(1):
         s->colors_written |= BITFIELD_BIT(0);
         s->color_type[0] = s->color_type[1];
         s->spi_shader_col_format |= (s->spi_shader_col_format & 0xf0) >> 4;
         break;
      case BITFIELD_RANGE(0, 2):
         break;
      default:
         unreachable("unexpected number of color outputs for dual source blending");
      }
   }

   if (s->options->broadcast_last_cbuf > 0) {
      /* write to all color buffers */
      assert(s->colors_written & 0x1);
      for (int cbuf = 0; cbuf <= s->options->broadcast_last_cbuf; cbuf++)
         emit_ps_color_export(b, s, 0, cbuf);
   } else {
      for (int cbuf = 0; cbuf < MAX_DRAW_BUFFERS; cbuf++)
         emit_ps_color_export(b, s, cbuf, cbuf);
   }

   if (s->exp_num) {
      if (s->options->dual_src_blend_swizzle) {
         emit_ps_dual_src_blend_swizzle(b, s, first_color_export);
         /* Skip last export flag setting because they have been replaced by
          * a pseudo instruction.
          */
         if (s->options->use_aco)
            return;
      }

      /* Specify that this is the last export */
      nir_intrinsic_instr *final_exp = s->exp[s->exp_num - 1];
      unsigned final_exp_flags = nir_intrinsic_flags(final_exp);
      final_exp_flags |= AC_EXP_FLAG_DONE | AC_EXP_FLAG_VALID_MASK;
      nir_intrinsic_set_flags(final_exp, final_exp_flags);

      /* The `done` export exits the POPS ordered section on GFX11+, make sure UniformMemory and
       * ImageMemory (in SPIR-V terms) accesses from the ordered section may not be reordered below
       * it.
       */
      if (s->options->gfx_level >= GFX11 &&
          (b->shader->info.fs.sample_interlock_ordered ||
           b->shader->info.fs.sample_interlock_unordered ||
           b->shader->info.fs.pixel_interlock_ordered ||
           b->shader->info.fs.pixel_interlock_unordered)) {
         b->cursor = nir_before_instr(&final_exp->instr);
         nir_scoped_memory_barrier(b, SCOPE_QUEUE_FAMILY, NIR_MEMORY_RELEASE,
                                   nir_var_image | nir_var_mem_ubo | nir_var_mem_ssbo |
                                   nir_var_mem_global);
      }
   } else {
      emit_ps_null_export(b, s);
   }
}

void
ac_nir_lower_ps(nir_shader *nir, const ac_nir_lower_ps_options *options)
{
   nir_function_impl *impl = nir_shader_get_entrypoint(nir);

   nir_builder builder = nir_builder_create(impl);
   nir_builder *b = &builder;

   lower_ps_state state = {
      .options = options,
      .has_dual_src_blending = options->dual_src_blend_swizzle,
      .spi_shader_col_format = options->spi_shader_col_format,
   };

   create_interp_param(b, &state);

   nir_shader_instructions_pass(nir, lower_ps_intrinsic,
                                nir_metadata_control_flow,
                                &state);

   /* Must be after lower_ps_intrinsic() to prevent it lower added intrinsic here. */
   init_interp_param(b, &state);

   export_ps_outputs(b, &state);

   /* Cleanup nir variable, as RADV won't do this. */
   if (state.lower_load_barycentric)
      nir_lower_vars_to_ssa(nir);
}
