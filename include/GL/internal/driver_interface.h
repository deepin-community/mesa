/*
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

#ifndef DRIVER_INTERFACE_H
#define DRIVER_INTERFACE_H

#include "dri_interface.h"
#include "main/menums.h"

struct dri_screen;
struct dri_context;
struct dri_drawable;
struct gl_config;
struct __DriverContextConfig;

typedef struct __DRIDriverAPIExtensionRec __DRIDriverAPIExtension;

#define __DRI_DRIVER_API "DRI_API_Driver"
#define __DRI_DRIVER_API_VERSION 1

struct __DRIDriverAPIExtensionRec {
   __DRIextension base;

   const __DRIconfig **(*initScreen)(struct dri_screen *screen);

   void (*destroyScreen)(struct dri_screen *screen);

   GLboolean (*createContext)(gl_api api,
                              const struct gl_config *visual,
                              struct dri_context *context,
                              const struct __DriverContextConfig *ctx_config,
                              unsigned int *error,
                              struct dri_context *shared_context);

  void (*destroyContext)(struct dri_context *context);

  GLboolean (*makeCurrent)(struct dri_context *context,
                           struct dri_drawable *draw,
                           struct dri_drawable *read);

  void (*unbindContext)(struct dri_context *context);

  GLboolean (*createBuffer)(struct dri_screen *screen,
                            struct dri_drawable *drawable,
                            const struct gl_config *visual,
                            GLboolean is_pixmap);

  void (*destroyBuffer)(struct dri_drawable *drawable);

  __DRIbuffer *(*allocateBuffer) (struct dri_screen *screen,
                                  unsigned int attachment,
                                  unsigned int format,
                                  int width, int height);

  void (*releaseBuffer) (struct dri_screen *screen, __DRIbuffer *buffer);
};

#endif /* DRIVER_INTERFACE_H */
