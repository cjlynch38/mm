/**
 * Stand-in for libogc's gccore.h in the gfx host test: gfx_internal.h includes it, but the code under
 * test (gfx_rsp.c, gfx_s2dex.c) uses only C types, the texture map numbers and (in declarations) the
 * texture object type from it.
 */
#ifndef GFX_HOST_FAKE_GCCORE_H
#define GFX_HOST_FAKE_GCCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define GX_TEXMAP0 0
#define GX_TEXMAP1 1

typedef struct {
    uint32_t val[8];
} GXTexObj;

#endif
