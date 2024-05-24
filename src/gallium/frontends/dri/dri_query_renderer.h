#ifndef DRI_QUERY_RENDERER_H
#define DRI_QUERY_RENDERER_H

#include "dri_util.h"

int
dri2_query_renderer_integer_common(__DRIscreen *_screen, int param,
                                   unsigned int *value);

extern const
__DRI2rendererQueryExtension dri2RendererQueryExtension;

#endif
