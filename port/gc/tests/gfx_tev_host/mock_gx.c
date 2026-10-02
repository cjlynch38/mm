/**
 * GX mock of the gfx_tev host test: records the GX calls gfx_tev.c makes and evaluates the recorded TEV,
 * alpha compare and blend state for one pixel the way the GameCube does (TEV arithmetic as in Dolphin's
 * software renderer: 8-bit a/b/c inputs, 11-bit signed d, c + c/128 weights, rounding, optional clamp).
 * Also provides gc_log for gfx_tev.c.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "mock_gx.h"
#include "gc_bridge.h"

MockGx gMockGx;

static int sLogs;
static bool sQuiet;

void gc_log(const char* fmt, ...) {
    va_list ap;

    sLogs++;
    if (sQuiet) {
        return;
    }
    va_start(ap, fmt);
    printf("  [log] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

int mock_log_count(void) {
    return sLogs;
}

void mock_log_quiet(bool quiet) {
    sQuiet = quiet;
}

static MockStage* stage(u8 s) {
    if (s >= GX_MAX_TEVSTAGE) {
        printf("FAIL: TEV stage %u out of range\n", s);
        s = 0;
    }
    return &gMockGx.st[s];
}

void GX_SetNumTevStages(u8 num) {
    gMockGx.calls++;
    gMockGx.programCalls++;
    gMockGx.numStages = num;
}

void GX_SetTevOrder(u8 tevstage, u8 texcoord, u32 texmap, u8 color) {
    MockStage* st = stage(tevstage);

    gMockGx.calls++;
    st->texcoord = texcoord;
    st->texmap = texmap;
    st->chan = color;
}

void GX_SetTevColorIn(u8 tevstage, u8 a, u8 b, u8 c, u8 d) {
    MockStage* st = stage(tevstage);

    gMockGx.calls++;
    st->cin[0] = a;
    st->cin[1] = b;
    st->cin[2] = c;
    st->cin[3] = d;
}

void GX_SetTevAlphaIn(u8 tevstage, u8 a, u8 b, u8 c, u8 d) {
    MockStage* st = stage(tevstage);

    gMockGx.calls++;
    st->ain[0] = a;
    st->ain[1] = b;
    st->ain[2] = c;
    st->ain[3] = d;
}

void GX_SetTevColorOp(u8 tevstage, u8 tevop, u8 tevbias, u8 tevscale, u8 clamp, u8 tevregid) {
    MockStage* st = stage(tevstage);

    gMockGx.calls++;
    st->cop = tevop;
    st->cbias = tevbias;
    st->cscale = tevscale;
    st->cclamp = clamp;
    st->creg = tevregid;
}

void GX_SetTevAlphaOp(u8 tevstage, u8 tevop, u8 tevbias, u8 tevscale, u8 clamp, u8 tevregid) {
    MockStage* st = stage(tevstage);

    gMockGx.calls++;
    st->aop = tevop;
    st->abias = tevbias;
    st->ascale = tevscale;
    st->aclamp = clamp;
    st->areg = tevregid;
}

void GX_SetTevKColorSel(u8 tevstage, u8 sel) {
    gMockGx.calls++;
    stage(tevstage)->kcsel = sel;
}

void GX_SetTevKAlphaSel(u8 tevstage, u8 sel) {
    gMockGx.calls++;
    stage(tevstage)->kasel = sel;
}

void GX_SetTevColor(u8 tev_regid, GXColor color) {
    gMockGx.calls++;
    gMockGx.colorCalls++;
    gMockGx.reg[tev_regid & 3] = color;
}

void GX_SetTevKColor(u8 sel, GXColor col) {
    gMockGx.calls++;
    gMockGx.colorCalls++;
    gMockGx.konst[sel & 3] = col;
}

void GX_SetBlendMode(u8 type, u8 src_fact, u8 dst_fact, u8 op) {
    gMockGx.calls++;
    gMockGx.blendType = type;
    gMockGx.blendSrc = src_fact;
    gMockGx.blendDst = dst_fact;
    gMockGx.blendOp = op;
}

void GX_SetAlphaCompare(u8 comp0, u8 ref0, u8 aop, u8 comp1, u8 ref1) {
    gMockGx.calls++;
    gMockGx.acCalls++;
    gMockGx.acComp0 = comp0;
    gMockGx.acRef0 = ref0;
    gMockGx.acOp = aop;
    gMockGx.acComp1 = comp1;
    gMockGx.acRef1 = ref1;
}

void GX_SetZMode(u8 enable, u8 func, u8 update_enable) {
    gMockGx.calls++;
    gMockGx.zEnable = enable;
    gMockGx.zFunc = func;
    gMockGx.zUpdate = update_enable;
}

void GX_SetZCompLoc(u8 before_tex) {
    gMockGx.calls++;
    gMockGx.zCompLoc = before_tex;
}

void GX_SetColorUpdate(u8 enable) {
    gMockGx.calls++;
    gMockGx.colorUpdate = enable;
}

void GX_SetAlphaUpdate(u8 enable) {
    gMockGx.calls++;
    gMockGx.alphaUpdate = enable;
}

/* ------------------------------------------------------------------------------------------------ */
/* Pixel model                                                                                      */
/* ------------------------------------------------------------------------------------------------ */

typedef struct {
    int c[4]; /* r, g, b, a */
} Reg;

static const int sKonstFrac[8] = { 255, 223, 191, 159, 128, 96, 64, 32 };

static int konst_color(u8 sel, int ch) {
    if (sel < 8) {
        return sKonstFrac[sel];
    }
    if (sel >= 0x0C && sel <= 0x0F) {
        const GXColor* k = &gMockGx.konst[sel - 0x0C];
        return ch == 0 ? k->r : ch == 1 ? k->g : k->b;
    }
    if (sel >= 0x10) {
        const GXColor* k = &gMockGx.konst[sel & 3];
        int comp = (sel - 0x10) >> 2;
        return comp == 0 ? k->r : comp == 1 ? k->g : comp == 2 ? k->b : k->a;
    }
    printf("FAIL: bad konst color selection %u\n", sel);
    return 0;
}

static int konst_alpha(u8 sel) {
    if (sel < 8) {
        return sKonstFrac[sel];
    }
    if (sel >= 0x10) {
        const GXColor* k = &gMockGx.konst[sel & 3];
        int comp = (sel - 0x10) >> 2;
        return comp == 0 ? k->r : comp == 1 ? k->g : comp == 2 ? k->b : k->a;
    }
    printf("FAIL: bad konst alpha selection %u\n", sel);
    return 0;
}

static int c4_get(const C4* c, int ch) {
    return ch == 0 ? c->r : ch == 1 ? c->g : ch == 2 ? c->b : c->a;
}

static void c4_set(C4* c, int ch, int v) {
    if (ch == 0) {
        c->r = v;
    } else if (ch == 1) {
        c->g = v;
    } else if (ch == 2) {
        c->b = v;
    } else {
        c->a = v;
    }
}

static int color_input(u8 in, int ch, const Reg* regs, const C4* tex, const C4* ras, u8 kcsel) {
    switch (in) {
        case GX_CC_CPREV:
        case GX_CC_C0:
        case GX_CC_C1:
        case GX_CC_C2:
            return regs[in >> 1].c[ch];
        case GX_CC_APREV:
        case GX_CC_A0:
        case GX_CC_A1:
        case GX_CC_A2:
            return regs[in >> 1].c[3];
        case GX_CC_TEXC:
            return c4_get(tex, ch);
        case GX_CC_TEXA:
            return tex->a;
        case GX_CC_RASC:
            return c4_get(ras, ch);
        case GX_CC_RASA:
            return ras->a;
        case GX_CC_ONE:
            return 255;
        case GX_CC_HALF:
            return 128;
        case GX_CC_KONST:
            return konst_color(kcsel, ch);
        default:
            return 0;
    }
}

static int alpha_input(u8 in, const Reg* regs, const C4* tex, const C4* ras, u8 kasel) {
    switch (in) {
        case GX_CA_APREV:
        case GX_CA_A0:
        case GX_CA_A1:
        case GX_CA_A2:
            return regs[in].c[3];
        case GX_CA_TEXA:
            return tex->a;
        case GX_CA_RASA:
            return ras->a;
        case GX_CA_KONST:
            return konst_alpha(kasel);
        default:
            return 0;
    }
}

static int sext11(int v) {
    v &= 0x7FF;
    return (v & 0x400) ? v - 0x800 : v;
}

static int tev_combine(int a, int b, int c, int d, u8 op, u8 clamp) {
    int cw, lerp, result;

    a &= 0xFF;
    b &= 0xFF;
    c &= 0xFF;
    d = sext11(d);
    cw = c + (c >> 7);
    lerp = a * (256 - cw) + b * cw;
    lerp += (op == GX_TEV_SUB) ? 127 : 128;
    lerp >>= 8;
    if (op == GX_TEV_SUB) {
        lerp = -lerp;
    }
    result = d + lerp;
    if (clamp) {
        result = result < 0 ? 0 : result > 255 ? 255 : result;
    } else {
        result = result < -1024 ? -1024 : result > 1023 ? 1023 : result;
    }
    return result;
}

static bool alpha_test(u8 comp, int a, int ref) {
    switch (comp) {
        case GX_NEVER:
            return false;
        case GX_LESS:
            return a < ref;
        case GX_EQUAL:
            return a == ref;
        case GX_LEQUAL:
            return a <= ref;
        case GX_GREATER:
            return a > ref;
        case GX_NEQUAL:
            return a != ref;
        case GX_GEQUAL:
            return a >= ref;
        default:
            return true;
    }
}

static int blend_factor(u8 f, const C4* src, const C4* dst, int ch, bool isSrc) {
    const C4* other = isSrc ? dst : src;

    switch (f) {
        case GX_BL_ZERO:
            return 0;
        case GX_BL_ONE:
            return 255;
        case GX_BL_SRCCLR: /* destination color as source factor, source color as destination factor */
            return c4_get(other, ch);
        case GX_BL_INVSRCCLR:
            return 255 - c4_get(other, ch);
        case GX_BL_SRCALPHA:
            return src->a;
        case GX_BL_INVSRCALPHA:
            return 255 - src->a;
        case GX_BL_DSTALPHA: /* RGB8 EFB: no alpha */
            return 255;
        default:
            return 0;
    }
}

bool mock_gx_eval(C4 tex0, C4 tex1, C4 shade, C4 dst, C4* out, C4* tevOut) {
    Reg regs[4];
    C4 src;
    int s, i;
    bool pass;
    static const C4 zero = { 0, 0, 0, 0 };

    for (i = 0; i < 4; i++) {
        regs[i].c[0] = gMockGx.reg[i].r;
        regs[i].c[1] = gMockGx.reg[i].g;
        regs[i].c[2] = gMockGx.reg[i].b;
        regs[i].c[3] = gMockGx.reg[i].a;
    }
    for (s = 0; s < gMockGx.numStages; s++) {
        const MockStage* st = &gMockGx.st[s];
        const C4* tex = (st->texmap == GX_TEXMAP0) ? &tex0 : (st->texmap == GX_TEXMAP1) ? &tex1 : &zero;
        const C4* ras = (st->chan == GX_COLOR0A0) ? &shade : &zero;
        int cres[3], ares;

        if ((st->texmap == GX_TEXMAP0 && st->texcoord != GX_TEXCOORD0) ||
            (st->texmap == GX_TEXMAP1 && st->texcoord != GX_TEXCOORD1)) {
            printf("FAIL: stage %d texcoord %u does not match texmap %u\n", s, st->texcoord, st->texmap);
        }
        if (st->cbias != GX_TB_ZERO || st->cscale != GX_CS_SCALE_1 || st->abias != GX_TB_ZERO ||
            st->ascale != GX_CS_SCALE_1) {
            printf("FAIL: stage %d uses bias/scale\n", s);
        }
        for (i = 0; i < 3; i++) {
            cres[i] = tev_combine(color_input(st->cin[0], i, regs, tex, ras, st->kcsel),
                                  color_input(st->cin[1], i, regs, tex, ras, st->kcsel),
                                  color_input(st->cin[2], i, regs, tex, ras, st->kcsel),
                                  color_input(st->cin[3], i, regs, tex, ras, st->kcsel), st->cop, st->cclamp);
        }
        ares = tev_combine(alpha_input(st->ain[0], regs, tex, ras, st->kasel),
                           alpha_input(st->ain[1], regs, tex, ras, st->kasel),
                           alpha_input(st->ain[2], regs, tex, ras, st->kasel),
                           alpha_input(st->ain[3], regs, tex, ras, st->kasel), st->aop, st->aclamp);
        for (i = 0; i < 3; i++) {
            regs[st->creg & 3].c[i] = cres[i];
        }
        regs[st->areg & 3].c[3] = ares;
    }
    src.r = regs[0].c[0] & 0xFF;
    src.g = regs[0].c[1] & 0xFF;
    src.b = regs[0].c[2] & 0xFF;
    src.a = regs[0].c[3] & 0xFF;
    if (tevOut != NULL) {
        *tevOut = src;
    }

    {
        bool p0 = alpha_test(gMockGx.acComp0, src.a, gMockGx.acRef0);
        bool p1 = alpha_test(gMockGx.acComp1, src.a, gMockGx.acRef1);

        switch (gMockGx.acOp) {
            case GX_AOP_AND:
                pass = p0 && p1;
                break;
            case GX_AOP_OR:
                pass = p0 || p1;
                break;
            case GX_AOP_XOR:
                pass = p0 != p1;
                break;
            default:
                pass = p0 == p1;
                break;
        }
    }
    *out = dst;
    if (!pass || !gMockGx.colorUpdate) {
        return false;
    }
    if (gMockGx.blendType == GX_BM_NONE) {
        out->r = src.r;
        out->g = src.g;
        out->b = src.b;
    } else if (gMockGx.blendType == GX_BM_BLEND) {
        for (i = 0; i < 3; i++) {
            int v = (c4_get(&src, i) * blend_factor(gMockGx.blendSrc, &src, &dst, i, true) +
                     c4_get(&dst, i) * blend_factor(gMockGx.blendDst, &src, &dst, i, false) + 127) / 255;
            c4_set(out, i, v > 255 ? 255 : v);
        }
    } else if (gMockGx.blendType == GX_BM_SUBTRACT) {
        out->r = dst.r - src.r < 0 ? 0 : dst.r - src.r;
        out->g = dst.g - src.g < 0 ? 0 : dst.g - src.g;
        out->b = dst.b - src.b < 0 ? 0 : dst.b - src.b;
    } else {
        printf("FAIL: logic blending\n");
    }
    return true;
}
