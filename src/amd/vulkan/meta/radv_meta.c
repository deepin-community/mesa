/*
 * Copyright © 2016 Red Hat
 * based on intel anv code:
 * Copyright © 2015 Intel Corporation
 *
 * SPDX-License-Identifier: MIT
 */

#include "radv_meta.h"
#include "radv_printf.h"

#include "vk_common_entrypoints.h"
#include "vk_pipeline_cache.h"
#include "vk_util.h"

static void
radv_suspend_queries(struct radv_meta_saved_state *state, struct radv_cmd_buffer *cmd_buffer)
{
   const uint32_t num_pipeline_stat_queries = radv_get_num_pipeline_stat_queries(cmd_buffer);

   if (num_pipeline_stat_queries > 0) {
      cmd_buffer->state.flush_bits &= ~RADV_CMD_FLAG_START_PIPELINE_STATS;
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_STOP_PIPELINE_STATS;
   }

   /* Pipeline statistics queries. */
   if (cmd_buffer->state.active_pipeline_queries > 0) {
      state->active_emulated_pipeline_queries = cmd_buffer->state.active_emulated_pipeline_queries;
      cmd_buffer->state.active_emulated_pipeline_queries = 0;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;
   }

   /* Occlusion queries. */
   if (cmd_buffer->state.active_occlusion_queries) {
      state->active_occlusion_queries = cmd_buffer->state.active_occlusion_queries;
      cmd_buffer->state.active_occlusion_queries = 0;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_OCCLUSION_QUERY;
   }

   /* Primitives generated queries (legacy). */
   if (cmd_buffer->state.active_prims_gen_queries) {
      cmd_buffer->state.suspend_streamout = true;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_ENABLE;
   }

   /* Primitives generated queries (NGG). */
   if (cmd_buffer->state.active_emulated_prims_gen_queries) {
      state->active_emulated_prims_gen_queries = cmd_buffer->state.active_emulated_prims_gen_queries;
      cmd_buffer->state.active_emulated_prims_gen_queries = 0;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;
   }

   /* Transform feedback queries (NGG). */
   if (cmd_buffer->state.active_emulated_prims_xfb_queries) {
      state->active_emulated_prims_xfb_queries = cmd_buffer->state.active_emulated_prims_xfb_queries;
      cmd_buffer->state.active_emulated_prims_xfb_queries = 0;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;
   }
}

static void
radv_resume_queries(const struct radv_meta_saved_state *state, struct radv_cmd_buffer *cmd_buffer)
{
   const uint32_t num_pipeline_stat_queries = radv_get_num_pipeline_stat_queries(cmd_buffer);

   if (num_pipeline_stat_queries > 0) {
      cmd_buffer->state.flush_bits &= ~RADV_CMD_FLAG_STOP_PIPELINE_STATS;
      cmd_buffer->state.flush_bits |= RADV_CMD_FLAG_START_PIPELINE_STATS;
   }

   /* Pipeline statistics queries. */
   if (cmd_buffer->state.active_pipeline_queries > 0) {
      cmd_buffer->state.active_emulated_pipeline_queries = state->active_emulated_pipeline_queries;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;
   }

   /* Occlusion queries. */
   if (state->active_occlusion_queries) {
      cmd_buffer->state.active_occlusion_queries = state->active_occlusion_queries;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_OCCLUSION_QUERY;
   }

   /* Primitives generated queries (legacy). */
   if (cmd_buffer->state.active_prims_gen_queries) {
      cmd_buffer->state.suspend_streamout = false;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_STREAMOUT_ENABLE;
   }

   /* Primitives generated queries (NGG). */
   if (state->active_emulated_prims_gen_queries) {
      cmd_buffer->state.active_emulated_prims_gen_queries = state->active_emulated_prims_gen_queries;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;
   }

   /* Transform feedback queries (NGG). */
   if (state->active_emulated_prims_xfb_queries) {
      cmd_buffer->state.active_emulated_prims_xfb_queries = state->active_emulated_prims_xfb_queries;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_SHADER_QUERY;
   }
}

void
radv_meta_save(struct radv_meta_saved_state *state, struct radv_cmd_buffer *cmd_buffer, uint32_t flags)
{
   VkPipelineBindPoint bind_point =
      flags & RADV_META_SAVE_GRAPHICS_PIPELINE ? VK_PIPELINE_BIND_POINT_GRAPHICS : VK_PIPELINE_BIND_POINT_COMPUTE;
   struct radv_descriptor_state *descriptors_state = radv_get_descriptors_state(cmd_buffer, bind_point);

   assert(flags & (RADV_META_SAVE_GRAPHICS_PIPELINE | RADV_META_SAVE_COMPUTE_PIPELINE));

   state->flags = flags;
   state->active_occlusion_queries = 0;
   state->active_emulated_prims_gen_queries = 0;
   state->active_emulated_prims_xfb_queries = 0;

   if (state->flags & RADV_META_SAVE_GRAPHICS_PIPELINE) {
      assert(!(state->flags & RADV_META_SAVE_COMPUTE_PIPELINE));

      state->old_graphics_pipeline = cmd_buffer->state.graphics_pipeline;

      /* Save all dynamic states. */
      state->dynamic = cmd_buffer->state.dynamic;
   }

   if (state->flags & RADV_META_SAVE_COMPUTE_PIPELINE) {
      assert(!(state->flags & RADV_META_SAVE_GRAPHICS_PIPELINE));

      state->old_compute_pipeline = cmd_buffer->state.compute_pipeline;
   }

   for (unsigned i = 0; i <= MESA_SHADER_MESH; i++) {
      state->old_shader_objs[i] = cmd_buffer->state.shader_objs[i];
   }

   if (state->flags & RADV_META_SAVE_DESCRIPTORS) {
      state->old_descriptor_set0 = descriptors_state->sets[0];
      if (!(descriptors_state->valid & 1))
         state->flags &= ~RADV_META_SAVE_DESCRIPTORS;
   }

   if (state->flags & RADV_META_SAVE_CONSTANTS) {
      memcpy(state->push_constants, cmd_buffer->push_constants, MAX_PUSH_CONSTANTS_SIZE);
   }

   if (state->flags & RADV_META_SAVE_RENDER) {
      state->render = cmd_buffer->state.render;
      radv_cmd_buffer_reset_rendering(cmd_buffer);
   }

   if (state->flags & RADV_META_SUSPEND_PREDICATING) {
      state->predicating = cmd_buffer->state.predicating;
      cmd_buffer->state.predicating = false;
   }

   radv_suspend_queries(state, cmd_buffer);
}

void
radv_meta_restore(const struct radv_meta_saved_state *state, struct radv_cmd_buffer *cmd_buffer)
{
   VkPipelineBindPoint bind_point = state->flags & RADV_META_SAVE_GRAPHICS_PIPELINE ? VK_PIPELINE_BIND_POINT_GRAPHICS
                                                                                    : VK_PIPELINE_BIND_POINT_COMPUTE;

   if (state->flags & RADV_META_SAVE_GRAPHICS_PIPELINE) {
      if (state->old_graphics_pipeline) {
         radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_GRAPHICS,
                              radv_pipeline_to_handle(&state->old_graphics_pipeline->base));
      }

      /* Restore all dynamic states. */
      cmd_buffer->state.dynamic = state->dynamic;
      cmd_buffer->state.dirty_dynamic |= RADV_DYNAMIC_ALL;

      /* Re-emit the guardband state because meta operations changed dynamic states. */
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_GUARDBAND;
   }

   if (state->flags & RADV_META_SAVE_COMPUTE_PIPELINE) {
      if (state->old_compute_pipeline) {
         radv_CmdBindPipeline(radv_cmd_buffer_to_handle(cmd_buffer), VK_PIPELINE_BIND_POINT_COMPUTE,
                              radv_pipeline_to_handle(&state->old_compute_pipeline->base));
      }
   }

   VkShaderEXT shaders[MESA_SHADER_MESH + 1];
   VkShaderStageFlagBits stages[MESA_SHADER_MESH + 1];
   uint32_t stage_count = 0;

   for (unsigned i = 0; i <= MESA_SHADER_MESH; i++) {
      if (state->old_shader_objs[i]) {
         stages[stage_count] = mesa_to_vk_shader_stage(i);
         shaders[stage_count] = radv_shader_object_to_handle(state->old_shader_objs[i]);
         stage_count++;
      }
   }

   if (stage_count > 0) {
      radv_CmdBindShadersEXT(radv_cmd_buffer_to_handle(cmd_buffer), stage_count, stages, shaders);
   }

   if (state->flags & RADV_META_SAVE_DESCRIPTORS) {
      radv_set_descriptor_set(cmd_buffer, bind_point, state->old_descriptor_set0, 0);
   }

   if (state->flags & RADV_META_SAVE_CONSTANTS) {
      VkShaderStageFlags stage_flags = VK_SHADER_STAGE_COMPUTE_BIT;

      if (state->flags & RADV_META_SAVE_GRAPHICS_PIPELINE)
         stage_flags |= VK_SHADER_STAGE_ALL_GRAPHICS;

      vk_common_CmdPushConstants(radv_cmd_buffer_to_handle(cmd_buffer), VK_NULL_HANDLE, stage_flags, 0,
                                 MAX_PUSH_CONSTANTS_SIZE, state->push_constants);
   }

   if (state->flags & RADV_META_SAVE_RENDER) {
      cmd_buffer->state.render = state->render;
      cmd_buffer->state.dirty |= RADV_CMD_DIRTY_FRAMEBUFFER;
   }

   if (state->flags & RADV_META_SUSPEND_PREDICATING)
      cmd_buffer->state.predicating = state->predicating;

   radv_resume_queries(state, cmd_buffer);
}

VkImageViewType
radv_meta_get_view_type(const struct radv_image *image)
{
   switch (image->vk.image_type) {
   case VK_IMAGE_TYPE_1D:
      return VK_IMAGE_VIEW_TYPE_1D;
   case VK_IMAGE_TYPE_2D:
      return VK_IMAGE_VIEW_TYPE_2D;
   case VK_IMAGE_TYPE_3D:
      return VK_IMAGE_VIEW_TYPE_3D;
   default:
      unreachable("bad VkImageViewType");
   }
}

/**
 * When creating a destination VkImageView, this function provides the needed
 * VkImageViewCreateInfo::subresourceRange::baseArrayLayer.
 */
uint32_t
radv_meta_get_iview_layer(const struct radv_image *dst_image, const VkImageSubresourceLayers *dst_subresource,
                          const VkOffset3D *dst_offset)
{
   switch (dst_image->vk.image_type) {
   case VK_IMAGE_TYPE_1D:
   case VK_IMAGE_TYPE_2D:
      return dst_subresource->baseArrayLayer;
   case VK_IMAGE_TYPE_3D:
      /* HACK: Vulkan does not allow attaching a 3D image to a framebuffer,
       * but meta does it anyway. When doing so, we translate the
       * destination's z offset into an array offset.
       */
      return dst_offset->z;
   default:
      assert(!"bad VkImageType");
      return 0;
   }
}

static VKAPI_ATTR void *VKAPI_CALL
meta_alloc(void *_device, size_t size, size_t alignment, VkSystemAllocationScope allocationScope)
{
   struct radv_device *device = _device;
   return device->vk.alloc.pfnAllocation(device->vk.alloc.pUserData, size, alignment,
                                         VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
}

static VKAPI_ATTR void *VKAPI_CALL
meta_realloc(void *_device, void *original, size_t size, size_t alignment, VkSystemAllocationScope allocationScope)
{
   struct radv_device *device = _device;
   return device->vk.alloc.pfnReallocation(device->vk.alloc.pUserData, original, size, alignment,
                                           VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
}

static VKAPI_ATTR void VKAPI_CALL
meta_free(void *_device, void *data)
{
   struct radv_device *device = _device;
   device->vk.alloc.pfnFree(device->vk.alloc.pUserData, data);
}

static void
radv_init_meta_cache(struct radv_device *device)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   struct vk_pipeline_cache *cache;

   VkPipelineCacheCreateInfo create_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
   };

   struct vk_pipeline_cache_create_info info = {
      .pCreateInfo = &create_info,
      .disk_cache = pdev->disk_cache_meta,
   };

   cache = vk_pipeline_cache_create(&device->vk, &info, NULL);
   if (cache)
      device->meta_state.cache = vk_pipeline_cache_to_handle(cache);
}

VkResult
radv_device_init_meta(struct radv_device *device)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   VkResult result;

   memset(&device->meta_state, 0, sizeof(device->meta_state));

   device->meta_state.alloc = (VkAllocationCallbacks){
      .pUserData = device,
      .pfnAllocation = meta_alloc,
      .pfnReallocation = meta_realloc,
      .pfnFree = meta_free,
   };

   radv_init_meta_cache(device);

   result = vk_meta_device_init(&device->vk, &device->meta_state.device);
   if (result != VK_SUCCESS)
      return result;

   device->meta_state.device.pipeline_cache = device->meta_state.cache;

   mtx_init(&device->meta_state.mtx, mtx_plain);

   if (pdev->emulate_etc2) {
      device->meta_state.etc_decode.allocator = &device->meta_state.alloc;
      device->meta_state.etc_decode.nir_options = &pdev->nir_options[MESA_SHADER_COMPUTE];
      device->meta_state.etc_decode.pipeline_cache = device->meta_state.cache;

      vk_texcompress_etc2_init(&device->vk, &device->meta_state.etc_decode);
   }

   if (pdev->emulate_astc) {
      result = vk_texcompress_astc_init(&device->vk, &device->meta_state.alloc, device->meta_state.cache,
                                        &device->meta_state.astc_decode);
      if (result != VK_SUCCESS)
         return result;
   }

   if (device->vk.enabled_features.nullDescriptor) {
      result = radv_device_init_null_accel_struct(device);
      if (result != VK_SUCCESS)
         return result;
   }

   return VK_SUCCESS;
}

void
radv_device_finish_meta(struct radv_device *device)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);

   if (pdev->emulate_etc2)
      vk_texcompress_etc2_finish(&device->vk, &device->meta_state.etc_decode);

   if (pdev->emulate_astc) {
      if (device->meta_state.astc_decode)
         vk_texcompress_astc_finish(&device->vk, &device->meta_state.alloc, device->meta_state.astc_decode);
   }

   radv_device_finish_accel_struct_build_state(device);

   vk_common_DestroyPipelineCache(radv_device_to_handle(device), device->meta_state.cache, NULL);
   mtx_destroy(&device->meta_state.mtx);

   if (device->meta_state.device.cache)
      vk_meta_device_finish(&device->vk, &device->meta_state.device);
}

nir_builder PRINTFLIKE(3, 4)
   radv_meta_init_shader(struct radv_device *dev, gl_shader_stage stage, const char *name, ...)
{
   const struct radv_physical_device *pdev = radv_device_physical(dev);
   nir_builder b = nir_builder_init_simple_shader(stage, NULL, NULL);
   if (name) {
      va_list args;
      va_start(args, name);
      b.shader->info.name = ralloc_vasprintf(b.shader, name, args);
      va_end(args);
   }

   b.shader->options = &pdev->nir_options[stage];

   radv_device_associate_nir(dev, b.shader);

   return b;
}

/* vertex shader that generates vertices */
nir_shader *
radv_meta_build_nir_vs_generate_vertices(struct radv_device *dev)
{
   const struct glsl_type *vec4 = glsl_vec4_type();

   nir_variable *v_position;

   nir_builder b = radv_meta_init_shader(dev, MESA_SHADER_VERTEX, "meta_vs_gen_verts");

   nir_def *outvec = nir_gen_rect_vertices(&b, NULL, NULL);

   v_position = nir_variable_create(b.shader, nir_var_shader_out, vec4, "gl_Position");
   v_position->data.location = VARYING_SLOT_POS;

   nir_store_var(&b, v_position, outvec, 0xf);

   return b.shader;
}

nir_shader *
radv_meta_build_nir_fs_noop(struct radv_device *dev)
{
   return radv_meta_init_shader(dev, MESA_SHADER_FRAGMENT, "meta_noop_fs").shader;
}

void
radv_meta_build_resolve_shader_core(struct radv_device *device, nir_builder *b, bool is_integer, int samples,
                                    nir_variable *input_img, nir_variable *color, nir_def *img_coord)
{
   const struct radv_physical_device *pdev = radv_device_physical(device);
   nir_deref_instr *input_img_deref = nir_build_deref_var(b, input_img);
   nir_def *sample0 = nir_txf_ms_deref(b, input_img_deref, img_coord, nir_imm_int(b, 0));

   if (is_integer || samples <= 1) {
      nir_store_var(b, color, sample0, 0xf);
      return;
   }

   if (pdev->use_fmask) {
      nir_def *all_same = nir_samples_identical_deref(b, input_img_deref, img_coord);
      nir_push_if(b, nir_inot(b, all_same));
   }

   nir_def *accum = sample0;
   for (int i = 1; i < samples; i++) {
      nir_def *sample = nir_txf_ms_deref(b, input_img_deref, img_coord, nir_imm_int(b, i));
      accum = nir_fadd(b, accum, sample);
   }

   accum = nir_fdiv_imm(b, accum, samples);
   nir_store_var(b, color, accum, 0xf);

   if (pdev->use_fmask) {
      nir_push_else(b, NULL);
      nir_store_var(b, color, sample0, 0xf);
      nir_pop_if(b, NULL);
   }
}

nir_def *
radv_meta_load_descriptor(nir_builder *b, unsigned desc_set, unsigned binding)
{
   nir_def *rsrc = nir_vulkan_resource_index(b, 3, 32, nir_imm_int(b, 0), .desc_set = desc_set, .binding = binding);
   return nir_trim_vector(b, rsrc, 2);
}

nir_def *
get_global_ids(nir_builder *b, unsigned num_components)
{
   unsigned mask = BITFIELD_MASK(num_components);

   nir_def *local_ids = nir_channels(b, nir_load_local_invocation_id(b), mask);
   nir_def *block_ids = nir_channels(b, nir_load_workgroup_id(b), mask);
   nir_def *block_size =
      nir_channels(b,
                   nir_imm_ivec4(b, b->shader->info.workgroup_size[0], b->shader->info.workgroup_size[1],
                                 b->shader->info.workgroup_size[2], 0),
                   mask);

   return nir_iadd(b, nir_imul(b, block_ids, block_size), local_ids);
}

void
radv_break_on_count(nir_builder *b, nir_variable *var, nir_def *count)
{
   nir_def *counter = nir_load_var(b, var);

   nir_break_if(b, nir_uge(b, counter, count));

   counter = nir_iadd_imm(b, counter, 1);
   nir_store_var(b, var, counter, 0x1);
}

VkResult
radv_meta_get_noop_pipeline_layout(struct radv_device *device, VkPipelineLayout *layout_out)
{
   enum radv_meta_object_key_type key = RADV_META_OBJECT_KEY_NOOP;

   return vk_meta_get_pipeline_layout(&device->vk, &device->meta_state.device, NULL, NULL, &key, sizeof(key),
                                      layout_out);
}
