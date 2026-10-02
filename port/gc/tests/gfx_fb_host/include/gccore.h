/**
 * Host stand-in for libogc's <gccore.h> in the gfx_fb host test: the types, constants and calls port/gc/gfx/gfx_fb.c
 * uses. gfx_fb_host_test.c implements the calls on a model of the EFB (copies with their tiled formats, the quads
 * gfx_fb.c draws). Texture format values match libogc's ogc/gx.h; the other constants only need to be distinct.
 */
#ifndef GFX_FB_HOST_GCCORE_H
#define GFX_FB_HOST_GCCORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef float f32;

typedef float Mtx44[4][4];

typedef struct {
    u8 r, g, b, a;
} GXColor;

/* What the model needs of a texture object */
typedef struct {
    void* img;
    u16 width, height;
    u8 fmt, wrapS, wrapT, minFilter, magFilter;
} GXTexObj;

typedef struct _gx_rmodeobj {
    u8 aa;
    u8 sample_pattern[12][2];
    u8 vfilter[7];
} GXRModeObj;

#define GX_FALSE 0
#define GX_TRUE 1

#define GX_TF_I8 0x1
#define GX_TF_RGB565 0x4
#define GX_TF_RGB5A3 0x5
#define GX_TF_RGBA8 0x6
#define GX_TF_Z8 0x11
#define GX_TF_Z24X8 0x16
#define GX_CTF_R8 0x28

#define GX_CLAMP 0
#define GX_NEAR 0
#define GX_LINEAR 1

#define GX_TEXMAP0 0
#define GX_TEXMAP1 1
#define GX_TEXMAP6 6
#define GX_TEXMAP7 7

#define GX_MAX_Z24 0x00FFFFFF

#define GX_VA_POS 9
#define GX_VA_TEX0 13
#define GX_DIRECT 1
#define GX_VTXFMT1 1
#define GX_POS_XYZ 1
#define GX_TEX_ST 1
#define GX_F32 4
#define GX_TEXCOORD0 0
#define GX_TG_MTX2x4 1
#define GX_TG_TEX0 4
#define GX_IDENTITY 60
#define GX_TEVSTAGE0 0
#define GX_TEVSTAGE1 1
#define GX_COLORNULL 0xFF
#define GX_CC_ZERO 15
#define GX_CC_TEXC 8
#define GX_CC_CPREV 0
#define GX_CA_ZERO 7
#define GX_CA_TEXA 4
#define GX_CA_APREV 0
#define GX_TEV_ADD 0
#define GX_TB_ZERO 0
#define GX_CS_SCALE_1 0
#define GX_TEVPREV 0
#define GX_ZT_DISABLE 0
#define GX_ZT_REPLACE 2
#define GX_ALWAYS 7
#define GX_FOG_NONE 0
#define GX_BM_NONE 0
#define GX_BL_ONE 1
#define GX_BL_ZERO 0
#define GX_LO_COPY 3
#define GX_AOP_AND 0
#define GX_ORTHOGRAPHIC 1
#define GX_QUADS 0x80

void GX_SetCopyFilter(u8 aa, u8 sample_pattern[12][2], u8 vf, u8 vfilter[7]);
void GX_SetCopyClear(GXColor color, u32 zvalue);
void GX_SetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht);
void GX_SetTexCopyDst(u16 wd, u16 ht, u32 fmt, u8 mipmap);
void GX_CopyTex(void* dest, u8 clear);
void GX_PixModeSync(void);
void GX_DrawDone(void);
void GX_InvalidateTexAll(void);
void GX_InitTexObj(GXTexObj* obj, void* img_ptr, u16 wd, u16 ht, u8 fmt, u8 wrap_s, u8 wrap_t, u8 mipmap);
void GX_InitTexObjFilterMode(GXTexObj* obj, u8 minfilt, u8 magfilt);
void GX_LoadTexObj(GXTexObj* obj, u8 mapid);

void GX_ClearVtxDesc(void);
void GX_SetVtxDesc(u8 attr, u8 type);
void GX_SetVtxAttrFmt(u8 vtxfmt, u32 vtxattr, u32 comptype, u32 compsize, u32 frac);
void GX_SetNumChans(u8 num);
void GX_SetNumTexGens(u32 nr);
void GX_SetTexCoordGen(u16 texcoord, u32 tgen_typ, u32 tgen_src, u32 mtxsrc);
void GX_SetNumIndStages(u8 nstages);
void GX_SetTevDirect(u8 tevstage);
void GX_SetTevOrder(u8 tevstage, u8 texcoord, u32 texmap, u8 color);
void GX_SetTevColorIn(u8 tevstage, u8 a, u8 b, u8 c, u8 d);
void GX_SetTevAlphaIn(u8 tevstage, u8 a, u8 b, u8 c, u8 d);
void GX_SetTevColorOp(u8 tevstage, u8 tevop, u8 tevbias, u8 tevscale, u8 clamp, u8 tevregid);
void GX_SetTevAlphaOp(u8 tevstage, u8 tevop, u8 tevbias, u8 tevscale, u8 clamp, u8 tevregid);
void GX_SetNumTevStages(u8 num);
void GX_SetZTexture(u8 op, u8 fmt, u32 bias);
void GX_SetZCompLoc(u8 before_tex);
void GX_SetZMode(u8 enable, u8 func, u8 update_enable);
void GX_SetFog(u8 type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor col);
void GX_SetBlendMode(u8 type, u8 src_fact, u8 dst_fact, u8 op);
void GX_SetAlphaCompare(u8 comp0, u8 ref0, u8 aop, u8 comp1, u8 ref1);
void GX_SetColorUpdate(u8 enable);
void GX_SetAlphaUpdate(u8 enable);
void GX_LoadProjectionMtx(Mtx44 mt, u8 type);
void GX_SetViewport(f32 xOrig, f32 yOrig, f32 wd, f32 ht, f32 nearZ, f32 farZ);
void GX_SetScissor(u32 xOrigin, u32 yOrigin, u32 wd, u32 ht);
void GX_Begin(u8 primitve, u8 vtxfmt, u16 vtxcnt);
void GX_Position3f32(f32 x, f32 y, f32 z);
void GX_TexCoord2f32(f32 s, f32 t);
void GX_End(void);
void guOrtho(Mtx44 mt, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f);

void DCFlushRange(void* startaddress, u32 len);
void DCInvalidateRange(void* startaddress, u32 len);
void* SYS_GetArena1Hi(void);
void* SYS_GetArena1Lo(void);

#endif
