/**
 * Shared by the gfx_tev tests (host test and Dolphin test DOL): GBI encodings of combine and render modes
 * (from the decomp's include/PR/gbi.h), and a model of the N64 RDP's color combiner, alpha compare and
 * blender for one fully covered pixel, written from angrylion-rdp-plus (combiner.c, blender.c, rasterizer.c).
 */
#ifndef GFX_TEV_RDP_MODEL_H
#define GFX_TEV_RDP_MODEL_H

#include <stdbool.h>
#include <stdint.h>
#include "gfx_gbi.h"

typedef struct {
    int r, g, b, a;
} C4;

/* ------------------------------------------------------------------------------------------------ */
/* GBI encodings (gbi.h gsDPSetCombineLERP / gsDPSetCombineMode, render modes)                     */
/* ------------------------------------------------------------------------------------------------ */

#define CCMUX(x) ((uint32_t)G_CCMUX_##x)
#define ACMUX(x) ((uint32_t)G_ACMUX_##x)
#define CC_W0(a0, c0, Aa0, Ac0, a1, c1)                                                                 \
    (((CCMUX(a0) & 0xF) << 20) | ((CCMUX(c0) & 0x1F) << 15) | ((ACMUX(Aa0) & 7) << 12) |                 \
     ((ACMUX(Ac0) & 7) << 9) | ((CCMUX(a1) & 0xF) << 5) | (CCMUX(c1) & 0x1F))
#define CC_W1(b0, d0, Ab0, Ad0, b1, Aa1, Ac1, d1, Ab1, Ad1)                                             \
    (((CCMUX(b0) & 0xF) << 28) | ((CCMUX(b1) & 0xF) << 24) | ((ACMUX(Aa1) & 7) << 21) |                  \
     ((ACMUX(Ac1) & 7) << 18) | ((CCMUX(d0) & 7) << 15) | ((ACMUX(Ab0) & 7) << 12) | ((ACMUX(Ad0) & 7) << 9) | \
     ((CCMUX(d1) & 7) << 6) | ((ACMUX(Ab1) & 7) << 3) | (ACMUX(Ad1) & 7))
#define LERP(a0, b0, c0, d0, Aa0, Ab0, Ac0, Ad0, a1, b1, c1, d1, Aa1, Ab1, Ac1, Ad1) \
    CC_W0(a0, c0, Aa0, Ac0, a1, c1), CC_W1(b0, d0, Ab0, Ad0, b1, Aa1, Ac1, d1, Ab1, Ad1)
#define MODE_(...) LERP(__VA_ARGS__)
#define MODE(a, b) MODE_(a, b)

/* G_CC_* presets (gbi.h) */
#define G_CC_PRIMITIVE 0, 0, 0, PRIMITIVE, 0, 0, 0, PRIMITIVE
#define G_CC_SHADE 0, 0, 0, SHADE, 0, 0, 0, SHADE
#define G_CC_MODULATEI TEXEL0, 0, SHADE, 0, 0, 0, 0, SHADE
#define G_CC_MODULATEIA TEXEL0, 0, SHADE, 0, TEXEL0, 0, SHADE, 0
#define G_CC_MODULATEIDECALA TEXEL0, 0, SHADE, 0, 0, 0, 0, TEXEL0
#define G_CC_MODULATEI_PRIM TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE
#define G_CC_MODULATEIA_PRIM TEXEL0, 0, PRIMITIVE, 0, TEXEL0, 0, PRIMITIVE, 0
#define G_CC_MODULATEIDECALA_PRIM TEXEL0, 0, PRIMITIVE, 0, 0, 0, 0, TEXEL0
#define G_CC_DECALRGB 0, 0, 0, TEXEL0, 0, 0, 0, SHADE
#define G_CC_DECALRGBA 0, 0, 0, TEXEL0, 0, 0, 0, TEXEL0
#define G_CC_BLENDI ENVIRONMENT, SHADE, TEXEL0, SHADE, 0, 0, 0, SHADE
#define G_CC_BLENDIA ENVIRONMENT, SHADE, TEXEL0, SHADE, TEXEL0, 0, SHADE, 0
#define G_CC_BLENDRGBA TEXEL0, SHADE, TEXEL0_ALPHA, SHADE, 0, 0, 0, SHADE
#define G_CC_ADDRGB 1, 0, TEXEL0, SHADE, 0, 0, 0, SHADE
#define G_CC_REFLECTRGB ENVIRONMENT, 0, TEXEL0, SHADE, 0, 0, 0, SHADE
#define G_CC_HILITERGBA PRIMITIVE, SHADE, TEXEL0, SHADE, PRIMITIVE, SHADE, TEXEL0, SHADE
#define G_CC_SHADEDECALA 0, 0, 0, SHADE, 0, 0, 0, TEXEL0
#define G_CC_BLENDPE PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, TEXEL0, 0, SHADE, 0
#define G_CC_BLENDPEDECALA PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, 0, 0, 0, TEXEL0
#define G_CC_TEMPLERP TEXEL1, TEXEL0, PRIM_LOD_FRAC, TEXEL0, TEXEL1, TEXEL0, PRIM_LOD_FRAC, TEXEL0
#define G_CC_TRILERP TEXEL1, TEXEL0, LOD_FRACTION, TEXEL0, TEXEL1, TEXEL0, LOD_FRACTION, TEXEL0
#define G_CC_INTERFERENCE TEXEL0, 0, TEXEL1, 0, TEXEL0, 0, TEXEL1, 0
#define G_CC_PASS2 0, 0, 0, COMBINED, 0, 0, 0, COMBINED
#define G_CC_MODULATEIA2 COMBINED, 0, SHADE, 0, COMBINED, 0, SHADE, 0
#define G_CC_MODULATEI_PRIM2 COMBINED, 0, PRIMITIVE, 0, 0, 0, 0, PRIMITIVE
#define G_CC_MODULATEIA_PRIM2 COMBINED, 0, PRIMITIVE, 0, COMBINED, 0, PRIMITIVE, 0
#define G_CC_DECALRGBA2 COMBINED, SHADE, COMBINED_ALPHA, SHADE, 0, 0, 0, SHADE
#define G_CC_HILITERGBA2 ENVIRONMENT, COMBINED, TEXEL0, COMBINED, ENVIRONMENT, COMBINED, TEXEL0, COMBINED

/* Render mode bits and macros (gbi.h) */
#define AA_EN 0x0008
#define Z_CMP 0x0010
#define Z_UPD 0x0020
#define IM_RD 0x0040
#define CLR_ON_CVG 0x0080
#define CVG_DST_CLAMP 0x0000
#define CVG_DST_WRAP 0x0100
#define CVG_DST_FULL 0x0200
#define CVG_DST_SAVE 0x0300
#define ZMODE_OPA 0x0000
#define ZMODE_INTER 0x0400
#define ZMODE_XLU 0x0800
#define ZMODE_DEC 0x0C00
#define CVG_X_ALPHA 0x1000
#define ALPHA_CVG_SEL 0x2000
#define FORCE_BL 0x4000
#define GBL_c1(m1a, m1b, m2a, m2b) ((uint32_t)(m1a) << 30 | (uint32_t)(m1b) << 26 | (uint32_t)(m2a) << 22 | (uint32_t)(m2b) << 18)
#define GBL_c2(m1a, m1b, m2a, m2b) ((uint32_t)(m1a) << 28 | (uint32_t)(m1b) << 24 | (uint32_t)(m2a) << 20 | (uint32_t)(m2b) << 16)

#define RM_AA_ZB_OPA_SURF(clk) \
    (AA_EN | Z_CMP | Z_UPD | IM_RD | CVG_DST_CLAMP | ZMODE_OPA | ALPHA_CVG_SEL | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_A_MEM))
#define RM_AA_ZB_XLU_SURF(clk) \
    (AA_EN | Z_CMP | IM_RD | CVG_DST_WRAP | CLR_ON_CVG | FORCE_BL | ZMODE_XLU | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA))
#define RM_AA_ZB_OPA_DECAL(clk) \
    (AA_EN | Z_CMP | IM_RD | CVG_DST_WRAP | ALPHA_CVG_SEL | ZMODE_DEC | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_A_MEM))
#define RM_AA_ZB_XLU_DECAL(clk) \
    (AA_EN | Z_CMP | IM_RD | CVG_DST_WRAP | CLR_ON_CVG | FORCE_BL | ZMODE_DEC | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA))
#define RM_AA_ZB_OPA_INTER(clk) \
    (AA_EN | Z_CMP | Z_UPD | IM_RD | CVG_DST_CLAMP | ALPHA_CVG_SEL | ZMODE_INTER | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_A_MEM))
#define RM_AA_ZB_TEX_EDGE(clk)                                                                  \
    (AA_EN | Z_CMP | Z_UPD | IM_RD | CVG_DST_CLAMP | CVG_X_ALPHA | ALPHA_CVG_SEL | ZMODE_OPA | \
     GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_A_MEM))
#define RM_AA_XLU_SURF(clk) \
    (AA_EN | IM_RD | CVG_DST_WRAP | CLR_ON_CVG | FORCE_BL | ZMODE_OPA | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA))
#define RM_AA_OPA_SURF(clk) \
    (AA_EN | IM_RD | CVG_DST_CLAMP | ZMODE_OPA | ALPHA_CVG_SEL | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_A_MEM))
#define RM_ZB_XLU_SURF(clk) \
    (Z_CMP | IM_RD | CVG_DST_FULL | FORCE_BL | ZMODE_XLU | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA))
#define RM_ZB_CLD_SURF(clk) \
    (Z_CMP | IM_RD | CVG_DST_SAVE | FORCE_BL | ZMODE_XLU | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA))
#define RM_ZB_OVL_SURF(clk) \
    (Z_CMP | IM_RD | CVG_DST_SAVE | FORCE_BL | ZMODE_DEC | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA))
#define RM_OPA_SURF(clk) (CVG_DST_CLAMP | FORCE_BL | ZMODE_OPA | GBL_c##clk(G_BL_CLR_IN, G_BL_0, G_BL_CLR_IN, G_BL_1))
#define RM_XLU_SURF(clk) \
    (IM_RD | CVG_DST_FULL | FORCE_BL | ZMODE_OPA | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA))
#define RM_TEX_EDGE(clk)                                                                    \
    (CVG_DST_CLAMP | CVG_X_ALPHA | ALPHA_CVG_SEL | FORCE_BL | ZMODE_OPA | AA_EN |            \
     GBL_c##clk(G_BL_CLR_IN, G_BL_0, G_BL_CLR_IN, G_BL_1))
#define RM_CLD_SURF(clk) \
    (IM_RD | CVG_DST_SAVE | FORCE_BL | ZMODE_OPA | GBL_c##clk(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA))
#define RM_ADD(clk) (IM_RD | CVG_DST_SAVE | FORCE_BL | ZMODE_OPA | GBL_c##clk(G_BL_CLR_IN, G_BL_A_FOG, G_BL_CLR_MEM, G_BL_1))
#define RM_NOOP(clk) GBL_c##clk(0, 0, 0, 0)
#define RM_VISCVG(clk) (IM_RD | FORCE_BL | GBL_c##clk(G_BL_CLR_IN, G_BL_0, G_BL_CLR_BL, G_BL_A_MEM))

#define G_RM_FOG_SHADE_A GBL_c1(G_BL_CLR_FOG, G_BL_A_SHADE, G_BL_CLR_IN, G_BL_1MA)
#define G_RM_FOG_PRIM_A GBL_c1(G_BL_CLR_FOG, G_BL_A_FOG, G_BL_CLR_IN, G_BL_1MA)
#define G_RM_PASS GBL_c1(G_BL_CLR_IN, G_BL_0, G_BL_CLR_IN, G_BL_1)

#define RM1(x) (RM_##x(1) | RM_##x(2))

/* ------------------------------------------------------------------------------------------------ */
/* RDP model                                                                                        */
/* ------------------------------------------------------------------------------------------------ */

typedef struct {
    C4 tex0, tex1, shade, prim, env, fog, blend, mem;
    int primLod;
} N64In;

typedef struct {
    bool written;
    C4 color;      /* framebuffer color */
    C4 pixel;      /* combiner output (clamped), alpha after the coverage selection */
    int rawAlpha;  /* final combined alpha, clamped */
    bool overflow; /* a value the RDP wraps and GX clamps: combiner result outside -128..383, cycle 0 result
                      outside 0..255 (gfx_tev.c clamps it), blender sum above 255 */
    int acValue;   /* alpha tested against the threshold, -1 if no threshold test */
    bool cutout;   /* coverage x alpha rule active */
} N64Out;

/** Combiner, alpha compare and blender for one fully covered pixel (no dither, no z). hi/lo: G_SETCOMBINE
 *  words, omh/L: othermode H/L, shadeAvail: shade coefficients present (triangle with G_SHADE). NOISE is
 *  0.5, LOD_FRACTION 0, K4/K5 114/42, chroma key center/scale 0, memory coverage full. */
void rdp_pixel(uint32_t hi, uint32_t lo, uint32_t omh, uint32_t L, bool shadeAvail, const N64In* n, N64Out* o);

#endif
