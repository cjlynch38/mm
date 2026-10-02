/**
 * GX mock of the gfx_tev host test: the GX state recorded from gfx_tev.c's calls, and a software model of
 * what the GameCube does with it for one pixel (TEV stages, alpha compare, blending; after Dolphin's software
 * renderer).
 */
#ifndef GFX_TEV_MOCK_GX_H
#define GFX_TEV_MOCK_GX_H

#include "gccore.h"
#include "rdp_model.h"

typedef struct {
    u8 texcoord, chan;
    u32 texmap;
    u8 cin[4], ain[4];
    u8 cop, cbias, cscale, cclamp, creg;
    u8 aop, abias, ascale, aclamp, areg;
    u8 kcsel, kasel;
} MockStage;

typedef struct {
    int numStages;
    MockStage st[GX_MAX_TEVSTAGE];
    GXColor reg[4];   /* initial TEVPREV, TEVREG0..2 */
    GXColor konst[4];
    u8 blendType, blendSrc, blendDst, blendOp;
    u8 acComp0, acRef0, acOp, acComp1, acRef1;
    u8 zEnable, zFunc, zUpdate, zCompLoc;
    u8 colorUpdate, alphaUpdate;
    int calls;        /* all recorded GX calls */
    int programCalls; /* GX_SetNumTevStages calls */
    int colorCalls;   /* GX_SetTevColor / GX_SetTevKColor calls */
    int acCalls;      /* GX_SetAlphaCompare calls */
} MockGx;

extern MockGx gMockGx;

/** Run the recorded TEV program for one pixel. tex0/tex1: texels of GX_TEXMAP0/1, shade: rasterized
 *  GX_COLOR0A0, dst: EFB color. Returns whether the pixel is written (alpha compare, color update) and the
 *  resulting EFB color in *out (the TEV output before blending in *tevOut, if not NULL). */
bool mock_gx_eval(C4 tex0, C4 tex1, C4 shade, C4 dst, C4* out, C4* tevOut);

/** gc_log calls so far */
int mock_log_count(void);
void mock_log_quiet(bool quiet);

#endif
