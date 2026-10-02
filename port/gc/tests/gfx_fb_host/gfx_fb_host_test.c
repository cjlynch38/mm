/**
 * Host test of the renderer's framebuffer effects (port/gc/gfx/gfx_fb.c). The GX calls gfx_fb.c makes run on a model of
 * the EFB (include/gccore.h): texture copies write their tiled formats (RGB565, RGB5A3, R8, RGBA8, Z24X8; the box
 * filter averages 2x2 pixels), and the quads gfx_fb.c draws (the frame's pixels back after an off-screen pass, an image
 * loaded into the canvas) copy their texture's texels 1:1 into the EFB. N64 RAM is a buffer (GFX_HOST_TEST).
 *
 * The expected pixels are computed here the way the renderer did before textures of the frame and of written images
 * (RGB565 readbacks converted to RGBA5551, gfx_tex.c's RGB5A3 texels of RAM), so the tests check that:
 *   - a background read from the frame gets those texels without the frame's RAM being written, and the RAM gets the
 *     same pixels when something reads it, also in the middle of an off-screen pass (from the copy made when it
 *     started) and before a draw that reads the canvas reuses the transfer buffer;
 *   - an off-screen image written whole is its own texture, in that task and, while its RAM is unchanged, later ones;
 *     a partly written one is not; a rewritten one (by the CPU, or by the renderer's depth) is not any more;
 *   - a served texture whose memory gfx_fb.c reuses before its draws are issued (gfx_gx_image_rect switches the render
 *     target or loads the canvas after the bind) is bound from RAM first, with the same texels;
 *   - the z-buffer gets one N64 depth value per 4x4 pixels, from the EFB depth at each block's pixel (1, 1).
 *
 *   make -C port/gc/tests/gfx_fb_host run
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gc_ogc.h"
#include "gfx_internal.h"
#include <ogc/lwp_watchdog.h>

#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x01800000u
#define EFB_W 640
#define EFB_H 528
#define W GFX_N64_WIDTH
#define H GFX_N64_HEIGHT

/* N64 RAM addresses of the images the tests use */
#define FRAME (RAM_BASE + 0x100000)
#define IMG_X (RAM_BASE + 0x200000)
#define IMG_Y (RAM_BASE + 0x300000)
#define ZBUF (RAM_BASE + 0x400000)
#define IMG_I8 (RAM_BASE + 0x500000)

uintptr_t gGfxHostRamBias;
static uint8_t* sRam;

static int sChecks, sFailures;

#define CHECK(cond, ...)                                    \
    do {                                                    \
        sChecks++;                                          \
        if (!(cond)) {                                      \
            sFailures++;                                    \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                            \
            printf("\n");                                   \
        }                                                   \
    } while (0)

static uint32_t sRng = 0x12345678;

static uint32_t rnd(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static void* ptr(uint32_t addr) {
    return sRam + (addr - RAM_BASE);
}

static uint16_t* ram16(uint32_t addr) {
    return (uint16_t*)ptr(addr);
}

/* ============================================================================================== */
/* Bridge, gfx_gx.c and gfx_tex.c as gfx_fb.c calls them                                          */
/* ============================================================================================== */

static int sLogs;
static GXRModeObj sMode;
static int sRamWrites;
static uint32_t sRamWriteAddr, sRamWriteBytes;

void gc_log(const char* fmt, ...) {
    va_list ap;

    sLogs++;
    if (getenv("GFX_TEST_VERBOSE") != NULL) {
        printf("  [log] ");
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        printf("\n");
    }
}

void gc_halt(const char* fmt, ...) {
    (void)fmt;
    abort();
}

void* gc_mem_alloc(unsigned int size, unsigned int align) {
    return aligned_alloc(align, (size + align - 1) / align * align);
}

struct _gx_rmodeobj* gc_ogc_video_mode(void) {
    return &sMode;
}

void gfx_gx_flush(void) {
}

void gfx_gx_state_lost(void) {
}

void gfx_tex_ram_written(uint32_t addr, uint32_t bytes) {
    sRamWrites++;
    sRamWriteAddr = addr;
    sRamWriteBytes = bytes;
}

u64 gettime(void) {
    static u64 t;

    return t += 40;
}

void* SYS_GetArena1Lo(void) {
    return (void*)(uintptr_t)0x10000000;
}

void* SYS_GetArena1Hi(void) {
    return (void*)(uintptr_t)(0x10000000 + (8 << 20));
}

void DCFlushRange(void* startaddress, u32 len) {
}

void DCInvalidateRange(void* startaddress, u32 len) {
}

/* ============================================================================================== */
/* The EFB and GX                                                                                 */
/* ============================================================================================== */

static uint32_t sEfb[EFB_H][EFB_W];   /* 0x00RRGGBB */
static uint32_t sDepth[EFB_H][EFB_W]; /* 24 bits */

static struct {
    int x, y, w, h, tw, th;
    u32 fmt;
    bool half;
} sCopy;
static GXColor sClear;
static u32 sClearZ;
static int sCopies;
static GXTexObj sMaps[8];
static int sLoads[8];
static u8 sZTex;
static int sVerts;
static float sQuadX[4], sQuadY[4];

void GX_SetCopyFilter(u8 aa, u8 sample_pattern[12][2], u8 vf, u8 vfilter[7]) {
}

void GX_SetCopyClear(GXColor color, u32 zvalue) {
    sClear = color;
    sClearZ = zvalue;
}

void GX_SetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    sCopy.x = left;
    sCopy.y = top;
    sCopy.w = wd;
    sCopy.h = ht;
}

void GX_SetTexCopyDst(u16 wd, u16 ht, u32 fmt, u8 mipmap) {
    sCopy.tw = wd;
    sCopy.th = ht;
    sCopy.fmt = fmt;
    sCopy.half = mipmap != 0;
}

static uint32_t efb_at(int x, int y) {
    x = (x < EFB_W) ? x : EFB_W - 1;
    y = (y < EFB_H) ? y : EFB_H - 1;
    return sEfb[y][x];
}

/* The copy's source color for texel (u, v): the pixel, or the average of 2x2 pixels (each channel truncated) */
static uint32_t copy_color(int u, int v) {
    uint32_t r = 0, g = 0, b = 0;
    int i;

    if (!sCopy.half) {
        return efb_at(sCopy.x + u, sCopy.y + v);
    }
    for (i = 0; i < 4; i++) {
        uint32_t c = efb_at(sCopy.x + u * 2 + (i & 1), sCopy.y + v * 2 + (i >> 1));

        r += (c >> 16) & 0xFF;
        g += (c >> 8) & 0xFF;
        b += c & 0xFF;
    }
    return ((r / 4) << 16) | ((g / 4) << 8) | (b / 4);
}

static int tile16(int tw, int u, int v) {
    return (((v >> 2) * ((tw + 3) / 4) + (u >> 2)) * 16) + (v & 3) * 4 + (u & 3);
}

static int tile8(int tw, int u, int v) {
    return (((v >> 2) * ((tw + 7) / 8) + (u >> 3)) * 32) + (v & 3) * 8 + (u & 7);
}

static uint16_t enc565(uint32_t c) {
    return (uint16_t)((((c >> 16) & 0xFF) >> 3) << 11 | (((c >> 8) & 0xFF) >> 2) << 5 | ((c & 0xFF) >> 3));
}

/* The EFB has no alpha: copies of it are opaque */
static uint16_t enc5a3(uint32_t c) {
    return (uint16_t)(0x8000 | (((c >> 16) & 0xFF) >> 3) << 10 | (((c >> 8) & 0xFF) >> 3) << 5 | ((c & 0xFF) >> 3));
}

void GX_CopyTex(void* dest, u8 clear) {
    uint8_t* d = dest;
    int u, v;

    for (v = 0; v < sCopy.th; v++) {
        for (u = 0; u < sCopy.tw; u++) {
            int i = (v & 3) * 4 + (u & 3);
            uint8_t* t = d + (((v >> 2) * ((sCopy.tw + 3) / 4) + (u >> 2)) * 64);
            uint32_t c;

            switch (sCopy.fmt) {
                case GX_TF_RGB565:
                    ((uint16_t*)d)[tile16(sCopy.tw, u, v)] = enc565(copy_color(u, v));
                    break;
                case GX_TF_RGB5A3:
                    ((uint16_t*)d)[tile16(sCopy.tw, u, v)] = enc5a3(copy_color(u, v));
                    break;
                case GX_CTF_R8:
                    d[tile8(sCopy.tw, u, v)] = (uint8_t)(copy_color(u, v) >> 16);
                    break;
                case GX_TF_RGBA8:
                    c = copy_color(u, v);
                    t[i * 2] = 0xFF;
                    t[i * 2 + 1] = (uint8_t)(c >> 16);
                    t[32 + i * 2] = (uint8_t)(c >> 8);
                    t[32 + i * 2 + 1] = (uint8_t)c;
                    break;
                case GX_TF_Z24X8:
                    c = sDepth[(sCopy.y + v < EFB_H) ? sCopy.y + v : EFB_H - 1][sCopy.x + u];
                    t[i * 2] = 0xFF;
                    t[i * 2 + 1] = (uint8_t)(c >> 16);
                    ((uint16_t*)(t + 32))[i] = (uint16_t)c;
                    break;
                default:
                    printf("  model: copy format %u not modeled\n", (unsigned int)sCopy.fmt);
                    abort();
            }
        }
    }
    if (clear) {
        for (v = sCopy.y; v < sCopy.y + sCopy.h; v++) {
            for (u = sCopy.x; u < sCopy.x + sCopy.w; u++) {
                sEfb[v][u] = ((uint32_t)sClear.r << 16) | ((uint32_t)sClear.g << 8) | sClear.b;
                sDepth[v][u] = sClearZ;
            }
        }
    }
    sCopies++;
}

void GX_PixModeSync(void) {
}

void GX_DrawDone(void) {
}

void GX_InvalidateTexAll(void) {
}

void GX_InitTexObj(GXTexObj* obj, void* img_ptr, u16 wd, u16 ht, u8 fmt, u8 wrap_s, u8 wrap_t, u8 mipmap) {
    memset(obj, 0, sizeof(*obj));
    obj->img = img_ptr;
    obj->width = wd;
    obj->height = ht;
    obj->fmt = fmt;
    obj->wrapS = wrap_s;
    obj->wrapT = wrap_t;
}

void GX_InitTexObjFilterMode(GXTexObj* obj, u8 minfilt, u8 magfilt) {
    obj->minFilter = minfilt;
    obj->magFilter = magfilt;
}

void GX_LoadTexObj(GXTexObj* obj, u8 mapid) {
    sMaps[mapid] = *obj;
    sLoads[mapid]++;
}

/* A texel of a texture gfx_fb.c draws with, as an EFB color */
static uint32_t tex_color(const GXTexObj* t, int u, int v) {
    const uint8_t* d = t->img;
    const uint8_t* tile = d + (((v >> 2) * ((t->width + 3) / 4) + (u >> 2)) * 64);
    int i = (v & 3) * 4 + (u & 3);
    uint32_t c, r, g, b;

    switch (t->fmt) {
        case GX_TF_RGBA8:
            return ((uint32_t)tile[i * 2 + 1] << 16) | ((uint32_t)tile[32 + i * 2] << 8) | tile[32 + i * 2 + 1];
        case GX_TF_RGB565:
            c = ((const uint16_t*)d)[tile16(t->width, u, v)];
            r = c >> 11, g = (c >> 5) & 0x3F, b = c & 0x1F;
            return (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((b << 3) | (b >> 2));
        case GX_TF_I8:
            c = d[tile8(t->width, u, v)];
            return (c << 16) | (c << 8) | c;
        default:
            printf("  model: texture format %u not modeled for drawing\n", t->fmt);
            abort();
    }
}

static uint32_t tex_depth(const GXTexObj* t, int u, int v) {
    const uint8_t* tile = (const uint8_t*)t->img + (((v >> 2) * ((t->width + 3) / 4) + (u >> 2)) * 64);
    int i = (v & 3) * 4 + (u & 3);

    return ((uint32_t)tile[i * 2 + 1] << 16) | ((const uint16_t*)(tile + 32))[i];
}

void GX_SetZTexture(u8 op, u8 fmt, u32 bias) {
    sZTex = op;
}

void GX_Begin(u8 primitve, u8 vtxfmt, u16 vtxcnt) {
    sVerts = 0;
}

void GX_Position3f32(f32 x, f32 y, f32 z) {
    if (sVerts < 4) {
        sQuadX[sVerts] = x;
        sQuadY[sVerts] = y;
    }
    sVerts++;
}

void GX_TexCoord2f32(f32 s, f32 t) {
}

/* gfx_fb.c's quads map EFB pixel (x0 + u, y0 + v) to texel (u, v) of GX_TEXMAP7 (and the depth of GX_TEXMAP6) */
void GX_End(void) {
    int x0 = (int)sQuadX[0], y0 = (int)sQuadY[0], x1 = (int)sQuadX[2], y1 = (int)sQuadY[2];
    int x, y;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            sEfb[y][x] = tex_color(&sMaps[GX_TEXMAP7], x - x0, y - y0);
            if (sZTex == GX_ZT_REPLACE) {
                sDepth[y][x] = tex_depth(&sMaps[GX_TEXMAP6], x - x0, y - y0);
            }
        }
    }
}

void GX_ClearVtxDesc(void) {
}
void GX_SetVtxDesc(u8 attr, u8 type) {
}
void GX_SetVtxAttrFmt(u8 vtxfmt, u32 vtxattr, u32 comptype, u32 compsize, u32 frac) {
}
void GX_SetNumChans(u8 num) {
}
void GX_SetNumTexGens(u32 nr) {
}
void GX_SetTexCoordGen(u16 texcoord, u32 tgen_typ, u32 tgen_src, u32 mtxsrc) {
}
void GX_SetNumIndStages(u8 nstages) {
}
void GX_SetTevDirect(u8 tevstage) {
}
void GX_SetTevOrder(u8 tevstage, u8 texcoord, u32 texmap, u8 color) {
}
void GX_SetTevColorIn(u8 tevstage, u8 a, u8 b, u8 c, u8 d) {
}
void GX_SetTevAlphaIn(u8 tevstage, u8 a, u8 b, u8 c, u8 d) {
}
void GX_SetTevColorOp(u8 tevstage, u8 tevop, u8 tevbias, u8 tevscale, u8 clamp, u8 tevregid) {
}
void GX_SetTevAlphaOp(u8 tevstage, u8 tevop, u8 tevbias, u8 tevscale, u8 clamp, u8 tevregid) {
}
void GX_SetNumTevStages(u8 num) {
}
void GX_SetZCompLoc(u8 before_tex) {
}
void GX_SetZMode(u8 enable, u8 func, u8 update_enable) {
}
void GX_SetFog(u8 type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor col) {
}
void GX_SetBlendMode(u8 type, u8 src_fact, u8 dst_fact, u8 op) {
}
void GX_SetAlphaCompare(u8 comp0, u8 ref0, u8 aop, u8 comp1, u8 ref1) {
}
void GX_SetColorUpdate(u8 enable) {
}
void GX_SetAlphaUpdate(u8 enable) {
}
void GX_LoadProjectionMtx(Mtx44 mt, u8 type) {
}
void GX_SetViewport(f32 xOrig, f32 yOrig, f32 wd, f32 ht, f32 nearZ, f32 farZ) {
}
void GX_SetScissor(u32 xOrigin, u32 yOrigin, u32 wd, u32 ht) {
}
void guOrtho(Mtx44 mt, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
}

/* ============================================================================================== */
/* The RAM path, as the renderer read and wrote images before                                     */
/* ============================================================================================== */

/* RGB565 -> RGBA5551 with the alpha bit set (the readbacks) */
static uint16_t ram_5551(uint16_t c565) {
    return (uint16_t)((c565 & 0xFFC0) | ((c565 & 0x1F) << 1) | 1);
}

/* N64 pixel (x, y) of the frame the EFB holds at GFX_SCALE: the half-size RGB565 readback, as RGBA5551 */
static uint16_t ref_frame(int x, int y) {
    uint32_t r = 0, g = 0, b = 0;
    int i;

    for (i = 0; i < 4; i++) {
        uint32_t c = sEfb[y * 2 + (i >> 1)][x * 2 + (i & 1)];

        r += (c >> 16) & 0xFF;
        g += (c >> 8) & 0xFF;
        b += c & 0xFF;
    }
    return ram_5551(enc565(((r / 4) << 16) | ((g / 4) << 8) | (b / 4)));
}

/* Pixel (x, y) of an off-screen image drawn at 1x in the canvas, as RGBA5551 */
static uint16_t ref_canvas(int x, int y) {
    return ram_5551(enc565(sEfb[y][x]));
}

/* gfx_tex.c's RGB5A3 texel of an RGBA5551 pixel */
static uint16_t tex_5a3(uint16_t c) {
    if (c & 1) {
        return (uint16_t)(0x8000 | (c >> 1));
    }
    return (uint16_t)((((c >> 12) & 0xF) << 8) | (((c >> 7) & 0xF) << 4) | ((c >> 2) & 0xF));
}

/* RGB5A3 texel (u, v) of a texture gfx_fb_bind_image loaded */
static uint16_t bound_texel(int map, int u, int v) {
    return ((const uint16_t*)sMaps[map].img)[tile16(sMaps[map].width, u, v)];
}

/* gfx_tex_bind_image as gfx_fb_bind_image's way back to RAM: gfx_tex.c's RGB5A3 texels of the image's RAM */
static uint16_t sRamTex[W * H];
static int sRamBinds;
static uint32_t sRamBindAddr;

static bool ram_bind_image(const void* addr, uint8_t fmt, uint8_t siz, uint16_t width, uint16_t height,
                           uint16_t stride, const void* tlut, bool tlutIA, bool linear, int texMap,
                           GfxTexBinding* out) {
    const uint16_t* p = addr;
    GXTexObj obj;
    int x, y;

    sRamBinds++;
    sRamBindAddr = (uint32_t)((uintptr_t)addr - gGfxHostRamBias);
    if (fmt != G_IM_FMT_RGBA || siz != G_IM_SIZ_16b || width != W || height != H || stride != W || tlut != NULL) {
        return false;
    }
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            sRamTex[tile16(W, x, y)] = tex_5a3(p[y * W + x]);
        }
    }
    GX_InitTexObj(&obj, sRamTex, W, H, GX_TF_RGB5A3, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&obj, linear ? GX_LINEAR : GX_NEAR, linear ? GX_LINEAR : GX_NEAR);
    GX_LoadTexObj(&obj, (u8)texMap);
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->width = width;
    out->height = height;
    out->sShiftScale = out->tShiftScale = 1.0f;
    out->linear = linear;
    return true;
}

static void efb_random(int x0, int y0, int x1, int y1) {
    int x, y;

    for (y = y0; y < y1; y++) {
        for (x = x0; x < x1; x++) {
            sEfb[y][x] = rnd() & 0xFFFFFF;
            sDepth[y][x] = rnd() & 0xFFFFFF;
        }
    }
}

static void ram_fill(uint32_t addr, uint32_t bytes, uint8_t v) {
    memset(ptr(addr), v, bytes);
}

/* A task whose frame (FRAME, 320 wide) was drawn whole with random pixels */
static void frame_task(void) {
    gfx_fb_task_begin();
    gfx_fb_set_frame(FRAME, W);
    efb_random(0, 0, EFB_W, H * 2);
    gfx_fb_frame_drawn(0, H);
}

/* Texels of the texture bound to GX_TEXMAP0 that differ from gfx_tex.c's RGB5A3 of the RAM path's pixels */
static int frame_texture_errors(void) {
    int x, y, bad = 0;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            bad += bound_texel(GX_TEXMAP0, x, y) != tex_5a3(ref_frame(x, y));
        }
    }
    return bad;
}

static int image_texture_errors(uint32_t addr) {
    int x, y, bad = 0;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            bad += bound_texel(GX_TEXMAP0, x, y) != tex_5a3(ram16(addr)[y * W + x]);
        }
    }
    return bad;
}

/* A background bound and drawn (gfx_fb_image_done: its draws were issued) */
static bool bind(uint32_t addr, bool linear, GfxTexBinding* b) {
    bool ok;

    memset(b, 0xA5, sizeof(*b));
    ok = gfx_fb_bind_image(ptr(addr), W, H, W, linear, GX_TEXMAP0, ram_bind_image, b);
    gfx_fb_image_done();
    return ok;
}

/* A background bound whose draws are still to come */
static bool bind_pending(uint32_t addr, GfxTexBinding* b) {
    memset(b, 0xA5, sizeof(*b));
    return gfx_fb_bind_image(ptr(addr), W, H, W, false, GX_TEXMAP0, ram_bind_image, b);
}

/* ============================================================================================== */
/* Tests                                                                                          */
/* ============================================================================================== */

static void test_frame_texture(void) {
    static uint16_t expect[W * H];
    GfxTexBinding b;
    int x, y, bad, writes;

    printf("frame texture\n");
    frame_task();
    ram_fill(FRAME, W * H * 2, 0xEE);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            expect[y * W + x] = ref_frame(x, y);
        }
    }
    writes = sRamWrites;
    CHECK(bind(FRAME, false, &b), "the frame is served");
    CHECK(b.valid && b.width == W && b.height == H && b.sShiftScale == 1.0f && b.tShiftScale == 1.0f &&
              b.sOffset == 0.0f && b.tOffset == 0.0f && !b.linear,
          "binding as gfx_tex_bind_image makes it");
    CHECK(sMaps[GX_TEXMAP0].fmt == GX_TF_RGB5A3 && sMaps[GX_TEXMAP0].width == W && sMaps[GX_TEXMAP0].height == H &&
              sMaps[GX_TEXMAP0].wrapS == GX_CLAMP && sMaps[GX_TEXMAP0].minFilter == GX_NEAR,
          "an RGB5A3 320x240 texture, clamped, point sampled");
    bad = frame_texture_errors();
    CHECK(bad == 0, "%d texels differ from the RAM path's", bad);
    CHECK(((uint8_t*)ptr(FRAME))[0] == 0xEE && ((uint8_t*)ptr(FRAME))[W * H * 2 - 1] == 0xEE && sRamWrites == writes,
          "the frame's RAM stays pending");

    // Read as RAM: the same pixels as the readback before
    gfx_fb_sync_ram(ptr(FRAME), W * H * 2);
    CHECK(memcmp(ptr(FRAME), expect, sizeof(expect)) == 0, "the frame's RAM is the readback's pixels");
    CHECK(sRamWrites == writes + 1 && sRamWriteAddr == FRAME && sRamWriteBytes == W * H * 2,
          "gfx_tex.c told about the write");

    // Rows in RAM and not drawn since still count as the frame's; filtering is the caller's choice
    CHECK(bind(FRAME, true, &b) && b.linear && sMaps[GX_TEXMAP0].minFilter == GX_LINEAR &&
              sMaps[GX_TEXMAP0].magFilter == GX_LINEAR && frame_texture_errors() == 0,
          "bilinear, rows written to RAM: served");

    // A draw into the frame after the copy: copied again
    efb_random(0, 20, EFB_W, 40);
    gfx_fb_frame_drawn(10, 20);
    CHECK(bind(FRAME, false, &b) && frame_texture_errors() == 0, "drawn again: the new pixels");

    // Not served: rows that are neither drawn in this task nor written from the EFB, another image size or stride,
    // another frame width
    gfx_fb_task_begin();
    gfx_fb_set_frame(FRAME, W);
    gfx_fb_frame_drawn(0, 100);
    CHECK(!bind(FRAME, false, &b), "rows 100-239 neither drawn nor written in this task: not served");
    frame_task();
    CHECK(!gfx_fb_bind_image(ptr(FRAME), W, H - 1, W, false, GX_TEXMAP0, ram_bind_image, &b) &&
              !gfx_fb_bind_image(ptr(FRAME), W - 4, H, W, false, GX_TEXMAP0, ram_bind_image, &b) &&
              !gfx_fb_bind_image(ptr(FRAME), W, H, W + 4, false, GX_TEXMAP0, ram_bind_image, &b) &&
              !gfx_fb_bind_image(ptr(FRAME), W, H, W, false, GX_TEXMAP0, NULL, &b),
          "other sizes and strides, no way back to RAM: not served");
    gfx_fb_task_begin();
    gfx_fb_set_frame(FRAME, 576);
    gfx_fb_frame_drawn(0, H);
    CHECK(!bind(FRAME, false, &b), "a 576-wide frame: not served");

    // The VisMono copy (RGB565 in sSaveColor) is unaffected
    frame_task();
    {
        GXTexObj obj;

        CHECK(gfx_fb_frame_texture(&obj) && obj.fmt == GX_TF_RGB565 && obj.width == W && obj.height == H,
              "VisMono's frame texture");
        bad = 0;
        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                bad += ram_5551(((const uint16_t*)obj.img)[tile16(W, x, y)]) != ref_frame(x, y);
            }
        }
        CHECK(bad == 0, "VisMono's texture: %d texels differ", bad);
    }
}

/* Off-screen pass X: what the canvas holds when the pass ends goes to X's RAM, and stays X's texture */
static void test_pass(void) {
    static uint16_t frame[W * H], canvas[W * H];
    static uint32_t efbSaved[H][W], depthSaved[H][W];
    GfxTexBinding b;
    int x, y, bad;

    printf("off-screen pass\n");
    frame_task();
    ram_fill(FRAME, W * H * 2, 0xEE);
    ram_fill(IMG_X, W * H * 2, 0x11);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            frame[y * W + x] = ref_frame(x, y);
            efbSaved[y][x] = sEfb[y][x];
            depthSaved[y][x] = sDepth[y][x];
        }
    }
    CHECK(gfx_fb_canvas_begin(IMG_X, G_IM_FMT_RGBA, G_IM_SIZ_16b, W) && gfx_fb_canvas_active(), "pass starts");
    CHECK(((uint8_t*)ptr(FRAME))[0] == 0xEE, "the frame's RAM is not written when a pass starts");

    // Drawn into the canvas: the EFB's top-left 320x240
    efb_random(0, 0, W, H);
    gfx_fb_canvas_draw(0, 0, W, H, true);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            canvas[y * W + x] = ref_canvas(x, y);
        }
    }
    // The frame read during the pass: its pixels from when the pass started
    gfx_fb_sync_ram(ptr(FRAME), W * H * 2);
    CHECK(memcmp(ptr(FRAME), frame, sizeof(frame)) == 0, "frame read during the pass: the pixels it had");

    gfx_fb_canvas_end();
    CHECK(!gfx_fb_canvas_active(), "pass ends");
    CHECK(memcmp(ptr(IMG_X), canvas, sizeof(canvas)) == 0, "the image's RAM is what the canvas held");
    bad = 0;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            bad += sEfb[y][x] != efbSaved[y][x] || sDepth[y][x] != depthSaved[y][x];
        }
    }
    CHECK(bad == 0, "the frame's pixels and depth are back under the canvas (%d differ)", bad);

    CHECK(bind(IMG_X, false, &b) && b.valid && image_texture_errors(IMG_X) == 0,
          "the written image is served: gfx_tex.c's texels of its RAM");
    CHECK(!gfx_fb_bind_image((uint8_t*)ptr(IMG_X) + 64, W, H, W, false, GX_TEXMAP0, ram_bind_image, &b),
          "another address: not served");

    // Later tasks: served while its RAM is unchanged
    gfx_fb_task_begin();
    CHECK(bind(IMG_X, true, &b) && image_texture_errors(IMG_X) == 0, "next task, RAM unchanged: served");
    ((uint8_t*)ptr(IMG_X))[12345] ^= 0x40;
    CHECK(bind(IMG_X, false, &b), "same task: not checked again");
    gfx_fb_task_begin();
    CHECK(!bind(IMG_X, false, &b), "rewritten between tasks: not served");
    ((uint8_t*)ptr(IMG_X))[12345] ^= 0x40;
    gfx_fb_task_begin();
    CHECK(!bind(IMG_X, false, &b), "and not again (the texture was dropped)");
}

/* A pass that does not draw all of its image, an I8 image, a depth write over a written image */
static void test_partial(void) {
    static uint16_t old[W * H], expect[W * H];
    GfxTexBinding b;
    int x, y, bad;

    printf("partial images\n");
    frame_task();
    for (x = 0; x < W * H; x++) {
        old[x] = (uint16_t)rnd();
    }
    memcpy(ptr(IMG_Y), old, sizeof(old));
    CHECK(gfx_fb_canvas_begin(IMG_Y, G_IM_FMT_RGBA, G_IM_SIZ_16b, W), "pass");
    efb_random(0, 0, W, H);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            bool in = x >= 10 && x < 110 && y >= 20 && y < 70;

            expect[y * W + x] = in ? ref_canvas(x, y) : old[y * W + x];
        }
    }
    gfx_fb_canvas_draw(10, 20, 110, 70, true);
    gfx_fb_canvas_end();
    bad = 0;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            bad += ram16(IMG_Y)[y * W + x] != expect[y * W + x];
        }
    }
    CHECK(bad == 0, "only the drawn rectangle goes to RAM (%d pixels differ)", bad);
    CHECK(!bind(IMG_Y, false, &b), "a partly written image is not a texture");

    // The renderer's own RAM write over a written image drops its texture: the z-buffer's depth into IMG_X
    frame_task();
    CHECK(gfx_fb_canvas_begin(IMG_X, G_IM_FMT_RGBA, G_IM_SIZ_16b, W), "pass");
    efb_random(0, 0, W, H);
    gfx_fb_canvas_draw(0, 0, W, H, true);
    gfx_fb_canvas_end();
    CHECK(bind(IMG_X, false, &b), "written whole: served");
    gfx_fb_task_end(ptr(IMG_X), true);
    CHECK(!bind(IMG_X, false, &b), "depth written over it: not served");
}

/* I8 images get the canvas's red channel */
static void test_i8(void) {
    static uint8_t expect[W * H];
    int x, y;

    printf("I8 image\n");
    frame_task();
    ram_fill(IMG_I8, W * H, 0x33);
    CHECK(gfx_fb_canvas_begin(IMG_I8, G_IM_FMT_I, G_IM_SIZ_8b, W), "I8 pass");
    efb_random(0, 0, W, H);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            expect[y * W + x] = (uint8_t)(sEfb[y][x] >> 16);
        }
    }
    gfx_fb_canvas_draw(0, 0, W, H, true);
    gfx_fb_canvas_end();
    CHECK(memcmp(ptr(IMG_I8), expect, sizeof(expect)) == 0, "the I8 image is the canvas's red");
}

/* A draw that reads the canvas (blending) loads the image into it; the transfer buffer that held the frame's pending
 * rows is reused, so they go to RAM first */
static void test_canvas_load(void) {
    static uint16_t frame[W * H], old[W * H];
    int x, y, bad;

    printf("canvas loaded for a draw that reads it\n");
    frame_task();
    ram_fill(FRAME, W * H * 2, 0xEE);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            frame[y * W + x] = ref_frame(x, y);
            old[y * W + x] = (uint16_t)rnd();
        }
    }
    memcpy(ptr(IMG_Y), old, sizeof(old));
    CHECK(gfx_fb_canvas_begin(IMG_Y, G_IM_FMT_RGBA, G_IM_SIZ_16b, W), "pass");
    gfx_fb_canvas_draw(0, 0, 50, 50, false);
    CHECK(memcmp(ptr(FRAME), frame, sizeof(frame)) == 0, "the frame's pending rows went to RAM first");
    bad = 0;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint16_t p = old[y * W + x];
            uint16_t c565 = (uint16_t)((p & 0xFFC0) | ((p >> 5) & 0x20) | ((p >> 1) & 0x1F));

            bad += enc565(sEfb[y][x]) != c565;
        }
    }
    CHECK(bad == 0, "the canvas holds the image (%d pixels differ)", bad);
    gfx_fb_canvas_end();
    bad = 0;
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            uint16_t p = old[y * W + x];
            uint16_t e = (x < 50 && y < 50) ? (uint16_t)(p | 1) : p;

            bad += ram16(IMG_Y)[y * W + x] != e;
        }
    }
    CHECK(bad == 0, "drawn pixels back to RAM (%d differ)", bad);
}

/* Texels of the texture bound to GX_TEXMAP0 that differ from `expect` (RGB5A3, row by row) */
static int texture_errors(const uint16_t* expect) {
    int x, y, bad = 0;

    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            bad += bound_texel(GX_TEXMAP0, x, y) != expect[y * W + x];
        }
    }
    return bad;
}

/* An off-screen image IMG_X drawn whole with random pixels (written to RAM, kept as the transfer buffer's texture) */
static void pass_x(void) {
    CHECK(gfx_fb_canvas_begin(IMG_X, G_IM_FMT_RGBA, G_IM_SIZ_16b, W), "pass");
    efb_random(0, 0, W, H);
    gfx_fb_canvas_draw(0, 0, W, H, true);
    gfx_fb_canvas_end();
}

/* gfx_gx_image_rect selects its render target and prepares the canvas after the background was bound and before it
 * queues the draw: a served texture whose memory gfx_fb.c needs before then moves to RAM, with the same texels */
static void test_served_until_drawn(void) {
    static uint16_t frame[W * H], expect[W * H];
    GfxTexBinding b;
    int x, y, binds;

    printf("served textures until their draws are issued\n");

    // The frame drawn into the frame right after an off-screen pass: the pass ends in the draw's target switch, and
    // its copy to RAM goes through the transfer buffer that holds the frame's texture
    frame_task();
    ram_fill(FRAME, W * H * 2, 0xEE);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            frame[y * W + x] = ref_frame(x, y);
            expect[y * W + x] = tex_5a3(frame[y * W + x]);
        }
    }
    CHECK(gfx_fb_canvas_begin(IMG_X, G_IM_FMT_RGBA, G_IM_SIZ_16b, W), "pass");
    efb_random(0, 0, W, H);
    gfx_fb_canvas_draw(0, 0, W, H, true);
    binds = sRamBinds;
    CHECK(bind_pending(FRAME, &b) && texture_errors(expect) == 0, "the frame is served during the pass");
    gfx_fb_canvas_end();
    CHECK(sRamBinds == binds + 1 && sRamBindAddr == FRAME, "the pass's end moved the binding to RAM (%d binds)",
          sRamBinds - binds);
    CHECK(texture_errors(expect) == 0, "bound from RAM: the same texels");
    CHECK(memcmp(ptr(FRAME), frame, sizeof(frame)) == 0, "the frame's rows went to RAM first");
    gfx_fb_image_done();

    // A written image drawn into a new off-screen image while the frame has pending rows: the pass's start copies the
    // frame into the transfer buffer
    frame_task();
    pass_x();
    efb_random(0, 0, EFB_W, 40);
    gfx_fb_frame_drawn(0, 20);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            expect[y * W + x] = tex_5a3(ram16(IMG_X)[y * W + x]);
        }
    }
    binds = sRamBinds;
    CHECK(bind_pending(IMG_X, &b) && texture_errors(expect) == 0, "the image is served");
    CHECK(gfx_fb_canvas_begin(IMG_Y, G_IM_FMT_RGBA, G_IM_SIZ_16b, W), "pass");
    CHECK(sRamBinds == binds + 1 && sRamBindAddr == IMG_X && texture_errors(expect) == 0,
          "the pass's start moved the binding to RAM, the same texels (%d binds)", sRamBinds - binds);
    gfx_fb_canvas_draw(0, 0, W, H, true);
    gfx_fb_image_done();
    gfx_fb_canvas_end();

    // A written image drawn into the canvas by a draw that reads the canvas: loading the canvas reuses the buffer
    frame_task();
    gfx_fb_sync_ram(ptr(FRAME), W * H * 2);
    pass_x();
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            expect[y * W + x] = tex_5a3(ram16(IMG_X)[y * W + x]);
        }
    }
    CHECK(gfx_fb_canvas_begin(IMG_Y, G_IM_FMT_RGBA, G_IM_SIZ_16b, W), "pass");
    binds = sRamBinds;
    CHECK(bind_pending(IMG_X, &b) && texture_errors(expect) == 0, "the image is served");
    gfx_fb_canvas_draw(0, 0, 50, 50, false);
    CHECK(sRamBinds == binds + 1 && sRamBindAddr == IMG_X && texture_errors(expect) == 0,
          "the canvas load moved the binding to RAM, the same texels (%d binds)", sRamBinds - binds);
    gfx_fb_image_done();
    gfx_fb_canvas_end();

    // The frame read outside a pass by a RAM reader while still bound: written once, then bound from RAM
    frame_task();
    ram_fill(FRAME, W * H * 2, 0xEE);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            frame[y * W + x] = ref_frame(x, y);
            expect[y * W + x] = tex_5a3(frame[y * W + x]);
        }
    }
    binds = sRamBinds;
    x = sRamWrites;
    CHECK(bind_pending(FRAME, &b), "the frame is served");
    gfx_fb_sync_ram(ptr(FRAME), W * H * 2);
    CHECK(sRamBinds == binds + 1 && texture_errors(expect) == 0 && memcmp(ptr(FRAME), frame, sizeof(frame)) == 0 &&
              sRamWrites == x + 1,
          "written to RAM once, bound from there (%d binds, %d writes)", sRamBinds - binds, sRamWrites - x);
    gfx_fb_image_done();

    // Once its draws were issued, the buffer is reused without moving anything
    frame_task();
    binds = sRamBinds;
    CHECK(bind(FRAME, false, &b), "the frame is served");
    pass_x();
    CHECK(gfx_fb_canvas_begin(IMG_Y, G_IM_FMT_RGBA, G_IM_SIZ_16b, W), "pass");
    gfx_fb_canvas_draw(0, 0, 50, 50, false);
    gfx_fb_canvas_end();
    CHECK(sRamBinds == binds, "done: nothing bound from RAM (%d binds)", sRamBinds - binds);
}

/* N64 depth value of a 24-bit window depth: the 18-bit z (screen z << 8, screen z = depth * G_MAXZ) with a 3-bit
 * exponent counting its leading ones (at most 7) and the 11 bits after them */
static uint16_t ref_n64_depth(uint32_t d) {
    uint32_t z, e = 0;

    if (d >= 0xFFFFFF) {
        return 0xFFFC;
    }
    z = ((d >> 4) * G_MAXZ) >> 12;
    while (e < 7 && (z & (0x20000u >> e))) {
        e++;
    }
    if (e == 7) {
        return (uint16_t)((7 << 13) | ((z & 0x7FF) << 2));
    }
    return (uint16_t)((e << 13) | (((z >> (6 - e)) & 0x7FF) << 2));
}

static void test_depth(void) {
    int x, y, bad = 0;

    printf("depth\n");
    frame_task();
    for (y = 0; y < H * 2; y += 7) {
        sDepth[y][(y * 13) % EFB_W] = 0xFFFFFF; // never drawn
    }
    for (x = 0; x < EFB_W; x++) {
        sDepth[2][x] = 0xFFFFFF; // a whole sample row never drawn
    }
    ram_fill(ZBUF, W * H * 2, 0x77);
    gfx_fb_task_end(ptr(ZBUF), true);
    for (y = 0; y < H; y++) {
        for (x = 0; x < W; x++) {
            bad += ram16(ZBUF)[y * W + x] != ref_n64_depth(sDepth[((y & ~3) + 1) * 2][((x & ~3) + 1) * 2]);
        }
    }
    CHECK(bad == 0, "%d z-buffer pixels differ", bad);
    CHECK(ram16(ZBUF)[0] == 0xFFFC, "never drawn: G_MAXFBZ");

    // Not written when the task did not clear the z-buffer, or when it is the frame
    frame_task();
    ram_fill(ZBUF, W * H * 2, 0x77);
    gfx_fb_task_end(ptr(ZBUF), false);
    CHECK(((uint8_t*)ptr(ZBUF))[100] == 0x77, "no depth without a z clear");
}

int main(void) {
    uint8_t* mem = aligned_alloc(4096, RAM_SIZE);

    if (mem == NULL) {
        return 2;
    }
    memset(mem, 0, RAM_SIZE);
    sRam = mem;
    gGfxHostRamBias = (uintptr_t)mem - RAM_BASE;

    gfx_fb_init();
    CHECK(gfx_fb_ready(), "EFB copies reach RAM (probe)");
    test_frame_texture();
    test_pass();
    test_partial();
    test_i8();
    test_canvas_load();
    test_served_until_drawn();
    test_depth();
    printf("gfx_fb host test: %d checks, %d failures (%d EFB copies)\n", sChecks, sFailures, sCopies);
    free(mem);
    return sFailures ? 1 : 0;
}
