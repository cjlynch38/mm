/**
 * Host test of port/gc/gfx/gfx_tev.c (N64 color combiner and blender -> GX TEV and pixel engine state).
 *
 * gfx_tev.c runs against a mock GX (mock_gx.c) that records its calls. For a table of combine modes the
 * game uses, random inputs are pushed through the recorded TEV program (GameCube arithmetic) and through a
 * model of the RDP written from angrylion-rdp-plus (combiner_1cycle/2cycle, blender, alpha compare, coverage
 * with a fully covered pixel), and the results are compared. A second table checks the blend, alpha compare
 * and z settings chosen for the render modes MM uses. Approximations documented in gfx_tev.c (NOISE = 0.5,
 * LOD_FRACTION = 0, K4/K5 = 114/42) are modelled the same way; samples whose cycle-0 combined value leaves
 * [0, 255] (clamped by gfx_tev.c, 9-bit on the RDP) are skipped. Finally, random combine words and random
 * blender settings are compiled one by one; every program gfx_tev.c does not log as approximated must match
 * the model too.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_internal.h"
#include "mock_gx.h"
#include "rdp_model.h"
#include "tev_cases.h"

GfxRdpState gGfxRdp;
GfxRspState gGfxRsp;

static int sFailures;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        if (!(cond)) {                                       \
            sFailures++;                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)

/* ------------------------------------------------------------------------------------------------ */
/* Helpers                                                                                          */
/* ------------------------------------------------------------------------------------------------ */

static uint32_t sRng = 0x2545F491u;

static uint32_t rng(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static int rnd8(void) {
    uint32_t r = rng() % 100;
    return r < 8 ? 0 : r < 16 ? 255 : (int)(rng() & 0xFF);
}

static C4 rnd_c4(void) {
    C4 c = { rnd8(), rnd8(), rnd8(), rnd8() };
    return c;
}

static uint32_t pack(C4 c) {
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | (uint32_t)c.a;
}

static int c4_diff(C4 a, C4 b, bool alpha) {
    int d = abs(a.r - b.r);

    d = abs(a.g - b.g) > d ? abs(a.g - b.g) : d;
    d = abs(a.b - b.b) > d ? abs(a.b - b.b) : d;
    if (alpha) {
        d = abs(a.a - b.a) > d ? abs(a.a - b.a) : d;
    }
    return d;
}

static void dump_program(void) {
    int s;

    printf("    %d stages, blend %u %u %u, ac %u/%u %u %u/%u, z %u %u %u, color update %u\n", gMockGx.numStages,
           gMockGx.blendType, gMockGx.blendSrc, gMockGx.blendDst, gMockGx.acComp0, gMockGx.acRef0, gMockGx.acOp,
           gMockGx.acComp1, gMockGx.acRef1, gMockGx.zEnable, gMockGx.zFunc, gMockGx.zUpdate, gMockGx.colorUpdate);
    for (s = 0; s < gMockGx.numStages; s++) {
        const MockStage* st = &gMockGx.st[s];

        printf("    stage %d: map %02X chan %02X | C %2u %2u %2u %2u op %u cl %u -> %u k %02X | A %u %u %u %u op %u cl %u "
               "-> %u k %02X\n",
               s, (unsigned)st->texmap, st->chan, st->cin[0], st->cin[1], st->cin[2], st->cin[3], st->cop,
               st->cclamp, st->creg, st->kcsel, st->ain[0], st->ain[1], st->ain[2], st->ain[3], st->aop,
               st->aclamp, st->areg, st->kasel);
    }
}

static void set_state(uint32_t hi, uint32_t lo, uint32_t cycle, uint32_t modeL, uint32_t geom) {
    gGfxRdp.combineHi = hi;
    gGfxRdp.combineLo = lo;
    gGfxRdp.otherModeH = cycle | G_TP_PERSP | G_TF_BILERP;
    gGfxRdp.otherModeL = modeL;
    gGfxRsp.geometryMode = geom;
    gGfxRdp.dirty = GFX_DIRTY_COMBINE | GFX_DIRTY_OTHERMODE;
}

/* The info flags must describe the uploaded program */
static void check_info(const char* name, const GfxTevInfo* info) {
    bool t0 = false, t1 = false, shade = false;
    int s;

    for (s = 0; s < gMockGx.numStages; s++) {
        t0 |= gMockGx.st[s].texmap == GX_TEXMAP0;
        t1 |= gMockGx.st[s].texmap == GX_TEXMAP1;
        shade |= gMockGx.st[s].chan == GX_COLOR0A0;
    }
    CHECK(info->usesTexel0 == t0 && info->usesTexel1 == t1 && info->usesShade == shade,
          "%s: info textures %d %d shade %d, program %d %d %d", name, info->usesTexel0, info->usesTexel1,
          info->usesShade, t0, t1, shade);
    CHECK(gMockGx.numStages >= 1 && gMockGx.numStages <= 12, "%s: %d stages", name, gMockGx.numStages);
}

/* ------------------------------------------------------------------------------------------------ */
/* Numeric combiner / blender tests                                                                 */
/* ------------------------------------------------------------------------------------------------ */

/* One random sample through both models; returns the largest difference, -1 if skipped */
static int run_sample(const char* name, uint32_t cycle, uint32_t modeL, GfxPrimKind kind, uint32_t geom,
                      bool checkAlpha, int tol, int tolBlend, bool* reported) {
    N64In in;
    N64Out n;
    GfxTevInfo info;
    C4 gx, tevOut;
    bool written;
    int err = 0;
    bool shadeAvail = (kind == GFX_PRIM_TRIANGLE) && (geom & G_SHADE);

    in.tex0 = rnd_c4();
    in.tex1 = rnd_c4();
    in.shade = rnd_c4();
    in.prim = rnd_c4();
    in.env = rnd_c4();
    in.fog = rnd_c4();
    in.blend = rnd_c4();
    in.mem = rnd_c4();
    in.mem.a = 0xE0;
    in.primLod = rnd8();
    gGfxRdp.primColor = pack(in.prim);
    gGfxRdp.envColor = pack(in.env);
    gGfxRdp.fogColor = pack(in.fog);
    gGfxRdp.blendColor = pack(in.blend);
    gGfxRdp.primLodFrac = (uint8_t)in.primLod;
    gGfxRdp.dirty |= GFX_DIRTY_COLORS;

    gfx_tev_apply(kind, &info);
    gGfxRdp.dirty = 0;
    check_info(name, &info);
    if (!info.usesShade) {
        C4 garbage = { 77, 99, 11, 201 }; /* gfx_gx.c may not send vertex colors */
        in.shade = shadeAvail ? in.shade : garbage;
    }
    written = mock_gx_eval(in.tex0, in.tex1, in.shade, in.mem, &gx, &tevOut);
    rdp_pixel(gGfxRdp.combineHi, gGfxRdp.combineLo, gGfxRdp.otherModeH, modeL, shadeAvail, &in, &n);

    if (n.overflow) {
        return -1;
    }
    if (n.acValue >= 0 && abs(n.acValue - in.blend.a) <= 3) {
        return -1;
    }
    if (n.cutout && abs(n.rawAlpha - 32) <= 3) {
        return -1;
    }
    /* Both models leave the framebuffer color when they do not write */
    err = c4_diff(gx, n.color, false);
    if (err <= tolBlend) {
        err = 0;
    }
    if (checkAlpha && written && n.written) {
        int ae = abs(tevOut.a - n.pixel.a);

        err = (ae > tol && ae > err) ? ae : err;
    }
    if (err > 0 && !*reported) {
        *reported = true;
        printf("FAIL %s: written gx %d rdp %d, gx %d %d %d (tev %d %d %d %d), rdp %d %d %d (pixel %d %d %d %d)\n",
               name, written, n.written, gx.r, gx.g, gx.b, tevOut.r, tevOut.g, tevOut.b, tevOut.a, n.color.r,
               n.color.g, n.color.b, n.pixel.r, n.pixel.g, n.pixel.b, n.pixel.a);
        printf("    in: t0 %d %d %d %d t1 %d %d %d %d shade %d %d %d %d prim %d %d %d %d env %d %d %d %d fog %d %d %d "
               "%d blend %d %d %d %d mem %d %d %d lod %d\n",
               in.tex0.r, in.tex0.g, in.tex0.b, in.tex0.a, in.tex1.r, in.tex1.g, in.tex1.b, in.tex1.a, in.shade.r,
               in.shade.g, in.shade.b, in.shade.a, in.prim.r, in.prim.g, in.prim.b, in.prim.a, in.env.r, in.env.g,
               in.env.b, in.env.a, in.fog.r, in.fog.g, in.fog.b, in.fog.a, in.blend.r, in.blend.g, in.blend.b,
               in.blend.a, in.mem.r, in.mem.g, in.mem.b, in.primLod);
        dump_program();
    }
    return err;
}

static void run_numeric(const char* name, uint32_t hi, uint32_t lo, uint32_t cycle, uint32_t modeL, GfxPrimKind kind,
                        uint32_t geom, bool checkAlpha, int tol, int tolBlend) {
    int s, compared = 0, skipped = 0, worst = 0, bad = 0;
    bool reported = false;

    set_state(hi, lo, cycle, modeL, geom);
    for (s = 0; s < 1500; s++) {
        int err = run_sample(name, cycle, modeL, kind, geom, checkAlpha, tol, tolBlend, &reported);

        if (err < 0) {
            skipped++;
            continue;
        }
        compared++;
        if (err > 0) {
            bad++;
        }
        worst = err > worst ? err : worst;
    }
    if (bad > 0) {
        sFailures++;
    }
    if (compared < 150) {
        sFailures++;
        printf("FAIL %s: only %d samples compared\n", name, compared);
    }
    printf("  %-48s %2d stages  %4d compared %3d skipped  %s\n", name, gMockGx.numStages, compared, skipped,
           bad ? "MISMATCH" : "ok");
}

static void test_combiners(void) {
    size_t i, v;

    printf("combiner modes (GX TEV vs RDP model):\n");
    for (i = 0; i < sizeof(sCombCases) / sizeof(sCombCases[0]); i++) {
        const CombCase* cc = &sCombCases[i];

        for (v = 0; v < sizeof(sModeVariants) / sizeof(sModeVariants[0]); v++) {
            const ModeVariant* mv = &sModeVariants[v];
            char name[128];
            int tolBlend = mv->checkAlpha ? 10 : mv->tol;

            snprintf(name, sizeof(name), "%s [%s]", cc->name, mv->name);
            run_numeric(name, cc->hi, cc->lo, cc->cycle, cc->cycle == G_CYC_2CYCLE ? mv->modeL2 : mv->modeL1,
                        cc->kind, cc->geom, mv->checkAlpha, mv->tol, tolBlend);
        }
    }
}

static void test_blenders(void) {
    size_t i;

    printf("render modes (GX TEV + pixel engine vs RDP model):\n");
    for (i = 0; i < sizeof(sBlendCases) / sizeof(sBlendCases[0]); i++) {
        const BlendCase* bc = &sBlendCases[i];

        /* Two blender cycles at the RDP's 5-bit factor precision differ more from GX's 8-bit blend */
        run_numeric(bc->name, bc->hi, bc->lo, bc->cycle, bc->modeL, bc->kind, bc->geom, bc->checkAlpha, 4, 16);
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Render mode -> GX settings                                                                       */
/* ------------------------------------------------------------------------------------------------ */

#define ANY -1

typedef struct {
    const char* name;
    uint32_t cycle, modeL;
    GfxPrimKind kind;
    uint32_t geom;
    int blendType, blendSrc, blendDst, colorUpdate;
    int zEnable, zFunc, zUpdate;
    int acComp0, acComp1, acRef1;
    int decal, zCompLoc;
} RmCase;

static const RmCase sRmCases[] = {
    { "AA_ZB_OPA_SURF", G_CYC_1CYCLE, RM1(AA_ZB_OPA_SURF), GFX_PRIM_TRIANGLE, TRI, GX_BM_NONE, ANY, ANY, 1, 1, GX_LEQUAL,
      1, GX_ALWAYS, GX_ALWAYS, ANY, 0, 1 },
    { "AA_ZB_XLU_SURF", G_CYC_1CYCLE, RM1(AA_ZB_XLU_SURF), GFX_PRIM_TRIANGLE, TRI, GX_BM_BLEND, GX_BL_SRCALPHA,
      GX_BL_INVSRCALPHA, 1, 1, GX_LESS, 0, GX_ALWAYS, GX_ALWAYS, ANY, 0, 1 },
    { "XLU_SURF", G_CYC_1CYCLE, RM1(XLU_SURF), GFX_PRIM_TEXRECT, 0, GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, 1, 0,
      ANY, 0, GX_ALWAYS, GX_ALWAYS, ANY, 0, ANY },
    { "OPA_SURF", G_CYC_1CYCLE, RM1(OPA_SURF), GFX_PRIM_TEXRECT, 0, GX_BM_NONE, ANY, ANY, 1, 0, ANY, 0, GX_ALWAYS,
      GX_ALWAYS, ANY, 0, ANY },
    { "FOG_SHADE_A + AA_ZB_OPA_SURF2", G_CYC_2CYCLE, G_RM_FOG_SHADE_A | RM_AA_ZB_OPA_SURF(2), GFX_PRIM_TRIANGLE,
      TRI | G_FOG, GX_BM_NONE, ANY, ANY, 1, 1, GX_LEQUAL, 1, GX_ALWAYS, GX_ALWAYS, ANY, 0, 1 },
    { "FOG_SHADE_A + AA_ZB_XLU_SURF2", G_CYC_2CYCLE, G_RM_FOG_SHADE_A | RM_AA_ZB_XLU_SURF(2), GFX_PRIM_TRIANGLE,
      TRI | G_FOG, GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, 1, 1, GX_LESS, 0, GX_ALWAYS, GX_ALWAYS, ANY, 0, 1 },
    { "FOG_SHADE_A + ZB_XLU_SURF2", G_CYC_2CYCLE, G_RM_FOG_SHADE_A | RM_ZB_XLU_SURF(2), GFX_PRIM_TRIANGLE, TRI | G_FOG,
      GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, 1, 1, GX_LESS, 0, GX_ALWAYS, GX_ALWAYS, ANY, 0, 1 },
    { "FOG_SHADE_A + AA_ZB_TEX_EDGE2", G_CYC_2CYCLE, G_RM_FOG_SHADE_A | RM_AA_ZB_TEX_EDGE(2), GFX_PRIM_TRIANGLE,
      TRI | G_FOG, GX_BM_NONE, ANY, ANY, 1, 1, GX_LEQUAL, 1, GX_ALWAYS, GX_GEQUAL, 32, 0, 0 },
    { "FOG_SHADE_A + AA_ZB_OPA_INTER2", G_CYC_2CYCLE, G_RM_FOG_SHADE_A | RM_AA_ZB_OPA_INTER(2), GFX_PRIM_TRIANGLE,
      TRI | G_FOG, GX_BM_NONE, ANY, ANY, 1, 1, GX_LEQUAL, 1, GX_ALWAYS, GX_ALWAYS, ANY, 0, 1 },
    { "PASS + AA_ZB_OPA_DECAL2", G_CYC_2CYCLE, G_RM_PASS | RM_AA_ZB_OPA_DECAL(2), GFX_PRIM_TRIANGLE, TRI, GX_BM_NONE,
      ANY, ANY, 1, 1, GX_LEQUAL, 0, GX_ALWAYS, GX_ALWAYS, ANY, 1, 1 },
    { "FOG_SHADE_A + AA_ZB_XLU_DECAL2", G_CYC_2CYCLE, G_RM_FOG_SHADE_A | RM_AA_ZB_XLU_DECAL(2), GFX_PRIM_TRIANGLE,
      TRI | G_FOG, GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, 1, 1, GX_LEQUAL, 0, GX_ALWAYS, GX_ALWAYS, ANY, 1,
      1 },
    { "PASS + CLD_SURF2 (logo text)", G_CYC_2CYCLE, G_RM_PASS | RM_CLD_SURF(2), GFX_PRIM_TEXRECT, 0, GX_BM_BLEND,
      GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, 1, 0, ANY, 0, GX_ALWAYS, GX_ALWAYS, ANY, 0, ANY },
    { "PASS + ZB_CLD_SURF2", G_CYC_2CYCLE, G_RM_PASS | RM_ZB_CLD_SURF(2), GFX_PRIM_TRIANGLE, TRI, GX_BM_BLEND,
      GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, 1, 1, GX_LESS, 0, GX_ALWAYS, GX_ALWAYS, ANY, 0, 1 },
    { "PASS + ZB_OVL_SURF2", G_CYC_2CYCLE, G_RM_PASS | RM_ZB_OVL_SURF(2), GFX_PRIM_TRIANGLE, TRI, GX_BM_BLEND,
      GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, 1, 1, GX_LEQUAL, 0, GX_ALWAYS, GX_ALWAYS, ANY, 1, 1 },
    { "PASS + AA_XLU_SURF2", G_CYC_2CYCLE, G_RM_PASS | RM_AA_XLU_SURF(2), GFX_PRIM_TRIANGLE, TRI, GX_BM_BLEND,
      GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, 1, 0, ANY, 0, GX_ALWAYS, GX_ALWAYS, ANY, 0, ANY },
    { "PASS + AA_OPA_SURF2", G_CYC_2CYCLE, G_RM_PASS | RM_AA_OPA_SURF(2), GFX_PRIM_TRIANGLE, TRI, GX_BM_NONE, ANY, ANY,
      1, 0, ANY, 0, GX_ALWAYS, GX_ALWAYS, ANY, 0, ANY },
    { "ADD", G_CYC_1CYCLE, RM1(ADD), GFX_PRIM_TRIANGLE, TRI, GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_ONE, 1, 0, ANY, 0,
      GX_ALWAYS, GX_ALWAYS, ANY, 0, ANY },
    { "NOOP", G_CYC_1CYCLE, RM1(NOOP), GFX_PRIM_TRIANGLE, TRI, GX_BM_NONE, ANY, ANY, 1, 0, ANY, 0, GX_ALWAYS, GX_ALWAYS,
      ANY, 0, ANY },
    { "VISCVG", G_CYC_1CYCLE, RM1(VISCVG), GFX_PRIM_TRIANGLE, TRI, GX_BM_NONE, ANY, ANY, 1, 0, ANY, 0, GX_ALWAYS,
      GX_ALWAYS, ANY, 0, ANY },
    { "threshold + XLU_SURF", G_CYC_1CYCLE, G_AC_THRESHOLD | RM1(XLU_SURF), GFX_PRIM_TEXRECT, 0, GX_BM_BLEND,
      GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, 1, 0, ANY, 0, GX_GEQUAL, GX_ALWAYS, ANY, 0, 0 },
    { "threshold + AA_ZB_OPA_SURF (coverage)", G_CYC_1CYCLE, G_AC_THRESHOLD | RM1(AA_ZB_OPA_SURF), GFX_PRIM_TRIANGLE,
      TRI, GX_BM_NONE, ANY, ANY, 1, 1, GX_LEQUAL, 1, GX_ALWAYS, GX_ALWAYS, ANY, 0, 1 },
    { "AA_ZB_OPA_SURF rect: no z", G_CYC_1CYCLE, RM1(AA_ZB_OPA_SURF), GFX_PRIM_TEXRECT, 0, GX_BM_NONE, ANY, ANY, 1, 0,
      ANY, 0, GX_ALWAYS, GX_ALWAYS, ANY, 0, ANY },
    { "AA_ZB_OPA_SURF without G_ZBUFFER", G_CYC_1CYCLE, RM1(AA_ZB_OPA_SURF), GFX_PRIM_TRIANGLE, G_SHADE, GX_BM_NONE, ANY,
      ANY, 1, 0, ANY, 0, GX_ALWAYS, GX_ALWAYS, ANY, 0, ANY },
    { "G_ZS_PRIM z write, no color", G_CYC_1CYCLE,
      G_ZS_PRIM | Z_UPD | IM_RD | CVG_DST_SAVE | ZMODE_OPA | FORCE_BL | GBL_c1(G_BL_CLR_BL, G_BL_0, G_BL_CLR_MEM, G_BL_1MA) |
          GBL_c2(G_BL_CLR_BL, G_BL_0, G_BL_CLR_MEM, G_BL_1MA),
      GFX_PRIM_TRIANGLE, TRI, ANY, ANY, ANY, 0, 1, GX_ALWAYS, 1, GX_ALWAYS, GX_ALWAYS, ANY, 0, ANY },
    { "G_ZS_PRIM rect z test", G_CYC_1CYCLE, G_ZS_PRIM | RM1(AA_ZB_OPA_SURF), GFX_PRIM_TEXRECT, 0, GX_BM_NONE, ANY, ANY,
      1, 1, GX_LEQUAL, 1, GX_ALWAYS, GX_ALWAYS, ANY, 0, ANY },
    { "IN*0 + MEM*1 FORCE_BL: color off", G_CYC_1CYCLE,
      G_AC_THRESHOLD | G_ZS_PRIM | AA_EN | IM_RD | CVG_DST_WRAP | ZMODE_OPA | CVG_X_ALPHA | ALPHA_CVG_SEL | FORCE_BL |
          GBL_c1(G_BL_CLR_IN, G_BL_0, G_BL_CLR_MEM, G_BL_1) | GBL_c2(G_BL_CLR_IN, G_BL_0, G_BL_CLR_MEM, G_BL_1),
      GFX_PRIM_TRIANGLE, TRI, ANY, ANY, ANY, 0, 0, ANY, 0, GX_GEQUAL, GX_GEQUAL, 32, 0, 0 },
    { "fbdemo circle: MEM blend in cycle 0", G_CYC_2CYCLE,
      AA_EN | IM_RD | CVG_DST_FULL | ZMODE_OPA | CVG_X_ALPHA | FORCE_BL | GBL_c1(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM, G_BL_1MA) |
          GBL_c2(G_BL_CLR_IN, G_BL_0, G_BL_CLR_IN, G_BL_1),
      GFX_PRIM_TRIANGLE, TRI, GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, 1, 0, ANY, 0, GX_ALWAYS, GX_GEQUAL, 32, 0,
      0 },
    { "COPY with alpha compare", G_CYC_COPY, G_AC_THRESHOLD | RM1(AA_ZB_OPA_SURF), GFX_PRIM_TEXRECT, 0, GX_BM_NONE, ANY,
      ANY, 1, 0, ANY, 0, GX_ALWAYS, GX_GEQUAL, 0x80, 0, 0 },
    { "COPY without alpha compare", G_CYC_COPY, RM1(XLU_SURF), GFX_PRIM_TEXRECT, 0, GX_BM_NONE, ANY, ANY, 1, 0, ANY, 0,
      GX_ALWAYS, GX_ALWAYS, ANY, 0, 1 },
};

static void test_render_modes(void) {
    size_t i;

    printf("render mode settings:\n");
    for (i = 0; i < sizeof(sRmCases) / sizeof(sRmCases[0]); i++) {
        const RmCase* rc = &sRmCases[i];
        GfxTevInfo info;
        int before = sFailures;

        if (rc->cycle == G_CYC_2CYCLE) {
            set_state(MODE(G_CC_MODULATEIA, G_CC_MODULATEIA2), rc->cycle, rc->modeL, rc->geom);
        } else {
            set_state(MODE(G_CC_MODULATEIA, G_CC_MODULATEIA), rc->cycle, rc->modeL, rc->geom);
        }
        gGfxRdp.blendColor = 0x10203040;
        gfx_tev_apply(rc->kind, &info);
        check_info(rc->name, &info);

#define EXPECT(field, val)                                                                                  \
    CHECK((val) == ANY || (int)gMockGx.field == (val), "%s: " #field " %d, expected %d", rc->name, \
          (int)gMockGx.field, (val))
        EXPECT(blendType, rc->blendType);
        if (rc->blendType == GX_BM_BLEND) {
            EXPECT(blendSrc, rc->blendSrc);
            EXPECT(blendDst, rc->blendDst);
        }
        EXPECT(colorUpdate, rc->colorUpdate);
        EXPECT(zEnable, rc->zEnable);
        if (rc->zEnable) {
            EXPECT(zFunc, rc->zFunc);
        }
        EXPECT(zUpdate, rc->zUpdate);
        EXPECT(acComp0, rc->acComp0);
        if (rc->acComp0 == GX_GEQUAL) {
            CHECK(gMockGx.acRef0 == 0x40, "%s: alpha reference %d, expected the blend alpha", rc->name, gMockGx.acRef0);
        }
        EXPECT(acComp1, rc->acComp1);
        if (rc->acComp1 != GX_ALWAYS) {
            EXPECT(acRef1, rc->acRef1);
        }
        EXPECT(zCompLoc, rc->zCompLoc);
        EXPECT(alphaUpdate, 0);
#undef EXPECT
        CHECK(info.decal == rc->decal, "%s: decal %d", rc->name, info.decal);
        CHECK(info.depthTest == (rc->zEnable == 1 && rc->zFunc != GX_ALWAYS) &&
                  info.depthWrite == (rc->zUpdate == 1),
              "%s: info depth test %d write %d", rc->name, info.depthTest, info.depthWrite);
        if (sFailures != before) {
            dump_program();
        }
        printf("  %-48s %s\n", rc->name, sFailures == before ? "ok" : "MISMATCH");
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Program structure, caching                                                                       */
/* ------------------------------------------------------------------------------------------------ */

static int count_konst(int sel) {
    int s, n = 0;

    for (s = 0; s < gMockGx.numStages; s++) {
        bool uses = false;
        int j;

        for (j = 0; j < 4; j++) {
            uses |= gMockGx.st[s].cin[j] == GX_CC_KONST;
        }
        n += uses && gMockGx.st[s].kcsel == sel;
    }
    return n;
}

static void test_structure(void) {
    GfxTevInfo info;
    int calls, programs;

    printf("program structure and caching:\n");

    /* Fog: a stage lerping toward K0 by the shade alpha */
    set_state(MODE(G_CC_MODULATEIDECALA, G_CC_MODULATEIA_PRIM2), G_CYC_2CYCLE, G_RM_FOG_SHADE_A | RM_AA_ZB_OPA_SURF(2),
              TRI | G_FOG);
    gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
    CHECK(count_konst(GX_TEV_KCSEL_K0) == 1, "fog stage reads K0");
    CHECK(info.usesTexel0 && !info.usesTexel1 && info.usesShade, "fog: textures/shade");
    CHECK(gMockGx.numStages == 3, "T0*S, *P, fog: %d stages", gMockGx.numStages);

    /* Single-stage combiners */
    set_state(MODE(G_CC_MODULATEIA, G_CC_MODULATEIA), G_CYC_1CYCLE, RM1(AA_ZB_OPA_SURF), TRI);
    gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
    CHECK(gMockGx.numStages == 1, "MODULATEIA: %d stages", gMockGx.numStages);
    set_state(MODE(G_CC_BLENDPE, G_CC_BLENDPE), G_CYC_1CYCLE, RM1(XLU_SURF), 0);
    gfx_tev_apply(GFX_PRIM_TEXRECT, &info);
    CHECK(gMockGx.numStages == 1 && !info.usesShade, "BLENDPE rect: %d stages, shade %d", gMockGx.numStages,
          info.usesShade);
    set_state(MODE(G_CC_MODULATEIDECALA, G_CC_PASS2), G_CYC_2CYCLE, G_RM_PASS | RM_AA_ZB_OPA_SURF(2), TRI);
    gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
    CHECK(gMockGx.numStages == 1, "MODULATEIDECALA, PASS2: %d stages", gMockGx.numStages);

    /* TEXEL1 in 1-cycle mode reads tile, in cycle 1 TEXEL0 reads tile + 1 */
    set_state(LERP(0, 0, 0, 0, 0, 0, 0, 0, TEXEL1, 0, SHADE, 0, 0, 0, 0, 1), G_CYC_1CYCLE, RM1(OPA_SURF), TRI);
    gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
    CHECK(info.usesTexel0 && !info.usesTexel1, "1-cycle TEXEL1 = tile");
    set_state(LERP(0, 0, 0, SHADE, 0, 0, 0, 1, TEXEL0, 0, COMBINED, 0, 0, 0, 0, 1), G_CYC_2CYCLE,
              G_RM_PASS | RM_OPA_SURF(2), TRI);
    gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
    CHECK(!info.usesTexel0 && info.usesTexel1, "cycle 1 TEXEL0 = tile + 1");

    /* COMBINED in 1-cycle mode: both cycles run (with the 1-cycle texel mapping) */
    {
        int logs = mock_log_count();
        N64In in;

        set_state(MODE(G_CC_MODULATEIA, G_CC_MODULATEIA_PRIM2), G_CYC_1CYCLE, RM1(OPA_SURF), TRI);
        gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
        CHECK(gMockGx.numStages == 2 && info.usesTexel0 && !info.usesTexel1, "1-cycle COMBINED: %d stages",
              gMockGx.numStages);
        CHECK(mock_log_count() == logs + 1, "1-cycle COMBINED warns once");
        memset(&in, 0, sizeof(in));
        in.tex0.r = in.tex0.g = in.tex0.b = 200;
        in.shade.r = in.shade.g = in.shade.b = 128;
        gGfxRdp.primColor = 0x80808080;
        gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
        {
            C4 out, tev;

            mock_gx_eval(in.tex0, in.tex1, in.shade, in.mem, &out, &tev);
            CHECK(abs(tev.r - 50) <= 2, "1-cycle COMBINED: T0*S*P = %d, expected 50", tev.r);
        }
    }

    /* Out of TEV registers (prim, env, a saved cycle 0 and TEXEL1 all needed): warns, still a valid program */
    {
        int logs = mock_log_count();

        set_state(LERP(TEXEL1, PRIMITIVE, ENV_ALPHA, TEXEL0, 0, 0, 0, 1, PRIMITIVE, ENVIRONMENT, COMBINED, TEXEL0, 0,
                       0, 0, COMBINED),
                  G_CYC_2CYCLE, G_RM_PASS | RM_OPA_SURF(2), TRI);
        gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
        check_info("out of registers", &info);
        CHECK(mock_log_count() > logs, "out of registers warns");
    }

    /* Cache: the same state twice uploads nothing; colors alone upload colors only */
    set_state(MODE(G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM2), G_CYC_2CYCLE, G_RM_FOG_SHADE_A | RM_AA_ZB_XLU_SURF(2),
              TRI | G_FOG);
    gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
    calls = gMockGx.calls;
    programs = gMockGx.programCalls;
    gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
    CHECK(gMockGx.calls == calls, "repeated apply made %d GX calls", gMockGx.calls - calls);
    gGfxRdp.primColor ^= 0x01020300;
    gGfxRdp.dirty = GFX_DIRTY_COLORS;
    gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
    CHECK(gMockGx.programCalls == programs && gMockGx.calls == calls + 1, "prim color change: %d calls, %d programs",
          gMockGx.calls - calls, gMockGx.programCalls - programs);
    gGfxRdp.blendColor ^= 0x00000011;
    calls = gMockGx.calls;
    gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
    CHECK(gMockGx.calls == calls + 1, "blend color change (no threshold): %d calls", gMockGx.calls - calls);

    /* Same combiner as a rectangle is a different program (no shade) */
    gfx_tev_apply(GFX_PRIM_TEXRECT, &info);
    CHECK(gMockGx.programCalls == programs + 1 && !info.usesShade, "rect after triangle");
    gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
    CHECK(gMockGx.programCalls == programs + 2 && info.usesShade, "triangle again");

    /* Invalidate re-uploads */
    gfx_tev_invalidate();
    gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
    CHECK(gMockGx.programCalls == programs + 3, "invalidate");

    /* FILL mode: nothing touched, info cleared, next apply uploads again */
    calls = gMockGx.calls;
    gGfxRdp.otherModeH = G_CYC_FILL;
    memset(&info, 0xFF, sizeof(info));
    gfx_tev_apply(GFX_PRIM_FILLRECT, &info);
    CHECK(gMockGx.calls == calls && !info.usesTexel0 && !info.usesShade && !info.depthTest, "FILL mode");
    gGfxRdp.otherModeH = G_CYC_2CYCLE;
    gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
    CHECK(gMockGx.programCalls == programs + 4, "after FILL mode");

    /* Many distinct programs: the cache keeps working when full */
    {
        uint32_t i;

        for (i = 0; i < 2000; i++) {
            set_state(MODE(G_CC_MODULATEIA, G_CC_MODULATEIA2), G_CYC_2CYCLE, G_RM_PASS | RM_XLU_SURF(2) | (i << 4 & 0x30),
                      TRI);
            gGfxRdp.combineHi = (gGfxRdp.combineHi & ~0x1Fu) | (i % 13);
            gfx_tev_apply(GFX_PRIM_TRIANGLE, &info);
            check_info("cache churn", &info);
        }
    }
    printf("  %-48s %s\n", "structure/caching", "done");
}

/* ------------------------------------------------------------------------------------------------ */
/* Random combine and render modes                                                                  */
/* ------------------------------------------------------------------------------------------------ */

/* A combiner code biased toward the inputs that exist (the rest of each range reads zero). Without `comb`,
 * never COMBINED / COMBINED_ALPHA (code 0, and 7 in the color multiplier): it has no value in the first
 * cycle (gfx_tev.c logs an approximation and the program would be skipped). */
static uint32_t rnd_code(uint32_t n, uint32_t zero, bool comb, bool mul) {
    uint32_t c;

    if (rng() % 8 == 0) {
        return zero;
    }
    do {
        c = rng() % n;
    } while (!comb && (c == 0 || (mul && c == 7)));
    return c;
}

static uint32_t rnd_alpha(bool comb) {
    uint32_t c;

    do {
        c = rng() & 7;
    } while (!comb && c == 0);
    return c;
}

static uint32_t rnd_blender_cycle(int clk) {
    uint32_t p = rng() & 3, a = rng() & 3, m = rng() & 3, b = rng() & 3;

    return clk == 1 ? GBL_c1(p, a, m, b) : GBL_c2(p, a, m, b);
}

/* Each program gfx_tev.c compiles without logging an approximation must match the RDP model, sample by
 * sample. The curated tables only cover modes someone thought of; this covers scheduling, register
 * allocation and substitution paths they miss. gfx_tev_init() before each program re-arms the warnings, so
 * that a program that was approximated is recognised (and skipped). */
static void fuzz_one(const char* what, uint32_t hi, uint32_t lo, uint32_t cycle, uint32_t modeL, GfxPrimKind kind,
                     uint32_t geom, bool checkAlpha, int tolBlend, int* programs, int* approximated, int* bad) {
    GfxTevInfo info;
    int logs, s, compared = 0, worst = 0;
    bool reported = (*bad >= 6); /* print the first few failing programs only */
    char name[160];

    gfx_tev_init();
    set_state(hi, lo, cycle, modeL, geom);
    logs = mock_log_count();
    gfx_tev_apply(kind, &info);
    if (mock_log_count() != logs) {
        (*approximated)++;
        return;
    }
    (*programs)++;
    snprintf(name, sizeof(name), "%s %06X %08X cycle %u mode L %08X kind %d geom %X", what, (unsigned)hi,
             (unsigned)lo, (unsigned)(cycle >> G_MDSFT_CYCLETYPE), (unsigned)modeL, (int)kind, (unsigned)geom);
    for (s = 0; s < 120; s++) {
        int err = run_sample(name, cycle, modeL, kind, geom, checkAlpha, 4, tolBlend, &reported);

        if (err >= 0) {
            compared++;
            worst = err > worst ? err : worst;
        }
    }
    if (worst > 0) {
        (*bad)++;
        sFailures++;
    }
}

static void test_fuzz(void) {
    /* Blend tolerances as in the tables above: the RDP blender's 5-bit factors (one blending cycle 10, two 16) */
    static const struct {
        uint32_t modeL1, modeL2;
        bool checkAlpha;
        int tol1, tol2;
    } modes[] = {
        { RM1(XLU_SURF), G_RM_PASS | RM_XLU_SURF(2), true, 10, 10 },
        { RM1(OPA_SURF), G_RM_PASS | RM_OPA_SURF(2), false, 4, 4 },
        { RM1(AA_ZB_XLU_SURF), G_RM_FOG_SHADE_A | RM_AA_ZB_XLU_SURF(2), true, 10, 16 },
        { RM1(AA_ZB_OPA_SURF), G_RM_FOG_SHADE_A | RM_AA_ZB_OPA_SURF(2), false, 4, 10 },
        { RM1(AA_ZB_TEX_EDGE), G_RM_FOG_SHADE_A | RM_AA_ZB_TEX_EDGE(2), false, 4, 10 },
        { G_AC_THRESHOLD | RM1(XLU_SURF), G_AC_THRESHOLD | G_RM_PASS | RM_XLU_SURF(2), false, 10, 10 },
    };
    int i, programs = 0, approximated = 0, bad = 0, before;

    printf("random modes (GX TEV + pixel engine vs RDP model):\n");
    mock_log_quiet(true);

    /* Random combine words under common render modes */
    before = sFailures;
    for (i = 0; i < 20000; i++) {
        uint32_t cycle = (i & 1) ? G_CYC_2CYCLE : G_CYC_1CYCLE;
        bool comb1 = (cycle == G_CYC_2CYCLE); /* COMBINED is defined in the second of two cycles only */
        /* [cycle][a, b, c, d] for color and alpha */
        uint32_t cc[2][4], ac[2][4];
        uint32_t hi, lo;
        int cyc, k;

        for (cyc = 0; cyc < 2; cyc++) {
            bool comb = (cyc == 1) && comb1;

            cc[cyc][0] = rnd_code(8, 15, comb, false);
            cc[cyc][1] = rnd_code(8, 15, comb, false);
            cc[cyc][2] = rnd_code(16, 31, comb, true);
            cc[cyc][3] = rnd_alpha(comb);
            ac[cyc][0] = rnd_alpha(comb);
            ac[cyc][1] = rnd_alpha(comb);
            ac[cyc][2] = rng() & 7;
            ac[cyc][3] = rnd_alpha(comb);
            /* Often one of the shapes the compiler special-cases: D alone, C alone (1 - 0) * C + 0, a lerp
             * (D == B), a multiply-add (B == 0) */
            for (k = 0; k < 2; k++) {
                uint32_t* f = k ? ac[cyc] : cc[cyc];
                uint32_t zero = k ? 7 : 15, one = 6;

                switch (rng() % 6) {
                    case 0:
                        f[2] = k ? 7 : 31;
                        break;
                    case 1:
                        f[0] = one;
                        f[1] = zero;
                        f[3] = 7;
                        break;
                    case 2:
                        f[3] = (f[1] < 8) ? f[1] : 7;
                        break;
                    case 3:
                        f[1] = zero;
                        break;
                    default:
                        break;
                }
            }
        }
        hi = (cc[0][0] << 20) | (cc[0][2] << 15) | (ac[0][0] << 12) | (ac[0][2] << 9) | (cc[1][0] << 5) | cc[1][2];
        lo = (cc[0][1] << 28) | (cc[1][1] << 24) | (ac[1][0] << 21) | (ac[1][2] << 18) | (cc[0][3] << 15) |
             (ac[0][1] << 12) | (ac[0][3] << 9) | (cc[1][3] << 6) | (ac[1][1] << 3) | ac[1][3];
        int m = (int)(rng() % (sizeof(modes) / sizeof(modes[0])));
        bool rect = (rng() % 6) == 0;

        fuzz_one("combine", hi, lo, cycle, cycle == G_CYC_2CYCLE ? modes[m].modeL2 : modes[m].modeL1,
                 rect ? GFX_PRIM_TEXRECT : GFX_PRIM_TRIANGLE, rect ? 0 : TRI | G_FOG, modes[m].checkAlpha,
                 cycle == G_CYC_2CYCLE ? modes[m].tol2 : modes[m].tol1, &programs, &approximated, &bad);
    }
    printf("  %-48s %4d programs %4d approximated  %s\n", "random combine modes", programs, approximated,
           sFailures == before ? "ok" : "MISMATCH");

    /* Random blender settings under a few combiners */
    before = sFailures;
    programs = approximated = bad = 0;
    for (i = 0; i < 6000; i++) {
        static const uint32_t combs[][2] = {
            { MODE(G_CC_MODULATEIA, G_CC_MODULATEIA) },
            { MODE(G_CC_MODULATEIA_PRIM, G_CC_MODULATEIA_PRIM2) },
            { MODE(G_CC_BLENDPE, G_CC_PASS2) },
        };
        static const uint32_t flags[] = { AA_EN, CVG_X_ALPHA, ALPHA_CVG_SEL, FORCE_BL, Z_CMP, Z_UPD };
        int c = (int)(rng() % 3);
        uint32_t cycle = (i & 1) ? G_CYC_2CYCLE : G_CYC_1CYCLE;
        uint32_t modeL = rnd_blender_cycle(1) | rnd_blender_cycle(2) | (rng() % 4 == 0 ? G_AC_THRESHOLD : 0);
        bool rect = (rng() % 6) == 0;
        size_t f;

        for (f = 0; f < sizeof(flags) / sizeof(flags[0]); f++) {
            modeL |= (rng() & 1) ? flags[f] : 0;
        }
        /* A_MEM (memory coverage) counts as 1 in gfx_tev.c, where the RDP also quantizes the P factor to 4/32
         * steps: keep it only where the cycle does not blend (the last cycle without FORCE_BL) */
        if (cycle == G_CYC_2CYCLE && ((modeL >> 18) & 3) == G_BL_A_MEM) {
            modeL ^= (uint32_t)(G_BL_A_MEM ^ G_BL_1) << 18;
        }
        if ((modeL & FORCE_BL) && ((modeL >> (cycle == G_CYC_2CYCLE ? 16 : 18)) & 3) == G_BL_A_MEM) {
            modeL ^= (uint32_t)(G_BL_A_MEM ^ G_BL_1) << (cycle == G_CYC_2CYCLE ? 16 : 18);
        }
        /* Random blender cycles chain products (X * A, then IN * (A + 1)) where the RDP's 5-bit factors
         * (floor(a / 8) / 32, and the "+ 1" of the M factor) compound to ~20 levels; translation errors are
         * far larger */
        fuzz_one("blend", combs[c][0], combs[c][1], cycle, modeL, rect ? GFX_PRIM_TEXRECT : GFX_PRIM_TRIANGLE,
                 rect ? 0 : TRI | G_FOG, false, cycle == G_CYC_2CYCLE ? 24 : 12, &programs, &approximated, &bad);
    }
    printf("  %-48s %4d programs %4d approximated  %s\n", "random render modes", programs, approximated,
           sFailures == before ? "ok" : "MISMATCH");

    mock_log_quiet(false);
    gfx_tev_init();
}

int main(void) {
    mock_log_quiet(false);
    gfx_tev_init();
    test_combiners();
    test_blenders();
    test_render_modes();
    test_structure();
    test_fuzz();
    printf("gc_log calls (warnings, once each): %d\n", mock_log_count());
    if (sFailures) {
        printf("%d FAILURES\n", sFailures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
