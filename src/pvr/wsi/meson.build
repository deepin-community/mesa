# Copyright (c) Imagination Technologies Ltd.

# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:

# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.

# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

pvr_wsi_args = []

if with_pvr_vulkan_display
  pvr_wsi_args += '-DPVR_DISPLAY_DRIVER=@0@'.format(pvr_vulkan_display)
endif

pvr_wsi_depends = [
  idep_vulkan_util,
  idep_xmlconfig,
  idep_vulkan_wsi,
  idep_vulkan_runtime_headers,
  idep_vulkan_runtime,
  idep_nir,
  idep_vtn
]

pvr_wsi_includes = [
  inc_include,
  inc_src,
  inc_vulkan_util
]

pvr_wsi_src = [
  'pvr_wsi.c',
  'pvr_wsi_headless.c',
  'pvr_wsi_extensions.c'
]

if with_platform_wayland
  pvr_wsi_args += '-DVK_USE_PLATFORM_WAYLAND_KHR'

  pvr_wsi_depends += dep_wayland_client

  pvr_wsi_src += 'pvr_wsi_wayland.c'
endif

if with_platform_x11
  pvr_wsi_args += [
    '-DVK_USE_PLATFORM_XCB_KHR',
    '-DVK_USE_PLATFORM_XLIB_KHR',
  ]

  pvr_wsi_depends += [
    dep_xcb,
    dep_x11_xcb,
    dep_xcb_dri3,
    dep_xcb_present,
    dep_xcb_xfixes,
    dep_xcb_sync,
    dep_xcb_xrandr,
    dep_xcb_shm,
    dep_xshmfence,
  ]

  if with_xcb_keysyms
    pvr_wsi_depends += dep_xcb_keysyms
  endif

  pvr_wsi_src += 'pvr_wsi_x11.c'
endif

if system_has_kms_drm and not with_platform_android
  pvr_wsi_args += '-DVK_USE_PLATFORM_DISPLAY_KHR'

  pvr_wsi_depends += dep_libdrm

  pvr_wsi_src += 'pvr_wsi_display.c'
endif

if with_xlib_lease
  pvr_wsi_args += '-DVK_USE_PLATFORM_XLIB_XRANDR_EXT'

  pvr_wsi_depends += dep_xlib_xrandr
endif

libpvr_mesa_wsi = shared_library(
  'pvr_mesa_wsi',
  pvr_wsi_src,
  include_directories : pvr_wsi_includes,
  dependencies : pvr_wsi_depends,
  c_args : pvr_wsi_args,
  link_with: libvulkan_wsi,
  gnu_symbol_visibility : 'hidden',
  build_by_default : true,
  install : true
)
