/*
 * Mesa 3-D graphics library
 *
 * Copyright (c) Imagination Technologies Ltd.
 * Copyright © 2011 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <tpl.h>
#include <xf86drm.h>

#include "egl_dri2.h"
#include "loader.h"

#define TIZEN_DRM_RENDER_MINOR_START 128
#define TIZEN_DRM_RENDER_MINOR_MAX 191

#define TIZEN_SWAP_N_RECTS_MIN 32

struct rgba_shifts_and_sizes {
   int shifts[4];
   unsigned int sizes[4];
};

static void *swap_queue_processor_worker(void *data)
{
   struct dri2_egl_surface *dri2_surf = data;

   pthread_mutex_lock(&dri2_surf->mutex);
   while (1) {
      struct tpl_swap_queue_elem *queue_elem;
      int i;
      tpl_result_t res;

      if (dri2_surf->swap_queue_idx_head == dri2_surf->swap_queue_idx_tail) {
         /* Wait for new swaps to be added to the queue */
         pthread_cond_wait(&dri2_surf->swap_queue_cond, &dri2_surf->mutex);
         continue;
      }

      queue_elem = &dri2_surf->swap_queue[dri2_surf->swap_queue_idx_head];
      pthread_mutex_unlock(&dri2_surf->mutex);

      res = tpl_surface_enqueue_buffer_with_damage(dri2_surf->tpl_surf,
                                                   queue_elem->tbm_surf,
                                                   queue_elem->n_rects,
                                                   queue_elem->rects);
      if (res != TPL_ERROR_NONE)
         return NULL;

      pthread_mutex_lock(&dri2_surf->mutex);
      for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
         if (dri2_surf->color_buffers[i].tbm_surf == queue_elem->tbm_surf) {
            dri2_surf->color_buffers[i].locked = false;
            break;
         }
      }

      dri2_surf->swap_queue_idx_head++;
      dri2_surf->swap_queue_idx_head %= ARRAY_SIZE(dri2_surf->swap_queue);

      /*
       * Notify get_back_bo that a buffer has become available, reset_surface_cb
       * that it may be able to reset the surface or dri2_tizen_destroy_surface
       * that it may be able to proceed with surface destruction.
       */
      pthread_cond_signal(&dri2_surf->swap_queue_cond);
   }
   pthread_mutex_unlock(&dri2_surf->mutex);

   return NULL;
}

static __DRIimage *
create_image_from_native(struct dri2_egl_surface *dri2_surf,
                         tbm_surface_h tbm_surf,
                         int *width, int *height,
                         void *loaderPrivate)
{
   _EGLSurface *surf = &dri2_surf->base;
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(surf->Resource.Display);
   struct dri2_egl_config *dri2_conf = dri2_egl_config(surf->Config);
   const __DRIconfig *config =
      dri2_get_dri_config(dri2_conf, surf->Type, surf->GLColorspace);
   tbm_bo tbm_buf;
   tbm_surface_info_s info;
   int fd[TBM_SURF_PLANE_MAX];
   int offset[TBM_SURF_PLANE_MAX];
   int pitch[TBM_SURF_PLANE_MAX];
   int fourcc;
   __DRIimage *dri_image;
   enum __DRIYUVColorSpace color_space;
   enum __DRISampleRange sample_range;
   unsigned csc_standard;
   unsigned depth_range;
   unsigned create_error;

   if (tbm_surface_get_info(tbm_surf, &info)) {
      _eglLog(_EGL_DEBUG, "%s: failed to get tbm surface info", __func__);
      return NULL;
   }

   fourcc = dri2_fourcc_from_tbm_format(info.format);

   for (unsigned i = 0; i < info.num_planes; i++) {
      int index = tbm_surface_internal_get_plane_bo_idx(tbm_surf, i);

      tbm_buf = tbm_surface_internal_get_bo(tbm_surf, index);
      if (!tbm_buf) {
         while (i--)
            close(fd[i]);
         _eglLog(_EGL_DEBUG, "%s: failed to get bo %d for tbm surface",
                 __func__, i);
         return NULL;
      }

      fd[i] = tbm_bo_export_fd(tbm_buf);
      offset[i] = info.planes[i].offset;
      pitch[i] = info.planes[i].stride;
   }

   dri2_dpy->core->getConfigAttrib(config,
                                   __DRI_ATTRIB_YUV_CSC_STANDARD, &csc_standard);
   switch (csc_standard) {
   case __DRI_ATTRIB_YUV_CSC_STANDARD_601_BIT:
      color_space = __DRI_YUV_COLOR_SPACE_ITU_REC601;
      break;
   case __DRI_ATTRIB_YUV_CSC_STANDARD_709_BIT:
      color_space = __DRI_YUV_COLOR_SPACE_ITU_REC709;
      break;
   case __DRI_ATTRIB_YUV_CSC_STANDARD_2020_BIT:
      color_space = __DRI_YUV_COLOR_SPACE_ITU_REC2020;
      break;
   default:
      color_space = __DRI_YUV_COLOR_SPACE_UNDEFINED;
      break;
   }

   dri2_dpy->core->getConfigAttrib(config,
                                   __DRI_ATTRIB_YUV_DEPTH_RANGE, &depth_range);
   switch (depth_range) {
   case __DRI_ATTRIB_YUV_DEPTH_RANGE_LIMITED_BIT:
      sample_range = __DRI_YUV_NARROW_RANGE;
      break;
   case __DRI_ATTRIB_YUV_DEPTH_RANGE_FULL_BIT:
      sample_range = __DRI_YUV_FULL_RANGE;
      break;
   default:
      sample_range = __DRI_YUV_RANGE_UNDEFINED;
      break;
   }

   dri_image = dri2_dpy->image->createImageFromDmaBufs(dri2_dpy->dri_screen,
                                                       info.width,
                                                       info.height,
                                                       fourcc,
                                                       fd,
                                                       info.num_planes,
                                                       pitch,
                                                       offset,
                                                       color_space,
                                                       sample_range,
                                                       __DRI_YUV_CHROMA_SITING_UNDEFINED,
                                                       __DRI_YUV_CHROMA_SITING_UNDEFINED,
                                                       &create_error,
                                                       loaderPrivate);
   for (unsigned i = 0; i < info.num_planes; i++)
      close(fd[i]);

   if (!dri_image) {
      _eglLog(_EGL_DEBUG, "%s: failed to create dri image from tbm bo", __func__);
      return NULL;
   }

   if (width)
      *width = info.width;

   if (height)
      *height = info.height;

   return dri_image;
}

static int
get_back_bo(struct dri2_egl_surface *dri2_surf, bool allow_update,
            bool allow_preserve)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);
   tbm_surface_h tbm_surf;
   tpl_bool_t tpl_surf_valid;
   int i;

   if (dri2_surf->base.Type == EGL_PIXMAP_BIT) {
      _eglLog(_EGL_DEBUG, "%s: can't get back bo for pixmap surface", __func__);
      return -1;
   }

   if (dri2_surf->back && !allow_update)
      return 0;

   /*
    * If the tpl surface is no longer valid, e.g. it has been resized, then
    * we should get a new back buffer.
    */
   tpl_surf_valid = tpl_surface_validate(dri2_surf->tpl_surf);
   if (dri2_surf->back && tpl_surf_valid == TPL_TRUE)
      return 0;

   tbm_surf = tpl_surface_dequeue_buffer(dri2_surf->tpl_surf);
   if (!tbm_surf) {
      _eglLog(_EGL_DEBUG, "%s: surface buffer dequeue failed", __func__);
      return -1;
   }

   pthread_mutex_lock(&dri2_surf->mutex);
   /*
    * If the tpl surface is no longer valid, e.g. it has been resized, then the
    * call to tpl_surface should've resulted in a call to reset_surface_cb,
    * which sets the dri2_surf reset flag. In this case we need to reset the
    * color_buffer state.
    *
    * Note: we can't rely on the return value of tpl_surface_validate
    * since the surface may have become invalid between this call and
    * tpl_surface_dequeue_buffer.
    */
   if (dri2_surf->reset) {
      /* Wait for any outstanding swaps to complete */
      while (dri2_surf->swap_queue_idx_head != dri2_surf->swap_queue_idx_tail)
         pthread_cond_wait(&dri2_surf->swap_queue_cond, &dri2_surf->mutex);

      /* Cancel the old back buffer */
      if (dri2_surf->back) {
         tbm_surface_internal_unref(dri2_surf->back->tbm_surf);
         dri2_surf->back->locked = false;
         dri2_surf->back = NULL;
      }

      /* Reset the color buffers */
      for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
         assert(!dri2_surf->color_buffers[i].locked);

         if (dri2_surf->color_buffers[i].dri_image) {
            dri2_dpy->image->destroyImage(dri2_surf->color_buffers[i].dri_image);
            dri2_surf->color_buffers[i].dri_image = NULL;
         }

         dri2_surf->color_buffers[i].tbm_surf = NULL;
         dri2_surf->color_buffers[i].age = 0;
      }

      dri2_surf->back = NULL;
      dri2_surf->current = NULL;
      dri2_surf->reset = false;
   }

   assert(!dri2_surf->back);

   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].tbm_surf == tbm_surf) {
         dri2_surf->back = &dri2_surf->color_buffers[i];
         assert(!dri2_surf->back->locked);
         break;
      }
   }

   while (!dri2_surf->back) {
      int age;

      /*
       * Search for a free color buffer. Free the oldest buffer if one
       * cannot be found.
       */
      for (i = 0, age = -1; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
         if (dri2_surf->color_buffers[i].locked)
            continue;
         if (!dri2_surf->color_buffers[i].dri_image) {
            dri2_surf->back = &dri2_surf->color_buffers[i];
            break;
         }

         if (dri2_surf->color_buffers[i].age > age) {
            dri2_surf->back = &dri2_surf->color_buffers[i];
            age = dri2_surf->color_buffers[i].age;
         }
      }

      if (!dri2_surf->back) {
         /*
          * There aren't any available back buffers so wait for
          * swap_queue_processor_worker to make one available.
          */
         pthread_cond_wait(&dri2_surf->swap_queue_cond, &dri2_surf->mutex);
         continue;
      }

      if (dri2_surf->back->dri_image)
         dri2_dpy->image->destroyImage(dri2_surf->back->dri_image);

      dri2_surf->back->dri_image =
         create_image_from_native(dri2_surf, tbm_surf,
                                  &dri2_surf->base.Width,
                                  &dri2_surf->base.Height,
                                  dri2_surf);
      if (!dri2_surf->back->dri_image) {
         dri2_surf->back = NULL;
         pthread_mutex_unlock(&dri2_surf->mutex);
         return -1;
      }
   }

   dri2_surf->back->tbm_surf = tbm_surf;
   dri2_surf->back->locked = true;

   if (dri2_surf->base.SwapBehavior == EGL_BUFFER_PRESERVED &&
       allow_preserve && dri2_surf->current) {
      _EGLContext *ctx = _eglGetCurrentContext();
      struct dri2_egl_context *dri2_ctx = dri2_egl_context(ctx);

      if (dri2_ctx)
      {
         dri2_dpy->image->blitImage(dri2_ctx->dri_context,
                                    dri2_surf->back->dri_image,
                                    dri2_surf->current->dri_image,
                                    0, 0, dri2_surf->base.Width,
                                    dri2_surf->base.Height,
                                    0, 0, dri2_surf->base.Width,
                                    dri2_surf->base.Height,
                                    __BLIT_FLAG_FLUSH);
      }
   }

   pthread_mutex_unlock(&dri2_surf->mutex);

   return 0;
}

static int
get_front_bo(struct dri2_egl_surface *dri2_surf)
{
   struct dri2_egl_display *dri2_dpy =
      dri2_egl_display(dri2_surf->base.Resource.Display);

   if (dri2_surf->front)
      return 0;

   if (dri2_surf->base.Type == EGL_PIXMAP_BIT) {
      tbm_surface_h tbm_surf;

      tbm_surf = tpl_surface_dequeue_buffer(dri2_surf->tpl_surf);
      if (!tbm_surf)
         return -1;

      dri2_surf->front = create_image_from_native(dri2_surf, tbm_surf,
                                                  &dri2_surf->base.Width,
                                                  &dri2_surf->base.Height,
                                                  dri2_surf);
   } else {
      dri2_surf->front = dri2_dpy->image->createImage(dri2_dpy->dri_screen,
                                                      dri2_surf->base.Width,
                                                      dri2_surf->base.Height,
                                                      dri2_surf->visual,
                                                      0,
                                                      dri2_surf);
   }

   return dri2_surf->front ? 0 : -1;
}

static _EGLSurface *
create_surface(_EGLDisplay *dpy, _EGLConfig *config, EGLint type,
               const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   struct dri2_egl_config *dri2_config = dri2_egl_config(config);
   struct dri2_egl_surface *dri2_surf;
   const __DRIconfig *dri_config;

   dri2_surf = calloc(1, sizeof(*dri2_surf));
   if (!dri2_surf) {
      _eglError(EGL_BAD_ALLOC, "DRI2: failed to create surface");
      return NULL;
   }

   if (!dri2_init_surface(&dri2_surf->base, dpy, type, config, attrib_list,
                          false, NULL))
      goto err_free_surface;

   dri_config = dri2_get_dri_config(dri2_config, type,
                                    dri2_surf->base.GLColorspace);
   if (!dri_config) {
      _eglError(EGL_BAD_MATCH, "Unsupported surfacetype/colorspace configuration");
      goto err_free_surface;
   }

   dri2_surf->dri_drawable =
      dri2_dpy->image_driver->createNewDrawable(dri2_dpy->dri_screen,
                                                dri_config, dri2_surf);
   if (!dri2_surf->dri_drawable) {
      _eglError(EGL_BAD_ALLOC, "DRI2: failed to create drawable");
      goto err_free_surface;
   }

   pthread_mutex_init(&dri2_surf->mutex, NULL);
   pthread_cond_init(&dri2_surf->swap_queue_cond, NULL);

   return &dri2_surf->base;

err_free_surface:
   free(dri2_surf);
   return NULL;
}

static void
destroy_surface(_EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(surf->Resource.Display);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   pthread_cond_destroy(&dri2_surf->swap_queue_cond);
   pthread_mutex_destroy(&dri2_surf->mutex);
   dri2_dpy->core->destroyDrawable(dri2_surf->dri_drawable);
   free(dri2_surf);
}

static void
reset_surface_cb(void *data)
{
   struct dri2_egl_surface *dri2_surf = data;

   dri2_surf->reset = true;
}

static _EGLSurface *
dri2_tizen_create_window_surface(_EGLDisplay *dpy, _EGLConfig *config,
                                 void *native_window, const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   struct dri2_egl_surface *dri2_surf;
   _EGLSurface *surf;
   int width, height;
   tbm_format format;
   tpl_result_t res;
   int err;

   surf = create_surface(dpy, config, EGL_WINDOW_BIT, attrib_list);
   if (!surf)
      return NULL;

   res = tpl_display_get_native_window_info(dri2_dpy->tpl_dpy, native_window,
                                            &width, &height, &format,
                                            config->RedSize +
                                            config->GreenSize +
                                            config->BlueSize +
                                            config->AlphaSize,
                                            config->AlphaSize);
   if (res != TPL_ERROR_NONE) {
      _eglError(EGL_BAD_NATIVE_WINDOW, "DRI2: failed to get window info");
      goto err_destroy_surface;
   }

   dri2_surf = dri2_egl_surface(surf);
   dri2_surf->base.Width = width;
   dri2_surf->base.Height = height;

   dri2_surf->tpl_surf = tpl_surface_create(dri2_dpy->tpl_dpy, native_window,
                                            TPL_SURFACE_TYPE_WINDOW, format);
   if (!dri2_surf->tpl_surf) {
      _eglError(EGL_BAD_ALLOC, "DRI2: failed to create TPL window surface");
      goto err_destroy_surface;
   }

   res = tpl_surface_set_reset_cb(dri2_surf->tpl_surf, dri2_surf,
                                  reset_surface_cb);
   if (res != TPL_ERROR_NONE) {
      _eglError(EGL_BAD_NATIVE_WINDOW,
                "DRI2: failed to register TPL window surface reset callback");
      goto err_tpl_surface_destroy;
   }

   err = pthread_create(&dri2_surf->swap_queue_processor, NULL,
                        swap_queue_processor_worker, dri2_surf);
   if (err) {
      _eglError(EGL_BAD_ALLOC, "DRI2: failed to create TPL window thread");
      goto err_tpl_surface_destroy;
   }

   return surf;

err_tpl_surface_destroy:
   tpl_object_unreference((tpl_object_t *) dri2_surf->tpl_surf);
err_destroy_surface:
   destroy_surface(surf);
   return NULL;
}

static _EGLSurface *
dri2_tizen_create_pixmap_surface(_EGLDisplay *dpy, _EGLConfig *config,
                                 void *native_pixmap, const EGLint *attrib_list)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   struct dri2_egl_surface *dri2_surf;
   _EGLSurface *surf;
   int width, height;
   tbm_format format;
   tpl_result_t res;

   surf = create_surface(dpy, config, EGL_PIXMAP_BIT, attrib_list);
   if (!surf)
      return NULL;

   res = tpl_display_get_native_pixmap_info(dri2_dpy->tpl_dpy, native_pixmap,
                                            &width, &height, &format);
   if (res != TPL_ERROR_NONE) {
      _eglError(EGL_BAD_NATIVE_PIXMAP, "DRI2: failed to get pixmap info");
      goto err_destroy_surface;
   }

   dri2_surf = dri2_egl_surface(surf);
   dri2_surf->base.Width = width;
   dri2_surf->base.Height = height;

   dri2_surf->tpl_surf = tpl_surface_create(dri2_dpy->tpl_dpy, native_pixmap,
                                            TPL_SURFACE_TYPE_PIXMAP, format);
   if (!dri2_surf->tpl_surf) {
      _eglError(EGL_BAD_ALLOC, "DRI2: failed to create TPL pixmap surface");
      goto err_destroy_surface;
   }

   return surf;

err_destroy_surface:
   destroy_surface(surf);
   return NULL;
}

static _EGLSurface *
dri2_tizen_create_pbuffer_surface(_EGLDisplay *dpy, _EGLConfig *config,
                                  const EGLint *attrib_list)
{
   struct dri2_egl_surface *dri2_surf;
   _EGLSurface *surf;

   surf = create_surface(dpy, config, EGL_PBUFFER_BIT, attrib_list);
   if (!surf)
      return NULL;

   dri2_surf = dri2_egl_surface(surf);

   if (config->RedSize == 5)
      dri2_surf->visual = __DRI_IMAGE_FORMAT_RGB565;
   else if (config->AlphaSize)
      dri2_surf->visual = __DRI_IMAGE_FORMAT_ARGB8888;
   else
      dri2_surf->visual = __DRI_IMAGE_FORMAT_XRGB8888;

   return surf;
}

static EGLBoolean
dri2_tizen_destroy_surface(_EGLDisplay *dpy, _EGLSurface *surf)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   int i;

   if (dri2_surf->swap_queue_processor) {
      /* Wait for any outstanding swaps to complete */
      pthread_mutex_lock(&dri2_surf->mutex);
      while (dri2_surf->swap_queue_idx_head != dri2_surf->swap_queue_idx_tail)
         pthread_cond_wait(&dri2_surf->swap_queue_cond, &dri2_surf->mutex);
      pthread_mutex_unlock(&dri2_surf->mutex);

      pthread_cancel(dri2_surf->swap_queue_processor);
      pthread_join(dri2_surf->swap_queue_processor, NULL);
   }

   for (i = 0; i < ARRAY_SIZE(dri2_surf->swap_queue); i++)
      free(dri2_surf->swap_queue[i].rects);

   if (dri2_surf->front)
      dri2_dpy->image->destroyImage(dri2_surf->front);

   if (dri2_surf->back) {
      tbm_surface_internal_unref(dri2_surf->back->tbm_surf);
      dri2_surf->back->locked = false;
   }

   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (dri2_surf->color_buffers[i].dri_image)
         dri2_dpy->image->destroyImage(dri2_surf->color_buffers[i].dri_image);
      assert(!dri2_surf->color_buffers[i].locked);
   }

   if (dri2_surf->tpl_surf)
      tpl_object_unreference((tpl_object_t *) dri2_surf->tpl_surf);

   destroy_surface(surf);

   return EGL_TRUE;
}

static EGLBoolean
dri2_tizen_swap_interval(_EGLDisplay *dpy, _EGLSurface *surf, EGLint interval)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);
   tpl_result_t res;

   if (interval > surf->Config->MaxSwapInterval)
      interval = surf->Config->MaxSwapInterval;
   else if (interval < surf->Config->MinSwapInterval)
      interval = surf->Config->MinSwapInterval;

   if (interval == surf->SwapInterval)
      return EGL_TRUE;

   res = tpl_surface_set_post_interval(dri2_surf->tpl_surf, interval);
   if (res == TPL_ERROR_NONE) {
      surf->SwapInterval = interval;
      return EGL_TRUE;
   }

   return EGL_FALSE;
}

static EGLBoolean
dri2_tizen_swap_buffers_with_damage(_EGLDisplay *dpy, _EGLSurface *draw,
                                    const EGLint *rects, EGLint n_rects)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(draw);
   struct tpl_swap_queue_elem *queue_elem;
   int i;

   if (dri2_surf->base.Type != EGL_WINDOW_BIT)
      return EGL_TRUE;

   for (i = 0; i < ARRAY_SIZE(dri2_surf->color_buffers); i++) {
      if (draw->SwapBehavior == EGL_BUFFER_PRESERVED)
         dri2_surf->color_buffers[i].age = 1;
      else if (dri2_surf->color_buffers[i].age > 0)
         dri2_surf->color_buffers[i].age++;
   }

   /*
    * Make sure we have a back buffer in case we're swapping without ever
    * rendering.
    */
   if (get_back_bo(dri2_surf, false, true) < 0) {
      _eglError(EGL_BAD_ALLOC, "DRI2: failed to get back buffer");
      return EGL_FALSE;
   }

   dri2_flush_drawable_for_swapbuffers(dpy, draw);

   pthread_mutex_lock(&dri2_surf->mutex);
   queue_elem =
      &dri2_surf->swap_queue[dri2_surf->swap_queue_idx_tail];

   queue_elem->tbm_surf = dri2_surf->back->tbm_surf;

   /* Allocate space for damage rectangles in swap queue element if necessary */
   if (queue_elem->n_rects_max < n_rects) {
      free(queue_elem->rects);

      if (!queue_elem->n_rects_max)
         queue_elem->n_rects_max = TIZEN_SWAP_N_RECTS_MIN;

      while (queue_elem->n_rects_max <= n_rects)
         queue_elem->n_rects_max *= 2;

      queue_elem->rects =
         malloc(sizeof(*queue_elem->rects) * queue_elem->n_rects_max);
      if (!queue_elem->rects) {
         _eglError(EGL_BAD_ALLOC,
                   "DRI2: failed to allocate space for damage rects");
         queue_elem->n_rects_max = 0;
         pthread_mutex_unlock(&dri2_surf->mutex);
         return EGL_FALSE;
      }
   }

   /* Copy damage rectangles into swap queue element */
   STATIC_ASSERT(sizeof(*queue_elem->rects) == sizeof(*rects));
   memcpy(&queue_elem->rects[0], &rects[0], sizeof(*rects) * n_rects);
   queue_elem->n_rects = n_rects;

   dri2_surf->swap_queue_idx_tail++;
   dri2_surf->swap_queue_idx_tail %= ARRAY_SIZE(dri2_surf->color_buffers);

   /* Notify swap_queue_processor_worker that there's new work */
   pthread_cond_signal(&dri2_surf->swap_queue_cond);
   pthread_mutex_unlock(&dri2_surf->mutex);

   dri2_surf->back->age = 1;
   dri2_surf->current = dri2_surf->back;
   dri2_surf->back = NULL;

   return EGL_TRUE;
}

static EGLBoolean
dri2_tizen_swap_buffers(_EGLDisplay *dpy, _EGLSurface *draw)
{
   return dri2_tizen_swap_buffers_with_damage(dpy, draw, NULL, 0);
}

static EGLint
dri2_tizen_query_buffer_age(_EGLDisplay *dpy, _EGLSurface *surface)
{
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surface);

   if (get_back_bo(dri2_surf, false, true) < 0) {
      _eglError(EGL_BAD_ALLOC, "DRI2: failed to get back buffer");
      return -1;
   }

   return dri2_surf->back->age;
}


static EGLBoolean
dri2_tizen_query_surface(_EGLDisplay *dpy, _EGLSurface *surf,
                         EGLint attribute, EGLint *value)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   struct dri2_egl_surface *dri2_surf = dri2_egl_surface(surf);

   switch (attribute) {
   case EGL_WIDTH:
   case EGL_HEIGHT:
      if (dri2_surf->base.Type == EGL_WINDOW_BIT) {
         tpl_handle_t window;
         int width, height;
         tbm_format format;
         tpl_result_t res;

         window = tpl_surface_get_native_handle(dri2_surf->tpl_surf);

         res = tpl_display_get_native_window_info(dri2_dpy->tpl_dpy, window,
                                                  &width, &height, &format,
                                                  surf->Config->RedSize +
                                                  surf->Config->GreenSize +
                                                  surf->Config->BlueSize +
                                                  surf->Config->AlphaSize,
                                                  surf->Config->AlphaSize);
         if (res == TPL_ERROR_NONE) {
            surf->Width = width;
            surf->Height = height;
         }
      }
      break;
   default:
      break;
   }

   return _eglQuerySurface(dpy, surf, attribute, value);
}


static struct dri2_egl_display_vtbl dri2_tizen_display_vtbl = {
   .create_window_surface = dri2_tizen_create_window_surface,
   .create_pixmap_surface = dri2_tizen_create_pixmap_surface,
   .create_pbuffer_surface = dri2_tizen_create_pbuffer_surface,
   .destroy_surface = dri2_tizen_destroy_surface,
   .create_image = dri2_create_image_khr,
   .swap_interval = dri2_tizen_swap_interval,
   .swap_buffers = dri2_tizen_swap_buffers,
   .swap_buffers_with_damage = dri2_tizen_swap_buffers_with_damage,
   .query_buffer_age = dri2_tizen_query_buffer_age,
   .query_surface = dri2_tizen_query_surface,
   .get_dri_drawable = dri2_surface_get_dri_drawable,
};

static int
dri2_tizen_get_buffers(__DRIdrawable *driDrawable,
                       unsigned int format,
                       uint32_t *stamp,
                       void *loaderPrivate,
                       uint32_t buffer_mask,
                       struct __DRIimageList *buffers)
{
   struct dri2_egl_surface *dri2_surf = loaderPrivate;

   buffers->image_mask = 0;
   buffers->front = NULL;
   buffers->back = NULL;
   buffers->prev = NULL;

   if (buffer_mask & __DRI_IMAGE_BUFFER_FRONT)
      if (get_front_bo(dri2_surf) < 0)
         return 0;

   if (buffer_mask & __DRI_IMAGE_BUFFER_BACK) {
      /*
       * The DRI driver has requested the previous buffer so it will take care
       * of preserving the previous content.
       */
      bool allow_preserve = !(buffer_mask & __DRI_IMAGE_BUFFER_PREV);

      if (get_back_bo(dri2_surf, true, allow_preserve) < 0)
         return 0;
   }

   if (buffer_mask & __DRI_IMAGE_BUFFER_FRONT) {
      buffers->front = dri2_surf->front;
      buffers->image_mask |= __DRI_IMAGE_BUFFER_FRONT;
   }

   if (buffer_mask & __DRI_IMAGE_BUFFER_BACK) {
      buffers->back = dri2_surf->back->dri_image;
      buffers->image_mask |= __DRI_IMAGE_BUFFER_BACK;
   }

   if (buffer_mask & __DRI_IMAGE_BUFFER_PREV) {
      if (dri2_surf->base.SwapBehavior == EGL_BUFFER_PRESERVED &&
          dri2_surf->current) {
         buffers->prev = dri2_surf->current->dri_image;
         buffers->image_mask |= __DRI_IMAGE_BUFFER_PREV;
      }
   }

   return 1;
}

static void
dri2_tizen_flush_front_buffer(__DRIdrawable * driDrawable, void *loaderPrivate)
{
}

static unsigned
dri2_tizen_get_capability(void *loaderPrivate, enum dri_loader_cap cap)
{
   switch (cap) {
   case DRI_LOADER_CAP_YUV_SURFACE_IMG:
      return 1;
   default:
      return 0;
   }
}

static const __DRIimageLoaderExtension tizen_image_loader_extension = {
   .base             = { __DRI_IMAGE_LOADER, 2 },
   .getBuffers       = dri2_tizen_get_buffers,
   .flushFrontBuffer = dri2_tizen_flush_front_buffer,
   .getCapability    = dri2_tizen_get_capability,
};

static const __DRIextension *image_loader_extensions[] = {
   &tizen_image_loader_extension.base,
   &image_lookup_extension.base,
   NULL,
};

static EGLBoolean
derive_yuv_native_visual_from_config(struct dri2_egl_display *dri2_dpy,
                                     const __DRIconfig *dri_config,
                                     int *native_visual)
{
   unsigned order, subsample, num_planes, plane_bpp;

   dri2_dpy->core->getConfigAttrib(dri_config, __DRI_ATTRIB_YUV_ORDER,
                                   &order);
   dri2_dpy->core->getConfigAttrib(dri_config, __DRI_ATTRIB_YUV_SUBSAMPLE,
                                   &subsample);
   dri2_dpy->core->getConfigAttrib(dri_config, __DRI_ATTRIB_YUV_NUMBER_OF_PLANES,
                                   &num_planes);
   dri2_dpy->core->getConfigAttrib(dri_config, __DRI_ATTRIB_YUV_PLANE_BPP,
                                   &plane_bpp);

   if ((plane_bpp & __DRI_ATTRIB_YUV_PLANE_BPP_8_BIT) == 0)
      return EGL_FALSE;

   if (num_planes != 2)
      return EGL_FALSE;

   if (subsample & __DRI_ATTRIB_YUV_SUBSAMPLE_4_2_0_BIT) {
      if (order & __DRI_ATTRIB_YUV_ORDER_YUV_BIT) {
         *native_visual = TBM_FORMAT_NV12;
         return EGL_TRUE;
      } else if (order & __DRI_ATTRIB_YUV_ORDER_YVU_BIT) {
         *native_visual = TBM_FORMAT_NV21;
         return EGL_TRUE;
      }
   }

   return EGL_FALSE;
}

static EGLBoolean
dri2_tizen_add_configs(_EGLDisplay *dpy)
{
   struct dri2_egl_display *dri2_dpy = dri2_egl_display(dpy);
   int count = 0;

   for (unsigned i = 0; dri2_dpy->driver_configs[i]; i++) {
      struct dri2_egl_config *dri2_cfg;
      unsigned int render_type;
      unsigned int caveat = 0;
      int surface_type = 0;
      EGLint attr_list[] = {
         EGL_NATIVE_VISUAL_ID, 0,
         EGL_CONFIG_CAVEAT, EGL_NONE,
         EGL_NONE,
      };

      dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[i],
                                      __DRI_ATTRIB_CONFIG_CAVEAT, &caveat);

      dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[i],
                                      __DRI_ATTRIB_RENDER_TYPE, &render_type);

      if (render_type & __DRI_ATTRIB_RGBA_BIT) {
         static const struct rgba_shifts_and_sizes pbuffer_sasa[] = {
            {
               /* ARGB8888 */
               { 16, 8, 0, 24 },
               { 8, 8, 8, 8 },
            },
            {
               /* RGB888 */
               { 16, 8, 0, -1 },
               { 8, 8, 8, 0 },
            },
            {
               /* RGB565 */
               { 11, 5, 0, -1 },
               { 5, 6, 5, 0 },
            },
         };
         int shifts[4];
         unsigned int sizes[4];
         unsigned int depth;
         tpl_bool_t is_slow;
         tpl_result_t res;

         dri2_get_shifts_and_sizes(dri2_dpy->core, dri2_dpy->driver_configs[i],
                                   shifts, sizes);

         dri2_dpy->core->getConfigAttrib(dri2_dpy->driver_configs[i],
                                         __DRI_ATTRIB_BUFFER_SIZE, &depth);

         res = tpl_display_query_config(dri2_dpy->tpl_dpy,
                                        TPL_SURFACE_TYPE_WINDOW,
                                        sizes[0], sizes[1], sizes[2], sizes[3],
                                        depth, &attr_list[1], &is_slow);
         if (res != TPL_ERROR_NONE)
            continue;

         surface_type |= EGL_WINDOW_BIT;

         if (is_slow)
            caveat |= __DRI_ATTRIB_SLOW_BIT;

         res = tpl_display_query_config(dri2_dpy->tpl_dpy,
                                        TPL_SURFACE_TYPE_PIXMAP,
                                        sizes[0], sizes[1], sizes[2], sizes[3],
                                        depth, NULL, &is_slow);
         if (res == TPL_ERROR_NONE) {
            surface_type |= EGL_PIXMAP_BIT;

            if (is_slow)
               caveat |= __DRI_ATTRIB_SLOW_BIT;
         }

         for (unsigned j = 0; j < ARRAY_SIZE(pbuffer_sasa); j++) {
            const struct rgba_shifts_and_sizes *pbuffer_sas = &pbuffer_sasa[j];

            if (shifts[0] == pbuffer_sas->shifts[0] &&
                shifts[1] == pbuffer_sas->shifts[1] &&
                shifts[2] == pbuffer_sas->shifts[2] &&
                shifts[3] == pbuffer_sas->shifts[3] &&
                sizes[0] == pbuffer_sas->sizes[0] &&
                sizes[1] == pbuffer_sas->sizes[1] &&
                sizes[2] == pbuffer_sas->sizes[2] &&
                sizes[3] == pbuffer_sas->sizes[3]) {
               surface_type |= EGL_PBUFFER_BIT;
               break;
            }

         }

         if (dri2_dpy->image->base.version >= 9 && dri2_dpy->image->blitImage)
            surface_type |= EGL_SWAP_BEHAVIOR_PRESERVED_BIT;
      } else if (render_type & __DRI_ATTRIB_YUV_BIT) {
         if (!derive_yuv_native_visual_from_config(dri2_dpy,
                                                   dri2_dpy->driver_configs[i],
                                                   &attr_list[1]))
            continue;
         surface_type = EGL_WINDOW_BIT;
         caveat = 0;
      }

      if (caveat & __DRI_ATTRIB_NON_CONFORMANT_CONFIG)
         attr_list[3] = EGL_NON_CONFORMANT_CONFIG;
      else if (caveat & __DRI_ATTRIB_SLOW_BIT)
         attr_list[3] = EGL_SLOW_CONFIG;

      dri2_cfg = dri2_add_config(dpy, dri2_dpy->driver_configs[i], count + 1,
                                 surface_type, &attr_list[0], NULL, NULL);
      if (dri2_cfg)
         count++;
   }

   return count != 0;
}

EGLBoolean
dri2_initialize_tizen(_EGLDisplay *dpy)
{
   struct dri2_egl_display *dri2_dpy;
   int i;
   int err;

   dri2_dpy = calloc(1, sizeof(*dri2_dpy));
   if (!dri2_dpy)
      return _eglError(EGL_BAD_ALLOC, "DRI2: failed to create display");

   dpy->DriverData = (void *) dri2_dpy;
   dri2_dpy->fd = -1;

   dri2_dpy->tpl_dpy =
      tpl_display_create(TPL_BACKEND_UNKNOWN, dpy->PlatformDisplay);
   if (!dri2_dpy->tpl_dpy) {
      _eglError(EGL_NOT_INITIALIZED, "DRI2: failed to create tpl display");
      goto cleanup;
   }

   err = pthread_mutex_init(&dri2_dpy->image_list_mutex, NULL);
   if (err)
      goto cleanup;
   dri2_dpy->image_list_mutex_initialized = true;

   for (i = TIZEN_DRM_RENDER_MINOR_START; i <= TIZEN_DRM_RENDER_MINOR_MAX; i++) {
      char *render_path;

      if (asprintf(&render_path, DRM_RENDER_DEV_NAME, DRM_DIR_NAME, i) < 0)
         continue;

      dri2_dpy->fd = loader_open_device(render_path);
      free(render_path);
      if (dri2_dpy->fd < 0)
         continue;

      dri2_dpy->driver_name = loader_get_driver_for_fd(dri2_dpy->fd);
      if (dri2_dpy->driver_name) {
         if (dri2_load_driver_dri3(dpy)) {
            _EGLDevice *dev = _eglAddDevice(dri2_dpy->fd, false);
            if (!dev) {
               dlclose(dri2_dpy->driver);
               _eglLog(_EGL_WARNING, "DRI2: failed to find EGLDevice");
            } else {
               dri2_dpy->own_device = 1;
               dpy->Device = dev;
               break;
	    }
         }

         free(dri2_dpy->driver_name);
         dri2_dpy->driver_name = NULL;
      }

      close(dri2_dpy->fd);
      dri2_dpy->fd = -1;
   }

   if (dri2_dpy->fd < 0) {
      _eglError(EGL_NOT_INITIALIZED, "DRI2: failed to load driver");
      goto cleanup;
   }

   dri2_dpy->loader_extensions = &image_loader_extensions[0];

   if (!dri2_create_screen(dpy)) {
      _eglError(EGL_NOT_INITIALIZED, "DRI2: failed to create screen");
      goto cleanup;
   }

   if (!dri2_setup_extensions(dpy))
      goto cleanup;

   dri2_setup_screen(dpy);

   if (!dri2_tizen_add_configs(dpy)) {
      _eglError(EGL_NOT_INITIALIZED, "DRI2: failed to add configs");
      goto cleanup;
   }

   dpy->Extensions.EXT_buffer_age = EGL_TRUE;
   dpy->Extensions.EXT_swap_buffers_with_damage = EGL_TRUE;
   dpy->Extensions.KHR_image_base = EGL_TRUE;
   dpy->Extensions.WL_bind_wayland_display = EGL_TRUE;
   dpy->Extensions.TIZEN_image_native_surface = EGL_TRUE;

   /*
    * Fill vtbl last to prevent accidentally calling virtual function during
    * initialization.
    */
   dri2_dpy->vtbl = &dri2_tizen_display_vtbl;

   return EGL_TRUE;

cleanup:
   dri2_display_destroy(dpy);
   return EGL_FALSE;
}
