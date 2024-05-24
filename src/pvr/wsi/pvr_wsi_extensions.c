/*
 * Copyright © Imagination Technologies Ltd.
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

#include "vk_physical_device.h"

#include "pvr_wsi.h"

static bool
pvr_vk_mesa_wsi_device_extension_supported(struct pvr_mesa_wsi *mwsi,
                                           const char *name)
{
   JUMP_DDK(mwsi, pvr_vk_mesa_wsi_device_extension_supported,
            mwsi->physicalDevice, name);

   return false;
}

#define CHECK_DEV_EXT(mwsi, det, name) \
   (det)->name = pvr_vk_mesa_wsi_device_extension_supported(mwsi, # name)

void
pvr_mesa_wsi_device_extensions(struct pvr_mesa_wsi *mwsi,
                               struct vk_device_extension_table *det)
{
   memset(det, 0, sizeof(*det));

   CHECK_DEV_EXT(mwsi, det, EXT_attachment_feedback_loop_layout);
   CHECK_DEV_EXT(mwsi, det, EXT_external_memory_host);
   CHECK_DEV_EXT(mwsi, det, EXT_pci_bus_info);
   CHECK_DEV_EXT(mwsi, det, KHR_present_id);
   CHECK_DEV_EXT(mwsi, det, KHR_present_wait);
   CHECK_DEV_EXT(mwsi, det, KHR_timeline_semaphore);
}
