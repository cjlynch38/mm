/**
 * Stand-alone test of the renderer's GX backend (port/gc/gfx/gfx_gx.c). Test-only stubs stand in for
 * the RSP/RDP state, the combiner (gfx_tev_apply), the texture cache (gfx_tex_bind) and the bridge;
 * the scene is drawn through the gfx_gx_* API the way gfx_rsp/gfx_rdp do: z-buffer and frame fills,
 * a floor crossing the near plane, a perspective cube drawn front faces first (the depth test must
 * hide the back faces), interpenetrating triangles, a decal, a triangle that takes the CPU path,
 * primitive-depth rectangles, plain and flipped texture rectangles, an N64 orthographic projection,
 * geometry closer than the near plane (F3DZEX2 NoN draws it with z clamped), a G_ZS_PRIM triangle and
 * rectangle at the largest primitive depth, and FILL-mode markers on the cube corners as projected with
 * N64 math on the CPU.
 *
 * The first frame's EFB is checked (GX_PeekARGB / GX_PeekZ) against colors and depths computed with
 * N64 math; results go to the log (USB Gecko in slot B). Then the scene animates (the cube spins)
 * through two alternating N64 framebuffers at 20 fps.
 */
#include <gccore.h>
#include <malloc.h>
#include <math.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/usbgecko.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gc_ogc.h"
#include "gfx_internal.h"

#define FB_A 0x80500000u
#define FB_B 0x80600000u
#define ZBUF 0x00700000u /* physical, as some games write it */

#define NEAR 10.0f
#define FAR 1000.0f

typedef float M44[4][4];

/* ============================================================================================== */
/* Stubs                                                                                          */
/* ============================================================================================== */

uint32_t gGfxSegments[16];
GfxRspState gGfxRsp;
GfxRdpState gGfxRdp;

static int sGecko;

static void test_vlog(const char* prefix, const char* fmt, va_list args) {
    char buf[512];
    int len = snprintf(buf, sizeof(buf), "%s", prefix);

    len += vsnprintf(buf + len, sizeof(buf) - len - 2, fmt, args);
    if (len > (int)sizeof(buf) - 2) {
        len = sizeof(buf) - 2;
    }
    buf[len++] = '\n';
    buf[len] = '\0';
    printf("%s", buf);
    if (sGecko) {
        usb_sendbuffer_safe(1, buf, len);
    }
}

void gc_log(const char* fmt, ...) {
    va_list args;

    va_start(args, fmt);
    test_vlog("", fmt, args);
    va_end(args);
}

void gc_halt(const char* fmt, ...) {
    va_list args;

    va_start(args, fmt);
    test_vlog("HALT: ", fmt, args);
    va_end(args);
    gc_ogc_video_show_console();
    for (;;) {
        VIDEO_WaitVSync();
    }
}

void* gc_mem_alloc(unsigned int size, unsigned int align) {
    void* p = memalign((align < 32) ? 32 : align, (size + 31) & ~31u);

    gc_log("test: gc_mem_alloc(%u) = %p", size, p);
    return p;
}

void* gfx_addr(uint32_t addr) {
    if (addr == 0) {
        return NULL;
    }
    if (addr & 0x80000000) {
        return (void*)(0x80000000 | (addr & 0x1FFFFFFF));
    }
    return (void*)(0x80000000 | (gGfxSegments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF)));
}

/* Combiner stub: vertex color, TEXEL0, or a konst color; depth test/write and decal as the test sets them */
enum { TEV_SHADE, TEV_TEXEL0, TEV_KONST };
static int sTevMode;
static bool sTevZTest, sTevZWrite, sTevDecal;
static GXColor sTevKonst;
static int sTevApplies;

void gfx_tev_apply(GfxPrimKind kind, GfxTevInfo* out) {
    memset(out, 0, sizeof(*out));
    GX_SetNumTevStages(1);
    switch (sTevMode) {
        case TEV_SHADE:
            GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLOR0A0);
            GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
            out->usesShade = true;
            break;
        case TEV_TEXEL0:
            GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
            GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
            out->usesTexel0 = true;
            break;
        default:
            GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
            GX_SetTevColor(GX_TEVREG0, sTevKonst);
            GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
            GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
            GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
            break;
    }
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(sTevZTest ? GX_TRUE : GX_FALSE, GX_LEQUAL, sTevZWrite ? GX_TRUE : GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    out->depthTest = sTevZTest;
    out->depthWrite = sTevZWrite;
    out->decal = sTevDecal;
    sTevApplies++;
}

void gfx_tev_invalidate(void) {
}

/* Texture stub: 8x8 RGB565, quadrants red (top left), green (top right), blue (bottom left), white */
static u16 sTexData[64] ATTRIBUTE_ALIGN(32);
static GXTexObj sTexObj;

bool gfx_tex_bind(int tile, int texMap, GfxTexBinding* out) {
    GX_LoadTexObj(&sTexObj, texMap);
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->width = 8;
    out->height = 8;
    out->sShiftScale = out->tShiftScale = 1.0f;
    return true;
}

static void tex_init(void) {
    static const u16 quadrant[4] = { 0xF800, 0x07E0, 0x001F, 0xFFFF };
    int i;

    // RGB565 is stored in 4x4 tiles; at 8x8 each tile is one quadrant
    for (i = 0; i < 64; i++) {
        sTexData[i] = quadrant[i / 16];
    }
    DCFlushRange(sTexData, sizeof(sTexData));
    GX_InitTexObj(&sTexObj, sTexData, 8, 8, GX_TF_RGB565, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&sTexObj, GX_NEAR, GX_NEAR);
}

/* ============================================================================================== */
/* N64 math (row vectors: clip = v * M)                                                           */
/* ============================================================================================== */

static M44 sP, sV, sVt, sVP, sOrtho;

static void m_identity(M44 m) {
    memset(m, 0, sizeof(M44));
    m[0][0] = m[1][1] = m[2][2] = m[3][3] = 1.0f;
}

static void m_mul(M44 out, M44 a, M44 b) {
    M44 r;
    int i, j, k;

    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            r[i][j] = 0.0f;
            for (k = 0; k < 4; k++) {
                r[i][j] += a[i][k] * b[k][j];
            }
        }
    }
    memcpy(out, r, sizeof(M44));
}

static void v_mul(float out[4], const float v[4], M44 m) {
    float r[4];
    int j;

    for (j = 0; j < 4; j++) {
        r[j] = v[0] * m[0][j] + v[1] * m[1][j] + v[2] * m[2][j] + v[3] * m[3][j];
    }
    memcpy(out, r, sizeof(r));
}

/* libultra guPerspectiveF (scale 1) */
static void m_persp(M44 m, float fovy, float aspect, float n, float f) {
    float cot = 1.0f / tanf(fovy * (float)M_PI / 360.0f);

    memset(m, 0, sizeof(M44));
    m[0][0] = cot / aspect;
    m[1][1] = cot;
    m[2][2] = (n + f) / (n - f);
    m[2][3] = -1.0f;
    m[3][2] = (2.0f * n * f) / (n - f);
}

/* libultra guOrthoF (scale 1) */
static void m_ortho(M44 m, float l, float r, float b, float t, float n, float f) {
    m_identity(m);
    m[0][0] = 2.0f / (r - l);
    m[1][1] = 2.0f / (t - b);
    m[2][2] = -2.0f / (f - n);
    m[3][0] = -(r + l) / (r - l);
    m[3][1] = -(t + b) / (t - b);
    m[3][2] = -(f + n) / (f - n);
}

static void m_rot(M44 m, int axis, float deg) {
    float s = sinf(deg * (float)M_PI / 180.0f);
    float c = cosf(deg * (float)M_PI / 180.0f);
    int a = (axis + 1) % 3, b = (axis + 2) % 3;

    m_identity(m);
    m[a][a] = c;
    m[a][b] = s;
    m[b][a] = -s;
    m[b][b] = c;
}

/* Eye-space point -> clip, through world space and the combined view-projection (as MM's P*V) */
static void eye_to_clip(float clip[4], float ex, float ey, float ez) {
    float e[4] = { ex, ey, ez, 1.0f };
    float w[4];

    v_mul(w, e, sVt);
    v_mul(clip, w, sVP);
}

static void eye_to_screen(float* sx, float* sy, float ex, float ey, float ez) {
    float c[4];

    eye_to_clip(c, ex, ey, ez);
    *sx = (c[0] / c[3]) * 160.0f + 160.0f;
    *sy = -(c[1] / c[3]) * 120.0f + 120.0f;
}

static void make_vtx(GfxVtx* v, const float clip[4], u8 r, u8 g, u8 b) {
    memset(v, 0, sizeof(*v));
    v->x = clip[0];
    v->y = clip[1];
    v->z = clip[2];
    v->w = clip[3];
    v->r = r;
    v->g = g;
    v->b = b;
    v->a = 255;
}

static void eye_tri(const float e[3][3], u8 r, u8 g, u8 b) {
    GfxVtx v[3];
    float c[4];
    int i;

    for (i = 0; i < 3; i++) {
        eye_to_clip(c, e[i][0], e[i][1], e[i][2]);
        make_vtx(&v[i], c, r, g, b);
    }
    gfx_gx_triangle(&v[0], &v[1], &v[2]);
}

/* Window depth (24-bit) of an N64 NDC z through the standard viewport */
static u32 depth24(float ndcZ) {
    float d = (ndcZ * 511.0f + 511.0f) / 1023.0f;

    return (u32)(d * 16777215.0f + 0.5f);
}

/* ============================================================================================== */
/* RDP helpers (what gfx_rdp would set)                                                           */
/* ============================================================================================== */

static void rdp_cycle(u32 cycle) {
    gGfxRdp.otherModeH = (gGfxRdp.otherModeH & ~(3u << G_MDSFT_CYCLETYPE)) | cycle;
    gGfxRdp.dirty |= GFX_DIRTY_OTHERMODE;
}

static void rdp_zprim(bool on, u16 z) {
    gGfxRdp.otherModeL = on ? (gGfxRdp.otherModeL | G_ZS_PRIM) : (gGfxRdp.otherModeL & ~G_ZS_PRIM);
    gGfxRdp.primDepthZ = z;
    gGfxRdp.dirty |= GFX_DIRTY_OTHERMODE | GFX_DIRTY_COLORS;
}

static void tev(int mode, bool zTest, bool zWrite, bool decal) {
    sTevMode = mode;
    sTevZTest = zTest;
    sTevZWrite = zWrite;
    sTevDecal = decal;
    gGfxRdp.dirty |= GFX_DIRTY_COMBINE | GFX_DIRTY_OTHERMODE;
}

static GXColor expand5551(u16 p) {
    u32 r = (p >> 11) & 0x1F, g = (p >> 6) & 0x1F, b = (p >> 1) & 0x1F;
    GXColor c = { (r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255 };

    return c;
}

static void fill(u32 cimg, u32 fillColor, float ulx, float uly, float lrx, float lry) {
    rdp_cycle(G_CYC_FILL);
    gGfxRdp.colorImageAddr = cimg;
    gGfxRdp.fillColor = fillColor;
    gfx_gx_fillrect(ulx, uly, lrx, lry);
}

/* ============================================================================================== */
/* Scene                                                                                          */
/* ============================================================================================== */

static const GXColor kFaceColors[6] = {
    { 200, 60, 60, 255 }, { 60, 200, 60, 255 }, { 60, 60, 200, 255 },
    { 200, 200, 60, 255 }, { 200, 60, 200, 255 }, { 60, 200, 200, 255 },
};
/* Faces of the cube: corner indices (bit 0 = +x, bit 1 = +y, bit 2 = +z), axis and sign of the normal */
static const u8 kFaces[6][4] = { { 0, 2, 6, 4 }, { 1, 3, 7, 5 }, { 0, 1, 5, 4 },
                                 { 2, 3, 7, 6 }, { 0, 1, 3, 2 }, { 4, 5, 7, 6 } };

#define CUBE_HALF 40.0f
static const float kCubeCenter[3] = { -50.0f, 0.0f, -220.0f };

static M44 sCubeRot;

static void cube_point(float out[3], float lx, float ly, float lz) {
    float l[4] = { lx, ly, lz, 1.0f };
    float r[4];

    v_mul(r, l, sCubeRot);
    out[0] = r[0] + kCubeCenter[0];
    out[1] = r[1] + kCubeCenter[1];
    out[2] = r[2] + kCubeCenter[2];
}

static void cube_corner(float out[3], int i) {
    cube_point(out, (i & 1) ? CUBE_HALF : -CUBE_HALF, (i & 2) ? CUBE_HALF : -CUBE_HALF,
               (i & 4) ? CUBE_HALF : -CUBE_HALF);
}

/* Local point on face f: normal * half + u * a + v * b (u, v the two other axes) */
static void face_point(float out[3], int f, float a, float b) {
    int axis = f / 2;
    float sign = (f & 1) ? 1.0f : -1.0f;
    float l[3];

    l[axis] = sign * CUBE_HALF;
    l[(axis + 1) % 3] = a;
    l[(axis + 2) % 3] = b;
    cube_point(out, l[0], l[1], l[2]);
}

/* The face the eye ray through the cube center hits: the one facing the eye most */
static int cube_front_face(void) {
    float best = -1e9f;
    int face = 0;
    int f;

    for (f = 0; f < 6; f++) {
        float c[3], d;

        face_point(c, f, 0, 0);
        // normal direction = face center - cube center; eye at the origin
        d = (c[0] - kCubeCenter[0]) * -kCubeCenter[0] + (c[1] - kCubeCenter[1]) * -kCubeCenter[1] +
            (c[2] - kCubeCenter[2]) * -kCubeCenter[2];
        if (d > best) {
            best = d;
            face = f;
        }
    }
    return face;
}

static void draw_cube(void) {
    float dist[6];
    int order[6];
    int i, j;

    // Front faces first: the back faces drawn afterwards must fail the depth test
    for (i = 0; i < 6; i++) {
        float c[3];

        face_point(c, i, 0, 0);
        dist[i] = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
        order[i] = i;
    }
    for (i = 0; i < 6; i++) {
        for (j = i + 1; j < 6; j++) {
            if (dist[order[j]] < dist[order[i]]) {
                int t = order[i];
                order[i] = order[j];
                order[j] = t;
            }
        }
    }
    for (i = 0; i < 6; i++) {
        int f = order[i];
        float e[4][3];
        float t0[3][3], t1[3][3];

        for (j = 0; j < 4; j++) {
            cube_corner(e[j], kFaces[f][j]);
        }
        memcpy(t0[0], e[0], 12);
        memcpy(t0[1], e[1], 12);
        memcpy(t0[2], e[2], 12);
        memcpy(t1[0], e[0], 12);
        memcpy(t1[1], e[2], 12);
        memcpy(t1[2], e[3], 12);
        eye_tri(t0, kFaceColors[f].r, kFaceColors[f].g, kFaceColors[f].b);
        eye_tri(t1, kFaceColors[f].r, kFaceColors[f].g, kFaceColors[f].b);
    }
}

static const u16 kClear5551 = (2 << 11) | (3 << 6) | (8 << 1) | 1;
static const GXColor kYellow = { 230, 210, 40, 255 };
static const GXColor kGrey = { 110, 110, 110, 255 };
static const GXColor kRed = { 230, 30, 30, 255 };
static const GXColor kGreen = { 30, 200, 30, 255 };
static const GXColor kWhite = { 255, 255, 255, 255 };
static const GXColor kPurple = { 128, 0, 255, 255 };
static const GXColor kMagenta = { 255, 0, 255, 255 };
static const GXColor kCyan = { 0, 255, 255, 255 };
static const GXColor kOrange = { 255, 160, 0, 255 };
static const GXColor kOrthoColor = { 255, 128, 64, 255 };
static const GXColor kPink = { 250, 120, 160, 255 };
static const GXColor kLime = { 160, 255, 60, 255 };
static const GXColor kSky = { 60, 120, 255, 255 };
static const GXColor kLavender = { 200, 140, 255, 255 };

/* Primitive depth is the RDP's 15-bit z: the RSP's screen z (0..G_MAXZ) shifted left by 5 */
#define PRIM_Z_BEHIND 32439 /* window depth 0.99 */
#define PRIM_Z_FRONT 16383  /* window depth 0.5 */
#define PRIM_DEPTH24(z) ((u32)((z) / (32.0f * 1023.0f) * 16777215.0f + 0.5f))

/* Step 11: eye-space triangle 7 units from the eye, closer than the near plane (10) */
static const float kNearTri[3][3] = { { -5.0f, 2.2f, -7.0f }, { -3.0f, 2.2f, -7.0f }, { -4.0f, 3.8f, -7.0f } };
/* Step 12: G_ZS_PRIM triangle */
static const float kZPrimTri[3][3] = { { -12.0f, 38.0f, -100.0f }, { 2.0f, 38.0f, -100.0f }, { -5.0f, 50.0f, -100.0f } };

static void draw_scene(u32 fb, float cubeAngle) {
    M44 rx, ry;
    float e[3][3];
    float c[4];
    GfxVtx v[3];
    int i;

    m_rot(ry, 1, cubeAngle);
    m_rot(rx, 0, 20.0f);
    m_mul(sCubeRot, ry, rx);

    // What the game's setup display list establishes
    gGfxRdp.scissorUlx = 0;
    gGfxRdp.scissorUly = 0;
    gGfxRdp.scissorLrx = GFX_N64_WIDTH << 2;
    gGfxRdp.scissorLry = GFX_N64_HEIGHT << 2;
    gGfxRdp.zImageAddr = ZBUF;
    gGfxRdp.colorImageSiz = G_IM_SIZ_16b;
    gGfxRdp.otherModeL = 0;
    gGfxRsp.geometryMode = G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH;
    gGfxRsp.textureTile = 0;
    gGfxRsp.viewportScale[0] = gGfxRsp.viewportTrans[0] = GFX_N64_WIDTH * 2;
    gGfxRsp.viewportScale[1] = gGfxRsp.viewportTrans[1] = GFX_N64_HEIGHT * 2;
    gGfxRsp.viewportScale[2] = gGfxRsp.viewportTrans[2] = G_MAXZ / 2;
    gGfxRdp.dirty = GFX_DIRTY_ALL;

    // 1. Clear the z-buffer (FILL into the z image), then the frame
    fill(ZBUF, 0xFFFCFFFC, 0, 0, 320, 240);
    fill(fb, (kClear5551 << 16) | kClear5551, 0, 0, 319, 239); // inclusive form: still a full-screen clear

    // 2. Floor crossing the near plane (its far corners are behind the eye), then a near occluder
    rdp_cycle(G_CYC_1CYCLE);
    gfx_gx_set_projection((const float(*)[4])sVP);
    tev(TEV_SHADE, true, true, false);
    {
        float t0[3][3] = { { -300, -60, -400 }, { 300, -60, -400 }, { 300, -60, 100 } };
        float t1[3][3] = { { -300, -60, -400 }, { 300, -60, 100 }, { -300, -60, 100 } };

        eye_tri(t0, kGrey.r, kGrey.g, kGrey.b);
        eye_tri(t1, kGrey.r, kGrey.g, kGrey.b);
    }
    {
        float t[3][3] = { { -35, -15, -50 }, { 5, -15, -50 }, { -15, 20, -50 } };

        eye_tri(t, kYellow.r, kYellow.g, kYellow.b);
    }

    // 3. Clear the z-buffer again: everything drawn next ignores the occluder's depth
    fill(ZBUF, 0xFFFCFFFC, 0, 0, 320, 240);
    gGfxRdp.colorImageAddr = fb;
    rdp_cycle(G_CYC_1CYCLE);

    // 4. Cube, interpenetrating triangles (red tilted in depth through a green one at constant depth)
    draw_cube();
    {
        float green[3][3] = { { 60, -20, -150 }, { 110, -20, -150 }, { 85, 30, -150 } };
        float red[3][3] = { { 55, -5, -120 }, { 115, -5, -180 }, { 85, 15, -150 } };

        eye_tri(green, kGreen.r, kGreen.g, kGreen.b);
        eye_tri(red, kRed.r, kRed.g, kRed.b);
    }

    // 5. Decal on the cube's front face, coplanar with it, a quarter of the way to one corner
    tev(TEV_SHADE, true, false, true);
    {
        int f = cube_front_face();
        float h = CUBE_HALF;

        face_point(e[0], f, 0.35f * h, 0.35f * h);
        face_point(e[1], f, 0.65f * h, 0.35f * h);
        face_point(e[2], f, 0.50f * h, 0.65f * h);
        eye_tri(e, kWhite.r, kWhite.g, kWhite.b);
    }

    // 6. A triangle whose z is off the projection's z = a*w + b plane: drawn through the CPU path
    tev(TEV_SHADE, true, true, false);
    {
        float t[3][3] = { { 130, -90, -300 }, { 190, -90, -300 }, { 160, -50, -300 } };

        for (i = 0; i < 3; i++) {
            eye_to_clip(c, t[i][0], t[i][1], t[i][2]);
            c[2] -= 0.5f * c[3];
            make_vtx(&v[i], c, kPurple.r, kPurple.g, kPurple.b);
        }
        gfx_gx_triangle(&v[0], &v[1], &v[2]);
    }

    // 7. Primitive-depth rectangles (1-cycle fills through the combiner): magenta behind the cube, cyan in front
    tev(TEV_KONST, true, false, false);
    sTevKonst = kMagenta;
    rdp_zprim(true, PRIM_Z_BEHIND);
    gfx_gx_fillrect(30, 60, 100, 110);
    tev(TEV_KONST, true, true, false);
    sTevKonst = kCyan;
    rdp_zprim(true, PRIM_Z_FRONT);
    gfx_gx_fillrect(90, 150, 170, 190);
    rdp_zprim(false, 0);

    // 8. Texture rectangles, 8x8 texels over 64x64 pixels; the second is G_TEXRECTFLIP
    tev(TEV_TEXEL0, false, false, false);
    gfx_gx_texrect(180, 10, 244, 74, 0, 0.0f, 0.0f, 0.125f, 0.125f, false);
    gfx_gx_texrect(250, 10, 314, 74, 0, 0.0f, 0.0f, 0.125f, 0.125f, true);

    // 9. 1-cycle fill rectangle without depth
    tev(TEV_KONST, false, false, false);
    sTevKonst = kOrange;
    gfx_gx_fillrect(250, 200, 310, 230);

    // 10. N64 orthographic projection (w = 1): a triangle in the bottom left corner
    gfx_gx_set_projection((const float(*)[4])sOrtho);
    tev(TEV_SHADE, false, false, false);
    {
        float pts[3][2] = { { -150, -110 }, { -110, -110 }, { -130, -80 } };

        for (i = 0; i < 3; i++) {
            float o[4] = { pts[i][0], pts[i][1], 0.0f, 1.0f };

            v_mul(c, o, sOrtho);
            make_vtx(&v[i], c, kOrthoColor.r, kOrthoColor.g, kOrthoColor.b);
        }
        gfx_gx_triangle(&v[0], &v[1], &v[2]);
    }

    // 11. F3DZEX2 NoN draws geometry between the eye and the near plane, its screen z clamped to 0: a
    //     triangle at w = 7 (near 10) must be drawn, at depth 0
    gfx_gx_set_projection((const float(*)[4])sVP);
    tev(TEV_SHADE, true, true, false);
    eye_tri(kNearTri, kPink.r, kPink.g, kPink.b);

    // 12. G_ZS_PRIM triangle at gDPSetPrimDepth(-1, -1) (0x7FFF, just past the far plane): drawn, at the far
    //     end of the viewport's depth range
    rdp_zprim(true, 0xFFFF);
    eye_tri(kZPrimTri, kLime.r, kLime.g, kLime.b);

    // 13. Primitive-depth rectangle at 0x7FFF: window depth 1, LEQUAL passes against the cleared buffer
    tev(TEV_KONST, true, true, false);
    sTevKonst = kSky;
    gfx_gx_fillrect(105, 4, 128, 18);
    rdp_zprim(false, 0);

    // 14. N64 orthographic triangle in front of the ortho near plane (ndc z -1.5, CPU path): NoN clamps its z
    gfx_gx_set_projection((const float(*)[4])sOrtho);
    tev(TEV_SHADE, true, true, false);
    {
        float pts[3][2] = { { -100, -110 }, { -60, -110 }, { -80, -80 } };

        for (i = 0; i < 3; i++) {
            float o[4] = { pts[i][0], pts[i][1], 150.0f, 1.0f };

            v_mul(c, o, sOrtho);
            make_vtx(&v[i], c, kLavender.r, kLavender.g, kLavender.b);
        }
        gfx_gx_triangle(&v[0], &v[1], &v[2]);
    }

    // 15. FILL-mode markers on the cube corners, projected on the CPU with N64 math
    for (i = 0; i < 8; i++) {
        float p[3], sx, sy;

        cube_corner(p, i);
        eye_to_screen(&sx, &sy, p[0], p[1], p[2]);
        fill(fb, 0xFFFFFFFF, floorf(sx) - 1, floorf(sy) - 1, floorf(sx) + 1, floorf(sy) + 1);
    }
}

/* ============================================================================================== */
/* Checks                                                                                         */
/* ============================================================================================== */

static int sChecks, sPassed;

static void check_color(const char* name, float sx, float sy, GXColor expect, int tol) {
    GXColor got;
    u16 x = (u16)(sx * GFX_SCALE), y = (u16)(sy * GFX_SCALE);
    bool ok;

    GX_PeekARGB(x, y, &got);
    ok = abs(got.r - expect.r) <= tol && abs(got.g - expect.g) <= tol && abs(got.b - expect.b) <= tol;
    sChecks++;
    sPassed += ok;
    gc_log("check %-34s EFB (%3u,%3u): color %02X%02X%02X, expected %02X%02X%02X  %s", name, x, y, got.r, got.g, got.b,
           expect.r, expect.g, expect.b, ok ? "ok" : "FAIL");
}

static void check_depth(const char* name, float sx, float sy, u32 expect, u32 tol) {
    u32 got;
    u16 x = (u16)(sx * GFX_SCALE), y = (u16)(sy * GFX_SCALE);
    int diff;
    bool ok;

    GX_PeekZ(x, y, &got);
    got &= 0xFFFFFF;
    diff = (int)got - (int)expect;
    ok = (u32)abs(diff) <= tol;
    sChecks++;
    sPassed += ok;
    gc_log("check %-34s EFB (%3u,%3u): depth %06X, expected %06X (diff %d)  %s", name, x, y, got, expect, diff,
           ok ? "ok" : "FAIL");
}

static void run_checks(void) {
    float sx, sy, c[4], p[3];
    int f = cube_front_face();

    gfx_gx_flush();
    GX_DrawDone();
    {
        // Dolphin skips CPU EFB access by default (peeks read 0): detect it with a poke/peek round trip
        GXColor probe;

        GX_PokeARGB(0, 0, kWhite);
        GX_PeekARGB(0, 0, &probe);
        if (probe.r != 255 || probe.g != 255 || probe.b != 255) {
            gc_log("gfx_gx_test: no CPU access to the EFB (Dolphin: set Graphics.Hacks.EFBAccessEnable=True), checks "
                   "skipped; judge the screenshot");
            return;
        }
    }

    // Empty background: the frame fill color and far depth
    check_color("background = clear color", 300, 100, expand5551(kClear5551), 2);
    check_depth("background depth = far", 300, 100, 0xFFFFFF, 0);

    // Constant-depth green triangle where it is in front of the red one: exact N64 window depth
    eye_to_screen(&sx, &sy, 95, 2, -150);
    eye_to_clip(c, 95, 2, -150);
    check_color("green in front (right half)", sx, sy, kGreen, 2);
    check_depth("green depth (w = 150)", sx, sy, depth24(c[2] / c[3]), 256);
    eye_to_screen(&sx, &sy, 75, 2, -140);
    check_color("red in front (left half)", sx, sy, kRed, 2);

    // Cube: front face color at its center (back faces drawn later were rejected), decal visible
    face_point(p, f, 0, 0);
    eye_to_screen(&sx, &sy, p[0], p[1], p[2]);
    check_color("cube front face at its center", sx, sy, kFaceColors[f], 2);
    eye_to_clip(c, p[0], p[1], p[2]);
    check_depth("cube front face depth", sx, sy, depth24(c[2] / c[3]), 2048);
    face_point(p, f, 0.5f * CUBE_HALF, 0.45f * CUBE_HALF);
    eye_to_screen(&sx, &sy, p[0], p[1], p[2]);
    check_color("decal on the front face", sx, sy, kWhite, 2);

    // Occluder before the second z clear: its color stays, its depth is gone
    check_color("near occluder outside the cube", 30, 178, kYellow, 2);
    check_color("floor (crosses the near plane)", 200, 225, kGrey, 2);

    // CPU-path triangle (z off the plane, divided on the CPU)
    eye_to_screen(&sx, &sy, 160, -77, -300);
    check_color("CPU-path triangle", sx, sy, kPurple, 2);

    // Primitive depth: magenta (0.99) visible over the background, cyan (0.5) wrote its depth
    check_color("prim-depth rect behind, outside cube", 35, 65, kMagenta, 2);
    check_color("prim-depth rect in front", 92, 188, kCyan, 2);
    check_depth("prim-depth rect depth (0.5)", 92, 188, PRIM_DEPTH24(PRIM_Z_FRONT), 4);

    // Texture rectangles: quadrants, and the flip that swaps the axes
    {
        GXColor red = { 255, 0, 0, 255 }, green = { 0, 255, 0, 255 }, blue = { 0, 0, 255, 255 };

        check_color("texrect top left = red", 190, 20, red, 8);
        check_color("texrect top right = green", 234, 20, green, 8);
        check_color("texrect bottom left = blue", 190, 64, blue, 8);
        check_color("texrect flip top right = blue", 304, 20, blue, 8);
        check_color("texrect flip bottom left = green", 260, 64, green, 8);
    }
    check_color("1-cycle fill rect", 280, 215, kOrange, 2);
    check_color("N64 ortho triangle", 30, 220, kOrthoColor, 2);

    // NoN: closer than the near plane, still drawn with depth clamped to 0 (perspective and ortho)
    eye_to_screen(&sx, &sy, (kNearTri[0][0] + kNearTri[1][0] + kNearTri[2][0]) / 3.0f,
                  (kNearTri[0][1] + kNearTri[1][1] + kNearTri[2][1]) / 3.0f, kNearTri[0][2]);
    check_color("triangle closer than the near plane", sx, sy, kPink, 2);
    check_depth("its depth (clamped to 0)", sx, sy, 0, 0);
    check_color("ortho triangle in front of near (CPU)", 80, 220, kLavender, 2);
    check_depth("its depth (clamped to 0)", 80, 220, 0, 0);

    // Primitive depth at 0x7FFF: triangle (clamped to the viewport's far end) and rectangle (window depth 1)
    eye_to_screen(&sx, &sy, (kZPrimTri[0][0] + kZPrimTri[1][0] + kZPrimTri[2][0]) / 3.0f,
                  (kZPrimTri[0][1] + kZPrimTri[1][1] + kZPrimTri[2][1]) / 3.0f, kZPrimTri[0][2]);
    check_color("G_ZS_PRIM triangle at depth 0x7FFF", sx, sy, kLime, 2);
    check_depth("its depth (viewport far end)", sx, sy, depth24(1.0f), 2);
    check_color("prim-depth rect at 0x7FFF", 115, 10, kSky, 2);
    check_depth("its depth (far)", 115, 10, 0xFFFFFF, 0);

    gc_log("gfx_gx_test: %d of %d checks passed%s", sPassed, sChecks, (sPassed == sChecks) ? "" : "  <-- FAILURES");
}

/* ============================================================================================== */

int main(void) {
    M44 ry;
    u64 start;
    int frame;

    gc_ogc_video_init();
    sGecko = usb_isgeckoalive(1);
    gc_log("gfx_gx_test: built " __DATE__ " " __TIME__);

    gfx_gx_init();
    if (!gfx_gx_ready()) {
        gc_halt("gfx_gx_init failed");
    }
    tex_init();

    // Camera: rotated about y, so the view-projection's w column has several non-zero entries
    m_persp(sP, 60.0f, 4.0f / 3.0f, NEAR, FAR);
    m_rot(ry, 1, 15.0f);
    memcpy(sV, ry, sizeof(M44));
    m_rot(sVt, 1, -15.0f);
    m_mul(sVP, sV, sP);
    m_ortho(sOrtho, -160, 160, -120, 120, -100, 100);

    for (frame = 0;; frame++) {
        u32 fb = (frame & 1) ? FB_B : FB_A;

        start = gettime();
        gfx_gx_task_begin();
        draw_scene(fb, 30.0f + frame * 1.5f);
        if (frame == 0) {
            run_checks();
        }
        gfx_gx_task_end();
        if (frame == 0) {
            gc_log("frame 0: %u us, %d combiner applies", (u32)ticks_to_microsecs(gettime() - start), sTevApplies);
        }
        // As the VI service thread would: the swapped buffer becomes current at the next retrace
        VIDEO_WaitVSync();
        gfx_gx_present((void*)fb);
        VIDEO_WaitVSync();
        VIDEO_WaitVSync();
    }
}
