/**
 * Color combiner, blender, alpha compare and z mode -> GX TEV and pixel engine state (DESIGN.md, "Combiner
 * and blender").
 *
 * gfx_tev_apply() compiles the RDP state that decides a pixel's color (G_SETCOMBINE, the othermode L render
 * mode, alpha compare and z source, the cycle type) for a primitive kind into a TEV program plus blend, alpha
 * compare and z settings. Programs are cached by that state; the constant colors (prim, env, fog, blend, prim
 * LOD fraction) are uploaded whenever they change.
 *
 * N64 behaviour followed here, as implemented by angrylion-rdp-plus (the reference RDP emulator):
 * - 1-cycle mode runs the combiner's SECOND cycle settings (combiner_1cycle reads index 1) but the blender's
 *   FIRST cycle settings (blender_1cycle reads index 0). Games normally program both cycles alike.
 * - Texels: in 1-cycle mode TEXEL1 is the next pixel's TEXEL0 (same tile), used here as TEXEL0. In 2-cycle mode
 *   cycle 0 sees TEXEL0 = tile and TEXEL1 = tile + 1, and cycle 1 sees TEXEL0 = tile + 1 and TEXEL1 = the next
 *   pixel's tile texel (used here as tile). "Tile" is GX_TEXMAP0, "tile + 1" GX_TEXMAP1.
 * - Combiner inputs are slot specific: code 6 is 1 in A and D, the chroma key center in B (0 here, MM never
 *   keys), the key scale in C (0 here); code 7 is noise in A (0.5 here), K4 in B; C 15 is K5. K4/K5 use the
 *   libultra defaults 114/42 (MM never sets G_SETCONVERT). LOD_FRACTION is 0 (no mipmaps). In the alpha C slot
 *   code 0 is LOD_FRACTION and 6 is PRIM_LOD_FRAC.
 * - Texture and fill rectangles, and triangles without G_SHADE, have no shade coefficients: shade is 0.
 * - Alpha compare (G_AC_THRESHOLD) passes when alpha >= blend color alpha. In 1-cycle mode it tests the pixel
 *   alpha, in 2-cycle mode cycle 0's alpha. ALPHA_CVG_SEL replaces the pixel alpha by the coverage (255 inside a
 *   primitive) unless CVG_X_ALPHA multiplies it by the alpha. With AA_EN and CVG_X_ALPHA the coverage becomes
 *   alpha * coverage and a pixel with zero coverage (alpha < 32) is not written: GX alpha compare >= 32.
 * - Blender: 2-cycle mode always blends in cycle 0. The last cycle blends only with FORCE_BL; otherwise (AA
 *   modes) it blends only at partially covered edges, which is ignored here, and outputs its P input.
 *
 * Differences from the RDP (approximations):
 * - The RDP computes in 9 bits (-128..383) and wraps beyond: sums above 383 turn black, results below -128
 *   white. TEV results saturate. The combined value of cycle 0 is clamped to [0, 255] before cycle 1 reads it
 *   (TEV a/b/c inputs are 8 bits). A cycle-0 result of exactly 0x100 is read as -1.0 by the RDP's multiplier
 *   input; that is reproduced when cycle 0 is the constant 1 (cycle 1's A and B swap), not when a computed
 *   value happens to reach 0x100.
 * - TEV multiplies by c + c/128 (255 means 1.0), the RDP by c/256; results differ by 1-2 LSB.
 * - The blender's 5-bit factor precision, its wrap of sums above 255 and edge antialiasing are not reproduced.
 *   A_MEM (memory coverage) counts as 1.
 * - COMBINED in the first cycle (the RDP's value from the previous pixel) is 0; a 1-cycle combiner that reads
 *   COMBINED runs cycle 0 first.
 * - When a 2-cycle program both blends with its final alpha and alpha-tests cycle 0's alpha, the test uses the
 *   final alpha (GX compares the TEV output). G_AC_DITHER (random threshold) becomes alpha blending.
 * - G_ZS_PRIM (primitive depth as z) is reported through depthTest/depthWrite; gfx_gx.c substitutes the depth.
 * Each approximation that changes a result is logged once with the combine and render mode words.
 *
 * One N64 cycle (A - B) * C + D maps to one TEV stage when it is a lerp (D == B), a multiply-add (B == 0),
 * (1 - B) * C + D or D - (1 - A) * C, and to two stages otherwise (DESIGN.md). PRIM is TEV register 0, ENV
 * register 1; register 2 (and register 0/1 when PRIM/ENV are unused) hold cycle 0's result across a two-stage
 * cycle 1 and TEXEL1 when a stage would need both textures. Konst colors: K0 fog, K1 blend color,
 * K2 = (prim LOD fraction, K4, K5, prim LOD fraction), K3 a premultiplied blender constant.
 */
#include <string.h>
#include "gfx_internal.h"

/* Render mode bits of othermode L (gbi.h G_SETOTHERMODE_L gSetRenderMode) */
#define AA_EN 0x0008
#define Z_CMP 0x0010
#define Z_UPD 0x0020
#define ZMODE_MASK 0x0C00
#define ZMODE_XLU 0x0800
#define ZMODE_DEC 0x0C00
#define CVG_X_ALPHA 0x1000
#define ALPHA_CVG_SEL 0x2000
#define FORCE_BL 0x4000

#define CYC_1CYCLE 0
#define CYC_2CYCLE 1
#define CYC_COPY 2
#define CYC_FILL 3

#define TEV_MAX_STAGES 12
#define TEV_CACHE_SIZE 256 /* power of two */
#define TEV_CACHE_PROBE 8

/* Sources of combiner/blender values, before they are lowered to GX TEV inputs (S_TEXn_A follows S_TEXn) */
enum {
    S_ZERO,
    S_ONE,
    S_HALF,   /* noise */
    S_PREV,   /* this channel's running value in TEVPREV (result of the previous op) */
    S_PREV_A, /* the alpha in TEVPREV, read by a color op */
    S_COMB,   /* COMBINED: the previous cycle's color / alpha */
    S_COMB_A,
    S_TEX0,   /* tile (GX_TEXMAP0) */
    S_TEX0_A,
    S_TEX1,   /* tile + 1 (GX_TEXMAP1) */
    S_TEX1_A,
    S_PRIM,
    S_PRIM_A,
    S_ENV,
    S_ENV_A,
    S_SHADE,
    S_SHADE_A,
    S_PRIM_LOD,
    S_K4,
    S_K5,
    S_FOG,
    S_FOG_A,
    S_BLEND,
    S_K3,
    S_IN,     /* blender: the color entering the blender cycle */
    S_AIN     /* blender: the pixel alpha */
};

/* TEV register numbers as GX uses them (GX_TEVPREV = 0, GX_TEVREG0..2 = 1..3) */
#define R_PREV GX_TEVPREV
#define R_NONE 0xFF

typedef struct {
    uint8_t a, b, c, d; /* sources (TEV operands: d +/- lerp(a, b, c)) */
    uint8_t sub;
    uint8_t clamp;
    uint8_t dst;        /* TEV register */
    uint8_t nop;        /* passes the channel through (d = S_PREV) */
} TevOp;

typedef struct {
    TevOp col, alp;
    uint8_t prefetch; /* loads TEXEL1 into a register */
} IrStage;

/* One N64 cycle of one channel: (A - B) * C + D */
typedef struct {
    uint8_t a, b, c, d;
} Formula;

/* A channel's cycle as TEV ops: 0 ops (value is a plain source), 1 op or 2 ops (value in TEVPREV) */
typedef struct {
    uint8_t n;
    TevOp op[2];
    uint8_t value; /* source, or S_PREV when computed */
} ChanCode;

/* Lowered GX stage */
typedef struct {
    uint8_t texmap; /* GX_TEXMAP0/1 or GX_TEXMAP_NULL */
    uint8_t chan;   /* GX_COLOR0A0 or GX_COLORNULL */
    uint8_t cin[4], ain[4];
    uint8_t cop, aop; /* bit 0: subtract, bit 1: clamp, bits 4-7: register */
    uint8_t kcsel, kasel;
} TevStage;

typedef struct {
    uint32_t hi, lo, modeL, flags;
} TevKey;

/* Key flags */
#define KF_CYCLE_MASK 0x03
#define KF_RECT 0x04
#define KF_SHADE 0x08
#define KF_ZBUF 0x10
#define KF_VALID 0x80000000u

/* K3 = P * fogAlpha + M * (1 - fogAlpha) per channel */
enum { K3C_ZERO, K3C_FOG, K3C_BLEND };

typedef struct {
    TevKey key;
    uint8_t numStages;
    uint8_t blendType, blendSrc, blendDst;
    uint8_t colorUpdate;
    uint8_t zEnable, zFunc, zUpdate;
    uint8_t acThreshold;        /* compare >= blend color alpha */
    uint8_t acComp1, acRef1;    /* second, fixed comparison */
    uint8_t k3Used, k3P, k3M;
    GfxTevInfo info;
    TevStage stages[TEV_MAX_STAGES];
} TevProgram;

/* ------------------------------------------------------------------------------------------------ */
/* Warnings, once each                                                                              */
/* ------------------------------------------------------------------------------------------------ */

enum {
    W_COMB_FIRST = 1 << 0,
    W_1CYCLE_COMB = 1 << 1,
    W_KONST = 1 << 2,
    W_REGS = 1 << 3,
    W_STAGES = 1 << 4,
    W_BLEND = 1 << 5,
    W_ALPHA = 1 << 6,
    W_K3 = 1 << 7,
    W_TEXTURES = 1 << 8,
    W_DITHER = 1 << 9
};

static uint32_t sWarned;

static void tev_warn(uint32_t bit, const TevKey* k, const char* what) {
    if (!(sWarned & bit)) {
        sWarned |= bit;
        gc_log("gfx_tev: %s (combine %06X %08X, mode L %08X, cycle %u)", what, (unsigned)k->hi, (unsigned)k->lo,
               (unsigned)k->modeL, (unsigned)(k->flags & KF_CYCLE_MASK));
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Combiner decoding                                                                                */
/* ------------------------------------------------------------------------------------------------ */

typedef struct {
    uint8_t t0, t1; /* S_TEX0 / S_TEX1 that TEXEL0 and TEXEL1 read in this cycle */
    bool shade;
} CycleCtx;

/* Codes 0-5 mean the same in every slot */
static uint8_t dec_common(const CycleCtx* x, uint32_t code, bool alpha) {
    switch (code) {
        case 0:
            return alpha ? S_COMB_A : S_COMB;
        case 1:
            return x->t0 + (alpha ? 1 : 0);
        case 2:
            return x->t1 + (alpha ? 1 : 0);
        case 3:
            return alpha ? S_PRIM_A : S_PRIM;
        case 4:
            return !x->shade ? S_ZERO : alpha ? S_SHADE_A : S_SHADE;
        default:
            return alpha ? S_ENV_A : S_ENV;
    }
}

static uint8_t dec_rgb_a(const CycleCtx* x, uint32_t code) {
    return code < 6 ? dec_common(x, code, false) : code == 6 ? S_ONE : code == 7 ? S_HALF : S_ZERO;
}

static uint8_t dec_rgb_b(const CycleCtx* x, uint32_t code) {
    return code < 6 ? dec_common(x, code, false) : code == 7 ? S_K4 : S_ZERO;
}

static uint8_t dec_rgb_c(const CycleCtx* x, uint32_t code) {
    if (code < 6) {
        return dec_common(x, code, false);
    }
    switch (code) {
        case 7:
            return S_COMB_A;
        case 8:
            return x->t0 + 1;
        case 9:
            return x->t1 + 1;
        case 10:
            return S_PRIM_A;
        case 11:
            return x->shade ? S_SHADE_A : S_ZERO;
        case 12:
            return S_ENV_A;
        case 14:
            return S_PRIM_LOD;
        case 15:
            return S_K5;
        default: /* 6 key scale, 13 LOD fraction, 16-31 zero */
            return S_ZERO;
    }
}

static uint8_t dec_rgb_d(const CycleCtx* x, uint32_t code) {
    return code < 6 ? dec_common(x, code, false) : code == 6 ? S_ONE : S_ZERO;
}

static uint8_t dec_alpha_abd(const CycleCtx* x, uint32_t code) {
    return code < 6 ? dec_common(x, code, true) : code == 6 ? S_ONE : S_ZERO;
}

static uint8_t dec_alpha_c(const CycleCtx* x, uint32_t code) {
    return (code >= 1 && code < 6) ? dec_common(x, code, true) : code == 6 ? S_PRIM_LOD : S_ZERO;
}

/* Hardware cycle `cyc` (0 or 1) of G_SETCOMBINE (gbi.h GCCc0w0/GCCc1w0/GCCc0w1/GCCc1w1) */
static void decode_cycle(uint32_t hi, uint32_t lo, int cyc, const CycleCtx* x, Formula* col, Formula* alp) {
    if (cyc == 0) {
        col->a = dec_rgb_a(x, (hi >> 20) & 0xF);
        col->b = dec_rgb_b(x, (lo >> 28) & 0xF);
        col->c = dec_rgb_c(x, (hi >> 15) & 0x1F);
        col->d = dec_rgb_d(x, (lo >> 15) & 7);
        alp->a = dec_alpha_abd(x, (hi >> 12) & 7);
        alp->b = dec_alpha_abd(x, (lo >> 12) & 7);
        alp->c = dec_alpha_c(x, (hi >> 9) & 7);
        alp->d = dec_alpha_abd(x, (lo >> 9) & 7);
    } else {
        col->a = dec_rgb_a(x, (hi >> 5) & 0xF);
        col->b = dec_rgb_b(x, (lo >> 24) & 0xF);
        col->c = dec_rgb_c(x, hi & 0x1F);
        col->d = dec_rgb_d(x, (lo >> 6) & 7);
        alp->a = dec_alpha_abd(x, (lo >> 21) & 7);
        alp->b = dec_alpha_abd(x, (lo >> 3) & 7);
        alp->c = dec_alpha_c(x, (lo >> 18) & 7);
        alp->d = dec_alpha_abd(x, lo & 7);
    }
}

static bool formula_reads(const Formula* f, uint8_t s) {
    return f->a == s || f->b == s || f->c == s || f->d == s;
}

static void formula_subst(Formula* f, uint8_t from, uint8_t to) {
    if (f->a == from) {
        f->a = to;
    }
    if (f->b == from) {
        f->b = to;
    }
    if (f->c == from) {
        f->c = to;
    }
    if (f->d == from) {
        f->d = to;
    }
}

/* Cycle 0's value `v`, a plain source, replaces COMBINED (`comb`) in cycle 1. The RDP keeps cycle 0's constant 1
 * as 0x100, which the multiplier input reads as -1 (9-bit signed) and the others as +1:
 * (A - B) * -1 + D = (B - A) * 1 + D. */
static void formula_subst_comb(Formula* f, uint8_t comb, uint8_t v) {
    if (v == S_ONE && f->c == comb) {
        uint8_t t = f->a;

        f->a = f->b;
        f->b = t;
    }
    formula_subst(f, comb, v);
}

static TevOp tev_op(uint8_t a, uint8_t b, uint8_t c, uint8_t d, bool sub, bool clamp) {
    TevOp op = { a, b, c, d, sub, clamp, R_PREV, false };
    return op;
}

static const TevOp sNop = { S_ZERO, S_ZERO, S_ZERO, S_PREV, false, true, R_PREV, true };

static void chan_op(ChanCode* cc, uint8_t a, uint8_t b, uint8_t c, uint8_t d, bool sub) {
    cc->op[cc->n++] = tev_op(a, b, c, d, sub, true);
    cc->value = S_PREV;
}

/* (A - B) * C + D as TEV ops; the shortcuts of DESIGN.md first */
static void chan_build(ChanCode* cc, const Formula* f) {
    uint8_t A = f->a, B = f->b, C = f->c, D = f->d;

    cc->n = 0;
    cc->value = D;
    if (C == S_ZERO || A == B) {
        return;
    }
    if (B == S_ZERO) {
        if (A == S_ZERO) {
            return;
        }
        if (A == S_ONE) {
            if (D == S_ZERO) {
                cc->value = C;
                return;
            }
            chan_op(cc, C, S_ZERO, S_ZERO, D, false); /* C + D, without the alpha konst 1 */
            return;
        }
        chan_op(cc, S_ZERO, A, C, D, false); /* A * C + D */
    } else if (D == B) {
        chan_op(cc, B, A, C, S_ZERO, false); /* lerp(B, A, C) */
    } else if (A == S_ONE) {
        chan_op(cc, C, S_ZERO, B, D, false); /* C * (1 - B) + D */
    } else if (B == S_ONE) {
        chan_op(cc, C, S_ZERO, A, D, true); /* D - C * (1 - A) */
    } else if (A == S_ZERO) {
        chan_op(cc, S_ZERO, B, C, D, true); /* D - B * C */
    } else if ((B == S_COMB || B == S_COMB_A) && A != S_COMB && A != S_COMB_A) {
        /* D - B * C first: COMBINED is read before the partial result overwrites it in TEVPREV */
        cc->op[0] = tev_op(S_ZERO, B, C, D, true, false);
        cc->op[1] = tev_op(S_ZERO, A, C, S_PREV, false, true);
        cc->n = 2;
        cc->value = S_PREV;
    } else {
        /* A * C + D, then - B * C: TEV keeps the unclamped 11-bit partial in TEVPREV */
        cc->op[0] = tev_op(S_ZERO, A, C, D, false, false);
        cc->op[1] = tev_op(S_ZERO, B, C, S_PREV, true, true);
        cc->n = 2;
        cc->value = S_PREV;
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Blender                                                                                          */
/* ------------------------------------------------------------------------------------------------ */

enum { BC_IN, BC_MEM, BC_BL, BC_FOG, BC_ZERO };       /* blender colors (P, M) */
enum { BF_AIN, BF_AFOG, BF_ASHADE, BF_ZERO, BF_ONE, BF_1MA }; /* blender factors (A, B) */

#define BLEND_MAX_STEPS 6

typedef struct {
    uint8_t set; /* true: the current color becomes `src`, no stage */
    uint8_t src;
    TevOp op;    /* color op; S_IN / S_AIN refer to the color / alpha at that point */
} BlendStep;

typedef struct {
    uint8_t nSteps;
    BlendStep steps[BLEND_MAX_STEPS];
    bool tevUsesAIn;
    bool memUsed;
    uint8_t gxType, gxSrc, gxDst;
    uint8_t alphaFactor; /* BF_AIN / BF_AFOG / BF_ASHADE when the GX blend reads the source alpha, else BF_ZERO */
    bool colorUpdate;
    bool k3Used;
    uint8_t k3P, k3M;
} BlendPlan;

static void bl_set(BlendPlan* bp, uint8_t src) {
    if (bp->nSteps < BLEND_MAX_STEPS) {
        bp->steps[bp->nSteps].set = true;
        bp->steps[bp->nSteps].src = src;
        bp->nSteps++;
    }
    if (src == S_AIN) {
        bp->tevUsesAIn = true;
    }
}

static void bl_op(BlendPlan* bp, TevOp op) {
    if (bp->nSteps < BLEND_MAX_STEPS) {
        bp->steps[bp->nSteps].set = false;
        bp->steps[bp->nSteps].op = op;
        bp->nSteps++;
    }
    if (op.a == S_AIN || op.b == S_AIN || op.c == S_AIN || op.d == S_AIN) {
        bp->tevUsesAIn = true;
    }
}

static uint8_t bl_color_src(int c) {
    return c == BC_IN ? S_IN : c == BC_BL ? S_BLEND : c == BC_FOG ? S_FOG : S_ZERO;
}

static uint8_t bl_factor_src(int f) {
    return f == BF_AIN ? S_AIN : f == BF_AFOG ? S_FOG_A : f == BF_ASHADE ? S_SHADE_A : f == BF_ONE ? S_ONE : S_ZERO;
}

static uint8_t bl_gx_factor(int f) {
    return f == BF_ZERO ? GX_BL_ZERO : f == BF_ONE ? GX_BL_ONE : f == BF_1MA ? GX_BL_INVSRCALPHA : GX_BL_SRCALPHA;
}

static uint8_t bl_k3_color(int c) {
    return c == BC_FOG ? K3C_FOG : c == BC_BL ? K3C_BLEND : K3C_ZERO;
}

static bool bl_use_k3(BlendPlan* bp, const TevKey* k, int p, int m) {
    if (bp->k3Used) {
        tev_warn(W_K3, k, "blender needs two premultiplied constants, using the last");
    }
    bp->k3Used = true;
    bp->k3P = bl_k3_color(p);
    bp->k3M = bl_k3_color(m);
    return true;
}

/* One blender cycle P * A + M * B. a is BF_AIN/AFOG/ASHADE/ZERO/ONE, b is BF_1MA/ONE/ZERO (A_MEM counts as 1). */
static void plan_cycle(BlendPlan* bp, const TevKey* k, int p, int a, int m, int b) {
    int pass = -1;

    if (b == BF_1MA && (a == BF_ZERO || a == BF_ONE)) {
        b = (a == BF_ZERO) ? BF_ONE : BF_ZERO;
    }
    if (a == BF_ZERO) {
        pass = (b == BF_ZERO) ? BC_ZERO : m;
    } else if (a == BF_ONE && b == BF_ZERO) {
        pass = p;
    } else if (b == BF_1MA && p == m) {
        pass = p;
    }

    if (bp->memUsed) {
        /* The GX blend already took the framebuffer term; only a pass of its result can follow */
        if (pass != BC_IN) {
            tev_warn(W_BLEND, k, "blender cycle after a framebuffer blend ignored");
        }
        return;
    }

    if (pass >= 0) {
        if (pass == BC_MEM) {
            bp->memUsed = true;
            bp->colorUpdate = false;
        } else if (pass != BC_IN) {
            bl_set(bp, bl_color_src(pass));
        }
        return;
    }

    if (p == BC_MEM && m == BC_MEM) {
        /* MEM * (A + B), B being 0 or 1 here: the framebuffer times the source alpha, or plus itself times the
         * factor A (the TEV outputs A as its color, the GX blend computes src * dst + dst) */
        bp->memUsed = true;
        bp->gxType = GX_BM_BLEND;
        if (b == BF_ZERO) {
            bp->gxSrc = GX_BL_ZERO;
            bp->gxDst = GX_BL_SRCALPHA;
            bp->alphaFactor = a;
        } else {
            bl_set(bp, bl_factor_src(a));
            bp->gxSrc = GX_BL_DSTCLR;
            bp->gxDst = GX_BL_ONE;
        }
        return;
    }
    if (p == BC_MEM || m == BC_MEM) {
        int x = (p == BC_MEM) ? m : p;
        int fx = (p == BC_MEM) ? b : a;
        int fm = (p == BC_MEM) ? a : b;

        if (x != BC_IN) {
            bl_set(bp, bl_color_src(x));
        }
        bp->memUsed = true;
        bp->gxSrc = bl_gx_factor(fx);
        bp->gxDst = bl_gx_factor(fm);
        if (bp->gxSrc == GX_BL_ONE && bp->gxDst == GX_BL_ZERO) {
            bp->gxType = GX_BM_NONE;
        } else {
            bp->gxType = GX_BM_BLEND;
            if (bp->gxSrc >= GX_BL_SRCALPHA || bp->gxDst >= GX_BL_SRCALPHA) {
                bp->alphaFactor = a; /* the TEV program outputs A as its alpha */
            }
        }
        return;
    }

    /* No framebuffer term: TEV stages. Fog alpha is a konst like the fog/blend colors, and a TEV operation can
     * select only one konst, so konst color * fog alpha goes through the premultiplied K3. */
    {
        uint8_t P = bl_color_src(p), M = bl_color_src(m), A = bl_factor_src(a);
        bool konstP = (p == BC_BL || p == BC_FOG), konstM = (m == BC_BL || m == BC_FOG);
        bool konstA = (a == BF_AFOG);

        if (b == BF_1MA) {
            if (!konstA || (!konstP && !konstM)) {
                bl_op(bp, tev_op(M, P, A, S_ZERO, false, true));
            } else if (konstP && !konstM) {
                bl_use_k3(bp, k, p, BC_ZERO);
                bl_op(bp, tev_op(M, S_ZERO, A, S_ZERO, false, false));
                bl_op(bp, tev_op(S_ZERO, S_K3, S_ONE, S_PREV, false, true));
            } else if (konstM && !konstP) {
                bl_use_k3(bp, k, BC_ZERO, m);
                bl_op(bp, tev_op(S_ZERO, P, A, S_ZERO, false, false));
                bl_op(bp, tev_op(S_ZERO, S_K3, S_ONE, S_PREV, false, true));
            } else {
                bl_use_k3(bp, k, p, m);
                bl_set(bp, S_K3);
            }
        } else if (b == BF_ONE) {
            if (!konstA || (!konstP && !konstM)) {
                bl_op(bp, tev_op(S_ZERO, P, A, M, false, true));
            } else if (konstP && !konstM) {
                bl_use_k3(bp, k, p, BC_ZERO);
                bl_op(bp, tev_op(S_ZERO, S_K3, S_ONE, M, false, true));
            } else if (konstM && !konstP) {
                bl_op(bp, tev_op(S_ZERO, P, A, S_ZERO, false, false));
                bl_op(bp, tev_op(S_ZERO, M, S_ONE, S_PREV, false, true));
            } else {
                tev_warn(W_BLEND, k, "blender sum of two constants approximated");
                bl_op(bp, tev_op(S_ZERO, P, A, M, false, true));
            }
        } else {
            if (konstA && konstP) {
                bl_use_k3(bp, k, p, BC_ZERO);
                bl_set(bp, S_K3);
            } else {
                bl_op(bp, tev_op(S_ZERO, P, A, S_ZERO, false, true));
            }
        }
    }
}

static int bl_factor_a(uint32_t sel, bool shade, bool selCvg) {
    switch (sel & 3) {
        case 0:
            return selCvg ? BF_ONE : BF_AIN; /* ALPHA_CVG_SEL: coverage, full inside the primitive */
        case 1:
            return BF_AFOG;
        case 2:
            return shade ? BF_ASHADE : BF_ZERO;
        default:
            return BF_ZERO;
    }
}

static int bl_factor_b(uint32_t sel) {
    switch (sel & 3) {
        case 0:
            return BF_1MA;
        case 3:
            return BF_ZERO;
        default:
            return BF_ONE; /* G_BL_A_MEM: memory coverage, full after opaque draws; G_BL_1 */
    }
}

/* Blender cycle settings: cycle 0 (GBL_c1) at bits 30/26/22/18, cycle 1 (GBL_c2) at bits 28/24/20/16 */
static void plan_blender_cycle(BlendPlan* bp, const TevKey* k, int cyc, bool last, bool shade, bool selCvg) {
    uint32_t L = k->modeL;
    int shift = (cyc == 0) ? 2 : 0;
    int p = (L >> (28 + shift)) & 3;
    int a = bl_factor_a(L >> (24 + shift), shade, selCvg);
    int m = (L >> (20 + shift)) & 3;
    int b = bl_factor_b(L >> (16 + shift));

    if (last && !(L & FORCE_BL)) {
        /* Last cycle without FORCE_BL outputs P (it blends only at antialiased edges) */
        a = BF_ONE;
        b = BF_ZERO;
    }
    plan_cycle(bp, k, p, a, m, b);
}

static void plan_blender(BlendPlan* bp, const TevKey* k, bool twoCycle, bool shade, bool selCvg) {
    memset(bp, 0, sizeof(*bp));
    bp->gxType = GX_BM_NONE;
    bp->gxSrc = GX_BL_ONE;
    bp->gxDst = GX_BL_ZERO;
    bp->alphaFactor = BF_ZERO;
    bp->colorUpdate = true;

    if (twoCycle) {
        /* Cycle 0 of 2-cycle mode always blends, cycle 1 is the last */
        plan_blender_cycle(bp, k, 0, false, shade, selCvg);
        plan_blender_cycle(bp, k, 1, true, shade, selCvg);
    } else {
        /* 1-cycle mode uses blender cycle 0 */
        plan_blender_cycle(bp, k, 0, true, shade, selCvg);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Program building                                                                                 */
/* ------------------------------------------------------------------------------------------------ */

typedef struct {
    const TevKey* key;
    IrStage st[TEV_MAX_STAGES + 4];
    int n;
    int cyc0End; /* stages [0, cyc0End) compute cycle 0 of a two-cycle program (0 otherwise) */
    bool overflow;
    uint8_t curColor, curAlpha; /* where the current color / alpha is: a source, or S_PREV */
    uint8_t combColorReg, combAlphaReg, tex1Reg;
} Compiler;

static IrStage* ir_append(Compiler* c) {
    IrStage* st;

    if (c->n >= TEV_MAX_STAGES) {
        c->overflow = true;
        c->n = TEV_MAX_STAGES - 1; /* keep writing into the last stage; the result is wrong but bounded */
    }
    st = &c->st[c->n++];
    st->col = sNop;
    st->alp = sNop;
    st->prefetch = false;
    return st;
}

/* Right-align both channels' ops so that each channel's result is written by the cycle's last stage and
 * COMBINED stays readable in TEVPREV until a channel's first op */
static void schedule_cycle(Compiler* c, const ChanCode* col, const ChanCode* alp) {
    int n = col->n > alp->n ? col->n : alp->n;
    int s;

    for (s = 0; s < n; s++) {
        IrStage* st = ir_append(c);
        int ci = s - (n - col->n), ai = s - (n - alp->n);

        if (ci >= 0) {
            st->col = col->op[ci];
        }
        if (ai >= 0) {
            st->alp = alp->op[ai];
        }
    }
}

static bool op_reads(const TevOp* op, uint8_t s) {
    return op->a == s || op->b == s || op->c == s || op->d == s;
}

static void op_subst(TevOp* op, uint8_t from, uint8_t to) {
    if (op->a == from) {
        op->a = to;
    }
    if (op->b == from) {
        op->b = to;
    }
    if (op->c == from) {
        op->c = to;
    }
    if (op->d == from) {
        op->d = to;
    }
}

static bool ir_reads(const Compiler* c, uint8_t s) {
    int i;

    for (i = 0; i < c->n; i++) {
        if (op_reads(&c->st[i].col, s) || op_reads(&c->st[i].alp, s)) {
            return true;
        }
    }
    return false;
}

static void append_blender(Compiler* c, const BlendPlan* bp) {
    int i;

    for (i = 0; i < bp->nSteps; i++) {
        const BlendStep* step = &bp->steps[i];
        IrStage* st;

        if (step->set) {
            c->curColor = (step->src == S_AIN) ? ((c->curAlpha == S_PREV) ? S_PREV_A : c->curAlpha) : step->src;
            continue;
        }
        st = ir_append(c);
        st->col = step->op;
        op_subst(&st->col, S_IN, c->curColor);
        op_subst(&st->col, S_AIN, (c->curAlpha == S_PREV) ? S_PREV_A : c->curAlpha);
        c->curColor = S_PREV;
    }
}

/* Whether a color op reading `s` in stage `i` would see a value that stage itself is still writing: a TEV
 * stage reads TEVPREV before it writes it, so the alpha that stage computes (S_PREV_A), or cycle 0's alpha
 * computed by the last cycle-0 stage (S_COMB_A), is not available in it yet. */
static bool stage_produces(const Compiler* c, int i, uint8_t s) {
    const IrStage* st = &c->st[i];

    if (s == S_PREV_A) {
        return !st->alp.nop;
    }
    if (s == S_COMB_A) {
        return i < c->cyc0End && !st->alp.nop;
    }
    return false;
}

/* Write the current color / alpha into TEVPREV with the last stage when they are still plain sources */
static void materialize(Compiler* c) {
    bool needC = (c->curColor != S_PREV), needA = (c->curAlpha != S_PREV);
    IrStage* st = (c->n > 0) ? &c->st[c->n - 1] : NULL;

    if (!needC && !needA && st != NULL) {
        return;
    }
    if (st == NULL || st->prefetch || (needC && !st->col.nop) || (needA && !st->alp.nop) ||
        (needC && stage_produces(c, c->n - 1, c->curColor))) {
        st = ir_append(c);
    }
    if (needC) {
        st->col = tev_op(S_ZERO, S_ZERO, S_ZERO, c->curColor, false, true);
    }
    if (needA) {
        st->alp = tev_op(S_ZERO, S_ZERO, S_ZERO, c->curAlpha, false, true);
    }
    c->curColor = c->curAlpha = S_PREV;
}

static int texmask_src(uint8_t s) {
    return (s == S_TEX0 || s == S_TEX0_A) ? 1 : (s == S_TEX1 || s == S_TEX1_A) ? 2 : 0;
}

static int texmask_op(const TevOp* op) {
    return texmask_src(op->a) | texmask_src(op->b) | texmask_src(op->c) | texmask_src(op->d);
}

/* ------------------------------------------------------------------------------------------------ */
/* Lowering to GX                                                                                   */
/* ------------------------------------------------------------------------------------------------ */

typedef struct {
    int tex;      /* -1 none, 0 or 1 */
    bool shade;
    int kc, ka;   /* konst selections, -1 none */
    bool conflict;
} StageUse;

static void use_tex(StageUse* u, int t) {
    if (u->tex >= 0 && u->tex != t) {
        u->conflict = true;
    }
    u->tex = t;
}

static void use_konst(StageUse* u, int* slot, int sel, const TevKey* k) {
    if (*slot >= 0 && *slot != sel) {
        tev_warn(W_KONST, k, "TEV operation needs two konst values, using the first");
        return;
    }
    *slot = sel;
}

static uint8_t reg_color(uint8_t reg) {
    static const uint8_t tbl[4] = { GX_CC_CPREV, GX_CC_C0, GX_CC_C1, GX_CC_C2 };
    return tbl[reg & 3];
}

static uint8_t reg_alpha_as_color(uint8_t reg) {
    static const uint8_t tbl[4] = { GX_CC_APREV, GX_CC_A0, GX_CC_A1, GX_CC_A2 };
    return tbl[reg & 3];
}

static uint8_t reg_alpha(uint8_t reg) {
    static const uint8_t tbl[4] = { GX_CA_APREV, GX_CA_A0, GX_CA_A1, GX_CA_A2 };
    return tbl[reg & 3];
}

static uint8_t lower_color(const Compiler* c, uint8_t s, StageUse* u, bool prefetch) {
    switch (s) {
        case S_ONE:
            return GX_CC_ONE;
        case S_HALF:
            return GX_CC_HALF;
        case S_PREV:
            return GX_CC_CPREV;
        case S_PREV_A:
            return GX_CC_APREV;
        case S_COMB:
            return reg_color(c->combColorReg);
        case S_COMB_A:
            return reg_alpha_as_color(c->combAlphaReg);
        case S_TEX0:
            use_tex(u, 0);
            return GX_CC_TEXC;
        case S_TEX0_A:
            use_tex(u, 0);
            return GX_CC_TEXA;
        case S_TEX1:
        case S_TEX1_A:
            if (c->tex1Reg != R_NONE && !prefetch) {
                return (s == S_TEX1) ? reg_color(c->tex1Reg) : reg_alpha_as_color(c->tex1Reg);
            }
            use_tex(u, 1);
            return (s == S_TEX1) ? GX_CC_TEXC : GX_CC_TEXA;
        case S_PRIM:
            return GX_CC_C0;
        case S_PRIM_A:
            return GX_CC_A0;
        case S_ENV:
            return GX_CC_C1;
        case S_ENV_A:
            return GX_CC_A1;
        case S_SHADE:
            u->shade = true;
            return GX_CC_RASC;
        case S_SHADE_A:
            u->shade = true;
            return GX_CC_RASA;
        case S_PRIM_LOD:
            use_konst(u, &u->kc, GX_TEV_KCSEL_K2_R, c->key);
            return GX_CC_KONST;
        case S_K4:
            use_konst(u, &u->kc, GX_TEV_KCSEL_K2_G, c->key);
            return GX_CC_KONST;
        case S_K5:
            use_konst(u, &u->kc, GX_TEV_KCSEL_K2_B, c->key);
            return GX_CC_KONST;
        case S_FOG:
            use_konst(u, &u->kc, GX_TEV_KCSEL_K0, c->key);
            return GX_CC_KONST;
        case S_FOG_A:
            use_konst(u, &u->kc, GX_TEV_KCSEL_K0_A, c->key);
            return GX_CC_KONST;
        case S_BLEND:
            use_konst(u, &u->kc, GX_TEV_KCSEL_K1, c->key);
            return GX_CC_KONST;
        case S_K3:
            use_konst(u, &u->kc, GX_TEV_KCSEL_K3, c->key);
            return GX_CC_KONST;
        default:
            return GX_CC_ZERO;
    }
}

static uint8_t lower_alpha(const Compiler* c, uint8_t s, StageUse* u, bool prefetch) {
    switch (s) {
        case S_ONE:
            use_konst(u, &u->ka, GX_TEV_KASEL_1, c->key);
            return GX_CA_KONST;
        case S_HALF:
            use_konst(u, &u->ka, GX_TEV_KASEL_1_2, c->key);
            return GX_CA_KONST;
        case S_PREV:
            return GX_CA_APREV;
        case S_COMB_A:
            return reg_alpha(c->combAlphaReg);
        case S_TEX0_A:
            use_tex(u, 0);
            return GX_CA_TEXA;
        case S_TEX1_A:
            if (c->tex1Reg != R_NONE && !prefetch) {
                return reg_alpha(c->tex1Reg);
            }
            use_tex(u, 1);
            return GX_CA_TEXA;
        case S_PRIM_A:
            return GX_CA_A0;
        case S_ENV_A:
            return GX_CA_A1;
        case S_SHADE_A:
            u->shade = true;
            return GX_CA_RASA;
        case S_PRIM_LOD:
            use_konst(u, &u->ka, GX_TEV_KASEL_K2_R, c->key);
            return GX_CA_KONST;
        case S_FOG_A:
            use_konst(u, &u->ka, GX_TEV_KASEL_K0_A, c->key);
            return GX_CA_KONST;
        default:
            return GX_CA_ZERO;
    }
}

static uint8_t lower_opbits(const TevOp* op) {
    return (op->sub ? 1 : 0) | (op->clamp ? 2 : 0) | (uint8_t)((op->dst & 0xF) << 4);
}

static void lower(Compiler* c, TevProgram* p) {
    int i;

    p->numStages = (uint8_t)c->n;
    for (i = 0; i < c->n; i++) {
        const IrStage* ir = &c->st[i];
        TevStage* st = &p->stages[i];
        StageUse u = { -1, false, -1, -1, false };

        st->cin[0] = lower_color(c, ir->col.a, &u, ir->prefetch);
        st->cin[1] = lower_color(c, ir->col.b, &u, ir->prefetch);
        st->cin[2] = lower_color(c, ir->col.c, &u, ir->prefetch);
        st->cin[3] = lower_color(c, ir->col.d, &u, ir->prefetch);
        st->ain[0] = lower_alpha(c, ir->alp.a, &u, ir->prefetch);
        st->ain[1] = lower_alpha(c, ir->alp.b, &u, ir->prefetch);
        st->ain[2] = lower_alpha(c, ir->alp.c, &u, ir->prefetch);
        st->ain[3] = lower_alpha(c, ir->alp.d, &u, ir->prefetch);
        st->cop = lower_opbits(&ir->col);
        st->aop = lower_opbits(&ir->alp);
        if (u.conflict) {
            tev_warn(W_TEXTURES, c->key, "TEV stage needs both textures");
        }
        st->texmap = (u.tex == 1) ? GX_TEXMAP1 : (u.tex == 0) ? GX_TEXMAP0 : GX_TEXMAP_NULL;
        st->chan = u.shade ? GX_COLOR0A0 : GX_COLORNULL;
        st->kcsel = (u.kc >= 0) ? (uint8_t)u.kc : GX_TEV_KCSEL_1;
        st->kasel = (u.ka >= 0) ? (uint8_t)u.ka : GX_TEV_KASEL_1;
        if (u.tex == 0) {
            p->info.usesTexel0 = true;
        } else if (u.tex == 1) {
            p->info.usesTexel1 = true;
        }
        if (u.shade) {
            p->info.usesShade = true;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Compilation                                                                                      */
/* ------------------------------------------------------------------------------------------------ */

/* Scratch register for cycle 0's result or TEXEL1: register 2, or the prim / env register when unused */
static uint8_t alloc_reg(Compiler* c, uint8_t* used) {
    static const uint8_t order[3] = { GX_TEVREG2, GX_TEVREG0, GX_TEVREG1 };
    int i;

    for (i = 0; i < 3; i++) {
        uint8_t r = order[i];

        if (*used & (1 << r)) {
            continue;
        }
        if (r == GX_TEVREG0 && (ir_reads(c, S_PRIM) || ir_reads(c, S_PRIM_A))) {
            continue;
        }
        if (r == GX_TEVREG1 && (ir_reads(c, S_ENV) || ir_reads(c, S_ENV_A))) {
            continue;
        }
        *used |= (uint8_t)(1 << r);
        return r;
    }
    tev_warn(W_REGS, c->key, "out of TEV registers");
    return R_PREV;
}

/* Alpha the TEV program outputs (for the GX blend and the alpha compare) */
enum { OUT_DONTCARE, OUT_FINAL, OUT_CYCLE0, OUT_FOG_A, OUT_SHADE_A };

static void compile_copy(TevProgram* p, const TevKey* k) {
    TevStage* st = &p->stages[0];

    /* COPY mode writes the texel unchanged; the only test is its alpha bit (16-bit framebuffer) */
    p->numStages = 1;
    st->texmap = GX_TEXMAP0;
    st->chan = GX_COLORNULL;
    st->cin[0] = st->cin[1] = st->cin[2] = GX_CC_ZERO;
    st->cin[3] = GX_CC_TEXC;
    st->ain[0] = st->ain[1] = st->ain[2] = GX_CA_ZERO;
    st->ain[3] = GX_CA_TEXA;
    st->cop = st->aop = 2 | (GX_TEVPREV << 4);
    st->kcsel = GX_TEV_KCSEL_1;
    st->kasel = GX_TEV_KASEL_1;
    if (k->modeL & 1) {
        p->acComp1 = GX_GEQUAL;
        p->acRef1 = 0x80;
    }
    p->info.usesTexel0 = true;
}

static void compile(TevProgram* p, const TevKey* k) {
    Compiler cc;
    Compiler* c = &cc;
    BlendPlan bp;
    uint32_t L = k->modeL;
    int cycleType = k->flags & KF_CYCLE_MASK;
    bool shade = (k->flags & KF_SHADE) != 0;
    bool selCvg = (L & ALPHA_CVG_SEL) && !(L & CVG_X_ALPHA);
    bool twoCycle = (cycleType == CYC_2CYCLE);
    bool thr = ((L & 3) == 1) && !selCvg; /* G_AC_THRESHOLD; coverage alpha always passes */
    bool dither = ((L & 3) == 3) && !selCvg;
    bool cutout = (L & AA_EN) && (L & CVG_X_ALPHA);
    /* Texture and fill rectangles have z 0 (no z coefficients, angrylion rdp_tex_rect / rdp_fill_rect) unless
     * G_ZS_PRIM; triangles without G_ZBUFFER have no z from the RSP */
    bool zAvail = (k->flags & (KF_ZBUF | KF_RECT)) || (L & G_ZS_PRIM);
    int out;
    Formula fcol[2], falp[2];
    ChanCode ccol[2], calp[2];
    int ncyc, i, cyc0End;
    uint8_t usedRegs = 0;

    memset(p, 0, sizeof(*p));
    p->key = *k;
    p->blendType = GX_BM_NONE;
    p->blendSrc = GX_BL_ONE;
    p->blendDst = GX_BL_ZERO;
    p->colorUpdate = GX_TRUE;
    p->zFunc = GX_ALWAYS;
    p->acComp1 = GX_ALWAYS;

    if (cycleType == CYC_COPY) {
        compile_copy(p, k);
        return;
    }

    memset(c, 0, sizeof(*c));
    c->key = k;
    c->combColorReg = c->combAlphaReg = R_PREV;
    c->tex1Reg = R_NONE;

    /* Blender, then which alpha the program must output */
    plan_blender(&bp, k, twoCycle, shade, selCvg);
    if (dither && bp.gxType == GX_BM_NONE && bp.colorUpdate) {
        /* Random-threshold alpha compare: a fraction alpha of the pixels survives; blend instead */
        bp.gxType = GX_BM_BLEND;
        bp.gxSrc = GX_BL_SRCALPHA;
        bp.gxDst = GX_BL_INVSRCALPHA;
        tev_warn(W_DITHER, k, "G_AC_DITHER approximated by alpha blending");
    }
    if (bp.alphaFactor == BF_AFOG) {
        out = OUT_FOG_A;
    } else if (bp.alphaFactor == BF_ASHADE) {
        out = OUT_SHADE_A;
    } else if (bp.alphaFactor == BF_AIN || bp.tevUsesAIn || cutout) {
        out = OUT_FINAL;
    } else if (thr || dither) {
        out = twoCycle ? OUT_CYCLE0 : OUT_FINAL;
    } else {
        out = OUT_DONTCARE;
    }
    if ((thr || cutout) && (out == OUT_FOG_A || out == OUT_SHADE_A)) {
        tev_warn(W_ALPHA, k, "alpha compare tests the blender's fog/shade alpha");
    }

    /* Combiner cycles (hardware cycle numbers and texel mapping, see the header) */
    {
        CycleCtx x0 = { S_TEX0, S_TEX1, shade }, x1 = { S_TEX1, S_TEX0, shade }, x1c = { S_TEX0, S_TEX0, shade };

        if (twoCycle) {
            decode_cycle(k->hi, k->lo, 0, &x0, &fcol[0], &falp[0]);
            decode_cycle(k->hi, k->lo, 1, &x1, &fcol[1], &falp[1]);
            ncyc = 2;
        } else {
            decode_cycle(k->hi, k->lo, 1, &x1c, &fcol[0], &falp[0]);
            ncyc = 1;
            if (formula_reads(&fcol[0], S_COMB) || formula_reads(&fcol[0], S_COMB_A) ||
                formula_reads(&falp[0], S_COMB_A)) {
                /* COMBINED in 1-cycle mode is the previous pixel's output; run cycle 0 first instead */
                tev_warn(W_1CYCLE_COMB, k, "1-cycle combiner reads COMBINED, running both cycles");
                fcol[1] = fcol[0];
                falp[1] = falp[0];
                decode_cycle(k->hi, k->lo, 0, &x1c, &fcol[0], &falp[0]);
                ncyc = 2;
            }
        }
    }
    if (formula_reads(&fcol[0], S_COMB) || formula_reads(&fcol[0], S_COMB_A) || formula_reads(&falp[0], S_COMB_A)) {
        tev_warn(W_COMB_FIRST, k, "first combiner cycle reads COMBINED (0 used)");
        formula_subst(&fcol[0], S_COMB, S_ZERO);
        formula_subst(&fcol[0], S_COMB_A, S_ZERO);
        formula_subst(&falp[0], S_COMB_A, S_ZERO);
    }
    if (twoCycle && thr && out == OUT_FINAL) {
        const Formula* f = &falp[1];

        if (!((f->c == S_ZERO || f->a == f->b) && f->d == S_COMB_A)) {
            tev_warn(W_ALPHA, k, "2-cycle alpha compare tests the final alpha instead of cycle 0's");
        }
    }

    /* Drop work whose result nobody reads */
    {
        Formula* lastA = &falp[ncyc - 1];
        Formula pass0 = { S_ZERO, S_ZERO, S_ZERO, S_ZERO };

        if (out == OUT_CYCLE0 && ncyc == 2 && !bp.tevUsesAIn) {
            Formula passComb = { S_ZERO, S_ZERO, S_ZERO, S_COMB_A };
            *lastA = passComb;
        } else if (out != OUT_FINAL && !bp.tevUsesAIn) {
            *lastA = pass0;
        }
        if (ncyc == 2) {
            if (!formula_reads(&fcol[1], S_COMB)) {
                fcol[0] = pass0;
            }
            if (!formula_reads(&fcol[1], S_COMB_A) && !formula_reads(&falp[1], S_COMB_A)) {
                falp[0] = pass0;
            }
        }
    }

    /* Cycle 0 values that are plain sources replace COMBINED directly */
    chan_build(&ccol[0], &fcol[0]);
    chan_build(&calp[0], &falp[0]);
    if (ncyc == 2) {
        if (ccol[0].n == 0) {
            formula_subst_comb(&fcol[1], S_COMB, ccol[0].value);
        }
        if (calp[0].n == 0) {
            formula_subst_comb(&fcol[1], S_COMB_A, calp[0].value);
            formula_subst_comb(&falp[1], S_COMB_A, calp[0].value);
        }
        chan_build(&ccol[1], &fcol[1]);
        chan_build(&calp[1], &falp[1]);
    }

    schedule_cycle(c, &ccol[0], &calp[0]);
    cyc0End = c->n;
    c->cyc0End = (ncyc == 2) ? cyc0End : 0;
    c->curColor = ccol[0].value;
    c->curAlpha = calp[0].value;
    if (ncyc == 2) {
        schedule_cycle(c, &ccol[1], &calp[1]);
        /* A cycle 1 that passes COMBINED leaves cycle 0's value in TEVPREV */
        c->curColor = (ccol[1].value == S_COMB) ? S_PREV : ccol[1].value;
        c->curAlpha = (calp[1].value == S_COMB_A) ? S_PREV : calp[1].value;
    }

    append_blender(c, &bp);
    if (out == OUT_FOG_A) {
        c->curAlpha = S_FOG_A;
    } else if (out == OUT_SHADE_A) {
        c->curAlpha = S_SHADE_A;
    }
    materialize(c);

    /* Cycle 0's result must move out of TEVPREV when a later stage reads it after TEVPREV was overwritten */
    if (ncyc == 2) {
        bool colIntact = true, alpIntact = true, saveC = false, saveA = false;

        for (i = cyc0End; i < c->n; i++) {
            const IrStage* st = &c->st[i];

            if (op_reads(&st->col, S_COMB) && !colIntact) {
                saveC = true;
            }
            if ((op_reads(&st->col, S_COMB_A) || op_reads(&st->alp, S_COMB_A)) && !alpIntact) {
                saveA = true;
            }
            if (!st->col.nop) {
                colIntact = false;
            }
            if (!st->alp.nop) {
                alpIntact = false;
            }
        }
        if ((saveC || saveA) && cyc0End > 0) {
            uint8_t r = alloc_reg(c, &usedRegs);

            if (saveC) {
                c->combColorReg = r;
                c->st[cyc0End - 1].col.dst = r;
            }
            if (saveA) {
                c->combAlphaReg = r;
                c->st[cyc0End - 1].alp.dst = r;
            }
        }
    }

    /* A TEV stage samples one texture: preload TEXEL1 into a register when a stage needs both */
    for (i = 0; i < c->n; i++) {
        if ((texmask_op(&c->st[i].col) | texmask_op(&c->st[i].alp)) == 3) {
            break;
        }
    }
    if (i < c->n && c->n < TEV_MAX_STAGES) {
        uint8_t r = alloc_reg(c, &usedRegs);

        if (r != R_PREV) {
            memmove(&c->st[1], &c->st[0], sizeof(c->st[0]) * (size_t)c->n);
            c->n++;
            c->st[0].col = tev_op(S_ZERO, S_ZERO, S_ZERO, S_TEX1, false, true);
            c->st[0].alp = tev_op(S_ZERO, S_ZERO, S_ZERO, S_TEX1_A, false, true);
            c->st[0].col.dst = c->st[0].alp.dst = r;
            c->st[0].prefetch = true;
            c->tex1Reg = r;
        }
    }
    if (c->overflow) {
        tev_warn(W_STAGES, k, "too many TEV stages");
    }

    lower(c, p);

    /* Pixel engine */
    p->k3Used = bp.k3Used;
    p->k3P = bp.k3P;
    p->k3M = bp.k3M;
    p->blendType = bp.gxType;
    p->blendSrc = bp.gxSrc;
    p->blendDst = bp.gxDst;
    p->colorUpdate = bp.colorUpdate ? GX_TRUE : GX_FALSE;
    p->acThreshold = thr;
    if (dither) {
        p->acComp1 = GX_GREATER;
        p->acRef1 = 0;
    }
    if (cutout) {
        /* coverage = alpha * coverage >> 8 (3 bits): zero below alpha 32 */
        p->acComp1 = GX_GEQUAL;
        p->acRef1 = 32;
    }

    /* Depth (angrylion z_compare): ZMODE_XLU passes strictly nearer ("infront"); ZMODE_OPA/INTER do too for a
     * fully covered pixel (coverage overflow), which every GX pixel is, and pass nearer or equal within the z slope
     * only at partially covered edges; ZMODE_DEC passes within the slope, here nearer or equal after the bias. The
     * RDP passes every mode but decal over a cleared z-buffer; GX does too, as only a primitive depth of 0x7FFF
     * reaches the cleared value (MM never compares with it). Rectangles test and write their z 0 (gfx_gx.c). */
    p->info.depthTest = (L & Z_CMP) && zAvail;
    p->info.depthWrite = (L & Z_UPD) && zAvail;
    p->info.decal = (L & ZMODE_MASK) == ZMODE_DEC;
    p->zEnable = (p->info.depthTest || p->info.depthWrite) ? GX_TRUE : GX_FALSE;
    p->zFunc = !p->info.depthTest ? GX_ALWAYS : p->info.decal ? GX_LEQUAL : GX_LESS;
    p->zUpdate = p->info.depthWrite ? GX_TRUE : GX_FALSE;
}

/* ------------------------------------------------------------------------------------------------ */
/* Cache and upload                                                                                 */
/* ------------------------------------------------------------------------------------------------ */

static TevProgram sCache[TEV_CACHE_SIZE];
static const TevProgram* sCurrent; /* program whose stages and pixel engine state GX holds */
static bool sColorsValid;
static uint32_t sPrim, sEnv, sFog, sBlend, sK3Key;
static uint8_t sPrimLod;
static uint8_t sAlphaRef; /* threshold uploaded with GX_SetAlphaCompare */

static inline GXColor rgba(uint32_t c) {
    GXColor col = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
    return col;
}

static uint32_t hash_key(const TevKey* k) {
    uint32_t h = k->hi * 0x9E3779B1u;

    h ^= k->lo * 0x85EBCA77u;
    h ^= k->modeL * 0xC2B2AE3Du;
    h ^= k->flags * 0x27D4EB2Fu;
    return h ^ (h >> 15);
}

static bool key_eq(const TevKey* a, const TevKey* b) {
    return a->hi == b->hi && a->lo == b->lo && a->modeL == b->modeL && a->flags == b->flags;
}

static const TevProgram* lookup(const TevKey* k) {
    uint32_t h = hash_key(k);
    TevProgram* victim = &sCache[h & (TEV_CACHE_SIZE - 1)];
    int i;

    for (i = 0; i < TEV_CACHE_PROBE; i++) {
        TevProgram* p = &sCache[(h + (uint32_t)i) & (TEV_CACHE_SIZE - 1)];

        if (!(p->key.flags & KF_VALID)) {
            victim = p;
            break;
        }
        if (key_eq(&p->key, k)) {
            return p;
        }
    }
    if (victim == sCurrent) {
        sCurrent = NULL;
    }
    compile(victim, k);
    return victim;
}

static void upload_program(const TevProgram* p) {
    int i;

    GX_SetNumTevStages(p->numStages);
    for (i = 0; i < p->numStages; i++) {
        const TevStage* st = &p->stages[i];
        uint8_t tc = (st->texmap == GX_TEXMAP1) ? GX_TEXCOORD1 : (st->texmap == GX_TEXMAP0) ? GX_TEXCOORD0
                                                                                             : GX_TEXCOORDNULL;

        GX_SetTevOrder(i, tc, st->texmap, st->chan);
        GX_SetTevColorIn(i, st->cin[0], st->cin[1], st->cin[2], st->cin[3]);
        GX_SetTevAlphaIn(i, st->ain[0], st->ain[1], st->ain[2], st->ain[3]);
        GX_SetTevColorOp(i, st->cop & 1, GX_TB_ZERO, GX_CS_SCALE_1, (st->cop >> 1) & 1, st->cop >> 4);
        GX_SetTevAlphaOp(i, st->aop & 1, GX_TB_ZERO, GX_CS_SCALE_1, (st->aop >> 1) & 1, st->aop >> 4);
        GX_SetTevKColorSel(i, st->kcsel);
        GX_SetTevKAlphaSel(i, st->kasel);
    }
    GX_SetBlendMode(p->blendType, p->blendSrc, p->blendDst, GX_LO_CLEAR);
    GX_SetZMode(p->zEnable, p->zFunc, p->zUpdate);
    GX_SetZCompLoc((p->acThreshold || p->acComp1 != GX_ALWAYS) ? GX_FALSE : GX_TRUE);
    GX_SetColorUpdate(p->colorUpdate);
    GX_SetAlphaUpdate(GX_FALSE);
}

static void upload_alpha_compare(const TevProgram* p, uint8_t ref) {
    GX_SetAlphaCompare(p->acThreshold ? GX_GEQUAL : GX_ALWAYS, ref, GX_AOP_AND, p->acComp1, p->acRef1);
    sAlphaRef = ref;
}

static uint8_t k3_channel(int sel, uint32_t fog, uint32_t blend, int shift) {
    uint32_t c = (sel == K3C_FOG) ? fog : (sel == K3C_BLEND) ? blend : 0;
    return (uint8_t)(c >> shift);
}

static uint8_t k3_lerp(const TevProgram* p, uint32_t fog, uint32_t blend, int shift) {
    uint32_t fa = fog & 0xFF;
    uint32_t v = k3_channel(p->k3P, fog, blend, shift) * fa + k3_channel(p->k3M, fog, blend, shift) * (255 - fa);

    return (uint8_t)((v + 127) / 255);
}

static void upload_k3(const TevProgram* p, uint32_t fog, uint32_t blend) {
    GXColor k;

    k.r = k3_lerp(p, fog, blend, 24);
    k.g = k3_lerp(p, fog, blend, 16);
    k.b = k3_lerp(p, fog, blend, 8);
    k.a = (uint8_t)fog;
    GX_SetTevKColor(GX_KCOLOR3, k);
}

void gfx_tev_invalidate(void) {
    sCurrent = NULL;
    sColorsValid = false;
}

void gfx_tev_init(void) {
    memset(sCache, 0, sizeof(sCache));
    sWarned = 0;
    gfx_tev_invalidate();
}

void gfx_tev_apply(GfxPrimKind kind, GfxTevInfo* out) {
    const TevProgram* p;
    TevKey k;
    uint32_t cycleType = (gGfxRdp.otherModeH >> G_MDSFT_CYCLETYPE) & 3;
    bool changed;
    uint32_t k3Key;
    uint8_t ref;

    if (cycleType == CYC_FILL) {
        /* gfx_gx.c draws FILL mode itself and changes TEV state for it */
        memset(out, 0, sizeof(*out));
        gfx_tev_invalidate();
        return;
    }
    if (gGfxRdp.dirty == GFX_DIRTY_ALL) {
        /* New task: GX state may have been changed by the EFB copy */
        gfx_tev_invalidate();
    }

    k.hi = gGfxRdp.combineHi & 0x00FFFFFF;
    k.lo = gGfxRdp.combineLo;
    k.modeL = gGfxRdp.otherModeL;
    k.flags = KF_VALID | cycleType;
    if (kind == GFX_PRIM_TRIANGLE) {
        if (gGfxRsp.geometryMode & G_SHADE) {
            k.flags |= KF_SHADE;
        }
        if (gGfxRsp.geometryMode & G_ZBUFFER) {
            k.flags |= KF_ZBUF;
        }
    } else {
        k.flags |= KF_RECT;
    }

    p = lookup(&k);
    changed = (p != sCurrent);
    if (changed) {
        upload_program(p);
        sCurrent = p;
    }

    if (!sColorsValid || gGfxRdp.primColor != sPrim) {
        GX_SetTevColor(GX_TEVREG0, rgba(gGfxRdp.primColor));
        sPrim = gGfxRdp.primColor;
    }
    if (!sColorsValid || gGfxRdp.envColor != sEnv) {
        GX_SetTevColor(GX_TEVREG1, rgba(gGfxRdp.envColor));
        sEnv = gGfxRdp.envColor;
    }
    if (!sColorsValid || gGfxRdp.fogColor != sFog) {
        GX_SetTevKColor(GX_KCOLOR0, rgba(gGfxRdp.fogColor));
    }
    if (!sColorsValid || gGfxRdp.blendColor != sBlend) {
        GX_SetTevKColor(GX_KCOLOR1, rgba(gGfxRdp.blendColor));
    }
    if (!sColorsValid || gGfxRdp.primLodFrac != sPrimLod) {
        GXColor k2 = { gGfxRdp.primLodFrac, 114, 42, gGfxRdp.primLodFrac }; /* LOD fraction, K4, K5 */

        GX_SetTevKColor(GX_KCOLOR2, k2);
        sPrimLod = gGfxRdp.primLodFrac;
    }
    if (p->k3Used) {
        k3Key = (uint32_t)p->k3P | ((uint32_t)p->k3M << 4);
        if (!sColorsValid || changed || gGfxRdp.fogColor != sFog || gGfxRdp.blendColor != sBlend || k3Key != sK3Key) {
            upload_k3(p, gGfxRdp.fogColor, gGfxRdp.blendColor);
            sK3Key = k3Key;
        }
    }
    ref = (uint8_t)gGfxRdp.blendColor;
    if (changed || !sColorsValid || (p->acThreshold && ref != sAlphaRef)) {
        upload_alpha_compare(p, ref);
    }
    sFog = gGfxRdp.fogColor;
    sBlend = gGfxRdp.blendColor;
    sColorsValid = true;

    *out = p->info;
}
