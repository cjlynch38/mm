/**
 * Host test of the renderer's RSP module (port/gc/gfx/gfx_rsp.c): builds F3DEX2 display lists in fake N64
 * RAM, runs them through gfx_rsp_run() and checks what reaches the mocked GX / RDP entry points against
 * references computed here: Mtx parsing, the matrix stack, clip coordinates and clip codes, directional
 * and point lighting, fog, texgen, culling, display list calls / branches / G_BRANCH_Z / G_CULLDL,
 * segments, G_MODIFYVTX, texture rectangles, forced matrices, S2DEX2 switching and logging, the microcode's
 * light direction quantization and matrix saturation, bad addresses in display lists, the statistics
 * lines, and random display lists (fuzzing under the sanitizers).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "test.h"

static int sChecks, sFailures;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        sChecks++;                                           \
        if (!(cond)) {                                       \
            sFailures++;                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)

/* ============================================================================================== */
/* Display list builder and GBI encoders (F3DEX2 encodings from include/PR/gbi.h)                 */
/* ============================================================================================== */

typedef struct {
    uint32_t start, cur;
} Dl;

static Dl dl_new(int cmds) {
    Dl d;

    d.start = d.cur = ram_alloc((uint32_t)cmds * 8);
    return d;
}

static void op(Dl* d, uint32_t w0, uint32_t w1) {
    wr32(d->cur, w0);
    wr32(d->cur + 4, w1);
    d->cur += 8;
}

#define CMD(c) ((uint32_t)(c) << 24)

static uint32_t tri_word(int a, int b, int c) {
    return ((uint32_t)(a * 2) << 16) | ((uint32_t)(b * 2) << 8) | (uint32_t)(c * 2);
}

static void gVtx(Dl* d, uint32_t addr, int n, int v0) {
    op(d, CMD(G_VTX) | ((uint32_t)n << 12) | ((uint32_t)(v0 + n) << 1), addr);
}

static void gMtx(Dl* d, uint32_t addr, uint32_t params) {
    op(d, CMD(G_MTX) | (((64 - 1) / 8) << 19) | ((params ^ G_MTX_PUSH) & 0xFF), addr);
}

static void gTri1(Dl* d, int a, int b, int c) {
    op(d, CMD(G_TRI1) | tri_word(a, b, c), 0);
}

static void gTri2(Dl* d, int a, int b, int c, int e, int f, int g) {
    op(d, CMD(G_TRI2) | tri_word(a, b, c), tri_word(e, f, g));
}

static void gDL(Dl* d, uint32_t addr) {
    op(d, CMD(G_DL) | ((uint32_t)G_DL_PUSH << 16), addr);
}

static void gBranch(Dl* d, uint32_t addr) {
    op(d, CMD(G_DL) | ((uint32_t)G_DL_NOPUSH << 16), addr);
}

static void gEnd(Dl* d) {
    op(d, CMD(G_ENDDL), 0);
}

static void gGeom(Dl* d, uint32_t clear, uint32_t set) {
    op(d, CMD(G_GEOMETRYMODE) | (~clear & 0xFFFFFF), set);
}

static void gTexture(Dl* d, uint32_t s, uint32_t t, int level, int tile, int on) {
    op(d, CMD(G_TEXTURE) | ((uint32_t)level << 11) | ((uint32_t)tile << 8) | ((uint32_t)on << 1), (s << 16) | t);
}

static void gMoveWd(Dl* d, uint32_t index, uint32_t ofs, uint32_t data) {
    op(d, CMD(G_MOVEWORD) | (index << 16) | ofs, data);
}

static void gSegment(Dl* d, int seg, uint32_t base) {
    gMoveWd(d, G_MW_SEGMENT, (uint32_t)seg * 4, base);
}

static void gNumLights(Dl* d, int n) {
    gMoveWd(d, G_MW_NUMLIGHT, G_MWO_NUMLIGHT, (uint32_t)n * 24);
}

static void gMoveMem(Dl* d, uint32_t addr, uint32_t len, uint32_t idx, uint32_t ofs) {
    op(d, CMD(G_MOVEMEM) | (((len - 1) / 8) << 19) | ((ofs / 8) << 8) | idx, addr);
}

static void gLight(Dl* d, uint32_t addr, int n) {
    gMoveMem(d, addr, 16, G_MV_LIGHT, (uint32_t)n * 24 + 24);
}

static void gLookAtX(Dl* d, uint32_t addr) {
    gMoveMem(d, addr, 16, G_MV_LIGHT, G_MVO_LOOKATX);
}

static void gLookAtY(Dl* d, uint32_t addr) {
    gMoveMem(d, addr, 16, G_MV_LIGHT, G_MVO_LOOKATY);
}

static void gFog(Dl* d, int fm, int fo) {
    gMoveWd(d, G_MW_FOG, G_MWO_FOG, ((uint32_t)(fm & 0xFFFF) << 16) | (uint32_t)(fo & 0xFFFF));
}

static void gViewport(Dl* d, uint32_t addr) {
    gMoveMem(d, addr, 16, G_MV_VIEWPORT, 0);
}

static void gPopMtx(Dl* d, int num) {
    op(d, CMD(G_POPMTX) | (((64 - 1) / 8) << 19) | 2, (uint32_t)num * 64);
}

static void gCullDL(Dl* d, int v0, int vn) {
    op(d, CMD(G_CULLDL) | (uint32_t)(v0 * 2), (uint32_t)(vn * 2));
}

static void gBranchZraw(Dl* d, uint32_t dl, int vtx, uint32_t zval) {
    op(d, CMD(G_RDPHALF_1), dl);
    op(d, CMD(G_BRANCH_Z) | ((uint32_t)(vtx * 5) << 12) | (uint32_t)(vtx * 2), zval);
}

static void gModifyVtx(Dl* d, int vtx, uint32_t where, uint32_t val) {
    op(d, CMD(G_MODIFYVTX) | (where << 16) | (uint32_t)(vtx * 2), val);
}

static void gLoadUcode(Dl* d, uint32_t text, uint32_t data) {
    op(d, CMD(G_RDPHALF_1), data);
    op(d, CMD(G_LOAD_UCODE) | (0x800 - 1), text);
}

static void gForceMtx(Dl* d, uint32_t addr) {
    gMoveMem(d, addr, 64, G_MV_MATRIX, 0);
    gMoveWd(d, G_MW_FORCEMTX, 0, 0x00010000);
}

static void gTexRect(Dl* d, bool flip, uint32_t xl, uint32_t yl, uint32_t xh, uint32_t yh, int tile, uint32_t s,
                     uint32_t t, uint32_t dsdx, uint32_t dtdy) {
    op(d, CMD(flip ? G_TEXRECTFLIP : G_TEXRECT) | (xh << 12) | yh, ((uint32_t)tile << 24) | (xl << 12) | yl);
    op(d, CMD(G_RDPHALF_1), (s << 16) | t);
    op(d, CMD(G_RDPHALF_2), (dsdx << 16) | dtdy);
}

/* An RDP command the RSP passes through; used to see which display list parts ran */
static void gMarker(Dl* d, uint32_t v) {
    op(d, CMD(G_SETPRIMCOLOR), v);
}

static int markers(uint32_t* out, int max) {
    int i, n = 0;

    for (i = 0; i < gRdpCount && i < MAX_RDP; i++) {
        if ((gRdp[i].w0 >> 24) == G_SETPRIMCOLOR && n < max) {
            out[n++] = gRdp[i].w1;
        }
    }
    return n;
}

/* KSEG0 address of a dummy microcode symbol, as the game passes SysUcode_GetUCode() */
static uint32_t ucode_addr(const void* sym) {
    return 0x80000000u | (uint32_t)(((uintptr_t)sym - gGfxHostRamBias) & 0x1FFFFFFF);
}

/* ============================================================================================== */
/* Matrices (double, row vectors: clip = v * MV * P)                                              */
/* ============================================================================================== */

typedef double M4[4][4];

static void m_identity(M4 m) {
    int i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            m[i][j] = (i == j) ? 1.0 : 0.0;
        }
    }
}

static void m_mul(M4 out, const M4 a, const M4 b) {
    M4 t;
    int i, j, k;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            t[i][j] = 0.0;
            for (k = 0; k < 4; k++) {
                t[i][j] += a[i][k] * b[k][j];
            }
        }
    }
    memcpy(out, t, sizeof(t));
}

static void m_translate(M4 m, double x, double y, double z) {
    m_identity(m);
    m[3][0] = x;
    m[3][1] = y;
    m[3][2] = z;
}

static void m_scale(M4 m, double x, double y, double z) {
    m_identity(m);
    m[0][0] = x;
    m[1][1] = y;
    m[2][2] = z;
}

/* Rotation about Y by deg degrees (row vectors) */
static void m_rot_y(M4 m, double deg) {
    double r = deg * M_PI / 180.0;

    m_identity(m);
    m[0][0] = cos(r);
    m[0][2] = -sin(r);
    m[2][0] = sin(r);
    m[2][2] = cos(r);
}

/* guPerspectiveF */
static void m_perspective(M4 m, double fovy, double aspect, double near, double far) {
    double cot = 1.0 / tan(fovy * M_PI / 360.0);

    memset(m, 0, sizeof(M4));
    m[0][0] = cot / aspect;
    m[1][1] = cot;
    m[2][2] = (near + far) / (near - far);
    m[2][3] = -1.0;
    m[3][2] = (2.0 * near * far) / (near - far);
}

static int32_t fixed(double v) {
    return (int32_t)llround(v * 65536.0);
}

/* Mtx in RAM: integer halves then fraction halves */
static uint32_t put_mtx(const M4 m) {
    uint32_t a = ram_alloc(64);
    int i;

    for (i = 0; i < 16; i++) {
        int32_t v = fixed(m[i / 4][i % 4]);

        wr16(a + 2 * i, (uint16_t)((uint32_t)v >> 16));
        wr16(a + 32 + 2 * i, (uint16_t)v);
    }
    return a;
}

/* The matrix as the N64 sees it (s15.16) */
static void m_quantize(M4 out, const M4 m) {
    int i, j;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            out[i][j] = fixed(m[i][j]) / 65536.0;
        }
    }
}

static void xform(const M4 m, double x, double y, double z, double out[4]) {
    int j;

    for (j = 0; j < 4; j++) {
        out[j] = x * m[0][j] + y * m[1][j] + z * m[2][j] + m[3][j];
    }
}

static uint8_t ref_clip(const double c[4]) {
    uint8_t f = 0;

    f |= (c[0] <= -c[3]) ? GFX_CLIP_NEG_X : 0;
    f |= (c[0] >= c[3]) ? GFX_CLIP_POS_X : 0;
    f |= (c[1] <= -c[3]) ? GFX_CLIP_NEG_Y : 0;
    f |= (c[1] >= c[3]) ? GFX_CLIP_POS_Y : 0;
    f |= (c[3] <= 0.0) ? GFX_CLIP_NEG_W : 0;
    f |= (c[2] >= c[3]) ? GFX_CLIP_FAR : 0;
    return f;
}

/* ============================================================================================== */
/* Vertices, lights, viewport                                                                     */
/* ============================================================================================== */

static uint32_t vtx_buf(int n) {
    return ram_alloc((uint32_t)n * 16);
}

/* Vtx: ob[3], flag, tc[2], cn[4] (color, or normal xyz + alpha) */
static void vtx_set(uint32_t buf, int i, int x, int y, int z, int s, int t, int c0, int c1, int c2, int c3) {
    uint32_t a = buf + (uint32_t)i * 16;
    uint8_t* p;

    wr16(a, (uint16_t)x);
    wr16(a + 2, (uint16_t)y);
    wr16(a + 4, (uint16_t)z);
    wr16(a + 6, 0);
    wr16(a + 8, (uint16_t)s);
    wr16(a + 10, (uint16_t)t);
    p = ram_ptr(a + 12);
    p[0] = (uint8_t)c0;
    p[1] = (uint8_t)c1;
    p[2] = (uint8_t)c2;
    p[3] = (uint8_t)c3;
}

/* Light_t: col, pad, colc, pad, dir, pad (+4 bytes of padding of the Light union) */
static uint32_t put_light2(int r, int g, int b, int rc, int gc, int bc, int dx, int dy, int dz) {
    uint32_t a = ram_alloc(16);
    uint8_t* p = ram_ptr(a);

    p[0] = (uint8_t)r, p[1] = (uint8_t)g, p[2] = (uint8_t)b;
    p[4] = (uint8_t)rc, p[5] = (uint8_t)gc, p[6] = (uint8_t)bc;
    p[8] = (uint8_t)dx, p[9] = (uint8_t)dy, p[10] = (uint8_t)dz;
    return a;
}

static uint32_t put_light(int r, int g, int b, int dx, int dy, int dz) {
    return put_light2(r, g, b, r, g, b, dx, dy, dz);
}

/* PointLight_t: col, kc, colc, kl, pos[3] (s16), kq */
static uint32_t put_point_light(int r, int g, int b, int x, int y, int z, int kc, int kl, int kq) {
    uint32_t a = ram_alloc(16);
    uint8_t* p = ram_ptr(a);

    p[0] = (uint8_t)r, p[1] = (uint8_t)g, p[2] = (uint8_t)b, p[3] = (uint8_t)kc;
    p[4] = (uint8_t)r, p[5] = (uint8_t)g, p[6] = (uint8_t)b, p[7] = (uint8_t)kl;
    wr16(a + 8, (uint16_t)x);
    wr16(a + 10, (uint16_t)y);
    wr16(a + 12, (uint16_t)z);
    p[14] = (uint8_t)kq;
    return a;
}

static uint32_t put_viewport(int sx, int sy, int sz, int tx, int ty, int tz) {
    uint32_t a = ram_alloc(16);

    wr16(a, (uint16_t)sx);
    wr16(a + 2, (uint16_t)sy);
    wr16(a + 4, (uint16_t)sz);
    wr16(a + 8, (uint16_t)tx);
    wr16(a + 10, (uint16_t)ty);
    wr16(a + 12, (uint16_t)tz);
    return a;
}

/* Projection and modelview loads, a 320x240 viewport, smooth shading, no culling */
static void setup(Dl* d, const M4 p, const M4 mv) {
    gMtx(d, put_mtx(p), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    gMtx(d, put_mtx(mv), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gViewport(d, put_viewport(640, 480, 511, 640, 480, 511));
    gGeom(d, 0xFFFFFFFF, G_SHADE | G_SHADING_SMOOTH);
}

static void run(Dl* d) {
    mock_reset();
    gfx_rsp_reset();
    gfx_rsp_run(d->start);
}

static bool near_rel(double a, double b, double tol) {
    return fabs(a - b) <= tol * (fabs(b) + 1.0);
}

/* ============================================================================================== */
/* Tests                                                                                          */
/* ============================================================================================== */

static void test_segments(void) {
    Dl d = dl_new(16);
    uint32_t vb = vtx_buf(3);
    M4 id;

    gfx_rsp_reset();
    CHECK(gfx_addr(0) == NULL, "address 0 is NULL");
    CHECK(gfx_addr(0x80001234) == (void*)ram_ptr(0x80001234), "KSEG0");
    CHECK(gfx_addr(0xA0001234) == (void*)ram_ptr(0x80001234), "KSEG1 maps to the cached address");
    gGfxSegments[6] = 0x00100000; // physical base
    gGfxSegments[7] = 0x80200000; // KSEG0 base
    CHECK(gfx_addr(0x06000010) == (void*)ram_ptr(0x80100010), "segment with physical base");
    CHECK(gfx_addr(0x07000020) == (void*)ram_ptr(0x80200020), "segment with KSEG0 base");
    CHECK(gfx_addr(0x00123456) == (void*)ram_ptr(0x80123456), "segment 0 = physical address");

    // G_MW_SEGMENT in a display list, then a vertex load through the segment
    m_identity(id);
    setup(&d, id, id);
    vtx_set(vb, 0, -100, -100, 0, 0, 0, 1, 2, 3, 4);
    vtx_set(vb, 1, 100, -100, 0, 0, 0, 5, 6, 7, 8);
    vtx_set(vb, 2, 0, 100, 0, 0, 0, 9, 10, 11, 12);
    gSegment(&d, 0xB, vb & 0x1FFFFFFF);
    gVtx(&d, 0x0B000000, 3, 0);
    gTri1(&d, 0, 1, 2);
    gEnd(&d);
    run(&d);
    CHECK(gGfxSegments[0xB] == (vb & 0x1FFFFFFF), "G_MW_SEGMENT stored %08X", gGfxSegments[0xB]);
    CHECK(gTriCount == 1 && gTris[0].v[2].r == 9 && gTris[0].v[1].a == 8, "vertices read through segment B");
    gfx_rsp_reset();
    CHECK(gGfxSegments[0xB] == 0, "segments cleared per task");
}

static void test_mtx_parse(void) {
    M4 p = { { 1.5, -2.25, 0.0001, 0.0 },
             { -32767.5, 12345.678, -0.5, 1.0 },
             { 0.0, 0.0, -1.000015, -1.0 },
             { 3.0, -4.0, -20.5, 0.0 } };
    M4 id, pq;
    Dl d = dl_new(16);
    uint32_t vb = vtx_buf(3);
    int i, j;
    bool ok = true;

    m_identity(id);
    m_quantize(pq, p);
    setup(&d, p, id);
    vtx_set(vb, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
    vtx_set(vb, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0);
    vtx_set(vb, 2, 0, 0, 1, 0, 0, 0, 0, 0, 0);
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2);
    gEnd(&d);
    run(&d);

    CHECK(gProjCount == 1, "projection sent once (%d)", gProjCount);
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            if (!near_rel(gProj[i][j], pq[i][j], 1e-6)) {
                ok = false;
                printf("  proj[%d][%d] = %.8f, expected %.8f\n", i, j, gProj[i][j], pq[i][j]);
            }
        }
    }
    CHECK(ok, "Mtx s15.16 parsing (integer and fraction halves, negative values)");
    // Fixed point split of -1.5: integer half 0xFFFE, fraction half 0x8000
    CHECK(fixed(-1.5) == (int32_t)0xFFFE8000, "test encoder sanity");
    // Row i of P is the clip position of unit vector i (MV = identity)
    CHECK(gTriCount == 1, "triangle drawn");
    if (gTriCount == 1) {
        for (i = 0; i < 3; i++) {
            const GfxVtx* v = &gTris[0].v[i];

            CHECK(near_rel(v->x, pq[i][0] + pq[3][0], 1e-6) && near_rel(v->y, pq[i][1] + pq[3][1], 1e-6) &&
                      near_rel(v->z, pq[i][2] + pq[3][2], 1e-6) && near_rel(v->w, pq[i][3] + pq[3][3], 1e-6),
                  "vertex %d = row %d of P + translation", i, i);
        }
    }
}

static void test_matrix_stack(void) {
    M4 p, id, t, s;
    Dl d = dl_new(64);
    uint32_t vb = vtx_buf(3);
    double expect[] = { 1, 11, 12, 11, 1, 1, 7 };
    int i;

    m_scale(p, 0.001, 0.001, 0.001);
    m_identity(id);
    m_translate(t, 10, 0, 0);
    m_scale(s, 2, 2, 2);
    setup(&d, p, id);
    vtx_set(vb, 0, -50, -50, 0, 0, 0, 0, 0, 0, 0);
    vtx_set(vb, 1, 50, -50, 0, 0, 0, 0, 0, 0, 0);
    vtx_set(vb, 2, 1, 1, 0, 0, 0, 0, 0, 0, 0);
#define SAMPLE() (gVtx(&d, vb, 3, 0), gTri1(&d, 0, 1, 2))
    SAMPLE();                                                          // I: 1
    gMtx(&d, put_mtx(t), G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_PUSH);   // T
    SAMPLE();                                                          // 11
    gMtx(&d, put_mtx(s), G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_PUSH);   // S * T
    SAMPLE();                                                          // 2 + 10
    gPopMtx(&d, 1);                                                    // T
    SAMPLE();
    gPopMtx(&d, 5);                                                    // past the base: I
    SAMPLE();
    gPopMtx(&d, 1);                                                    // empty stack: no change
    SAMPLE();
    m_translate(t, 6, 0, 0);
    gMtx(&d, put_mtx(t), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH); // load replaces
    SAMPLE();
#undef SAMPLE
    gEnd(&d);
    run(&d);
    CHECK(gTriCount == 7, "7 samples (%d)", gTriCount);
    for (i = 0; i < 7 && i < gTriCount; i++) {
        double e = expect[i] * fixed(0.001) / 65536.0; // P as the N64 sees it

        CHECK(near_rel(gTris[i].v[2].x, e, 1e-6), "stack sample %d: x %.6f, expected %.6f", i, gTris[i].v[2].x, e);
    }
    CHECK(gProjCount == 1, "projection unchanged by modelview operations (%d)", gProjCount);
}

static void test_clip_coords(void) {
    static const int pts[][3] = {
        { 0, 0, 0 },     { 100, 50, -30 }, { -200, 120, 80 }, { 2000, 0, 0 },     { -1500, 0, 0 },
        { 0, 1500, 0 },  { 0, -1500, 0 },  { 0, 0, 3000 },    { 0, 0, -32000 },   { 300, -250, 32767 },
        { -32768, 0, 0 }, { 7, 3, -1 },
    };
    const int n = (int)(sizeof(pts) / sizeof(pts[0]));
    M4 p, mv, r, s, t, pq, mvq, mvp;
    Dl d = dl_new(64);
    uint32_t vb = vtx_buf(2 + n);
    int i;

    m_perspective(p, 60.0, 4.0 / 3.0, 10.0, 12800.0);
    m_scale(s, 0.5, 0.5, 0.5);
    m_rot_y(r, 30.0);
    m_translate(t, 20.0, -10.0, -500.0);
    m_mul(mv, s, r);
    m_mul(mv, mv, t);
    m_quantize(pq, p);
    m_quantize(mvq, mv);
    m_mul(mvp, mvq, pq);

    setup(&d, p, mv);
    vtx_set(vb, 0, -50, -60, 0, 0, 0, 0, 0, 0, 0);
    vtx_set(vb, 1, 50, -60, 0, 0, 0, 0, 0, 0, 0);
    for (i = 0; i < n; i++) {
        vtx_set(vb, 2 + i, pts[i][0], pts[i][1], pts[i][2], 0, 0, 0, 0, 0, 0);
    }
    gVtx(&d, vb, 2 + n, 0);
    for (i = 0; i < n; i++) {
        gTri1(&d, 0, 1, 2 + i);
    }
    gEnd(&d);
    run(&d);

    CHECK(gTriCount == n, "every test triangle drawn (%d of %d)", gTriCount, n);
    for (i = 0; i < n && i < gTriCount; i++) {
        double c[4], tol;
        const GfxVtx* v = &gTris[i].v[2];

        xform(mvp, pts[i][0], pts[i][1], pts[i][2], c);
        tol = 1e-5 * (fabs(c[0]) + fabs(c[1]) + fabs(c[2]) + fabs(c[3]) + 1.0);
        CHECK(fabs(v->x - c[0]) <= tol && fabs(v->y - c[1]) <= tol && fabs(v->z - c[2]) <= tol &&
                  fabs(v->w - c[3]) <= tol,
              "point %d: clip (%.4f %.4f %.4f %.4f), expected (%.4f %.4f %.4f %.4f)", i, v->x, v->y, v->z, v->w, c[0],
              c[1], c[2], c[3]);
        CHECK(v->clip == ref_clip(c), "point %d: clip codes %02X, expected %02X", i, v->clip, ref_clip(c));
    }
    // The set covers every clip code
    {
        uint8_t all = 0;

        for (i = 0; i < n && i < gTriCount; i++) {
            all |= gTris[i].v[2].clip;
        }
        CHECK(all == (GFX_CLIP_NEG_X | GFX_CLIP_POS_X | GFX_CLIP_NEG_Y | GFX_CLIP_POS_Y | GFX_CLIP_NEG_W | GFX_CLIP_FAR),
              "clip codes covered: %02X", all);
    }
}

static int lit(int amb, int lc, int intensity) {
    int c = amb + ((lc * intensity + 128 - amb) >> 15);

    return (c > 255) ? 255 : c;
}

static void test_directional_lighting(void) {
    M4 p, id, rot;
    Dl d = dl_new(64);
    uint32_t vb = vtx_buf(4), vb2 = vtx_buf(4);
    uint32_t amb = put_light(32, 32, 32, 0, 0, 0);
    const int full = 2 * 127 * 127;

    m_scale(p, 0.001, 0.001, 0.001);
    m_identity(id);
    setup(&d, p, id);
    gGeom(&d, 0, G_LIGHTING);
    gNumLights(&d, 1);
    gLight(&d, put_light2(192, 128, 64, 10, 20, 30, 0, 0, 127), 1);
    gLight(&d, amb, 2);
    // Anchors face away from the light; vertex 2 (even: colc) and 3 (odd: col) face it
    vtx_set(vb, 0, -50, -50, 0, 0, 0, 0, 0, (uint8_t)-127, 255);
    vtx_set(vb, 1, 50, -50, 0, 0, 0, 0, 0, (uint8_t)-127, 255);
    vtx_set(vb, 2, 0, 50, 0, 0, 0, 0, 0, 127, 77);
    vtx_set(vb, 3, 10, 50, 0, 0, 0, 0, 0, 127, 88);
    gVtx(&d, vb, 4, 0);
    gTri1(&d, 0, 1, 2);
    gTri1(&d, 0, 1, 3);
    // Model space rotation: model x maps to world z, so the world +z light lights model +x normals
    m_identity(rot);
    rot[0][0] = 0, rot[0][2] = 1, rot[2][0] = -1, rot[2][2] = 0;
    gMtx(&d, put_mtx(rot), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    // (x, y, z) * rot = (-z, y, x)
    vtx_set(vb2, 0, 0, -50, 50, 0, 0, 0, 0, 0, 255);
    vtx_set(vb2, 1, 0, -50, -50, 0, 0, 0, 0, 0, 255);
    vtx_set(vb2, 2, 0, 50, 0, 0, 0, 127, 0, 0, 255);
    vtx_set(vb2, 3, 0, 50, 10, 0, 0, 0, 0, 127, 255);
    gVtx(&d, vb2, 4, 0);
    gTri1(&d, 0, 1, 2);
    gTri1(&d, 0, 1, 3);
    gEnd(&d);
    run(&d);

    CHECK(gTriCount == 4, "4 lit triangles (%d)", gTriCount);
    if (gTriCount < 4) {
        return;
    }
    // Facing away: ambient only
    CHECK(gTris[0].v[0].r == 32 && gTris[0].v[0].g == 32 && gTris[0].v[0].b == 32 && gTris[0].v[0].a == 255,
          "back-facing normal gets ambient (%d %d %d)", gTris[0].v[0].r, gTris[0].v[0].g, gTris[0].v[0].b);
    // Even vertex: colc (10, 20, 30); odd vertex: col (192, 128, 64)
    CHECK(gTris[0].v[2].r == lit(32, 10, full) && gTris[0].v[2].g == lit(32, 20, full) &&
              gTris[0].v[2].b == lit(32, 30, full) && gTris[0].v[2].a == 77,
          "even vertex lit with colc: %d %d %d a %d", gTris[0].v[2].r, gTris[0].v[2].g, gTris[0].v[2].b, gTris[0].v[2].a);
    CHECK(gTris[1].v[2].r == lit(32, 192, full) && gTris[1].v[2].g == lit(32, 128, full) &&
              gTris[1].v[2].b == lit(32, 64, full) && gTris[1].v[2].a == 88,
          "odd vertex lit with col: %d %d %d a %d (expected %d %d %d)", gTris[1].v[2].r, gTris[1].v[2].g,
          gTris[1].v[2].b, gTris[1].v[2].a, lit(32, 192, full), lit(32, 128, full), lit(32, 64, full));
    CHECK(abs(gTris[1].v[2].r - (32 + 192 * 127 * 127 / 16384)) <= 1, "close to ambient + color * n.d");
    // After the rotation: model +x faces the light, model +z does not
    CHECK(gTris[2].v[2].g == lit(32, 20, full), "rotated light direction lights +x normal (%d)", gTris[2].v[2].g);
    CHECK(gTris[3].v[2].r == 32, "rotated light direction misses +z normal (%d)", gTris[3].v[2].r);
}

static void test_light_refresh(void) {
    M4 p, id;
    Dl d = dl_new(64);
    uint32_t vb = vtx_buf(3);
    const int full = 2 * 127 * 127;

    m_scale(p, 0.001, 0.001, 0.001);
    m_identity(id);
    setup(&d, p, id);
    gGeom(&d, 0, G_LIGHTING);
    gNumLights(&d, 1);
    gLight(&d, put_light(100, 100, 100, 0, 0, 127), 1);
    gLight(&d, put_light(0, 0, 0, 0, 0, 0), 2);
    vtx_set(vb, 0, -50, -50, 0, 0, 0, 0, 0, 0, 255);
    vtx_set(vb, 1, 50, -50, 0, 0, 0, 0, 0, 0, 255);
    vtx_set(vb, 2, 0, 50, 0, 0, 0, 127, 0, 0, 255); // normal +x
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2); // light along +z: dark
    // New direction (+x) and color by G_MOVEMEM only: the color applies, the old direction stays
    gLight(&d, put_light(200, 200, 200, 127, 0, 0), 1);
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2);
    // G_MW_NUMLIGHT invalidates the transformed directions
    gNumLights(&d, 1);
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2);
    // G_MW_LIGHTCOL changes col of light 1 (and the byte after it); vertex 2 is even, so it still uses colc
    gMoveWd(&d, G_MW_LIGHTCOL, G_MWO_aLIGHT_1, 0x40404000);
    gMoveWd(&d, G_MW_LIGHTCOL, G_MWO_bLIGHT_1, 0x50505000);
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2);
    gEnd(&d);
    run(&d);

    CHECK(gTriCount == 4, "4 triangles (%d)", gTriCount);
    if (gTriCount < 4) {
        return;
    }
    CHECK(gTris[0].v[2].r == 0, "light along +z leaves +x normal dark (%d)", gTris[0].v[2].r);
    CHECK(gTris[1].v[2].r == 0, "G_MOVEMEM light does not refresh the transformed direction (%d)", gTris[1].v[2].r);
    CHECK(gTris[2].v[2].r == lit(0, 200, full), "after G_MW_NUMLIGHT the new direction applies (%d)", gTris[2].v[2].r);
    CHECK(gTris[3].v[2].r == lit(0, 0x50, full), "G_MW_LIGHTCOL sets colc (%d)", gTris[3].v[2].r);
}

/* The PosLight formula, written independently of gfx_rsp.c */
static int ref_point(int amb, int lc, const double light[3], const double vtx[3], const int n[3], double scale,
                     int kc, int kl, int kq) {
    double wp[3], l[3], lm[3], k, v = 0.0, att, acc;
    int i, len, q;

    scale = fixed(scale) / 65536.0; // MV as the N64 sees it
    for (i = 0; i < 3; i++) {
        wp[i] = floor(vtx[i] * scale);
        l[i] = fmin(32767.0, fmax(-32768.0, light[i] - wp[i])); // vsub saturates
    }
    k = l[0] * l[0] + l[1] * l[1] + 2.0 * l[2] * l[2];
    for (i = 0; i < 3; i++) {
        lm[i] = floor(l[i] * scale); // MV = scale * identity
        v += n[i] / 128.0 * fmin(1.0, fmax(-1.0, 4.0 * lm[i] / sqrt(k)));
    }
    v = fmin(fmax(v, 0.0), 32767.0 / 32768.0);
    len = (int)sqrt(k);
    q = (len * 16 > 32767) ? 32767 : len * 16;
    acc = kc * 4096.0 + 32768.0 + 2.0 * kl * len + (double)((q * q) >> 16) * kq * 32.0;
    att = fmin(65536.0 / acc, 32767.0 / 32768.0);
    return lit(amb, lc, (int)(v * att * 32768.0 + 0.5)); // vmulf rounds
}

static void test_point_lighting(void) {
    M4 p, mv;
    static const struct {
        int light[3];
        int vtx[3];
        int n[3];
        double scale;
    } cases[] = {
        { { 100, 0, 0 }, { 0, 0, 0 }, { 127, 0, 0 }, 1.0 },     // along x
        { { 0, 0, 100 }, { 0, 0, 0 }, { 0, 0, 127 }, 1.0 },     // along z: counted twice in the distance
        { { 0, 300, 0 }, { 0, 0, 0 }, { 0, 127, 0 }, 1.0 },     // farther
        { { 60, 80, 0 }, { 0, 0, 0 }, { 127, 0, 0 }, 1.0 },     // oblique
        { { 100, 0, 0 }, { 0, 0, 0 }, { -127, 0, 0 }, 1.0 },    // facing away
        { { 100, 0, 0 }, { 0, 0, 0 }, { 127, 0, 0 }, 0.01 },    // small model scale (Link): weak
        { { 500, 40, -20 }, { 1000, -500, 300 }, { 0, 90, 90 }, 0.25 },
    };
    const int n = (int)(sizeof(cases) / sizeof(cases[0]));
    int i;

    m_scale(p, 0.0001, 0.0001, 0.0001);
    for (i = 0; i < n; i++) {
        Dl d = dl_new(32);
        uint32_t vb = vtx_buf(3);
        double light[3] = { cases[i].light[0], cases[i].light[1], cases[i].light[2] };
        double vtx[3] = { cases[i].vtx[0], cases[i].vtx[1], cases[i].vtx[2] };
        int expect;

        m_scale(mv, cases[i].scale, cases[i].scale, cases[i].scale);
        setup(&d, p, mv);
        gGeom(&d, 0, G_LIGHTING | G_LIGHTING_POSITIONAL);
        gNumLights(&d, 1);
        gLight(&d, put_point_light(200, 150, 100, cases[i].light[0], cases[i].light[1], cases[i].light[2], 8, 255, 50), 1);
        gLight(&d, put_light(20, 20, 20, 0, 0, 0), 2);
        vtx_set(vb, 0, -3000, -3000, 0, 0, 0, 0, 0, 0, 255);
        vtx_set(vb, 1, 3000, -3000, 0, 0, 0, 0, 0, 0, 255);
        vtx_set(vb, 2, cases[i].vtx[0], cases[i].vtx[1], cases[i].vtx[2], 0, 0, cases[i].n[0], cases[i].n[1],
                cases[i].n[2], 255);
        gVtx(&d, vb, 3, 0);
        gTri1(&d, 0, 1, 2);
        gEnd(&d);
        run(&d);
        expect = ref_point(20, 200, light, vtx, cases[i].n, cases[i].scale, 8, 255, 50);
        CHECK(gTriCount == 1 && abs(gTris[0].v[2].r - expect) <= 1, "point light case %d: r %d, expected %d", i,
              gTriCount ? gTris[0].v[2].r : -1, expect);
    }

    // A point light without G_LIGHTING_POSITIONAL is treated as directional with its position bytes as the
    // direction; with G_LIGHTING_POSITIONAL, kc == 0 is still directional
    {
        Dl d = dl_new(32);
        uint32_t vb = vtx_buf(3);

        m_identity(mv);
        setup(&d, p, mv);
        gGeom(&d, 0, G_LIGHTING | G_LIGHTING_POSITIONAL);
        gNumLights(&d, 1);
        gLight(&d, put_light(100, 100, 100, 0, 127, 0), 1);
        gLight(&d, put_light(0, 0, 0, 0, 0, 0), 2);
        vtx_set(vb, 0, -3000, -3000, 0, 0, 0, 0, 0, 0, 255);
        vtx_set(vb, 1, 3000, -3000, 0, 0, 0, 0, 0, 0, 255);
        vtx_set(vb, 2, 0, 100, 0, 0, 0, 0, 127, 0, 255);
        gVtx(&d, vb, 3, 0);
        gTri1(&d, 0, 1, 2);
        gEnd(&d);
        run(&d);
        CHECK(gTriCount == 1 && gTris[0].v[2].r == lit(0, 100, 2 * 127 * 127),
              "kc == 0 is directional in positional mode (%d)", gTriCount ? gTris[0].v[2].r : -1);
    }
}

static void test_fog(void) {
    M4 p, id;
    Dl d = dl_new(32);
    uint32_t vb = vtx_buf(6);
    // z/w = z / 1000 with this projection
    static const int zs[] = { 750, 200, 1500, 999, 500 };
    int i;

    m_scale(p, 0.001, 0.001, 0.001);
    m_identity(id);
    setup(&d, p, id);
    gGeom(&d, 0, G_FOG);
    gFog(&d, 256, -128);
    vtx_set(vb, 0, -50, -50, 0, 0, 0, 1, 2, 3, 40);
    vtx_set(vb, 1, 50, -50, 0, 0, 0, 1, 2, 3, 40);
    for (i = 0; i < 4; i++) {
        vtx_set(vb, 2 + i, 0, 50 + i, zs[i], 0, 0, 1, 2, 3, 40);
    }
    gVtx(&d, vb, 6, 0);
    for (i = 0; i < 4; i++) {
        gTri1(&d, 0, 1, 2 + i);
    }
    gEnd(&d);
    run(&d);

    CHECK(gTriCount == 4, "fog triangles (%d)", gTriCount);
    for (i = 0; i < 4 && i < gTriCount; i++) {
        double f = floor(zs[i] * (fixed(0.001) / 65536.0) * 256.0) - 128.0;
        int expect = (f < 0) ? 0 : (f > 255) ? 255 : (int)f;

        CHECK(gTris[i].v[2].a == expect, "fog z/w %.3f: alpha %d, expected %d", zs[i] / 1000.0, gTris[i].v[2].a,
              expect);
        CHECK(gTris[i].v[2].r == 1 && gTris[i].v[2].b == 3, "fog keeps the vertex color");
    }

    // w <= 0: 1/w saturates to +32768, so the sign of z decides
    {
        Dl d2 = dl_new(32);
        uint32_t vb2 = vtx_buf(4);
        M4 pw;

        // clip = (x, y, y, z)
        memset(pw, 0, sizeof(pw));
        pw[0][0] = 1.0, pw[1][1] = 1.0, pw[1][2] = 1.0, pw[2][3] = 1.0;
        setup(&d2, pw, id);
        gGeom(&d2, 0, G_FOG);
        gFog(&d2, 256, 100);
        vtx_set(vb2, 0, 0, 0, 100, 0, 0, 0, 0, 0, 0);
        vtx_set(vb2, 1, 10, 10, 100, 0, 0, 0, 0, 0, 0);
        vtx_set(vb2, 2, 1, -1, -5, 0, 0, 0, 0, 0, 0);
        vtx_set(vb2, 3, 2, 1, -5, 0, 0, 0, 0, 0, 0);
        gVtx(&d2, vb2, 4, 0);
        gTri1(&d2, 0, 1, 2);
        gTri1(&d2, 0, 1, 3);
        gEnd(&d2);
        run(&d2);
        CHECK(gTriCount == 2 && gTris[0].v[2].a == 0 && gTris[1].v[2].a == 255,
              "fog behind the eye saturates by the sign of z (%d %d)", gTriCount ? gTris[0].v[2].a : -1,
              gTriCount > 1 ? gTris[1].v[2].a : -1);
        CHECK(gTriCount == 2 && gTris[0].v[0].a == 100, "fog z = 0 gives the offset (%d)", gTris[0].v[0].a);
    }
}

/* S = 0x4000 + x * 0x44D3 + x^3 * (0x7FFF + 0x6CB3) for x = (dot >> 1) as an s0.15 fraction */
static int ref_texgen_linear(int dot) {
    double x = (double)(dot >> 1) / 32768.0;

    return (int)(32768.0 * (0.5 + x * 17619.0 / 32768.0 + x * x * x * 60594.0 / 32768.0));
}

static void test_texgen(void) {
    M4 p, id;
    Dl d = dl_new(48);
    uint32_t vb = vtx_buf(5);
    int i;

    m_scale(p, 0.001, 0.001, 0.001);
    m_identity(id);
    setup(&d, p, id);
    gGeom(&d, 0, G_LIGHTING | G_TEXTURE_GEN);
    gTexture(&d, 0x0F80, 0x07C0, 0, 0, 1);
    gNumLights(&d, 1);
    gLight(&d, put_light(0, 0, 0, 0, 0, 127), 1);
    gLight(&d, put_light(0, 0, 0, 0, 0, 0), 2);
    gLookAtX(&d, put_light(0, 0, 0, 127, 0, 0));
    gLookAtY(&d, put_light(0, 0, 0, 0, 127, 0));
    vtx_set(vb, 0, -50, -50, 0, 7, 7, 0, 0, 127, 255);
    vtx_set(vb, 1, 50, -50, 0, 7, 7, 0, 0, 127, 255);
    vtx_set(vb, 2, 0, 50, 0, 999, 999, 127, 0, 0, 255);           // along lookat X
    vtx_set(vb, 3, 10, 50, 0, 999, 999, 0, (uint8_t)-127, 0, 255); // against lookat Y
    vtx_set(vb, 4, 20, 50, 0, 999, 999, 0, 0, 127, 255);          // perpendicular to both
    gVtx(&d, vb, 5, 0);
    gTri1(&d, 0, 1, 2);
    gTri1(&d, 0, 1, 3);
    gTri1(&d, 0, 1, 4);
    // Linear texgen with a unit scale
    gGeom(&d, 0, G_TEXTURE_GEN_LINEAR);
    gTexture(&d, 0xFFFF, 0xFFFF, 0, 0, 1);
    gVtx(&d, vb, 5, 0);
    gTri1(&d, 0, 1, 2);
    gTri1(&d, 0, 1, 3);
    gTri1(&d, 0, 1, 4);
    // Texgen needs lighting: without it the vertex ST are used
    gGeom(&d, G_LIGHTING, 0);
    gVtx(&d, vb, 5, 0);
    gTri1(&d, 0, 1, 2);
    gEnd(&d);
    run(&d);

    CHECK(gTriCount == 7, "texgen triangles (%d)", gTriCount);
    if (gTriCount < 7) {
        return;
    }
    // Spherical: S = 0x4000 + n.lookatX, scaled by 0x0F80 (s) / 0x07C0 (t), in texels
    CHECK(gTris[0].v[2].s == 61.5f && gTris[0].v[2].t == 15.5f, "spherical along X: %f %f", gTris[0].v[2].s,
          gTris[0].v[2].t);
    CHECK(gTris[1].v[2].s == 31.0f && gTris[1].v[2].t == (float)(((0x4000 - 16129) * 0x07C0) >> 16) / 32.0f,
          "spherical against Y: %f %f", gTris[1].v[2].s, gTris[1].v[2].t);
    CHECK(gTris[2].v[2].s == 31.0f && gTris[2].v[2].t == 15.5f, "spherical perpendicular: %f %f", gTris[2].v[2].s,
          gTris[2].v[2].t);
    // Linear: the cubic acos approximation; D = 0 -> 0x4000, D = +1 -> near 0x8000, D = -1 -> near 0
    for (i = 3; i < 6; i++) {
        CHECK(gTris[i].v[2].s >= 0.0f && gTris[i].v[2].s < 1024.0f, "linear texgen in range (%f)", gTris[i].v[2].s);
    }
    {
        int sx = (int)(gTris[3].v[2].s * 32.0f) + 1; // undo the 0xFFFF scale's truncation
        int ty = (int)(gTris[4].v[2].t * 32.0f) + 1;
        int s0 = (int)(gTris[5].v[2].s * 32.0f) + 1;
        int e = ref_texgen_linear(2 * 127 * 127);

        CHECK(abs(sx - e) <= 2, "linear along X: S %d, expected %d", sx, e);
        CHECK(abs(ty - (32768 - e)) <= 3, "linear against Y is symmetric: T %d, expected %d", ty, 32768 - e);
        CHECK(s0 == 0x4000, "linear perpendicular: S %d", s0);
    }
    CHECK(gTris[6].v[2].s == (float)((999 * 0xFFFF) >> 16) / 32.0f, "no texgen without lighting (%f)", gTris[6].v[2].s);
}

static void test_texcoords_and_modify(void) {
    M4 p, id;
    Dl d = dl_new(32);
    uint32_t vb = vtx_buf(3);

    m_scale(p, 0.001, 0.001, 0.001);
    m_identity(id);
    setup(&d, p, id);
    gTexture(&d, 0xFFFF, 0x8000, 2, 5, 1);
    vtx_set(vb, 0, -50, -50, 0, 1024, -32, 0, 0, 0, 0);
    vtx_set(vb, 1, 50, -50, 0, 0, 0, 0, 0, 0, 0);
    vtx_set(vb, 2, 0, 50, 0, 64, 64, 0, 0, 0, 0);
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2);
    gModifyVtx(&d, 2, G_MWO_POINT_ST, (64u << 16) | (uint16_t)-32);
    gModifyVtx(&d, 1, G_MWO_POINT_RGBA, 0x11223344);
    gTri1(&d, 0, 1, 2);
    // Screen position: (480, 240) in quarter pixels = (120, 60) pixels with the standard viewport
    gModifyVtx(&d, 2, G_MWO_POINT_XYSCREEN, (480u << 16) | 240u);
    gTri1(&d, 0, 1, 2);
    gEnd(&d);
    run(&d);

    CHECK(gGfxRsp.textureOn == G_ON && gGfxRsp.textureTile == 5 && gGfxRsp.textureLevels == 2 &&
              gGfxRsp.textureScaleS == 65535.0f / 65536.0f && gGfxRsp.textureScaleT == 0.5f,
          "G_TEXTURE fields");
    CHECK(gGfxRdp.dirty & GFX_DIRTY_TEXTURES, "G_TEXTURE marks textures dirty");
    CHECK(gTriCount == 3, "triangles (%d)", gTriCount);
    if (gTriCount < 3) {
        return;
    }
    // s = (1024 * 0xFFFF) >> 16 = 1023 -> 31.96875 texels; t = (-32 * 0x8000) >> 16 = -16 -> -0.5
    CHECK(gTris[0].v[0].s == 1023.0f / 32.0f && gTris[0].v[0].t == -0.5f, "ST scale truncation: %f %f",
          gTris[0].v[0].s, gTris[0].v[0].t);
    CHECK(gTris[0].v[2].s == 63.0f / 32.0f && gTris[0].v[2].t == 1.0f, "ST: %f %f", gTris[0].v[2].s, gTris[0].v[2].t);
    CHECK(gTris[1].v[2].s == 2.0f && gTris[1].v[2].t == -1.0f, "G_MODIFYVTX ST (already scaled): %f %f",
          gTris[1].v[2].s, gTris[1].v[2].t);
    CHECK(gTris[1].v[1].r == 0x11 && gTris[1].v[1].g == 0x22 && gTris[1].v[1].b == 0x33 && gTris[1].v[1].a == 0x44,
          "G_MODIFYVTX RGBA");
    {
        const GfxVtx* v = &gTris[2].v[2];
        double sx = (v->x / v->w) * 160.0 + 160.0, sy = -(v->y / v->w) * 120.0 + 120.0;

        CHECK(fabs(sx - 120.0) < 1e-3 && fabs(sy - 60.0) < 1e-3, "G_MODIFYVTX XYSCREEN: (%f, %f)", sx, sy);
    }
}

static void test_culling(void) {
    M4 p, id, pw;
    uint32_t vb = vtx_buf(4);
    static const uint32_t modes[] = { 0, G_CULL_BACK, G_CULL_FRONT, G_CULL_BOTH };
    // Expected draws of (CCW, CW) per mode
    static const int expect[4][2] = { { 1, 1 }, { 1, 0 }, { 0, 1 }, { 0, 0 } };
    int m;

    m_scale(p, 1.0 / 200, 1.0 / 200, 1.0 / 200);
    m_identity(id);
    vtx_set(vb, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    vtx_set(vb, 1, 100, 0, 0, 0, 0, 0, 0, 0, 0);
    vtx_set(vb, 2, 0, 100, 0, 0, 0, 0, 0, 0, 0);
    vtx_set(vb, 3, 50, 0, 0, 0, 0, 0, 0, 0, 0);
    for (m = 0; m < 4; m++) {
        Dl d = dl_new(32);

        setup(&d, p, id);
        gGeom(&d, 0, modes[m]);
        gVtx(&d, vb, 4, 0);
        gTri1(&d, 0, 1, 2); // counterclockwise with y up: front
        gMarker(&d, 1);
        gTri1(&d, 0, 2, 1); // clockwise: back
        gEnd(&d);
        run(&d);
        CHECK(gTriCount == expect[m][0] + expect[m][1], "cull mode %X: %d drawn", modes[m], gTriCount);
        if (expect[m][0] && gTriCount > 0) {
            CHECK(gTris[0].v[1].x > 0.0f, "cull mode %X: the front triangle was drawn", modes[m]);
        }
    }

    // Mirrored viewport (negative y scale) reverses the winding
    {
        Dl d = dl_new(32);

        setup(&d, p, id);
        gViewport(&d, put_viewport(640, -480, 511, 640, 480, 511));
        gGeom(&d, 0, G_CULL_BACK);
        gVtx(&d, vb, 4, 0);
        gTri1(&d, 0, 1, 2);
        gEnd(&d);
        run(&d);
        CHECK(gTriCount == 0, "mirrored viewport culls the CCW triangle as a back face");
    }

    // Zero area is always culled
    {
        Dl d = dl_new(32);

        setup(&d, p, id);
        gVtx(&d, vb, 4, 0);
        gTri1(&d, 0, 1, 3);
        gEnd(&d);
        run(&d);
        CHECK(gTriCount == 0, "degenerate triangle culled without culling enabled");
    }

    // A triangle crossing the eye plane: winding of its visible part (front here), not of the divided points
    {
        Dl d = dl_new(32);
        uint32_t vb2 = vtx_buf(3);

        memset(pw, 0, sizeof(pw)); // clip = (x, y, z, -z)
        pw[0][0] = 1.0, pw[1][1] = 1.0, pw[2][2] = 1.0, pw[2][3] = -1.0;
        vtx_set(vb2, 0, -5, -1, -10, 0, 0, 0, 0, 0, 0);
        vtx_set(vb2, 1, 5, -1, -10, 0, 0, 0, 0, 0, 0);
        vtx_set(vb2, 2, 0, 5, 10, 0, 0, 0, 0, 0, 0); // behind the eye
        setup(&d, pw, id);
        gGeom(&d, 0, G_CULL_BACK);
        gVtx(&d, vb2, 3, 0);
        gTri1(&d, 0, 1, 2);
        gGeom(&d, G_CULL_BACK, G_CULL_FRONT);
        gTri1(&d, 0, 1, 2);
        gEnd(&d);
        run(&d);
        CHECK(gTriCount == 1 && gTris[0].v[2].w < 0.0f, "triangle through the eye plane is front facing");
    }
}

static void test_reject_flat_tri2(void) {
    M4 p, id;
    Dl d = dl_new(32);
    uint32_t vb = vtx_buf(6);

    m_scale(p, 1.0 / 200, 1.0 / 200, 1.0 / 200);
    m_identity(id);
    setup(&d, p, id);
    vtx_set(vb, 0, 0, 0, 0, 0, 0, 10, 20, 30, 1);
    vtx_set(vb, 1, 100, 0, 0, 0, 0, 40, 50, 60, 2);
    vtx_set(vb, 2, 0, 100, 0, 0, 0, 70, 80, 90, 3);
    vtx_set(vb, 3, 300, 50, 0, 0, 0, 0, 0, 0, 0);  // x > w
    vtx_set(vb, 4, 400, 100, 0, 0, 0, 0, 0, 0, 0); // x > w
    vtx_set(vb, 5, 250, -50, 0, 0, 0, 0, 0, 0, 0); // x > w
    gVtx(&d, vb, 6, 0);
    gTri1(&d, 3, 4, 5); // all right of the screen: rejected
    gTri1(&d, 0, 1, 3); // partly visible: drawn
    gTri2(&d, 0, 1, 2, 2, 1, 0);
    gGeom(&d, G_SHADING_SMOOTH, 0);
    gTri1(&d, 1, 2, 0);
    gEnd(&d);
    run(&d);

    CHECK(gTriCount == 4, "rejection: %d drawn", gTriCount);
    if (gTriCount < 4) {
        return;
    }
    CHECK(gTris[0].v[2].x > 1.0f, "partly visible triangle drawn");
    // G_TRI2 draws the w1 triangle first
    CHECK(gTris[1].v[0].r == 70 && gTris[2].v[0].r == 10, "G_TRI2 order (%d then %d)", gTris[1].v[0].r,
          gTris[2].v[0].r);
    // Flat shading: RGB of the first vertex, alpha per vertex
    CHECK(gTris[3].v[0].r == 40 && gTris[3].v[1].r == 40 && gTris[3].v[2].g == 50 && gTris[3].v[1].a == 3 &&
              gTris[3].v[2].a == 1,
          "flat shading: rgb %d %d %d, alpha %d %d", gTris[3].v[1].r, gTris[3].v[2].r, gTris[3].v[2].g,
          gTris[3].v[1].a, gTris[3].v[2].a);
}

static void test_display_lists(void) {
    Dl m = dl_new(16), a = dl_new(8), b = dl_new(8), c = dl_new(8);
    uint32_t got[40];
    static const uint32_t order[] = { 1, 2, 3, 6, 4, 5 };
    int n, i;

    gMarker(&m, 1);
    gDL(&m, a.start);
    gMarker(&m, 5);
    gEnd(&m);
    gMarker(&a, 2);
    gBranch(&a, b.start);
    gMarker(&a, 99); // skipped: branched away
    gMarker(&b, 3);
    gDL(&b, c.start);
    gMarker(&b, 4);
    gEnd(&b); // returns to the caller of a
    gMarker(&c, 6);
    gEnd(&c);
    run(&m);
    n = markers(got, 32);
    CHECK(n == 6, "markers: %d", n);
    for (i = 0; i < 6 && i < n; i++) {
        CHECK(got[i] == order[i], "call/branch/end order at %d: %u, expected %u", i, got[i], order[i]);
    }

    // 18 nested calls (F3DEX2's depth) through a segment
    {
        Dl lists[18];
        bool ok = true;
        int logs = gLogCount;

        for (i = 0; i < 18; i++) {
            lists[i] = dl_new(4);
        }
        for (i = 0; i < 18; i++) {
            gMarker(&lists[i], 100 + (uint32_t)i);
            if (i < 17) {
                gDL(&lists[i], 0x05000000 | ((lists[i + 1].start - lists[0].start) & 0xFFFFFF));
            }
            gMarker(&lists[i], 200 + (uint32_t)i);
            gEnd(&lists[i]);
        }
        {
            Dl top = dl_new(4);

            gSegment(&top, 5, lists[0].start);
            gDL(&top, 0x05000000);
            gEnd(&top);
            run(&top);
        }
        n = markers(got, 40);
        ok = (n == 36);
        for (i = 0; i < 18 && ok; i++) {
            ok = (got[i] == 100 + (uint32_t)i) && (got[35 - i] == 200 + (uint32_t)i);
        }
        CHECK(ok, "18 nested display list calls (%d markers)", n);
        CHECK(gLogCount == logs, "18 levels do not exceed the N64 stack");
    }
}

static void test_branch_z(void) {
    static const struct {
        int z;
        uint32_t zval;
        uint32_t marker;
    } cases[] = {
        { 1000, 1500, 200 }, { 1000, 500, 100 }, { 1000, 1000, 100 }, { 1000, 1001, 200 },
        { 1000, 0x17D78400, 200 }, { -5, 0, 200 },
    };
    M4 p, id;
    int i;

    // clip w = model z
    memset(p, 0, sizeof(p));
    p[0][0] = 1.0, p[1][1] = 1.0, p[2][3] = 1.0;
    m_identity(id);
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        Dl d = dl_new(16), x = dl_new(4);
        uint32_t vb = vtx_buf(1);
        uint32_t got[4];
        int n;

        vtx_set(vb, 0, 0, 0, cases[i].z, 0, 0, 0, 0, 0, 0);
        setup(&d, p, id);
        gVtx(&d, vb, 1, 0);
        gBranchZraw(&d, x.start, 0, cases[i].zval);
        gMarker(&d, 100);
        gEnd(&d);
        gMarker(&x, 200);
        gEnd(&x);
        run(&d);
        n = markers(got, 4);
        CHECK(n == 1 && got[0] == cases[i].marker, "G_BRANCH_Z w %d vs %u: marker %u, expected %u", cases[i].z,
              cases[i].zval, n ? got[0] : 0, cases[i].marker);
    }
}

static void test_cull_dl(void) {
    M4 p, id;
    int visible;

    m_scale(p, 1.0 / 200, 1.0 / 200, 1.0 / 200);
    m_identity(id);
    for (visible = 0; visible < 2; visible++) {
        Dl m = dl_new(16), a = dl_new(8);
        uint32_t vb = vtx_buf(4);
        uint32_t got[4];
        int n, i;

        for (i = 0; i < 4; i++) {
            // all left of the screen, or one corner inside
            vtx_set(vb, i, (visible && i == 3) ? 0 : -300 - i * 10, i * 10, 0, 0, 0, 0, 0, 0, 0);
        }
        setup(&m, p, id);
        gDL(&m, a.start);
        gMarker(&m, 9);
        gEnd(&m);
        gVtx(&a, vb, 4, 0);
        gCullDL(&a, 0, 3);
        gMarker(&a, 8);
        gEnd(&a);
        run(&m);
        n = markers(got, 4);
        if (visible) {
            CHECK(n == 2 && got[0] == 8 && got[1] == 9, "G_CULLDL keeps a visible display list");
        } else {
            CHECK(n == 1 && got[0] == 9, "G_CULLDL ends an invisible display list like G_ENDDL");
        }
    }
}

static void test_texrect_and_rdp(void) {
    Dl d = dl_new(16);
    uint32_t got[4];

    gTexRect(&d, false, 40, 44, 400, 404, 3, 0x0120, 0x0340, 0x0400, 0x0200);
    gTexRect(&d, true, 4, 8, 12, 16, 1, 0, 0, 0x1000, 0x1000);
    op(&d, CMD(G_SETOTHERMODE_L) | (0 << 8) | 1, 0x12345678);
    op(&d, CMD(G_SETOTHERMODE_H) | (20 << 8) | 1, 0x00300000);
    gMarker(&d, 0xAABBCCDD);
    op(&d, CMD(G_FILLRECT) | (100 << 12) | 80, (20 << 12) | 10);
    op(&d, CMD(G_RDPFULLSYNC), 0);
    gEnd(&d);
    run(&d);

    CHECK(gTexrectCount == 2, "texrects: %d", gTexrectCount);
    CHECK(gTexrects[0].w0 == (CMD(G_TEXRECT) | (400 << 12) | 404) && gTexrects[0].w1 == ((3u << 24) | (40 << 12) | 44) &&
              gTexrects[0].half1 == 0x01200340 && gTexrects[0].half2 == 0x04000200 && !gTexrects[0].flip,
          "G_TEXRECT words");
    CHECK(gTexrects[1].flip && gTexrects[1].half2 == 0x10001000, "G_TEXRECTFLIP");
    CHECK(gRdpCount == 5 && (gRdp[0].w0 >> 24) == G_SETOTHERMODE_L && (gRdp[1].w0 >> 24) == G_SETOTHERMODE_H &&
              (gRdp[3].w0 >> 24) == G_FILLRECT && (gRdp[4].w0 >> 24) == G_RDPFULLSYNC,
          "RDP commands forwarded in order (%d)", gRdpCount);
    CHECK(markers(got, 4) == 1 && got[0] == 0xAABBCCDD, "RDP command words unchanged");
}

static void test_force_and_lazy_projection(void) {
    M4 id, f, fq, p1, p2;
    Dl d = dl_new(64);
    uint32_t vb = vtx_buf(3);
    bool ok;
    int i, j;

    m_identity(id);
    m_scale(f, 0.5, 0.25, 0.125);
    f[3][0] = 0.25;
    f[0][3] = 0.001; // a combined matrix that is not a plain projection
    m_quantize(fq, f);
    m_scale(p1, 0.01, 0.01, 0.01);
    m_scale(p2, 0.02, 0.02, 0.02);
    vtx_set(vb, 0, -10, -10, 0, 0, 0, 0, 0, 0, 0);
    vtx_set(vb, 1, 10, -10, 0, 0, 0, 0, 0, 0, 0);
    vtx_set(vb, 2, 4, 8, 2, 0, 0, 0, 0, 0, 0);

    setup(&d, p1, id);
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2);                                                 // 0: P1 sent
    gMtx(&d, put_mtx(p1), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH); // same values again
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2);                                                 // 1: not sent again
    gMtx(&d, put_mtx(p2), G_MTX_PROJECTION | G_MTX_LOAD | G_MTX_NOPUSH);
    gTri1(&d, 0, 1, 2);                                                 // 2: old vertices, P1 still current
    gMarker(&d, 1);
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2);                                                 // 3: P2 sent
    gForceMtx(&d, put_mtx(f));
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2);                                                 // 4: forced matrix
    gMtx(&d, put_mtx(id), G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2);                                                 // 5: MV * P2 again
    gEnd(&d);
    run(&d);

    CHECK(gTriCount == 6, "triangles (%d)", gTriCount);
    CHECK(gProjCount == 4, "projection sent 4 times: P1, P2, forced, P2 (%d)", gProjCount);
    if (gTriCount < 6) {
        return;
    }
    CHECK(near_rel(gTris[2].v[2].x, 0.04, 1e-4), "old vertices keep their transform (%f)", gTris[2].v[2].x);
    CHECK(near_rel(gTris[3].v[2].x, 0.08, 1e-4), "new projection used (%f)", gTris[3].v[2].x);
    {
        double c[4];

        xform(fq, 4, 8, 2, c);
        CHECK(near_rel(gTris[4].v[2].x, c[0], 1e-5) && near_rel(gTris[4].v[2].w, c[3], 1e-5),
              "forced MVP transforms vertices (%f %f, expected %f %f)", gTris[4].v[2].x, gTris[4].v[2].w, c[0], c[3]);
    }
    CHECK(near_rel(gTris[5].v[2].x, 0.08, 1e-4), "G_MTX after a forced matrix recomputes MV * P (%f)",
          gTris[5].v[2].x);
    // The last projection sent is P2; the forced one was sent before it
    ok = true;
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            ok = ok && near_rel(gProj[i][j], (i == j) ? ((i == 3) ? 1.0 : 0.02) : 0.0, 1e-4);
        }
    }
    CHECK(ok, "projection back to P2");
}

static void test_s2dex_switch(void) {
    M4 p, id, bad;
    Dl d = dl_new(64);
    uint32_t vb = vtx_buf(3);
    uint32_t got[4];
    int logs;

    m_scale(p, 0.01, 0.01, 0.01);
    m_identity(id);
    m_scale(bad, 50, 50, 50);
    vtx_set(vb, 0, -10, -10, 0, 0, 0, 0, 0, 127, 255);
    vtx_set(vb, 1, 10, -10, 0, 0, 0, 0, 0, 127, 255);
    vtx_set(vb, 2, 4, 8, 0, 0, 0, 0, 0, 127, 255);
    setup(&d, p, id);
    gGeom(&d, 0, G_ZBUFFER | G_FOG | G_LIGHTING);
    gTexture(&d, 0x8000, 0x8000, 0, 0, 1);
    gFog(&d, 256, -128);
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2);
    gLoadUcode(&d, ucode_addr(gspS2DEX2_fifoTextStart) & 0x1FFFFFFF, 0); // as gSPLoadUcodeL (physical)
    op(&d, CMD(0x0A), 0x80001000);                                         // G_BG_COPY: skipped
    op(&d, CMD(0xDA), put_mtx(bad));                                       // G_OBJ_RECTANGLE_R (= G_MTX in F3DEX2)
    op(&d, CMD(0x01) | (3 << 12) | (3 << 1), 0x80001000);                  // G_OBJ_RECTANGLE (= G_VTX)
    op(&d, CMD(G_TEXRECT), 0);                                             // G_RDPHALF_0 in S2DEX2
    gMarker(&d, 0x51);                                                     // RDP: passed through
    gSegment(&d, 9, 0x00123400);                                           // shared: applied
    gLoadUcode(&d, ucode_addr(gspF3DZEX2_NoN_PosLight_fifoTextStart), 0);
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2);
    gEnd(&d);
    logs = gLogCount;
    run(&d);

    CHECK(gTriCount == 2, "triangles before and after S2DEX2 (%d)", gTriCount);
    CHECK(markers(got, 4) == 1 && got[0] == 0x51, "RDP commands pass through while S2DEX2 is loaded");
    CHECK(gTexrectCount == 0, "S2DEX2's G_RDPHALF_0 is not a texture rectangle");
    CHECK(gGfxSegments[9] == 0x00123400, "segments set under S2DEX2");
    CHECK(gGfxRsp.geometryMode == G_CLIPPING, "F3DZEX2 reload resets the geometry mode (%08X)", gGfxRsp.geometryMode);
    CHECK(gGfxRsp.textureOn == G_OFF && gGfxRsp.textureScaleS == 0.0f && gGfxRsp.fogMultiplier == 0 &&
              gGfxRsp.fogOffset == 0,
          "F3DZEX2 reload resets G_TEXTURE and fog");
    CHECK(gLogCount > logs, "skipped S2DEX2 commands are logged");
    if (gTriCount == 2) {
        CHECK(gTris[1].v[2].x == gTris[0].v[2].x && gTris[1].v[2].w == gTris[0].v[2].w,
              "matrices survive the microcode switch (%f vs %f)", gTris[1].v[2].x, gTris[0].v[2].x);
        CHECK(gTris[1].v[2].b == 127 && gTris[1].v[2].a == 255, "lighting and fog off after the reload");
    }
}

static void test_state_words(void) {
    Dl d = dl_new(16);

    gfx_rsp_reset();
    CHECK(gGfxRsp.geometryMode == G_CLIPPING, "initial geometry mode is the microcode's G_CLIPPING");
    gGeom(&d, 0, G_ZBUFFER | G_SHADE);
    gViewport(&d, put_viewport(640, 480, 511, 640, 480, 511));
    gFog(&d, 0x1234, -77);
    gEnd(&d);
    run(&d);
    CHECK(gGfxRsp.geometryMode == (G_CLIPPING | G_ZBUFFER | G_SHADE), "set mode (%08X)", gGfxRsp.geometryMode);
    CHECK((gGfxRdp.dirty & (GFX_DIRTY_GEOMETRY | GFX_DIRTY_VIEWPORT)) == (GFX_DIRTY_GEOMETRY | GFX_DIRTY_VIEWPORT),
          "geometry and viewport dirty");
    CHECK(gGfxRsp.viewportScale[0] == 640 && gGfxRsp.viewportScale[1] == 480 && gGfxRsp.viewportScale[2] == 511 &&
              gGfxRsp.viewportTrans[0] == 640 && gGfxRsp.viewportTrans[2] == 511,
          "viewport parsed");
    CHECK(gGfxRsp.fogMultiplier == 0x1234 && gGfxRsp.fogOffset == -77, "fog factor");

    // Same viewport again: not dirty. Clear G_SHADE and load a whole mode (clear mask -1).
    {
        Dl e = dl_new(16);
        uint32_t vp = put_viewport(640, 480, 511, 640, 480, 511);

        gViewport(&e, vp);
        gGeom(&e, G_ZBUFFER | G_SHADE, G_FOG);
        gEnd(&e);
        mock_reset();
        gfx_rsp_reset();
        gGfxRsp.viewportScale[0] = 640, gGfxRsp.viewportScale[1] = 480, gGfxRsp.viewportScale[2] = 511;
        gGfxRsp.viewportTrans[0] = 640, gGfxRsp.viewportTrans[1] = 480, gGfxRsp.viewportTrans[2] = 511;
        gfx_rsp_run(e.start);
        CHECK(!(gGfxRdp.dirty & GFX_DIRTY_VIEWPORT), "unchanged viewport is not dirty");
        CHECK(gGfxRsp.geometryMode == (G_CLIPPING | G_FOG), "clear and set (%08X)", gGfxRsp.geometryMode);
    }
    {
        Dl e = dl_new(16);

        gGeom(&e, 0xFFFFFFFF, G_ZBUFFER | G_CULL_BACK);
        gEnd(&e);
        run(&e);
        CHECK(gGfxRsp.geometryMode == (G_ZBUFFER | G_CULL_BACK), "load mode (%08X)", gGfxRsp.geometryMode);
    }
}

static void test_vertex_range(void) {
    M4 p, id;
    Dl d = dl_new(16);
    uint32_t vb = vtx_buf(8);
    int logs, i;

    m_scale(p, 0.001, 0.001, 0.001);
    m_identity(id);
    for (i = 0; i < 8; i++) {
        vtx_set(vb, i, i * 10, (i & 1) * 10, 0, 0, 0, i, 0, 0, 0);
    }
    setup(&d, p, id);
    gVtx(&d, vb, 8, 28); // 28..35: only 28..31 fit
    gTri1(&d, 28, 29, 30);
    gTri1(&d, 30, 31, 33); // index past the buffer: skipped
    gEnd(&d);
    logs = gLogCount;
    run(&d);
    CHECK(gTriCount == 1 && gTris[0].v[0].r == 0 && gTris[0].v[2].r == 2, "G_VTX clamped to the buffer");
    CHECK(gLogCount == logs + 2, "range problems logged (%d)", gLogCount - logs);
}

static void test_logging(void) {
    Dl d = dl_new(16);
    int before;

    op(&d, CMD(0x50), 1);
    op(&d, CMD(0x50), 2);
    op(&d, CMD(0x51), 3);
    op(&d, CMD(G_LINE3D), 0);
    op(&d, CMD(G_LINE3D), 0);
    gEnd(&d);
    before = gLogCount;
    run(&d);
    CHECK(gLogCount - before == 3, "unknown opcodes and G_LINE3D are logged once each (%d)", gLogCount - before);
    before = gLogCount;
    run(&d);
    CHECK(gLogCount == before, "and not again in the next task");
}

/* Normalized light and lookat directions are stored as the microcode stores them: the top byte of
 * 32768 * d / |d|, i.e. floor(128 * d / |d|) saturated to s8. Against an axis that is -128 (not -127);
 * (30, 60, 100) becomes (31, 63, 106). */
static void test_light_quantization(void) {
    M4 p, id;
    Dl d = dl_new(48);
    uint32_t vb = vtx_buf(4);
    static const int dir[3] = { 31, 63, 106 };
    int i;

    m_scale(p, 0.001, 0.001, 0.001);
    m_identity(id);
    setup(&d, p, id);
    gGeom(&d, 0, G_LIGHTING | G_TEXTURE_GEN);
    gTexture(&d, 0x8000, 0x8000, 0, 0, 1);
    gNumLights(&d, 1);
    gLight(&d, put_light(100, 100, 100, 0, 0, -127), 1);
    gLight(&d, put_light(0, 0, 0, 0, 0, 0), 2);
    gLookAtX(&d, put_light(0, 0, 0, 30, 60, 100));
    gLookAtY(&d, put_light(0, 0, 0, 0, 127, 0));
    vtx_set(vb, 0, -50, -50, 0, 0, 0, 0, 0, -127, 255); // faces the light
    vtx_set(vb, 1, 50, -50, 0, 0, 0, 127, 0, 0, 255);
    vtx_set(vb, 2, 0, 50, 0, 0, 0, 0, 127, 0, 255);
    vtx_set(vb, 3, 10, 50, 0, 0, 0, 0, 0, 127, 255);
    gVtx(&d, vb, 4, 0);
    gTri1(&d, 0, 1, 2);
    gTri1(&d, 3, 1, 2);
    gEnd(&d);
    run(&d);

    CHECK(gTriCount == 2, "triangles (%d)", gTriCount);
    if (gTriCount < 2) {
        return;
    }
    CHECK(gTris[0].v[0].r == lit(0, 100, 2 * 127 * 128), "light against the z axis is -128: r %d, expected %d",
          gTris[0].v[0].r, lit(0, 100, 2 * 127 * 128));
    for (i = 0; i < 3; i++) {
        const GfxVtx* v = (i == 0) ? &gTris[0].v[1] : (i == 1) ? &gTris[0].v[2] : &gTris[1].v[0];
        int sExp = 0x4000 + 127 * dir[i]; // 0x4000 + (2 * n . d) / 2
        float s = (float)((sExp * 0x8000) >> 16) / 32.0f;

        CHECK(v->s == s, "lookat (30, 60, 100) component %d: s %f, expected %f (direction %d)", i, v->s, s, dir[i]);
    }
}

/* G_MW_PERSPNORM writes a word that starts 2 bytes before perspNorm, in the low half of the G_RDPHALF_1
 * value; a texture rectangle sent after it gets that half cleared, as on the RSP */
static void test_perspnorm_shares_half1(void) {
    Dl d = dl_new(16);

    op(&d, CMD(G_TEXRECT) | (400 << 12) | 404, (40 << 12) | 44);
    op(&d, CMD(G_RDPHALF_1), 0x12345678);
    gMoveWd(&d, G_MW_PERSPNORM, 0, 0xFFFF);
    op(&d, CMD(G_RDPHALF_2), 0x04000400);
    gEnd(&d);
    run(&d);
    CHECK(gTexrectCount == 1 && gTexrects[0].half1 == 0x12340000, "texrect after G_MW_PERSPNORM: half1 %08X",
          gTexrectCount ? gTexrects[0].half1 : 0);
}

/* Matrix products saturate to s15.16 like the microcode's mtx_multiply (and stay finite) */
static void test_matrix_saturation(void) {
    M4 big, id;
    Dl d = dl_new(16);
    uint32_t vb = vtx_buf(3);

    m_scale(big, 200, 200, 200);
    m_identity(id);
    setup(&d, id, big);
    gMtx(&d, put_mtx(big), G_MTX_MODELVIEW | G_MTX_MUL | G_MTX_NOPUSH); // 40000: saturates
    vtx_set(vb, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    vtx_set(vb, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0);
    vtx_set(vb, 2, 0, 1, 0, 0, 0, 0, 0, 0, 0);
    gVtx(&d, vb, 3, 0);
    gTri1(&d, 0, 1, 2);
    gEnd(&d);
    run(&d);
    CHECK(gTriCount == 1 && fabs(gTris[0].v[1].x - 32768.0) < 0.01 && fabs(gTris[0].v[2].y - 32768.0) < 0.01,
          "MV * MV saturates at 32768: %f", gTriCount ? gTris[0].v[1].x : 0.0f);
}

/* Bad addresses in display lists: calls and branches outside RAM are skipped (the list goes on), loads from
 * outside RAM are ignored, a task starting outside RAM does nothing, a branch to itself stops at the
 * command limit */
static void test_bad_pointers(void) {
    M4 id;
    Dl m = dl_new(32), loop = dl_new(4);
    uint32_t got[4];
    int logs;

    m_identity(id);
    gMarker(&m, 1);
    gDL(&m, 0x81F00000);                                         // past MEM1
    gDL(&m, 0);                                                  // null
    gSegment(&m, 0xD, 0x017FFFF0);
    gDL(&m, 0x0D000100);                                         // segment base + offset past RAM
    gBranch(&m, 0xDEAD0000);                                     // garbage
    gBranchZraw(&m, 0xC1F00000, 0, 0x7FFFFFFF);                  // taken, to garbage
    gVtx(&m, 0x817FFFF0, 4, 0);                                  // straddles the end of RAM
    gMtx(&m, 0x817FFFE0, G_MTX_MODELVIEW | G_MTX_LOAD | G_MTX_NOPUSH);
    gViewport(&m, 0x817FFFF8);
    gLight(&m, 0, 1);
    gMoveWd(&m, G_MW_SEGMENT, 0x40, 0x00123456); // past the segment table: ignored
    gMarker(&m, 2);
    gEnd(&m);
    run(&m);
    CHECK(markers(got, 4) == 2 && got[0] == 1 && got[1] == 2, "bad calls and branches are skipped");
    CHECK(gNonFinite == 0, "no NaN reached GX");
    CHECK(gGfxSegments[0] == 0 && gGfxSegments[0xD] == 0x017FFFF0, "G_MW_SEGMENT past the table changes no segment");

    mock_reset();
    gfx_rsp_reset();
    gfx_rsp_run(0x81F00000);
    gfx_rsp_run(0);
    CHECK(gRdpCount == 0 && gTriCount == 0, "tasks from bad addresses do nothing");

    gBranch(&loop, loop.start);
    logs = gLogCount;
    run(&loop);
    CHECK(gLogCount == logs + 1 && strstr(gLastLog, "commands in a task") != NULL, "runaway list stopped: %s",
          gLastLog);
}

/* Random display lists over random data: no crash, no sanitizer report, no hang, nothing non-finite
 * reaches GX. Every run is bounded: lists branch only forward, call only leaf lists (which neither call
 * nor branch), G_BRANCH_Z always follows a G_RDPHALF_1 with a forward target, and stray jumps land on
 * G_ENDDL filler. */
static uint32_t sRand = 0x12345678;

static uint32_t rnd(void) {
    sRand ^= sRand << 13;
    sRand ^= sRand >> 17;
    sRand ^= sRand << 5;
    return sRand;
}

/* Mtx at a given address */
static void put_mtx_at(uint32_t a, const M4 m) {
    int i;

    for (i = 0; i < 16; i++) {
        int32_t v = fixed(m[i / 4][i % 4]);

        wr16(a + 2 * i, (uint16_t)((uint32_t)v >> 16));
        wr16(a + 32 + 2 * i, (uint16_t)v);
    }
}

/* Triangle indices: mostly inside the vertex buffer, sometimes any byte */
static uint32_t fuzz_tri(void) {
    if ((rnd() & 15) == 0) {
        return rnd() & 0xFFFFFF;
    }
    return tri_word((int)(rnd() % 32), (int)(rnd() % 32), (int)(rnd() % 32));
}

static void test_fuzz(void) {
    enum { LISTS = 48, LEAVES = 12, CMDS = 64, DATA = 64 * 1024, RUNS = 5000, SANE_MTX = 8, SANE_VTX = 256 };
    static const uint8_t ops[] = {
        G_VTX,          G_VTX,    G_VTX,       G_MODIFYVTX,  G_CULLDL,         G_BRANCH_Z,       G_TRI1,
        G_TRI1,         G_TRI1,   G_TRI2,      G_TRI2,       G_QUAD,           G_LINE3D,         G_TEXTURE,
        G_POPMTX,       G_GEOMETRYMODE, G_MTX, G_MTX,        G_MOVEWORD,       G_MOVEWORD,       G_MOVEMEM,
        G_MOVEMEM,      G_LOAD_UCODE, G_DL,    G_DL,         G_SPNOOP,         G_RDPHALF_1,      G_TEXRECT,
        G_RDPHALF_2,    G_SETOTHERMODE_L, G_SETOTHERMODE_H, G_SETPRIMCOLOR, G_SPNOOP, G_DMA_IO, G_SPECIAL_1,
        0x0A,           0xDA,     0xE4,        0x55,
    };
    static const uint32_t geomFlags[] = {
        G_ZBUFFER, G_SHADE, G_CULL_FRONT, G_CULL_BACK, G_FOG, G_LIGHTING, G_TEXTURE_GEN, G_TEXTURE_GEN_LINEAR,
        G_SHADING_SMOOTH, G_LIGHTING_POSITIONAL, G_CLIPPING,
    };
    static const uint8_t mvIdx[] = { G_MV_VIEWPORT, G_MV_LIGHT, G_MV_LIGHT, G_MV_MMTX, G_MV_PMTX,
                                     G_MV_MATRIX,   0,          4,          12,        0x2A };
    static const uint8_t mwIdx[] = { G_MW_MATRIX,   G_MW_NUMLIGHT, G_MW_CLIP,      G_MW_SEGMENT, G_MW_FOG,
                                     G_MW_LIGHTCOL, G_MW_FORCEMTX, G_MW_PERSPNORM, 0x58 };
    static const uint8_t modOfs[] = { G_MWO_POINT_RGBA, G_MWO_POINT_ST, G_MWO_POINT_XYSCREEN, G_MWO_POINT_ZSCREEN,
                                      0x20 };
    uint32_t lists[LISTS], data, saneMtx, saneVtx, callAt;
    Dl top;
    M4 persp, mv;
    uint8_t* p;
    int i, j, run, logsBefore = gLogCount;
    clock_t c0;
    double secs;
    long tris = 0, rdp = 0, rects = 0;

    if (getenv("GFX_FUZZ_SEED") != NULL) { // other seeds: GFX_FUZZ_SEED=n make -C port/gc/tests/gfx_host run
        sRand = (uint32_t)strtoul(getenv("GFX_FUZZ_SEED"), NULL, 0) | 1;
    }
    // G_ENDDL everywhere past the first MiB (test data lives below), then random data and lists
    for (i = 1 << 20; i < (int)0x01800000; i += 8) {
        wr32(0x80000000u + (uint32_t)i, 0xDF000000u);
        wr32(0x80000000u + (uint32_t)i + 4, 0);
    }
    ram_reset();
    data = ram_alloc(DATA);
    p = ram_ptr(data);
    for (i = 0; i < DATA; i++) {
        p[i] = (uint8_t)rnd();
    }
    // Some well-formed matrices (small scales, perspective) and vertices near the origin, so that triangles
    // get through rejection and culling
    saneMtx = data;
    saneVtx = data + SANE_MTX * 64;
    for (i = 0; i < SANE_MTX; i++) {
        M4 m;

        if (i & 1) {
            m_perspective(m, 30.0 + 10 * i, 4.0 / 3.0, 5.0 + i, 5000.0);
        } else {
            m_scale(m, 0.002 * (i + 1), 0.002 * (i + 1), 0.002 * (i + 1));
            m[3][2] = -100.0 * i;
        }
        put_mtx_at(saneMtx + (uint32_t)i * 64, m);
    }
    for (i = 0; i < SANE_VTX; i++) {
        uint32_t a = saneVtx + (uint32_t)i * 16;

        wr16(a, (uint16_t)(int16_t)((int)(rnd() % 801) - 400));
        wr16(a + 2, (uint16_t)(int16_t)((int)(rnd() % 801) - 400));
        wr16(a + 4, (uint16_t)(int16_t)((int)(rnd() % 801) - 400));
    }
    for (i = 0; i < LISTS; i++) {
        lists[i] = ram_alloc(CMDS * 8);
    }
    for (i = 0; i < LISTS; i++) {
        const bool leaf = (i >= LISTS - LEAVES);

        for (j = 0; j < CMDS; j++) {
            uint32_t a = lists[i] + (uint32_t)j * 8;
            uint32_t op8 = ops[rnd() % sizeof(ops)];
            uint32_t w0 = (op8 << 24) | (rnd() & 0xFFFFFF);
            uint32_t r = rnd() % 8;
            // Data: into the random data (KSEG0 or through a segment), or anything
            uint32_t w1 = (r < 4) ? data + (rnd() % DATA) : (r < 6) ? ((rnd() % 16) << 24) | (rnd() % DATA) : rnd();
            // Branch targets: a later list (never from a leaf), or a bad / null address
            uint32_t fwd = (!leaf && r < 6) ? lists[i + 1 + (int)(rnd() % (uint32_t)(LISTS - i - 1))]
                                            : (r == 6) ? 0 : 0xDEAD0000u;
            bool any = (rnd() % 8) == 0; // keep the raw random command word

            if (j == CMDS - 1) {
                op8 = G_ENDDL, w0 = CMD(G_ENDDL), any = false;
            } else if (op8 == G_BRANCH_Z && j == CMDS - 2) {
                op8 = G_SPNOOP, w0 = CMD(G_SPNOOP), any = false;
            }
            switch (op8) {
                case G_VTX: {
                    int n = 1 + (int)(rnd() % 32), v0 = (int)(rnd() % (uint32_t)(33 - n));

                    if (!any) {
                        w0 = CMD(G_VTX) | ((uint32_t)n << 12) | ((uint32_t)(v0 + n) << 1);
                    }
                    if (r < 5) {
                        w1 = saneVtx + (rnd() % (uint32_t)(SANE_VTX - n)) * 16;
                    }
                    break;
                }
                case G_TRI1:
                    w0 = CMD(G_TRI1) | fuzz_tri();
                    break;
                case G_TRI2:
                case G_QUAD:
                    w0 = (op8 << 24) | fuzz_tri();
                    w1 = fuzz_tri();
                    break;
                case G_MODIFYVTX:
                    if (!any) {
                        w0 = CMD(G_MODIFYVTX) | ((uint32_t)modOfs[rnd() % sizeof(modOfs)] << 16) | ((rnd() % 32) * 2);
                    }
                    w1 = rnd();
                    break;
                case G_CULLDL:
                    if (!any) {
                        w0 = CMD(G_CULLDL) | ((rnd() % 32) * 2);
                        w1 = (rnd() % 32) * 2;
                    }
                    break;
                case G_GEOMETRYMODE: {
                    uint32_t clr = 0, set = 0;
                    int k;

                    for (k = 0; k < (int)(sizeof(geomFlags) / sizeof(geomFlags[0])); k++) {
                        clr |= (rnd() & 1) ? geomFlags[k] : 0;
                        set |= ((rnd() % 3) == 0) ? geomFlags[k] : 0;
                    }
                    if (!any) {
                        w0 = CMD(G_GEOMETRYMODE) | (~clr & 0xFFFFFF);
                        w1 = set & ~(uint32_t)G_CULL_FRONT; // keep most triangles
                    }
                    break;
                }
                case G_MTX:
                    if (!any) {
                        w0 = CMD(G_MTX) | (((64 - 1) / 8) << 19) | (rnd() & 7);
                    }
                    if (r < 5) {
                        w1 = saneMtx + (rnd() % SANE_MTX) * 64;
                    }
                    break;
                case G_POPMTX:
                    w1 = (rnd() % 4) * 64;
                    break;
                case G_MOVEMEM:
                    if (!any) {
                        w0 = CMD(G_MOVEMEM) | ((rnd() % 32) << 19) | ((rnd() % 32) << 8) | mvIdx[rnd() % sizeof(mvIdx)];
                    }
                    if ((w0 & 0xFE) == G_MV_MATRIX || (w0 & 0xFE) == G_MV_MMTX || (w0 & 0xFE) == G_MV_PMTX) {
                        w1 = saneMtx + (rnd() % SANE_MTX) * 64;
                    }
                    break;
                case G_MOVEWORD:
                    if (r < 3) { // segments into the random data
                        w0 = CMD(G_MOVEWORD) | ((uint32_t)G_MW_SEGMENT << 16) | ((rnd() % 16) * 4);
                        w1 = (data & 0x1FFFFFFF) + (rnd() % DATA);
                    } else {
                        if (!any) {
                            w0 = CMD(G_MOVEWORD) | ((uint32_t)mwIdx[rnd() % sizeof(mwIdx)] << 16) |
                                 ((rnd() & 3) ? (rnd() % 64) * 4 : rnd() % 0x10000);
                        }
                        w1 = rnd();
                    }
                    break;
                case G_DL:
                    if (leaf) {
                        w0 = CMD(G_SPNOOP), w1 = 0;
                    } else if (rnd() & 1) { // call a leaf
                        w0 = CMD(G_DL) | ((uint32_t)G_DL_PUSH << 16);
                        w1 = lists[LISTS - LEAVES + (int)(rnd() % LEAVES)];
                    } else {
                        w0 = CMD(G_DL) | ((uint32_t)G_DL_NOPUSH << 16);
                        w1 = fwd;
                    }
                    break;
                case G_BRANCH_Z:
                    wr32(a, CMD(G_RDPHALF_1));
                    wr32(a + 4, fwd);
                    a += 8, j++;
                    w0 = CMD(G_BRANCH_Z) | ((rnd() % 32) * 2);
                    w1 = rnd() % 0x8000;
                    break;
                case G_RDPHALF_1:
                    w1 = (r < 4) ? rnd() & 0x00FFFFFF : 0xDEAD0000u; // garbage, never a list
                    break;
                case G_LOAD_UCODE:
                    w1 = (r < 3) ? ucode_addr(gspS2DEX2_fifoTextStart)
                                 : (r < 7) ? ucode_addr(gspF3DZEX2_NoN_PosLight_fifoTextStart) : rnd();
                    break;
                default:
                    break;
            }
            wr32(a, w0);
            wr32(a + 4, w1);
        }
    }
    // Each task: a projection, a modelview, the viewport, then a call to a random list
    top = dl_new(16);
    m_perspective(persp, 60.0, 4.0 / 3.0, 10.0, 12800.0);
    m_translate(mv, 0.0, 0.0, -600.0);
    setup(&top, persp, mv);
    callAt = top.cur;
    gDL(&top, lists[0]);
    gEnd(&top);
    c0 = clock();
    for (run = 0; run < RUNS; run++) {
        wr32(callAt + 4, lists[rnd() % LISTS]);
        mock_reset();
        gfx_rsp_reset();
        gfx_rsp_run((run & 7) ? top.start : lists[rnd() % LISTS]);
        tris += gTriCount, rdp += gRdpCount, rects += gTexrectCount;
    }
    secs = (double)(clock() - c0) / CLOCKS_PER_SEC;
    CHECK(tris > 2000 && rdp > 10000 && rects > 1000, "fuzz exercised the pipeline: %ld tris, %ld RDP, %ld texrects",
          tris, rdp, rects);
    CHECK(gNonFinite == 0, "fuzz: %d non-finite vertices or projections reached GX", gNonFinite);
    CHECK(gLogCount - logsBefore < 200, "fuzz: logging stays bounded (%d lines)", gLogCount - logsBefore);
    // A run that hit the command limit would take a good fraction of a second
    CHECK(secs < 10.0, "fuzz: %d runs took %.2f s", RUNS, secs);
    if (getenv("GFX_TEST_VERBOSE") != NULL) {
        printf("  fuzz: %d runs, %ld tris, %ld RDP commands, %ld texrects, %.3f s\n", RUNS, tris, rdp, rects, secs);
    }
    ram_reset();
}

static void test_stats(void) {
    int i, before = gLogCount;

    // gc_time_ticks advances 100 us per call in the mock; stats log every 3 s. The texture cache line
    // (gfx_internal.h: gfx_rsp_stats_frame logs gfx_tex_get_stats) shows activity since the last line.
    gTexStats.binds = 1000, gTexStats.hits = 900, gTexStats.misses = 100;
    gTexStats.entries = 42, gTexStats.bytesUsed = 512 * 1024, gTexStats.bytesTotal = 2048 * 1024;
    for (i = 0; i < 100000 && gLogCount == before; i++) {
        gfx_rsp_stats_frame();
    }
    CHECK(gLogCount == before + 2 && strstr(gPrevLog, "tasks") != NULL, "statistics logged: %s", gPrevLog);
    CHECK(strstr(gLastLog, "gfx_tex: 1000 binds (900 hits, 100 misses") != NULL &&
              strstr(gLastLog, "42 textures, 512 of 2048 KB") != NULL,
          "texture statistics logged: %s", gLastLog);
    // Next interval: only the difference
    gfx_rsp_reset();
    gfx_rsp_run(0);
    gTexStats.binds = 1010, gTexStats.hits = 905, gTexStats.misses = 105;
    before = gLogCount;
    for (i = 0; i < 100000 && gLogCount == before; i++) {
        gfx_rsp_stats_frame();
    }
    CHECK(strstr(gLastLog, "gfx_tex: 10 binds (5 hits, 5 misses") != NULL, "texture statistics per interval: %s",
          gLastLog);
}

int main(void) {
    ram_init();

    test_segments();
    test_mtx_parse();
    test_matrix_stack();
    test_clip_coords();
    test_directional_lighting();
    test_light_refresh();
    test_point_lighting();
    test_fog();
    test_texgen();
    test_texcoords_and_modify();
    test_culling();
    test_reject_flat_tri2();
    test_display_lists();
    test_branch_z();
    test_cull_dl();
    test_texrect_and_rdp();
    test_force_and_lazy_projection();
    test_s2dex_switch();
    test_state_words();
    test_vertex_range();
    test_logging();
    test_light_quantization();
    test_perspnorm_shares_half1();
    test_matrix_saturation();
    test_bad_pointers();
    test_stats();
    test_fuzz();

    printf("gfx_rsp host test: %d checks, %d failures\n", sChecks, sFailures);
    return sFailures != 0;
}
