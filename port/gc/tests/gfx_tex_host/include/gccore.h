/**
 * Host stand-in for libogc's <gccore.h>: only the types, constants and GX texture calls that
 * port/gc/gfx/gfx_rdp.c and gfx_tex.c use. The calls are implemented (recorded) by gfx_tex_host_test.c.
 * Values match libogc's ogc/gx.h.
 */
#ifndef GFX_TEX_HOST_GCCORE_H
#define GFX_TEX_HOST_GCCORE_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef float f32;

typedef struct {
    u32 val[8];
} GXTexObj;

#define GX_FALSE 0
#define GX_TRUE 1

#define GX_TF_I4 0x0
#define GX_TF_I8 0x1
#define GX_TF_IA4 0x2
#define GX_TF_IA8 0x3
#define GX_TF_RGB565 0x4
#define GX_TF_RGB5A3 0x5
#define GX_TF_RGBA8 0x6

#define GX_CLAMP 0
#define GX_REPEAT 1
#define GX_MIRROR 2

#define GX_NEAR 0
#define GX_LINEAR 1

#define GX_TEXMAP0 0
#define GX_TEXMAP1 1

void GX_InitTexObj(GXTexObj* obj, void* img_ptr, u16 wd, u16 ht, u8 fmt, u8 wrap_s, u8 wrap_t, u8 mipmap);
void GX_InitTexObjFilterMode(GXTexObj* obj, u8 minfilt, u8 magfilt);
void GX_LoadTexObj(GXTexObj* obj, u8 mapid);
void GX_InvalidateTexAll(void);
void GX_DrawDone(void);
void DCFlushRange(void* startaddress, u32 len);

#endif
