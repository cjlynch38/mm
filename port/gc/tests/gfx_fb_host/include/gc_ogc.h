/* Host stand-in for port/gc/ogc/gc_ogc.h (gfx_fb host test): the video mode gfx_fb.c restores the copy filter from */
#ifndef GFX_FB_HOST_GC_OGC_H
#define GFX_FB_HOST_GC_OGC_H

#include <gccore.h>
#include "gc_bridge.h"

struct _gx_rmodeobj* gc_ogc_video_mode(void);

#endif
