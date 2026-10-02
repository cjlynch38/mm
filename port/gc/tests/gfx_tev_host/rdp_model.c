/**
 * Model of the N64 RDP color combiner, alpha compare and blender for one fully covered pixel, written from
 * angrylion-rdp-plus (combiner_1cycle/2cycle, blender_1cycle/2cycle, alpha_compare; MIT license). See
 * rdp_model.h.
 */
#include <stdlib.h>
#include <string.h>
#include "rdp_model.h"

typedef struct {
    C4 comb; /* 9-bit combined values */
    C4 t0, t1, prim, shade, env;
    int primLod;
} RdpCtx;

static int ext9(int v) {
    v &= 0x1FF;
    return ((v & 0x180) == 0x180) ? v - 0x200 : v;
}

static int sign9(int v) {
    v &= 0x1FF;
    return (v & 0x100) ? v - 0x200 : v;
}

static int clamp9(int v) {
    v &= 0x1FF;
    switch ((v >> 7) & 3) {
        case 0:
        case 1:
            return v & 0xFF;
        case 2:
            return 0xFF;
        default:
            return 0;
    }
}

static int ch_of(const C4* c, int ch) {
    return ch == 0 ? c->r : ch == 1 ? c->g : ch == 2 ? c->b : c->a;
}

#define NOISE_VALUE 128
#define K4_VALUE 114
#define K5_VALUE 42

static int rgb_common(const RdpCtx* x, int code, int ch) {
    switch (code) {
        case 0:
            return ch_of(&x->comb, ch);
        case 1:
            return ch_of(&x->t0, ch);
        case 2:
            return ch_of(&x->t1, ch);
        case 3:
            return ch_of(&x->prim, ch);
        case 4:
            return ch_of(&x->shade, ch);
        default:
            return ch_of(&x->env, ch);
    }
}

static int rgb_suba(const RdpCtx* x, int code, int ch) {
    code &= 0xF;
    return code < 6 ? rgb_common(x, code, ch) : code == 6 ? 0x100 : code == 7 ? NOISE_VALUE : 0;
}

static int rgb_subb(const RdpCtx* x, int code, int ch) {
    code &= 0xF;
    return code < 6 ? rgb_common(x, code, ch) : code == 7 ? K4_VALUE : 0; /* 6: key center (0) */
}

static int rgb_mul(const RdpCtx* x, int code, int ch) {
    code &= 0x1F;
    if (code < 6) {
        return rgb_common(x, code, ch);
    }
    switch (code) {
        case 7:
            return x->comb.a;
        case 8:
            return x->t0.a;
        case 9:
            return x->t1.a;
        case 10:
            return x->prim.a;
        case 11:
            return x->shade.a;
        case 12:
            return x->env.a;
        case 14:
            return x->primLod;
        case 15:
            return K5_VALUE;
        default: /* 6 key scale, 13 LOD fraction: 0 */
            return 0;
    }
}

static int rgb_add(const RdpCtx* x, int code, int ch) {
    code &= 7;
    return code < 6 ? rgb_common(x, code, ch) : code == 6 ? 0x100 : 0;
}

static int alpha_sub(const RdpCtx* x, int code) {
    code &= 7;
    return code < 6 ? rgb_common(x, code, 3) : code == 6 ? 0x100 : 0;
}

static int alpha_mul(const RdpCtx* x, int code) {
    code &= 7;
    return (code >= 1 && code < 6) ? rgb_common(x, code, 3) : code == 6 ? x->primLod : 0;
}

/* One combiner cycle as angrylion computes it. *wrap is set when a channel leaves the 9-bit range the RDP
 * keeps (-128..383): the RDP then wraps (bright sums turn black, very negative ones white), GX clamps. */
static int rdp_equation(int a, int b, int c, int d, bool* wrap) {
    int full = ((ext9(a) - ext9(b)) * sign9(c) + ext9(d) * 256 + 0x80) >> 8;

    if (full < -128 || full > 383) {
        *wrap = true;
    }
    return full & 0x1FF;
}

static void rdp_combine(const RdpCtx* x, uint32_t hi, uint32_t lo, int cyc, C4* out, bool* wrap) {
    int sa, sb, m, ad, asa, asb, am, aad;

    if (cyc == 0) {
        sa = (hi >> 20) & 0xF, m = (hi >> 15) & 0x1F, asa = (hi >> 12) & 7, am = (hi >> 9) & 7;
        sb = (lo >> 28) & 0xF, ad = (lo >> 15) & 7, asb = (lo >> 12) & 7, aad = (lo >> 9) & 7;
    } else {
        sa = (hi >> 5) & 0xF, m = hi & 0x1F, sb = (lo >> 24) & 0xF, asa = (lo >> 21) & 7;
        am = (lo >> 18) & 7, ad = (lo >> 6) & 7, asb = (lo >> 3) & 7, aad = lo & 7;
    }
    out->r = rdp_equation(rgb_suba(x, sa, 0), rgb_subb(x, sb, 0), rgb_mul(x, m, 0), rgb_add(x, ad, 0), wrap);
    out->g = rdp_equation(rgb_suba(x, sa, 1), rgb_subb(x, sb, 1), rgb_mul(x, m, 1), rgb_add(x, ad, 1), wrap);
    out->b = rdp_equation(rgb_suba(x, sa, 2), rgb_subb(x, sb, 2), rgb_mul(x, m, 2), rgb_add(x, ad, 2), wrap);
    out->a = rdp_equation(alpha_sub(x, asa), alpha_sub(x, asb), alpha_mul(x, am), alpha_sub(x, aad), wrap);
}

static int bl_color(int sel, int ch, const C4* in, const N64In* n) {
    switch (sel & 3) {
        case 0:
            return ch_of(in, ch);
        case 1:
            return ch_of(&n->mem, ch);
        case 2:
            return ch_of(&n->blend, ch);
        default:
            return ch_of(&n->fog, ch);
    }
}

/* P * A + M * B with 5-bit factors (blender_equation_cycle0_2 / FORCE_BL path), memory alpha 0xE0 */
static C4 rdp_blend(uint32_t L, int cyc, const C4* in, int pixelA, int shadeA, const N64In* n, bool* wrap) {
    int shift = (cyc == 0) ? 2 : 0;
    int p = (L >> (28 + shift)) & 3, a = (L >> (24 + shift)) & 3, m = (L >> (20 + shift)) & 3,
        b = (L >> (16 + shift)) & 3;
    int av = a == 0 ? pixelA : a == 1 ? n->fog.a : a == 2 ? shadeA : 0;
    int bv = b == 0 ? (~av & 0xFF) : b == 1 ? 0xE0 : b == 2 ? 0xFF : 0;
    int b1 = av >> 3, b2 = bv >> 3, ch;
    C4 out;
    int r[3];

    if (b == 1) {
        b1 &= 0x3C;
        b2 |= 3;
    }
    for (ch = 0; ch < 3; ch++) {
        r[ch] = (bl_color(p, ch, in, n) * b1 + bl_color(m, ch, in, n) * (b2 + 1)) >> 5;
        if (r[ch] > 0xFF) {
            *wrap = true;
            r[ch] &= 0xFF;
        }
    }
    out.r = r[0];
    out.g = r[1];
    out.b = r[2];
    out.a = 0;
    return out;
}

void rdp_pixel(uint32_t hi, uint32_t lo, uint32_t omh, uint32_t L, bool shadeAvail, const N64In* n,
                      N64Out* o) {
    bool twoCycle = ((omh >> G_MDSFT_CYCLETYPE) & 3) == 1;
    bool cvgTimesAlpha = (L & CVG_X_ALPHA) != 0, alphaCvgSel = (L & ALPHA_CVG_SEL) != 0;
    RdpCtx x;
    C4 c0, c1, pix, outc;
    int pa, cvg = 8, temp = 0, acalpha, shadeA;

    memset(o, 0, sizeof(*o));
    memset(&x, 0, sizeof(x));
    x.prim = n->prim;
    x.env = n->env;
    x.primLod = n->primLod;
    if (shadeAvail) {
        x.shade = n->shade;
    }
    shadeA = x.shade.a;

    if (twoCycle) {
        x.t0 = n->tex0;
        x.t1 = n->tex1;
        rdp_combine(&x, hi, lo, 0, &c0, &o->overflow);
        o->overflow |= (c0.r > 0x100 || c0.g > 0x100 || c0.b > 0x100 || c0.a > 0x100);
        acalpha = clamp9(c0.a);
        if (acalpha == 0xFF) {
            acalpha = 0x100;
        }
        if (!alphaCvgSel) {
            if (acalpha & 0x100) {
                acalpha = 0xFF;
            }
        } else {
            acalpha = cvgTimesAlpha ? (acalpha * cvg + 4) >> 3 : cvg << 5;
            if (acalpha > 0xFF) {
                acalpha = 0xFF;
            }
        }
        x.comb = c0;
        x.t0 = n->tex1; /* cycle 1: TEXEL0 = tile + 1, TEXEL1 = the next pixel's tile texel */
        x.t1 = n->tex0;
        rdp_combine(&x, hi, lo, 1, &c1, &o->overflow);
        if (c0.r == 0x100 || c0.g == 0x100 || c0.b == 0x100 || c0.a == 0x100) {
            /* Exactly 1.0 (the constant 1): gfx_tev.c passes 255. Fine as A/B/D, but as a multiplier the RDP
             * reads 0x100 as -256 (9-bit signed); skip samples where that matters. */
            RdpCtx y = x;
            C4 alt;
            bool altWrap = false;

            y.comb.r = c0.r == 0x100 ? 0xFF : c0.r;
            y.comb.g = c0.g == 0x100 ? 0xFF : c0.g;
            y.comb.b = c0.b == 0x100 ? 0xFF : c0.b;
            y.comb.a = c0.a == 0x100 ? 0xFF : c0.a;
            rdp_combine(&y, hi, lo, 1, &alt, &altWrap);
            /* (an alternative that leaves the 9-bit range is clamped by GX, so its clamp9 value means nothing) */
            o->overflow = o->overflow || altWrap || abs(clamp9(alt.r) - clamp9(c1.r)) > 2 ||
                          abs(clamp9(alt.g) - clamp9(c1.g)) > 2 || abs(clamp9(alt.b) - clamp9(c1.b)) > 2 ||
                          abs(clamp9(alt.a) - clamp9(c1.a)) > 2;
        }
    } else {
        x.t0 = x.t1 = n->tex0; /* TEXEL1 = the next pixel's TEXEL0 */
        rdp_combine(&x, hi, lo, 1, &c1, &o->overflow);
        acalpha = -1;
    }

    pix.r = clamp9(c1.r);
    pix.g = clamp9(c1.g);
    pix.b = clamp9(c1.b);
    pa = clamp9(c1.a);
    o->rawAlpha = pa;
    if (pa == 0xFF) {
        pa = 0x100;
    }
    if (cvgTimesAlpha) {
        temp = (pa * cvg + 4) >> 3;
        cvg = (temp >> 5) & 0xF;
    }
    if (!alphaCvgSel) {
        if (pa & 0x100) {
            pa = 0xFF;
        }
    } else {
        pa = cvgTimesAlpha ? temp : 8 << 5;
        if (pa > 0xFF) {
            pa = 0xFF;
        }
    }
    pix.a = pa;
    o->pixel = pix;
    if (!twoCycle) {
        acalpha = pa;
    }

    o->written = true;
    o->acValue = -1;
    if ((L & 3) == 1) {
        o->acValue = acalpha;
        o->written = acalpha >= n->blend.a;
    }
    o->cutout = (L & AA_EN) && cvgTimesAlpha;
    if ((L & AA_EN) && cvg == 0) {
        o->written = false;
    }

    /* Blender: full coverage overflows the memory coverage, so blending needs FORCE_BL */
    {
        int last = twoCycle ? 1 : 0;
        int shift = (last == 0) ? 2 : 0;
        int a = (L >> (24 + shift)) & 3, b = (L >> (16 + shift)) & 3;
        C4 in = pix;

        if (twoCycle) {
            in = rdp_blend(L, 0, &pix, pa, shadeA, n, &o->overflow);
        }
        if (!(L & FORCE_BL) || (a == 0 && b == 0 && pa >= 0xFF)) {
            int p = (L >> (28 + shift)) & 3, ch;
            int r[3];

            for (ch = 0; ch < 3; ch++) {
                r[ch] = bl_color(p, ch, &in, n);
            }
            outc.r = r[0];
            outc.g = r[1];
            outc.b = r[2];
        } else {
            outc = rdp_blend(L, last, &in, pa, shadeA, n, &o->overflow);
        }
    }
    o->color = o->written ? outc : n->mem;
}

