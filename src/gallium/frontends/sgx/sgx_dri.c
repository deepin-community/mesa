/*
 * Copyright (c) Imagination Technologies Ltd.
 *
 * The contents of this file are subject to the MIT license as set out below.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <xf86drm.h>

#include <util/driconf.h>

#include "EGL/egl.h"
#include "EGL/eglext.h"

#include "dri_util.h"
#include "dri_helpers.h"
#include "dri_drawable.h"
#include "pipe-loader/pipe_loader.h"
#include "loader/loader.h"

#include "git_sha1.h"
#include "GL/internal/mesa_interface.h"

#include "sgx_dri.h"
#include "sgx_mesa.h"

#define PVR_IMAGE_LOADER_VER_MIN 1
#define PVR_DRI2_LOADER_VER_MIN 3

struct PVRBuffer {
   __DRIbuffer sDRIBuffer;
   PVRDRIBufferImpl *psImpl;
};

/* We need to know the current screen in order to lookup EGL images. */
static __thread PVRDRIScreen *gpsPVRScreen;

/*************************************************************************//*!
 Global functions
 *//**************************************************************************/

void PVRDRIThreadSetCurrentScreen(PVRDRIScreen *psPVRScreen)
{
   gpsPVRScreen = psPVRScreen;
}

PVRDRIScreen* PVRDRIThreadGetCurrentScreen(void)
{
   return gpsPVRScreen;
}

/***********************************************************************************
 Function Name : PVRDRIScreenLock
 Inputs     : psPVRScreen - PVRDRI screen structure
 Returns : Boolean
 Description   : Lock screen mutex (can be called recursively)
 ************************************************************************************/
void PVRDRIScreenLock(PVRDRIScreen *psPVRScreen)
{
   int res = pthread_mutex_lock(&psPVRScreen->sMutex);
   if (res != 0) {
      mesa_loge("%s: Failed to lock screen (%d)\n", __func__, res);
      abort();
   }
}

/***********************************************************************************
 Function Name : PVRDRIScreenUnlock
 Inputs     : psPVRScreen - PVRDRI screen structure
 Returns : Boolean
 Description   : Unlock screen mutex (can be called recursively)
 ************************************************************************************/
void PVRDRIScreenUnlock(PVRDRIScreen *psPVRScreen)
{
   int res = pthread_mutex_unlock(&psPVRScreen->sMutex);
   if (res != 0) {
      mesa_loge("%s: Failed to unlock screen (%d)\n", __func__, res);
      abort();
   }
}

/*************************************************************************//*!
 Local functions
 *//**************************************************************************/

static bool
PVRLoaderIsSupported(struct dri_screen *psDRIScreen)
{
   if (psDRIScreen->image.loader) {
      if (psDRIScreen->image.loader->base.version < PVR_IMAGE_LOADER_VER_MIN) {
         mesa_logd("%s: Image loader extension version %d but need %d",
                          __func__, psDRIScreen->image.loader->base.version,
                          PVR_IMAGE_LOADER_VER_MIN);
         return false;
      } else if (!psDRIScreen->image.loader->getBuffers) {
         mesa_logd("%s: Image loader extension missing support for getBuffers",
                          __func__);
         return false;
      }
   } else if (psDRIScreen->dri2.loader) {
      if (psDRIScreen->dri2.loader->base.version < PVR_DRI2_LOADER_VER_MIN)
      {
         mesa_logd("%s: DRI2 loader extension version %d but need %d",
               __func__,
               psDRIScreen->dri2.loader->base.version,
               PVR_DRI2_LOADER_VER_MIN);
         return false;
      }
      else if (!psDRIScreen->dri2.loader->getBuffersWithFormat)
      {
         mesa_logd("%s: DRI2 loader extension missing support for getBuffersWithFormat",
               __func__);
         return false;
      }
   } else {
      mesa_logd("%s: Missing required loader extension (need "
            "either the image or DRI2 loader extension)",
            __func__);
      return false;
   }

   return true;
}

static bool PVRMutexInit(pthread_mutex_t *psMutex, int iType)
{
   pthread_mutexattr_t sMutexAttr;
   int res;

   res = pthread_mutexattr_init(&sMutexAttr);
   if (res != 0)
         {
      mesa_loge("%s: pthread_mutexattr_init failed (%d)",
            __func__,
            res);
      return false;
   }

   res = pthread_mutexattr_settype(&sMutexAttr, iType);
   if (res != 0)
         {
      mesa_loge("%s: pthread_mutexattr_settype failed (%d)",
            __func__,
            res);
      goto ErrorMutexAttrDestroy;
   }

   res = pthread_mutex_init(psMutex, &sMutexAttr);
   if (res != 0)
         {
      mesa_loge("%s: pthread_mutex_init failed (%d)",
            __func__,
            res);
      goto ErrorMutexAttrDestroy;
   }

   (void) pthread_mutexattr_destroy(&sMutexAttr);

   return true;

   ErrorMutexAttrDestroy:
   (void) pthread_mutexattr_destroy(&sMutexAttr);

   return false;
}

static void PVRMutexDeinit(pthread_mutex_t *psMutex)
{
   int res;

   res = pthread_mutex_destroy(psMutex);
   if (res != 0)
         {
      mesa_loge("%s: pthread_mutex_destroy failed (%d)",
            __func__,
            res);
   }
}

static inline bool
PVRDRIFlushBuffers(PVRDRIContext *psPVRContext,
      PVRDRIDrawable *psPVRDrawable,
      bool bFlushAllSurfaces,
      bool bSwapBuffers,
      bool bWaitForHW)
{
   return PVRDRIEGLFlushBuffers(psPVRContext->eAPI,
         psPVRContext->psPVRScreen->psImpl,
         psPVRContext->psImpl,
         psPVRDrawable ? psPVRDrawable->psImpl : NULL,
         bFlushAllSurfaces,
         bSwapBuffers,
         bWaitForHW);
}

static inline bool
PVRDRIFlushBuffersAndWait(PVRDRIContext *psPVRContext)
{
   return PVRDRIFlushBuffers(psPVRContext, NULL, true, false, true);
}

static inline bool
PVRDRIFlushContextForSwapNoWait(PVRDRIContext *psPVRContext,
      PVRDRIDrawable *psPVRDrawable)
{
   return PVRDRIFlushBuffers(psPVRContext, psPVRDrawable, false, true, false);
}

static inline bool
PVRDRIFlushBuffersForSwapAndWait(PVRDRIContext *psPVRContext)
{
   return PVRDRIFlushBuffers(psPVRContext, NULL, true, true, true);
}

static void
PVRDRIFlushDrawable(PVRDRIDrawable *psPVRDrawable,
bool bSwapBuffers,
      PVRDRIContext *psPVRSwapContext)
{
   PVRQElem *psQElem = psPVRDrawable->sPVRContextHead.pvForw;

   while (psQElem != &psPVRDrawable->sPVRContextHead)
   {
      PVRDRIContext *psPVRContext = PVRQ_CONTAINER_OF(psQElem, PVRDRIContext, sQElem);

      if (bSwapBuffers && (psPVRContext == psPVRSwapContext || psPVRSwapContext == NULL))
            {
         (void) PVRDRIFlushBuffersForSwapAndWait(psPVRContext);
      }
      else
      {
         (void) PVRDRIFlushBuffersAndWait(psPVRContext);
      }

      psQElem = psPVRContext->sQElem.pvForw;
   }
}

static void
PVRDRIFlushDrawableForSwapNoWait(PVRDRIDrawable *psPVRDrawable)
{
   PVRQElem *psQElem = psPVRDrawable->sPVRContextHead.pvForw;

   while (psQElem != &psPVRDrawable->sPVRContextHead)
   {
      PVRDRIContext *psPVRContext = PVRQ_CONTAINER_OF(psQElem, PVRDRIContext, sQElem);

      (void) PVRDRIFlushContextForSwapNoWait(psPVRContext, psPVRDrawable);

      psQElem = psPVRContext->sQElem.pvForw;
   }
}

static bool
PVRDRICacheFlushSurfaces(bool bSwapBuffers,
      PVRDRIContext *psPVRSwapContext,
      PVRDRIDrawable *psPVRDrawable)
{
   if (PVRQIsEmpty(&psPVRDrawable->sCacheFlushHead))
         {
      return false;
   }

   PVRDRIFlushDrawable(psPVRDrawable, bSwapBuffers, psPVRSwapContext);

   while (!PVRQIsEmpty(&psPVRDrawable->sCacheFlushHead))
   {
      PVRDRIBuffer *psPVRDRIBuffer = PVRQ_CONTAINER_OF(psPVRDrawable->sCacheFlushHead.pvForw,
            PVRDRIBuffer,
            sCacheFlushElem);

      PVRQDequeue(&psPVRDRIBuffer->sCacheFlushElem);

      switch (psPVRDRIBuffer->eBackingType)
      {
      case PVRDRI_BUFFER_BACKING_DRI2:
         PVRDRIBufferDestroy(psPVRDRIBuffer->uBacking.sDRI2.psBuffer);
         break;
      case PVRDRI_BUFFER_BACKING_IMAGE:
         PVRDRIUnrefImage(psPVRDRIBuffer->uBacking.sImage.psImage);
         break;
      default:
         assert(0);
         continue;
      }

      free(psPVRDRIBuffer);
   }

   return true;
}

bool
PVRDRIFlushBuffersForSwap(PVRDRIContext *psPVRContext, PVRDRIDrawable *psPVRDrawable)
{
   if (PVRDRICacheFlushSurfaces(true, psPVRContext, psPVRDrawable))
      return true;

   if (psPVRContext != NULL)
      return PVRDRIFlushContextForSwapNoWait(psPVRContext, psPVRDrawable);

   PVRDRIFlushDrawableForSwapNoWait(psPVRDrawable);

   return true;
}

static bool
PVRDRIFlushBuffersGC(PVRDRIContext *psPVRContext)
{
   if (psPVRContext->psPVRDrawable != NULL)
   {
      if (PVRDRICacheFlushSurfaces(false, NULL, psPVRContext->psPVRDrawable))
            {
         return true;
      }
   }

   return PVRDRIFlushBuffersAndWait(psPVRContext);
}

static void PVRDRIDisplayFrontBuffer(PVRDRIDrawable *psPVRDrawable)
{
   if (!psPVRDrawable->bDoubleBuffered)
   {
      PVRDRIScreen *psPVRScreen = psPVRDrawable->psPVRScreen;
      struct dri_screen *psDRIScreen = dri_screen(psPVRScreen->psDRIScreen);

      /* Only double buffered drawables should need flushing */
      assert(PVRQIsEmpty(&psPVRDrawable->sCacheFlushHead));

      if (psDRIScreen->image.loader && psDRIScreen->image.loader->flushFrontBuffer)
            {
         psDRIScreen->image.loader->flushFrontBuffer(psPVRDrawable->psDRIDrawable,
               dri_drawable(psPVRDrawable->psDRIDrawable)->loaderPrivate);
      }
      else if (psDRIScreen->dri2.loader && psDRIScreen->dri2.loader->flushFrontBuffer)
            {
         psDRIScreen->dri2.loader->flushFrontBuffer(psPVRDrawable->psDRIDrawable,
               dri_drawable(psPVRDrawable->psDRIDrawable)->loaderPrivate);
      }
   }
}

static void
PVRContextUnbind(PVRDRIContext *psPVRContext, bool bMakeUnCurrent, bool bMarkSurfaceInvalid)
{
   if (bMakeUnCurrent || psPVRContext->psPVRDrawable != NULL)
   {
      (void) PVRDRIFlushBuffersGC(psPVRContext);
   }

   if (bMakeUnCurrent)
   {
      PVRDRIMakeUnCurrentGC(psPVRContext->eAPI,
            psPVRContext->psPVRScreen->psImpl);
   }

   if (psPVRContext->psPVRDrawable != NULL)
   {
      if (bMarkSurfaceInvalid)
      {
         PVRDRIEGLMarkRendersurfaceInvalid(psPVRContext->eAPI,
               psPVRContext->psPVRScreen->psImpl,
               psPVRContext->psImpl);
      }

      psPVRContext->psPVRDrawable = NULL;
   }

   PVRQDequeue(&psPVRContext->sQElem);
}

static inline PVRDRIContextImpl*
getSharedContextImpl(struct dri_context *psSharedContext)
{
   if (psSharedContext == NULL)
      return NULL;
   PVRDRIContext *psPVRContext = psSharedContext->driverPrivate;

   return psPVRContext->psImpl;
}

static void
PVRDRIConfigFromMesa(PVRDRIConfigInfo *psConfigInfo, const struct gl_config *psGLMode)
{
   memset(psConfigInfo, 0, sizeof(*psConfigInfo));

   if (psGLMode)
   {
      psConfigInfo->samples = psGLMode->samples;
      psConfigInfo->redBits = psGLMode->redBits;
      psConfigInfo->greenBits = psGLMode->greenBits;
      psConfigInfo->blueBits = psGLMode->blueBits;
      psConfigInfo->alphaBits = psGLMode->alphaBits;
      psConfigInfo->rgbBits = psGLMode->rgbBits;
      psConfigInfo->depthBits = psGLMode->depthBits;
      psConfigInfo->stencilBits = psGLMode->stencilBits;
      psConfigInfo->doubleBufferMode = psGLMode->doubleBufferMode;

      psConfigInfo->sampleBuffers = psGLMode->samples > 0 ? 1 : 0;
      psConfigInfo->bindToTextureRgb = GL_TRUE;
      psConfigInfo->bindToTextureRgba = GL_TRUE;
   }
}

static void
PVRDRIScreenAddReference(PVRDRIScreen *psPVRScreen)
{
   int iRefCount = __sync_fetch_and_add(&psPVRScreen->iRefCount, 1);
   (void) iRefCount;
   assert(iRefCount > 0);
}

static void
PVRDRIScreenRemoveReference(PVRDRIScreen *psPVRScreen)
{
   int iRefCount = __sync_sub_and_fetch(&psPVRScreen->iRefCount, 1);

   assert(iRefCount >= 0);

   if (iRefCount != 0)
      return;

   pvrdri_free_dispatch_tables(psPVRScreen);

   (void) PVRDRIEGLFreeResources(psPVRScreen->psImpl);

   PVRDRIDestroyFencesImpl(psPVRScreen->psImpl);

   PVRDRIDestroyScreenImpl(psPVRScreen->psImpl);
   PVRMutexDeinit(&psPVRScreen->sMutex);

   PVRDRICompatDeinit();
   free(psPVRScreen);
}

static void
PVRDrawableUnbindContexts(PVRDRIDrawable *psPVRDrawable)
{
   PVRQElem *psQElem = psPVRDrawable->sPVRContextHead.pvForw;

   while (psQElem != &psPVRDrawable->sPVRContextHead)
   {
      PVRDRIContext *psPVRContext = PVRQ_CONTAINER_OF(psQElem,
            PVRDRIContext,
            sQElem);

      /* Get the next element in the list now, as the list will be modified */
      psQElem = psPVRContext->sQElem.pvForw;

      /* Draw surface? */
      if (psPVRContext->psPVRDrawable == psPVRDrawable)
            {
         PVRContextUnbind(psPVRContext, false, true);
      }
      /* Pixmap? */
      else
      {
         (void) PVRDRIFlushBuffersGC(psPVRContext);
         PVRQDequeue(&psPVRContext->sQElem);
      }
   }
}

static void
PVRScreenPrintExtensions(struct dri_screen *psDRIScreen)
{
   /* Don't attempt to print anything if LIBGL_DEBUG isn't in the environment */
   if (getenv("LIBGL_DEBUG") == NULL)
      return;

   if (psDRIScreen->extensions) {
      const __DRIextension *psScreenExtensionVersionInfo =
         SGXDRIScreenExtensionVersionInfo();
      int i;
      int j;

      mesa_logd("Supported screen extensions:");

      for (i = 0; psDRIScreen->extensions[i]; i++) {
         for (j = 0; psScreenExtensionVersionInfo[j].name; j++) {
            if (strcmp(psDRIScreen->extensions[i]->name,
                       psScreenExtensionVersionInfo[j].name) == 0) {
               mesa_logd("\t%s (supported version: %u - max version: %u)",
                                psDRIScreen->extensions[i]->name,
                                psDRIScreen->extensions[i]->version,
                                psScreenExtensionVersionInfo[j].version);
               break;
            }
         }

         if (psScreenExtensionVersionInfo[j].name == NULL) {
            mesa_logd("\t%s (supported version: %u - max version: unknown)",
                             psDRIScreen->extensions[i]->name,
                             psDRIScreen->extensions[i]->version);
         }
      }
   } else {
      mesa_logd("No psDRIScreen extensions found");
   }
}

/*************************************************************************//*!
 Mesa driver API functions
 *//**************************************************************************/

static const __DRIconfig **
PVRDRIInitScreen(struct dri_screen *psDRIScreen)
{
   PVRDRIScreen *psPVRScreen;
   const __DRIconfig **ppsConfigs;
   const struct SGXDRICallbacksV0 sDRICallbacks = {
         .v0.DrawableRecreate = PVRDRIDrawableRecreate,
         .v0.DrawableGetParameters = PVRDRIDrawableGetParameters,
         .v0.ImageGetSharedType = PVRDRIImageGetSharedType,
         .v0.ImageGetSharedBuffer = PVRDRIImageGetSharedBuffer,
         .v0.ImageGetSharedEGLImage = PVRDRIImageGetSharedEGLImage,
         .v0.ImageGetEGLImage = PVRDRIImageGetEGLImage,
         .v0.ScreenGetDRIImage = PVRDRIScreenGetDRIImage,
         .v0.RefImage = PVRDRIRefImage,
         .v0.UnrefImage = PVRDRIUnrefImage,
         .v0.RegisterSupportInterface = MODSUPRegisterSupportInterfaceV0,
   };

   if (!PVRLoaderIsSupported(psDRIScreen))
      return NULL;

   if (!PVRDRICompatInit(&sDRICallbacks, 0, 0))
      return NULL;

   psPVRScreen = calloc(1, sizeof(*psPVRScreen));
   if (psPVRScreen == NULL) {
      mesa_loge("%s: Couldn't allocate PVRDRIScreen", __func__);
      goto ErrorCompatDeinit;
   }

   psDRIScreen->driverPrivate = psPVRScreen;
   psPVRScreen->psDRIScreen = opaque_dri_screen(psDRIScreen);

   /*
    * KEGLGetDrawableParameters could be called with the mutex either
    * locked or unlocked, hence the use of a recursive mutex.
    */
   if (!PVRMutexInit(&psPVRScreen->sMutex, PTHREAD_MUTEX_RECURSIVE)) {
      mesa_loge("%s: Screen mutex initialisation failed",
            __func__);
      goto ErrorScreenFree;
   }

   psPVRScreen->iRefCount = 1;
   psPVRScreen->bUseInvalidate = (psDRIScreen->dri2.useInvalidate != NULL);

   psDRIScreen->extensions = SGXDRIScreenExtensions();

   psPVRScreen->psImpl = PVRDRICreateScreenImpl(psDRIScreen->fd);
   if (psPVRScreen->psImpl == NULL)
      goto ErrorScreenMutexDeinit;

   /*
    * OpenGL doesn't support concurrent EGL displays so only advertise
    * OpenGL support for the first display.
    */
   if (PVRDRIIsFirstScreen(psPVRScreen->psImpl)) {
      psDRIScreen->max_gl_compat_version =
         PVRDRIAPIVersion(PVRDRI_API_GL, PVRDRI_API_SUB_GL_COMPAT, psPVRScreen->psImpl);
      psDRIScreen->max_gl_core_version =
         PVRDRIAPIVersion(PVRDRI_API_GL, PVRDRI_API_SUB_GL_CORE, psPVRScreen->psImpl);
   }

   psDRIScreen->max_gl_es1_version =
      PVRDRIAPIVersion(PVRDRI_API_GLES1, PVRDRI_API_SUB_NONE, psPVRScreen->psImpl);

   psDRIScreen->max_gl_es2_version =
      PVRDRIAPIVersion(PVRDRI_API_GLES2, PVRDRI_API_SUB_NONE, psPVRScreen->psImpl);

   ppsConfigs = PVRDRICreateConfigs();
   if (ppsConfigs == NULL)
   {
      mesa_loge("%s: No framebuffer configs", __func__);
      goto ErrorScreenImplDeinit;
   }

   PVRScreenPrintExtensions(psDRIScreen);

   return ppsConfigs;

ErrorScreenImplDeinit:
   PVRDRIDestroyScreenImpl(psPVRScreen->psImpl);

ErrorScreenMutexDeinit:
   PVRMutexDeinit(&psPVRScreen->sMutex);

ErrorScreenFree:
   free(psPVRScreen);

ErrorCompatDeinit:
   PVRDRICompatDeinit();

   return NULL;
}

static void
PVRDRIDestroyScreen(struct dri_screen *psDRIScreen)
{
   PVRDRIScreen *psPVRScreen = psDRIScreen->driverPrivate;

#if defined(DEBUG)
   if (psPVRScreen->iRefCount > 1) {
      mesa_loge("%s: PVRDRIScreen resources will not be freed until its %d references are removed",
                   __func__, psPVRScreen->iRefCount - 1);
   }
#endif

   PVRDRIScreenRemoveReference(psPVRScreen);
}

static EGLint
PVRDRIScreenSupportedAPIs(PVRDRIScreen *psPVRScreen)
{
   unsigned int api_mask = dri_screen(psPVRScreen->psDRIScreen)->api_mask;
   EGLint supported = 0;

   if ((api_mask & (1 << __DRI_API_GLES)) != 0)
      supported |= PVRDRI_API_BIT_GLES;

   if ((api_mask & (1 << __DRI_API_GLES2)) != 0)
      supported |= PVRDRI_API_BIT_GLES2;

   if ((api_mask & (1 << __DRI_API_GLES3)) != 0)
      supported |= PVRDRI_API_BIT_GLES3;

   if ((api_mask & (1 << __DRI_API_OPENGL)) != 0)
      supported |= PVRDRI_API_BIT_GL;

   if ((api_mask & (1 << __DRI_API_OPENGL_CORE)) != 0)
      supported |= PVRDRI_API_BIT_GL;

   return supported;
}

static GLboolean
PVRDRICreateContext(gl_api eMesaAPI, const struct gl_config *psGLMode,
                    struct dri_context *psDRIContext,
                    const struct __DriverContextConfig *psCtxConfig,
                    unsigned int *puError, struct dri_context *psSharedContext)
{
   struct dri_screen *psDRIScreen = psDRIContext->screen;
   PVRDRIScreen *psPVRScreen = psDRIScreen->driverPrivate;
   PVRDRIContext *psPVRContext;
   unsigned uPriority;
   PVRDRIAPISubType eAPISub = PVRDRI_API_SUB_NONE;
   PVRDRIConfigInfo sConfigInfo;
   bool bResult;

   psPVRContext = calloc(1, sizeof(*psPVRContext));
   if (psPVRContext == NULL) {
      mesa_loge("%s: Couldn't allocate PVRDRIContext", __func__);
      *puError = __DRI_CTX_ERROR_NO_MEMORY;
      return GL_FALSE;
   }

   psPVRContext->psDRIContext = opaque_dri_context(psDRIContext);
   psPVRContext->psPVRScreen = psPVRScreen;

#if defined(__DRI_PRIORITY)
	uPriority = psDRIContext->priority;
#else
   uPriority = PVRDRI_CONTEXT_PRIORITY_MEDIUM;
#endif

   switch (eMesaAPI) {
   case API_OPENGL_COMPAT:
      psPVRContext->eAPI = PVRDRI_API_GL;
      eAPISub = PVRDRI_API_SUB_GL_COMPAT;
      break;
   case API_OPENGL_CORE:
      psPVRContext->eAPI = PVRDRI_API_GL;
      eAPISub = PVRDRI_API_SUB_GL_CORE;
      break;
   case API_OPENGLES:
      psPVRContext->eAPI = PVRDRI_API_GLES1;
      break;
   case API_OPENGLES2:
      psPVRContext->eAPI = PVRDRI_API_GLES2;
      break;
   default:
      mesa_loge("%s: Unsupported API: %d",
                       __func__, (int) eMesaAPI);
      goto ErrorContextFree;
   }

   PVRDRIConfigFromMesa(&sConfigInfo, psGLMode);

   *puError = PVRDRICreateContextImpl(&psPVRContext->psImpl,
         psPVRContext->eAPI,
         eAPISub,
         psPVRScreen->psImpl,
         &sConfigInfo,
         psCtxConfig->major_version,
         psCtxConfig->minor_version,
         psCtxConfig->flags,
         false, // FIXME
         uPriority,
         getSharedContextImpl(psSharedContext));
   if (*puError != __DRI_CTX_ERROR_SUCCESS)
      goto ErrorContextFree;

   /*
    * The dispatch table must be created after the psDRIContext, because
    * PVRDRIContextCreate loads the API library, and we need the
    * library handle to populate the dispatch table.
    */
   PVRDRIScreenLock(psPVRScreen);
   bResult = pvrdri_create_dispatch_table(psPVRScreen, psPVRContext->eAPI);
   PVRDRIScreenUnlock(psPVRScreen);

   if (!bResult) {
      mesa_loge("%s: Couldn't create dispatch table", __func__);
      *puError = __DRI_CTX_ERROR_BAD_API;
      goto ErrorContextDestroy;
   }

   psDRIContext->driverPrivate = (void*) psPVRContext;
   PVRDRIScreenAddReference(psPVRScreen);

   *puError = __DRI_CTX_ERROR_SUCCESS;

   return GL_TRUE;

   ErrorContextDestroy:
   PVRDRIDestroyContextImpl(psPVRContext->psImpl,
         psPVRContext->eAPI,
         psPVRScreen->psImpl);
   ErrorContextFree:
   free(psPVRContext);

   return GL_FALSE;
}

static void
PVRDRIDestroyContext(struct dri_context *psDRIContext)
{
   PVRDRIContext *psPVRContext = psDRIContext->driverPrivate;
   PVRDRIScreen *psPVRScreen = psPVRContext->psPVRScreen;

   PVRDRIScreenLock(psPVRScreen);

   PVRContextUnbind(psPVRContext, false, false);

   PVRDRIDestroyContextImpl(psPVRContext->psImpl,
         psPVRContext->eAPI,
         psPVRScreen->psImpl);

   free(psPVRContext);

   PVRDRIScreenUnlock(psPVRScreen);

   PVRDRIScreenRemoveReference(psPVRScreen);
}

static IMG_PIXFMT PVRDRIGetPixelFormat(const struct gl_config *psGLMode)
{
   switch (psGLMode->rgbBits)
   {
   case 32:
   case 24:
      if (psGLMode->redMask == 0x00FF0000 &&
          psGLMode->greenMask == 0x0000FF00 &&
          psGLMode->blueMask == 0x000000FF) {
         if (psGLMode->alphaMask == 0xFF000000)
            return IMG_PIXFMT_B8G8R8A8_UNORM;
         else if (psGLMode->alphaMask == 0)
            return IMG_PIXFMT_B8G8R8X8_UNORM;
      }

      if (psGLMode->redMask == 0x000000FF &&
          psGLMode->greenMask == 0x0000FF00 &&
          psGLMode->blueMask == 0x00FF0000) {
         if (psGLMode->alphaMask == 0xFF000000)
            return IMG_PIXFMT_R8G8B8A8_UNORM;
         else if (psGLMode->alphaMask == 0)
            return IMG_PIXFMT_R8G8B8X8_UNORM;
      }

      break;
   case 16:
      if (psGLMode->redMask == 0xF800 &&
          psGLMode->greenMask == 0x07E0 &&
          psGLMode->blueMask == 0x001F) {
         return IMG_PIXFMT_B5G6R5_UNORM;
      }

      break;

   default:
      break;
   }

   mesa_loge("%s: Unsupported screen format\n", __func__);
   return IMG_PIXFMT_UNKNOWN;
}

static GLboolean
PVRDRICreateBuffer(struct dri_screen *psDRIScreen, struct dri_drawable *psDRIDrawable,
                   const struct gl_config *psGLMode, GLboolean bIsPixmap)
{
   PVRDRIScreen *psPVRScreen = psDRIScreen->driverPrivate;
   PVRDRIDrawable *psPVRDrawable = NULL;
   PVRDRIDrawableImpl *psDrawableImpl = NULL;
   EGLint supportedAPIs = PVRDRIScreenSupportedAPIs(psPVRScreen);
   PVRDRIConfigInfo sConfigInfo;

   /* No known callers ever set this to true */
   if (bIsPixmap)
      return GL_FALSE;

   if (!psGLMode) {
      mesa_loge("%s: Invalid GL config", __func__);
      return GL_FALSE;
   }

   psPVRDrawable = calloc(1, sizeof(*psPVRDrawable));
   if (!psPVRDrawable) {
      mesa_loge("%s: Couldn't allocate PVR psDRIDrawable", __func__);
      goto ErrorDrawableFree;
   }

   psDrawableImpl = PVRDRICreateDrawableImpl(psPVRDrawable);
   if (!psDrawableImpl) {
      mesa_loge("%s: Couldn't allocate PVR psDRIDrawable", __func__);
      goto ErrorDrawableFree;
   }

   psPVRDrawable->psImpl = psDrawableImpl;

   psDRIDrawable->driverPrivate = (void*) psPVRDrawable;

   INITIALISE_PVRQ_HEAD(&psPVRDrawable->sPVRContextHead);
   INITIALISE_PVRQ_HEAD(&psPVRDrawable->sCacheFlushHead);

   psPVRDrawable->psDRIDrawable = opaque_dri_drawable(psDRIDrawable);
   psPVRDrawable->psPVRScreen = psPVRScreen;
   psPVRDrawable->bDoubleBuffered = psGLMode->doubleBufferMode;

   psPVRDrawable->ePixelFormat = PVRDRIGetPixelFormat(psGLMode);
   if (psPVRDrawable->ePixelFormat == IMG_PIXFMT_UNKNOWN) {
      mesa_loge("%s: Couldn't work out pixel format", __func__);
      goto ErrorDrawableFree;
   }

   if (!PVRMutexInit(&psPVRDrawable->sMutex, PTHREAD_MUTEX_RECURSIVE)) {
      mesa_loge("%s: Couldn't initialise psDRIDrawable mutex", __func__);
      goto ErrorDrawableFree;
   }

   PVRDRIConfigFromMesa(&sConfigInfo, psGLMode);
   if (!PVRDRIEGLDrawableConfigFromGLMode(psDrawableImpl,
         &sConfigInfo,
         supportedAPIs,
         psPVRDrawable->ePixelFormat)) {
      mesa_loge("%s: Couldn't derive EGL config", __func__);
      goto ErrorDrawableMutexDeinit;
   }

   /* Initialisation is completed in MakeCurrent */
   PVRDRIScreenAddReference(psPVRScreen);
   return GL_TRUE;

ErrorDrawableMutexDeinit:
   PVRMutexDeinit(&psPVRDrawable->sMutex);

ErrorDrawableFree:
   PVRDRIDestroyDrawableImpl(psDrawableImpl);
   free(psPVRDrawable);
   psDRIDrawable->driverPrivate = NULL;

   return GL_FALSE;
}

static void
PVRDRIDestroyBuffer(struct dri_drawable *psDRIDrawable)
{
   PVRDRIDrawable *psPVRDrawable = (PVRDRIDrawable*) psDRIDrawable->driverPrivate;
   PVRDRIScreen *psPVRScreen = psPVRDrawable->psPVRScreen;

   PVRDRIScreenLock(psPVRScreen);

   PVRDrawableUnbindContexts(psPVRDrawable);

   PVRDRIDrawableDeinit(psPVRDrawable);

   PVREGLDrawableDestroyConfig(psPVRDrawable->psImpl);

   PVRMutexDeinit(&psPVRDrawable->sMutex);

   PVRDRIDestroyDrawableImpl(psPVRDrawable->psImpl);

   free(psPVRDrawable);

   PVRDRIScreenUnlock(psPVRScreen);

   PVRDRIScreenRemoveReference(psPVRScreen);
}

static GLboolean
PVRDRIMakeCurrent(struct dri_context *psDRIContext,
                  struct dri_drawable *psDRIWrite, struct dri_drawable *psDRIRead)
{
   PVRDRIContext *psPVRContext = psDRIContext->driverPrivate;
   PVRDRIDrawable *psPVRWrite = (psDRIWrite) ? (PVRDRIDrawable*) psDRIWrite->driverPrivate : NULL;
   PVRDRIDrawable *psPVRRead = (psDRIRead) ? (PVRDRIDrawable*) psDRIRead->driverPrivate : NULL;

   PVRDRIScreenLock(psPVRContext->psPVRScreen);

   if (psPVRWrite != NULL)
   {
      if (!PVRDRIDrawableInit(psPVRWrite)) {
         mesa_loge("%s: Couldn't initialise write drawable", __func__);
         goto ErrorUnlock;
      }
   }

   if (psPVRRead != NULL) {
      if (!PVRDRIDrawableInit(psPVRRead)) {
         mesa_loge("%s: Couldn't initialise read drawable", __func__);
         goto ErrorUnlock;
      }
   }

   if (!PVRDRIMakeCurrentGC(psPVRContext->eAPI,
         psPVRContext->psPVRScreen->psImpl,
         psPVRContext->psImpl,
         psPVRWrite == NULL ? NULL : psPVRWrite->psImpl,
         psPVRRead == NULL ? NULL : psPVRRead->psImpl))
      goto ErrorUnlock;

   PVRQDequeue(&psPVRContext->sQElem);

   if (psPVRWrite != NULL)
      PVRQQueue(&psPVRWrite->sPVRContextHead, &psPVRContext->sQElem);

   psPVRContext->psPVRDrawable = psPVRWrite;

   if (psPVRWrite != NULL && psPVRContext->eAPI == PVRDRI_API_GL)
      PVRDRIEGLSetFrontBufferCallback(psPVRContext->eAPI,
                                      psPVRContext->psPVRScreen->psImpl,
                                      psPVRWrite->psImpl,
                                      PVRDRIDisplayFrontBuffer);

   pvrdri_set_dispatch_table(psPVRContext);

   PVRDRIThreadSetCurrentScreen(psPVRContext->psPVRScreen);

   PVRDRIScreenUnlock(psPVRContext->psPVRScreen);

   return GL_TRUE;

ErrorUnlock:
   PVRDRIScreenUnlock(psPVRContext->psPVRScreen);

   return GL_FALSE;
}

static void
PVRDRIUnbindContext(struct dri_context *psDRIContext)
{
   PVRDRIContext *psPVRContext = (PVRDRIContext*) psDRIContext->driverPrivate;
   PVRDRIScreen *psPVRScreen = psPVRContext->psPVRScreen;

   pvrdri_set_null_dispatch_table();

   PVRDRIScreenLock(psPVRScreen);
   PVRContextUnbind(psPVRContext, true, false);
   PVRDRIThreadSetCurrentScreen(NULL);
   PVRDRIScreenUnlock(psPVRScreen);
}

static __DRIbuffer *
PVRDRIAllocateBuffer(struct dri_screen *psDRIScreen, unsigned int uAttachment,
                     unsigned int uFormat, int iWidth, int iHeight)
{
   PVRDRIScreen *psPVRScreen = psDRIScreen->driverPrivate;
   struct PVRBuffer *psBuffer;
   unsigned int uiBpp;

   /* GEM names are only supported on primary nodes */
   if (drmGetNodeTypeFromFd(psDRIScreen->fd) != DRM_NODE_PRIMARY) {
      mesa_loge("%s: Cannot allocate buffer", __func__);
      return NULL;
   }

   /* This is based upon PVRDRIGetPixelFormat */
   switch (uFormat)
   {
   case 32:
   case 16:
      /* Format (depth) and bpp match */
      uiBpp = uFormat;
      break;
   case 24:
      uiBpp = 32;
      break;
   default:
      mesa_loge("%s: Unsupported format '%u'", __func__, uFormat);
      return NULL;
   }

   psBuffer = calloc(1, sizeof(*psBuffer));
   if (psBuffer == NULL) {
      mesa_loge("%s: Failed to allocate buffer", __func__);
      return NULL;
   }

   psBuffer->psImpl =
      PVRDRIBufferCreate(psPVRScreen->psImpl,
                         iWidth, iHeight, uiBpp,
                         PVDRI_BUFFER_USE_SHARE,
                         &psBuffer->sDRIBuffer.pitch);
   if (!psBuffer->psImpl) {
      mesa_loge("%s: Failed to create backing buffer", __func__);
      goto ErrorFreeDRIBuffer;
   }

   psBuffer->sDRIBuffer.attachment = uAttachment;
   psBuffer->sDRIBuffer.name = PVRDRIBufferGetName(psBuffer->psImpl);
   psBuffer->sDRIBuffer.cpp = uiBpp / 8;

   return &psBuffer->sDRIBuffer;

ErrorFreeDRIBuffer:
   free(psBuffer);

   return NULL;
}

static void
PVRDRIReleaseBuffer(struct dri_screen *psDRIScreen, __DRIbuffer *psDRIBuffer)
{
   struct PVRBuffer *psPVRBuffer = (struct PVRBuffer *) psDRIBuffer;

   (void) psDRIScreen;

   PVRDRIBufferDestroy(psPVRBuffer->psImpl);
   free(psPVRBuffer);
}

static char *
PVRDRIGetXMLConfigOptions(const char *pszDriverName)
{
   const driOptionDescription asConfigOptions[] =
   {
      DRI_CONF_SECTION_MISCELLANEOUS
      DRI_CONF_OPT_B("pvr_driconf_not_used", true,
                     "The PowerVR driver does not use DRIConf")
      DRI_CONF_SECTION_END
   };

   (void) pszDriverName;

   return driGetOptionsXml(&asConfigOptions[0], ARRAY_SIZE(asConfigOptions));
}

static bool
PVRDRICheckDriverCompatibility(int iFdRenderGPU,
                               const char *psDriverNameRenderGPU,
                               int iFdDisplayGPU,
                               const char *psDriverNameDisplayGPU)
{
   bool bCompat;

   mesa_logd("%s: Render driver name: %s FD: %d", __func__,
                    psDriverNameRenderGPU, iFdRenderGPU);
   mesa_logd("%s: Display driver name: %s FD: %d", __func__,
                    psDriverNameDisplayGPU ? psDriverNameDisplayGPU : "",
                    iFdDisplayGPU);

#if defined(PVR_DRIVER_ALIAS_STRING)
   mesa_logd("%s: PVR driver alias: %s", __func__,
                    PVR_DRIVER_ALIAS_STRING);
#endif

   if (!psDriverNameDisplayGPU)
      bCompat = false;
   else if (!strcmp(psDriverNameDisplayGPU, psDriverNameRenderGPU))
      bCompat = true;
   else if (!strcmp(psDriverNameDisplayGPU, "pvr"))
      bCompat = true;
#if defined(PVR_DRIVER_ALIAS_STRING)
   else if (!strcmp(psDriverNameDisplayGPU, PVR_DRIVER_ALIAS_STRING))
      bCompat = true;
#endif
   else
      bCompat = false;

   mesa_logd("%s: Render and display drivers are %s", __func__,
             bCompat ? "compatible" : "incompatible");

   return bCompat;
}

static int
PVRDRIQueryCompatibleRenderOnlyDeviceFd(int kms_only_fd)
{
   bool is_platform_device;
   struct pipe_loader_device *dev;
   const char * const drivers[] = {
      "pvr"
   };

   if (!pipe_loader_drm_probe_fd(&dev, kms_only_fd, false))
      return -1;
   is_platform_device = (dev->type == PIPE_LOADER_DEVICE_PLATFORM);
   pipe_loader_release(&dev, 1);

   /* For display-only devices that are not on the platform bus, we can't assume
    * that any of the rendering devices are compatible. */
   if (!is_platform_device)
      return -1;

   /* For platform display-only devices, we try to find a render-capable device
    * on the platform bus and that should be compatible with the display-only
    * device. */
   if (ARRAY_SIZE(drivers) == 0)
      return -1;

   return loader_open_render_node_platform_device(drivers, ARRAY_SIZE(drivers));
}

static const __DRIDriverAPIExtension sgx_driver_api_extension = {
   .base = {__DRI_DRIVER_API, 2},
   .initScreen = PVRDRIInitScreen,
   .destroyScreen = PVRDRIDestroyScreen,
   .createContext = PVRDRICreateContext,
   .destroyContext = PVRDRIDestroyContext,
   .createBuffer = PVRDRICreateBuffer,
   .destroyBuffer = PVRDRIDestroyBuffer,
   .makeCurrent = PVRDRIMakeCurrent,
   .unbindContext = PVRDRIUnbindContext,
   .allocateBuffer = PVRDRIAllocateBuffer,
   .releaseBuffer = PVRDRIReleaseBuffer,
};

static const __DRImesaCoreExtension sgx_mesa_core_extension = {
   .base = {__DRI_MESA, 1},
   .version_string = MESA_INTERFACE_VERSION_STRING,
   .createNewScreen = driCreateNewScreen2,
   .createContext = driCreateContextAttribs,
   .initScreen = NULL,
   .queryCompatibleRenderOnlyDeviceFd = PVRDRIQueryCompatibleRenderOnlyDeviceFd,
};

static const __DRIconfigOptionsExtension sgx_config_options = {
   .base = { __DRI_CONFIG_OPTIONS, 2 },
   .getXml = PVRDRIGetXMLConfigOptions,
};

static const __DRIdriverCompatibilityExtension sgx_driver_compatibility = {
   .base = { __DRI_DRIVER_COMPATIBILITY, 1 },
   .checkDriverCompatibility = PVRDRICheckDriverCompatibility,
};

const __DRIextension *sgx_driver_extensions[] = {
   &driCoreExtension.base,
   &sgx_mesa_core_extension.base,
   &driImageDriverExtension.base,
   &pvrDRI2Extension.base,
   &sgx_config_options.base,
   &sgx_driver_compatibility.base,
   &sgx_driver_api_extension.base,
   NULL
};
