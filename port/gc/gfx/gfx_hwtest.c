/**
 * GX hardware behaviour test (-DGFX_HW_TEST=1), run once after gfx_gx_init and before the game draws anything.
 *
 * On the console, actor shadows came out with a wrong red channel only (green and blue right), with the same
 * inputs as in Dolphin. This draws small quads with known TEV register values and register write orders, reads
 * the EFB back (GX_PeekARGB after GX_DrawDone) and logs got/want for each case, so the console shows how it
 * behaves where Dolphin's model may not match it:
 *  - each color register (C0-C2) and konst register (K0-K3) read back on its own;
 *  - writing a konst register after the color register at the same BP address (C0/K1, C1/K2, C2/K3, PREV/K0) and
 *    the other way round;
 *  - changing a register right after a large draw, before GX has finished it (is the register pipelined?);
 *  - an IA8 texture's intensity and alpha (TEXC, TEXA);
 *  - the shadows' fog step, lerp(C0, K0, RASA), with a vertex alpha.
 * The EFB is cleared afterwards; the renderer sets all of its state at the start of each task.
 */
#include <gccore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gc_ogc.h"
#include "gfx_internal.h"

#if defined(GFX_HW_TEST) && GFX_HW_TEST

#define CELL 24
#define COLS 24

static int sCell;
static int sPassed, sChecks;
static u8 sTex[32 * 4 * 2] __attribute__((aligned(32))); /* 32x4 IA8: one 4x4 tile is 32 bytes */

static GXColor rgba(u8 r, u8 g, u8 b, u8 a) {
    GXColor c = { r, g, b, a };
    return c;
}

/* Quad over cell n (EFB pixels), vertex color c */
static void quad(int n, int w, int h, GXColor c) {
    float x = (float)((n % COLS) * CELL + 2), y = (float)((n / COLS) * CELL + 40);

    GX_Begin(GX_QUADS, GX_VTXFMT7, 4);
    GX_Position3f32(x, y, -0.5f);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position3f32(x + (float)w, y, -0.5f);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(1.0f, 0.0f);
    GX_Position3f32(x + (float)w, y + (float)h, -0.5f);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(1.0f, 1.0f);
    GX_Position3f32(x, y + (float)h, -0.5f);
    GX_Color4u8(c.r, c.g, c.b, c.a);
    GX_TexCoord2f32(0.0f, 1.0f);
    GX_End();
}

static void check(const char* name, int n, GXColor want, int tol) {
    GXColor got;
    u16 x = (u16)((n % COLS) * CELL + 2 + 4), y = (u16)((n / COLS) * CELL + 40 + 4);
    bool ok;

    GX_DrawDone();
    GX_PeekARGB(x, y, &got);
    ok = abs(got.r - want.r) <= tol && abs(got.g - want.g) <= tol && abs(got.b - want.b) <= tol;
    sChecks++;
    sPassed += ok;
    gc_log("hwtest: %-34s got %02X%02X%02X want %02X%02X%02X %s", name, got.r, got.g, got.b, want.r, want.g, want.b,
           ok ? "ok" : "FAIL");
}

/* One stage: color = d input (a = b = c = 0), alpha = 0 */
static void prog_out(u8 cin, u8 kcsel, u8 texmap, u8 chan) {
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, (texmap == GX_TEXMAP_NULL) ? GX_TEXCOORDNULL : GX_TEXCOORD0, texmap, chan);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, cin);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevKColorSel(GX_TEVSTAGE0, kcsel);
    GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GX_SetTevDirect(GX_TEVSTAGE0);
}

static int next_cell(void) {
    return sCell++;
}

static void test_registers(void) {
    static const u8 kRegIn[3] = { GX_CC_C0, GX_CC_C1, GX_CC_C2 };
    static const u8 kKsel[4] = { GX_TEV_KCSEL_K0, GX_TEV_KCSEL_K1, GX_TEV_KCSEL_K2, GX_TEV_KCSEL_K3 };
    GXColor c[3] = { rgba(10, 20, 30, 40), rgba(50, 60, 70, 80), rgba(90, 100, 110, 120) };
    GXColor k[4] = { rgba(130, 140, 150, 160), rgba(170, 180, 190, 200), rgba(210, 220, 230, 240),
                     rgba(250, 5, 15, 25) };
    char name[48];
    int i, n;

    // 1. Each register alone: all colors first, then all konsts, then read each
    for (i = 0; i < 3; i++) {
        GX_SetTevColor(GX_TEVREG0 + i, c[i]);
    }
    for (i = 0; i < 4; i++) {
        GX_SetTevKColor(GX_KCOLOR0 + i, k[i]);
    }
    for (i = 0; i < 3; i++) {
        prog_out(kRegIn[i], GX_TEV_KCSEL_K0, GX_TEXMAP_NULL, GX_COLORNULL);
        n = next_cell();
        quad(n, 16, 16, rgba(0, 0, 0, 0));
        snprintf(name, sizeof(name), "C%d after K0-K3 writes", i);
        check(name, n, c[i], 1);
    }
    for (i = 0; i < 4; i++) {
        prog_out(GX_CC_KONST, kKsel[i], GX_TEXMAP_NULL, GX_COLORNULL);
        n = next_cell();
        quad(n, 16, 16, rgba(0, 0, 0, 0));
        snprintf(name, sizeof(name), "K%d after its writes", i);
        check(name, n, k[i], 1);
    }

    // 2. Konst written after the color register at the same address, and the other way round
    for (i = 0; i < 3; i++) {
        GXColor cv = rgba(0, 0, 0, 255), kv = rgba(200, 30, 40, 50);

        GX_SetTevColor(GX_TEVREG0 + i, cv);
        GX_SetTevKColor(GX_KCOLOR1 + i, kv);
        prog_out(kRegIn[i], GX_TEV_KCSEL_K0, GX_TEXMAP_NULL, GX_COLORNULL);
        n = next_cell();
        quad(n, 16, 16, rgba(0, 0, 0, 0));
        snprintf(name, sizeof(name), "C%d=0 then K%d=red: C%d", i, i + 1, i);
        check(name, n, cv, 1);

        GX_SetTevKColor(GX_KCOLOR1 + i, kv);
        GX_SetTevColor(GX_TEVREG0 + i, rgba(5, 6, 7, 8));
        prog_out(GX_CC_KONST, kKsel[i + 1], GX_TEXMAP_NULL, GX_COLORNULL);
        n = next_cell();
        quad(n, 16, 16, rgba(0, 0, 0, 0));
        snprintf(name, sizeof(name), "K%d=red then C%d: K%d", i + 1, i, i + 1);
        check(name, n, kv, 1);
    }
    // K0 and the PREV register share BP 0xE0/0xE1
    GX_SetTevKColor(GX_KCOLOR0, rgba(100, 120, 120, 255));
    GX_SetTevColor(GX_TEVPREV, rgba(250, 0, 0, 255));
    prog_out(GX_CC_KONST, GX_TEV_KCSEL_K0, GX_TEXMAP_NULL, GX_COLORNULL);
    n = next_cell();
    quad(n, 16, 16, rgba(0, 0, 0, 0));
    check("K0 then PREV written: K0", n, rgba(100, 120, 120, 255), 1);
}

/* A register changed right after a large draw that GX may not have finished */
static void test_pipeline(void) {
    int big, small;

    GX_SetTevColor(GX_TEVREG0, rgba(40, 80, 120, 255));
    prog_out(GX_CC_C0, GX_TEV_KCSEL_K0, GX_TEXMAP_NULL, GX_COLORNULL);
    big = next_cell();
    sCell += 3;
    quad(big, 4 * CELL - 4, 200, rgba(0, 0, 0, 0));
    GX_SetTevColor(GX_TEVREG0, rgba(220, 30, 30, 255));
    small = sCell;
    sCell += COLS * 9;
    quad(small, 16, 16, rgba(0, 0, 0, 0));
    check("C0 changed after a big draw: big", big, rgba(40, 80, 120, 255), 1);
    check("C0 changed after a big draw: next", small, rgba(220, 30, 30, 255), 1);

    GX_SetTevKColor(GX_KCOLOR1, rgba(30, 160, 60, 255));
    prog_out(GX_CC_KONST, GX_TEV_KCSEL_K1, GX_TEXMAP_NULL, GX_COLORNULL);
    big = small + 1;
    quad(big, 4 * CELL - 4, 200, rgba(0, 0, 0, 0));
    GX_SetTevKColor(GX_KCOLOR1, rgba(200, 200, 20, 255));
    quad(big + 4, 16, 16, rgba(0, 0, 0, 0));
    check("K1 changed after a big draw: big", big, rgba(30, 160, 60, 255), 1);
    check("K1 changed after a big draw: next", big + 4, rgba(200, 200, 20, 255), 1);

    // C0 in use by a big draw while K1 (same BP address) is written
    GX_SetTevColor(GX_TEVREG0, rgba(0, 0, 0, 255));
    prog_out(GX_CC_C0, GX_TEV_KCSEL_K0, GX_TEXMAP_NULL, GX_COLORNULL);
    big = big + 5;
    quad(big, 2 * CELL - 4, 200, rgba(0, 0, 0, 0));
    GX_SetTevKColor(GX_KCOLOR1, rgba(230, 10, 10, 255));
    check("C0=0 drawn, then K1=red: C0 draw", big, rgba(0, 0, 0, 255), 1);
}

/* IA8 texture: TEXC must be the intensity, TEXA the alpha */
static void test_ia8(void) {
    GXTexObj tex;
    int i, n;

    for (i = 0; i < (int)sizeof(sTex); i += 2) {
        sTex[i] = 0xC0;     // alpha first
        sTex[i + 1] = 0x40; // then intensity
    }
    DCFlushRange(sTex, sizeof(sTex));
    GX_InvalidateTexAll();
    GX_InitTexObj(&tex, sTex, 32, 4, GX_TF_IA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&tex, GX_NEAR, GX_NEAR);
    GX_LoadTexObj(&tex, GX_TEXMAP0);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);

    prog_out(GX_CC_TEXC, GX_TEV_KCSEL_K0, GX_TEXMAP0, GX_COLORNULL);
    n = next_cell();
    quad(n, 16, 16, rgba(0, 0, 0, 0));
    check("IA8 TEXC (I=40, A=C0)", n, rgba(0x40, 0x40, 0x40, 255), 1);

    prog_out(GX_CC_TEXA, GX_TEV_KCSEL_K0, GX_TEXMAP0, GX_COLORNULL);
    n = next_cell();
    quad(n, 16, 16, rgba(0, 0, 0, 0));
    check("IA8 TEXA (I=40, A=C0)", n, rgba(0xC0, 0xC0, 0xC0, 255), 1);

    // TEXC * C0 with C0 black: the foot shadow's first stage
    GX_SetTevColor(GX_TEVREG0, rgba(0, 0, 0, 255));
    prog_out(GX_CC_ZERO, GX_TEV_KCSEL_K0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_TEXC, GX_CC_C0, GX_CC_ZERO);
    n = next_cell();
    quad(n, 16, 16, rgba(0, 0, 0, 0));
    check("IA8 TEXC * C0 (C0 black)", n, rgba(0, 0, 0, 255), 1);
    GX_SetNumTexGens(0);
}

/* The shadows' fog step: lerp(C0, K0, RASA) with vertex alpha 0xC7 */
static void test_fog_lerp(void) {
    int n;

    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetTevColor(GX_TEVREG0, rgba(0, 0, 0, 255));
    GX_SetTevKColor(GX_KCOLOR0, rgba(100, 120, 120, 0));
    prog_out(GX_CC_ZERO, GX_TEV_KCSEL_K0, GX_TEXMAP_NULL, GX_COLOR0A0);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_C0, GX_CC_KONST, GX_CC_RASA, GX_CC_ZERO);
    n = next_cell();
    quad(n, 16, 16, rgba(0, 0, 0, 0xC7));
    // (1 - c) * a + c * b with c = 199 + (199 >> 7) = 200: b * 200 / 256
    check("lerp(C0=0, K0=fog, RASA=C7)", n, rgba(78, 94, 94, 255), 2);
}

void gfx_hw_test(void) {
    Mtx44 proj;
    Mtx identity;

    gc_log("hwtest: GX register behaviour test");
    sCell = 0;
    sPassed = sChecks = 0;
    GX_SetViewport(0, 0, GFX_EFB_WIDTH, GFX_EFB_HEIGHT, 0, 1);
    GX_SetScissor(0, 0, GFX_EFB_WIDTH, GFX_EFB_HEIGHT);
    guOrtho(proj, 0, GFX_EFB_HEIGHT, 0, GFX_EFB_WIDTH, 0.0f, 1.0f);
    GX_LoadProjectionMtx(proj, GX_ORTHOGRAPHIC);
    guMtxIdentity(identity);
    GX_LoadPosMtxImm(identity, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT7, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT7, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT7, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetNumTexGens(0);
    GX_SetNumIndStages(0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetFog(GX_FOG_NONE, 0, 1, 0.1f, 1, rgba(0, 0, 0, 0));
    GX_SetCullMode(GX_CULL_NONE);

    test_registers();
    test_ia8();
    test_fog_lerp();
    test_pipeline();
    gc_log("hwtest: %d of %d checks passed%s", sPassed, sChecks, (sPassed == sChecks) ? "" : "  <-- DIFFERENCES");

    // Clear what was drawn (color and depth) with a full-screen black quad at the far plane
    GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
    GX_SetTevColor(GX_TEVREG0, rgba(0, 0, 0, 255));
    prog_out(GX_CC_C0, GX_TEV_KCSEL_K0, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_Begin(GX_QUADS, GX_VTXFMT7, 4);
    GX_Position3f32(0.0f, 0.0f, -1.0f);
    GX_Color4u8(0, 0, 0, 0);
    GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position3f32((float)GFX_EFB_WIDTH, 0.0f, -1.0f);
    GX_Color4u8(0, 0, 0, 0);
    GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position3f32((float)GFX_EFB_WIDTH, (float)GFX_EFB_HEIGHT, -1.0f);
    GX_Color4u8(0, 0, 0, 0);
    GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position3f32(0.0f, (float)GFX_EFB_HEIGHT, -1.0f);
    GX_Color4u8(0, 0, 0, 0);
    GX_TexCoord2f32(0.0f, 0.0f);
    GX_End();
    GX_DrawDone();
}

#endif
