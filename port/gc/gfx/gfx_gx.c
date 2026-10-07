/**
 * GX backend of the N64 display list renderer: GX and EFB/XFB setup, render targets, viewport and
 * scissor, the projection trick that feeds N64 clip coordinates through GX's own perspective divide,
 * triangle batching, texture and fill rectangles, EFB -> XFB copies and presenting frames on the VI
 * thread. See DESIGN.md ("Projection on GX", "Frames and video", "Framebuffer effects").
 *
 * Render targets: the frame is drawn at GFX_SCALE over the whole EFB. Another color image (an off-screen
 * pass) is drawn at 1x into the EFB's top-left corner, the canvas, which gfx_fb.c saves and restores around
 * the pass and copies to RAM. Everything that depends on the target's scale goes through sScale.
 *
 * Depth: GX clip space keeps z in [-w, 0] (GX clips outside it) and the viewport maps z/w to window
 * depth (z/w) * (far - near) + far, so window depth grows with distance as on the N64, and the depth
 * test is LEQUAL against a buffer cleared to GX_MAX_Z24. The N64 window depth
 * d = (ndc * vscale[2] + vtrans[2]) / G_MAXZ is reproduced exactly, as the EFB depth
 * (d + GFX_DEPTH_UNDER) / (1 + GFX_DEPTH_UNDER) (gfx_internal.h), by submitting z_gx = GX_ZK * (ndc - 1) * w and
 * deriving the GX viewport near/far from the N64 viewport's z scale and translation (gx_viewport_n64).
 * GX_ZK < 1/2 puts GX's near clip plane closer to the eye than the N64's (see GX_NDC_NEAR), at EFB depth 0; the
 * far clip plane is the N64's, which F3DZEX2 also clips against.
 *
 * CPU/GPU overlap: a task ends after queueing the EFB -> XFB copy and a draw sync token behind it, without waiting
 * for GX. GX finishes the frame while the game runs its next frame; gfx_gx_present shows the XFB once the token has
 * come back (nearly always already the case), and the next task begins by waiting for GX (gfx_tex.c and gfx_fb.c
 * expect an idle GX at task start). -DGFX_LAZY_SYNC=0 waits at the end of each task instead.
 *
 * Texture coordinates divide by the texture size like gfx_tex_uv; for power-of-two sizes the division is a
 * multiplication by the exact reciprocal (the same float), prepared when the binding changes (gx_uv_update).
 */
#include <gccore.h>
#include <math.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <string.h>
#include <unistd.h>
#include "gc_ogc.h"
#include "gfx_internal.h"
#include "gfx_prof.h"

#define GX_FIFO_SIZE (256 * 1024)
#ifndef XFB_SLOTS
#define XFB_SLOTS 4 /* -DXFB_SLOTS=2 tests the low-memory path */
#endif
#define XFB_SLOTS_MIN 2
/* 1: a task ends without waiting for GX; the display copy is fenced with a draw sync token (gfx_gx_present) and the
 * next task begins by waiting for GX. 0: wait at the end of each task, as before. */
#ifndef GFX_LAZY_SYNC
#define GFX_LAZY_SYNC 1
#endif
#ifndef GFX_VTX_CHECK
#define GFX_VTX_CHECK 0
#endif
#ifndef GFX_SHADOW_PROBE
#define GFX_SHADOW_PROBE 0
#endif
#define BATCH_TRIS 128
#define BATCH_VERTS (BATCH_TRIS * 3)

/* Decal surfaces are drawn this much (EFB depth, 0..1) toward the viewer: GX has no polygon offset.
 * 2^-GFX_DECAL_BIAS_SHIFT: 2^-16 is 256 steps of the 24-bit depth buffer, about 6 units at 1000 units from a
 * 10-unit near plane. The viewport moves its far end by it, so a decal's bias shrinks to 0 at the near clip plane
 * (no depth goes below 0) and is 3/4 of it and more beyond the N64's near plane. */
#ifndef GFX_DECAL_BIAS_SHIFT
#define GFX_DECAL_BIAS_SHIFT 16
#endif
#define DECAL_BIAS (1.0f / (float)(1u << GFX_DECAL_BIAS_SHIFT))

/* Persp vertices whose z leaves the fitted plane z = a*w + b by more than this (relative to w) cannot
 * be drawn through the GX perspective matrix; they are divided on the CPU instead. */
#define PERSP_FIT_TOLERANCE 1.0e-3f

/* N64 NDC z at which GX clips near. F3DZEX2 NoN does not clip at the near plane (ndc -1): it clips at
 * w = 0 and clamps the vertex screen z to 0. GX always clips at z_gx = -w, so z_gx is scaled down to put
 * that plane closer to the eye: ndc = 1 - 2n/w for a projection with near n (and a far plane far away), so
 * ndc -7 is n/4. Its N64 window depth through the standard viewport, -GFX_DEPTH_UNDER, is EFB depth 0: geometry
 * between the two near planes keeps its order in depth, in front of everything beyond the N64's near plane. */
#define GX_NDC_NEAR (-(2.0f * GFX_DEPTH_UNDER + 1.0f))
#define GX_ZK (1.0f / (1.0f - GX_NDC_NEAR)) /* z_gx / w = GX_ZK * (ndc - 1), so ndc 1 (far) is z_gx = 0 */

#define STATS_INTERVAL_MS 5000
#define LOG_LIMIT 8

/* gx_prepare's tile for gfx_gx_image_rect: textures are bound by the caller */
#define GX_TILE_IMAGE (-2)

typedef struct {
    float x, y, z;
    u32 color;
    float u0, v0, u1, v1;
} BatchVtx;

/* How the current projection reaches GX */
typedef enum {
    PROJ_PERSP, /* submit (x, y, -w) through a GX perspective matrix */
    PROJ_ORTHO, /* no w column: submit NDC (x/w, y/w, z/w) through a GX orthographic matrix */
    PROJ_CPU    /* anything else: divide on the CPU, draw orthographically */
} ProjKind;

/* What a draw needs from the GX transform: projection matrix and viewport */
typedef enum {
    VMODE_NONE,
    VMODE_PERSP,  /* N64 perspective through the GX perspective matrix, N64 viewport */
    VMODE_NDC,    /* NDC coordinates divided on the CPU (ortho, CPU path, primitive depth), N64 viewport */
    VMODE_SCREEN  /* N64 screen pixels (rectangles), full EFB viewport */
} VMode;

typedef enum {
    TARGET_FRAME,  /* the task's frame, at GFX_SCALE */
    TARGET_DEPTH,  /* FILL into the z image: depth clear */
    TARGET_CANVAS, /* another color image, at 1x in the canvas (gfx_fb.c) */
    TARGET_SKIP    /* a color image gfx_fb.c cannot draw: draws are skipped */
} Target;

/* Render mode bits (othermode L) that gfx_gbi.h does not list */
#define RM_IM_RD 0x40 /* the blender reads the color image */

typedef struct {
    void* xfb;
    u32 key;    /* N64 framebuffer (KSEG0 form) whose frame this slot holds, 0 = none */
    u32 stamp;  /* order of the last render or presentation, for LRU */
    bool busy;  /* being copied into */
    bool copying; /* the copy was issued but GX may not have finished it: wait for `token` before showing it */
    u16 token;  /* draw sync token sent after the copy */
} XfbSlot;

typedef struct {
    u32 type; /* (no padding: the cache compares with memcmp) */
    float p[6];
} GxProj;

typedef struct {
    float x, y, w, h, n, f;
} GxViewport;

static bool sReady;
static GXRModeObj* sMode;
static void* sFifo;

/* Frames and presentation. The slot table is shared with the VI thread (gfx_gx_present); every access
 * runs with interrupts disabled, which on one CPU also excludes the other thread. */
static XfbSlot sSlots[XFB_SLOTS];
static int sSlotCount;
static int sPresented = -1; /* slot most recently handed to the VI */
static u32 sStamp;
static u16 sCopyToken;      /* draw sync token of the last display copy */
static bool sGpuBusy;       /* a task ended without waiting for GX (the next task begins with the wait) */

/* Render target of the current task */
static u32 sFrameKey;    /* the frame's color image (KSEG0 form), 0 until the first draw */
static u32 sTargetCimg;  /* gGfxRdp state sTarget was computed for */
static u32 sTargetZimg;
static u8 sTargetSiz;
static u16 sTargetWidth;
static bool sTargetFill;
static Target sTarget;
static bool sTargetValid;
static u32 sCanvasKey;   /* color image drawn in the canvas, 0 when the EFB holds only the frame */
static u8 sCanvasSiz;    /* its pixel size (its width is sTargetW) */
static int sScale = GFX_SCALE;                             /* EFB pixels per N64 pixel for the target */
static int sTargetW = GFX_EFB_WIDTH, sTargetH = GFX_EFB_HEIGHT; /* EFB area of the target */
static int sScissorY0, sScissorY1;                         /* the scissor's N64 rows (gx_apply_scissor) */
static bool sDepthFilled;  /* the task cleared the z-buffer... */
static bool sDepthColored; /* ...and then drew colors into its RAM (PreRender captures, Lens of Truth) */

/* EFB contents: after a copy the EFB holds the copy clear color and far depth everywhere */
static GXColor sClearColor = { 0, 0, 0, 255 };
static bool sColorClean;
static GXColor sCleanColor;
static bool sDepthClean;

/* Projection set by gfx_rsp */
static ProjKind sProjKind = PROJ_ORTHO;
static float sProjA, sProjB;

/* Draw state as last applied to GX */
static bool sStateValid; /* false: re-apply everything before the next draw */
static int sKind = -1;   /* GfxPrimKind the TEV was set up for */
static GfxTevInfo sInfo;
static int sTexTile = -1; /* tile bound to GX_TEXMAP0 (TEXEL1 is the next tile) */
static bool sTexBound[2];
/* Neutral until bound: TEXCOORD0 is still computed when only TEXEL1 is used (no division by 0) */
static GfxTexBinding sTex[2] = {
    { .width = 1, .height = 1, .sShiftScale = 1.0f, .tShiftScale = 1.0f },
    { .width = 1, .height = 1, .sShiftScale = 1.0f, .tShiftScale = 1.0f },
};
/* gfx_tex_uv of each binding, prepared when sTex changes (gx_uv_update): a division by a power of two is a
 * multiplication by its exact reciprocal, which gives the same float; other sizes still divide. */
typedef struct {
    float sScale, sOffset, tScale, tOffset;
    float width, height;
    float invWidth, invHeight; /* 0: divide */
} GxUv;
static GxUv sUv[2] = {
    { 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f },
    { 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1.0f },
};
static int sNumTex = -1;          /* texture coordinates in the vertex format */
static VMode sVMode = VMODE_NONE; /* loaded into GX */
static VMode sTriVMode;           /* for triangles under the current state (one may still take the CPU path) */
static bool sZPrim;               /* triangles use the primitive depth (G_ZS_PRIM) */
static float sPrimNdcZ;           /* N64 NDC z of the primitive depth */
static bool sScissorEmpty;
static GxProj sGxProj;
static bool sGxProjValid;
static GxViewport sGxVp;
static bool sGxVpValid;

static GXTexObj sDummyTex;
static u8 sDummyTexData[32] ATTRIBUTE_ALIGN(32);

/* Batch of triangles waiting for one GX_Begin */
static BatchVtx sBatch[BATCH_VERTS];
static int sBatchCount;

static struct {
    u32 tasks, frames;
    u32 tris, cpuTris, droppedTris, rects, fillQuads, depthQuads, fillsSkipped, offscreen, canvasDraws, visMono, batches;
    u64 taskTicks, maxTaskTicks;
} sStats;
static u64 sStatsStart;
static u64 sTaskStart;
static int sLogCount, sLogBindCount;
static bool sLoggedNoFrame, sLoggedCpuProj, sLoggedFillZ, sLoggedCopyWait;

static inline u32 gx_key(u32 addr) {
    return (addr == 0) ? 0 : ((addr & 0x1FFFFFFF) | 0x80000000);
}

static inline u32 gx_rgba(const GXColor* c) {
    return ((u32)c->r << 24) | ((u32)c->g << 16) | ((u32)c->b << 8) | c->a;
}

/* G_SETPRIMDEPTH z as window depth (0..1, the scale of gx_viewport_n64). The RDP's z is the RSP's screen
 * z (ndc * vscale[2] + vtrans[2], at most G_MAXZ) shifted left by 5, so 0x7FFF is just past G_MAXZ. */
static inline float gx_prim_depth(void) {
    float d = (gGfxRdp.primDepthZ & 0x7FFF) * (1.0f / (32.0f * G_MAXZ));

    return (d < 1.0f) ? d : 1.0f;
}

/* ============================================================================================== */
/* Init                                                                                           */
/* ============================================================================================== */

static void gx_clear_xfb(void* xfb, u32 size) {
    u32* p = xfb;
    u32 i;

    // YUY2 black: Y=16, U=V=128
    for (i = 0; i < size / 4; i++) {
        p[i] = 0x10801080;
    }
    DCFlushRange(xfb, size);
}

/* State that only gfx_gx_init sets, and gfx_fb.c's quads change: channels and texture coordinate generation */
static void gx_base_state(void) {
    GX_SetNumChans(1);
    GX_SetChanCtrl(GX_COLOR0A0, GX_DISABLE, GX_SRC_REG, GX_SRC_VTX, GX_LIGHTNULL, GX_DF_NONE, GX_AF_NONE);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);
}

void gfx_gx_init(void) {
    u32 xfbSize;
    u32 scan;
    f32 yScale;
    Mtx identity;
    int i;

    sMode = (GXRModeObj*)gc_ogc_video_mode();
    if (sMode == NULL) {
        gc_log("gfx: no video mode, renderer disabled");
        return;
    }

    sFifo = gc_mem_alloc(GX_FIFO_SIZE, 32);
    if (sFifo == NULL) {
        gc_log("gfx: no memory for the GX FIFO, renderer disabled");
        return;
    }
    memset(sFifo, 0, GX_FIFO_SIZE);
    DCFlushRange(sFifo, GX_FIFO_SIZE);

    xfbSize = VIDEO_GetFrameBufferSize(sMode);
    for (i = 0; i < XFB_SLOTS; i++) {
        sSlots[i].xfb = gc_mem_alloc(xfbSize, 32);
        if (sSlots[i].xfb == NULL) {
            break;
        }
        gx_clear_xfb(sSlots[i].xfb, xfbSize);
    }
    sSlotCount = i;
    if (sSlotCount < XFB_SLOTS_MIN) {
        gc_log("gfx: no memory for the XFBs, renderer disabled");
        return;
    }

    GX_Init(sFifo, GX_FIFO_SIZE);
    // Display copy tokens continue from what the token register holds (the loader may have left any value in it):
    // gx_token_reached compares within half the 16-bit range
    sCopyToken = GX_GetDrawSync();

    // EFB at 2x the N64 resolution, copied to an XFB of the TV mode (scaled vertically for 50 Hz modes)
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
#if defined(GFX_HW_TEST) && GFX_HW_TEST
    // Before the rest of the setup, which puts back everything the test changes
    gfx_hw_test();
#endif
    GX_SetCopyClear(sClearColor, GX_MAX_Z24);
    GX_SetViewport(0, 0, GFX_EFB_WIDTH, GFX_EFB_HEIGHT, 0, 1);
    GX_SetScissor(0, 0, GFX_EFB_WIDTH, GFX_EFB_HEIGHT);
    GX_SetDispCopySrc(0, 0, GFX_EFB_WIDTH, GFX_EFB_HEIGHT);
    yScale = GX_GetYScaleFactor(GFX_EFB_HEIGHT, sMode->xfbHeight);
    GX_SetDispCopyYScale(yScale);
    GX_SetDispCopyDst(sMode->fbWidth, sMode->xfbHeight);
    // The mode's sample pattern and vertical filter: the deflicker filter for interlaced modes on a CRT
    GX_SetCopyFilter(sMode->aa, sMode->sample_pattern, GX_TRUE, sMode->vfilter);
    GX_SetFieldMode(sMode->field_rendering, (sMode->viHeight == 2 * sMode->xfbHeight) ? GX_ENABLE : GX_DISABLE);
    GX_SetDispCopyGamma(GX_GM_1_0);

    // gfx_rsp culls; GX clips (near/far and the frustum sides) in clip space
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetClipMode(GX_CLIP_ENABLE);
    GX_SetDither(GX_FALSE);
    // Depth test after texturing, so texels rejected by the alpha compare never write depth
    GX_SetZCompLoc(GX_FALSE);

    // Vertex format: positions are already in clip (or NDC/screen) space, the position matrix is identity
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    sNumTex = 0;
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX1, GX_TEX_ST, GX_F32, 0);
    guMtxIdentity(identity);
    GX_LoadPosMtxImm(identity, GX_PNMTX0);
    GX_SetCurrentMtx(GX_PNMTX0);

    // Shade: the vertex color, no lighting (gfx_rsp lights on the CPU)
    gx_base_state();
    GX_SetNumTexGens(0);

    // Bound when a tile cannot be resolved, so the TEV never samples a stale texture: opaque white
    memset(sDummyTexData, 0xFF, sizeof(sDummyTexData));
    DCFlushRange(sDummyTexData, sizeof(sDummyTexData));
    GX_InitTexObj(&sDummyTex, sDummyTexData, 8, 4, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&sDummyTex, GX_NEAR, GX_NEAR);

    GX_Flush();
    sReady = true;
    sStatsStart = gettime();
    scan = sMode->viTVMode & 3;
    gc_log("gfx: GX ready, FIFO %u KB, %d XFBs of %u KB (%ux%u, %s)", GX_FIFO_SIZE / 1024, sSlotCount, xfbSize / 1024,
           sMode->fbWidth, sMode->xfbHeight,
           (scan == VI_INTERLACE) ? "interlaced" : (scan == VI_NON_INTERLACE) ? "240p" : "progressive");
}

bool gfx_gx_ready(void) {
    return sReady;
}

/* ============================================================================================== */
/* GX transform state                                                                             */
/* ============================================================================================== */

static void gx_load_proj(u32 type, float p0, float p1, float p2, float p3, float p4, float p5) {
    GxProj proj = { type, { p0, p1, p2, p3, p4, p5 } };
    Mtx44 m;

    if (sGxProjValid && memcmp(&proj, &sGxProj, sizeof(proj)) == 0) {
        return;
    }
    sGxProj = proj;
    sGxProjValid = true;

    memset(m, 0, sizeof(m));
    m[0][0] = p0;
    m[1][1] = p2;
    m[2][2] = p4;
    m[2][3] = p5;
    if (type == GX_PERSPECTIVE) {
        m[0][2] = p1;
        m[1][2] = p3;
        m[3][2] = -1.0f;
    } else {
        m[0][3] = p1;
        m[1][3] = p3;
        m[3][3] = 1.0f;
    }
    GX_LoadProjectionMtx(m, type);
}

static void gx_load_viewport(float x, float y, float w, float h, float n, float f) {
    GxViewport vp = { x, y, w, h, n, f };

    if (sGxVpValid && memcmp(&vp, &sGxVp, sizeof(vp)) == 0) {
        return;
    }
    sGxVp = vp;
    sGxVpValid = true;
    GX_SetViewport(x, y, w, h, n, f);
}

/* The N64 viewport (G_MV_VIEWPORT) scaled to the EFB. Window depth: GX gives (z_gx/w) * (f - n) + f with
 * z_gx/w = GX_ZK * (ndc - 1), the N64 gives (ndc * sz + tz) / G_MAXZ, so in N64 terms f = (sz + tz) / G_MAXZ and
 * f - n = sz / (GX_ZK * G_MAXZ), both then taken to EFB depth. The standard viewport (sz = tz = G_MAXZ / 2) gives
 * N64 depth 0 at ndc -1 as on the N64, and EFB depth 0 (N64 depth -GFX_DEPTH_UNDER) at GX's near plane. */
static void gx_n64_viewport_z(float* sz, float* tz) {
    *sz = gGfxRsp.viewportScale[2];
    *tz = gGfxRsp.viewportTrans[2];
    if (*sz == 0.0f && *tz == 0.0f) {
        // No viewport loaded: the standard one
        *sz = *tz = G_MAXZ / 2;
    }
}

static void gx_viewport_n64(float bias) {
    float sx = gGfxRsp.viewportScale[0] * 0.25f;
    float sy = gGfxRsp.viewportScale[1] * 0.25f;
    float tx = gGfxRsp.viewportTrans[0] * 0.25f;
    float ty = gGfxRsp.viewportTrans[1] * 0.25f;
    float sz, tz;
    float n, f;

    if (sx == 0.0f || sy == 0.0f) {
        sx = tx = GFX_N64_WIDTH / 2;
        sy = ty = GFX_N64_HEIGHT / 2;
    }
    gx_n64_viewport_z(&sz, &tz);
    f = (tz + sz) / (float)G_MAXZ;
    n = f - sz / (GX_ZK * (float)G_MAXZ);
    f = (f + GFX_DEPTH_UNDER) * (1.0f / (1 + GFX_DEPTH_UNDER));
    n = (n + GFX_DEPTH_UNDER) * (1.0f / (1 + GFX_DEPTH_UNDER));
    gx_load_viewport((tx - sx) * sScale, (ty - sy) * sScale, 2.0f * sx * sScale, 2.0f * sy * sScale, n, f - bias);
}

static void gx_set_vmode(VMode mode) {
    float bias = sInfo.decal ? DECAL_BIAS : 0.0f;

    // Batched vertices were computed for the current projection
    gfx_gx_flush();
    switch (mode) {
        case VMODE_PERSP:
            // clip = (x, y, z_gx, w) from the submitted (x, y, -w): z_gx = p4 * (-w) + p5 = GX_ZK * ((a - 1) w + b),
            // which is GX_ZK * (z - w) for z = a * w + b
            gx_load_proj(GX_PERSPECTIVE, 1.0f, 0.0f, 1.0f, 0.0f, (1.0f - sProjA) * GX_ZK, sProjB * GX_ZK);
            gx_viewport_n64(bias);
            break;
        case VMODE_NDC:
            // N64 NDC in (w = 1); z_gx = GX_ZK * (ndc - 1), as above
            gx_load_proj(GX_ORTHOGRAPHIC, 1.0f, 0.0f, 1.0f, 0.0f, GX_ZK, -GX_ZK);
            gx_viewport_n64(bias);
            break;
        case VMODE_SCREEN:
            // N64 screen pixels in, y down; z is the N64 window depth 0..1, which the viewport takes to EFB depth
            gx_load_proj(GX_ORTHOGRAPHIC, 2.0f / GFX_N64_WIDTH, -1.0f, -2.0f / GFX_N64_HEIGHT, 1.0f, 1.0f, -1.0f);
            gx_load_viewport(0, 0, GFX_N64_WIDTH * sScale, GFX_N64_HEIGHT * sScale,
                             (float)GFX_DEPTH_UNDER / (1 + GFX_DEPTH_UNDER) - bias, 1.0f - bias);
            break;
        default:
            break;
    }
    sVMode = mode;
}

static void gx_apply_scissor(void) {
    int x0 = (gGfxRdp.scissorUlx * sScale) >> 2;
    int y0 = (gGfxRdp.scissorUly * sScale) >> 2;
    int x1 = (gGfxRdp.scissorLrx * sScale) >> 2;
    int y1 = (gGfxRdp.scissorLry * sScale) >> 2;

    x0 = (x0 < 0) ? 0 : x0;
    y0 = (y0 < 0) ? 0 : y0;
    x1 = (x1 > sTargetW) ? sTargetW : x1;
    y1 = (y1 > sTargetH) ? sTargetH : y1;
    sScissorEmpty = (x1 <= x0) || (y1 <= y0);
    if (!sScissorEmpty) {
        GX_SetScissor(x0, y0, x1 - x0, y1 - y0);
    }
    // N64 rows a draw can change
    sScissorY0 = y0 / sScale;
    sScissorY1 = (y1 + sScale - 1) / sScale;
}

/* The scissor rectangle in N64 pixels (rounded out), from the RDP state */
static void gx_scissor_rect(int* x0, int* y0, int* x1, int* y1) {
    *x0 = gGfxRdp.scissorUlx >> 2;
    *y0 = gGfxRdp.scissorUly >> 2;
    *x1 = (gGfxRdp.scissorLrx + 3) >> 2;
    *y1 = (gGfxRdp.scissorLry + 3) >> 2;
}

void gfx_gx_set_projection(const float m[4][4]) {
    float best = 0.0f;
    int row = -1;
    int i;

    gfx_gx_flush();
    for (i = 0; i < 3; i++) {
        if (fabsf(m[i][3]) > best) {
            best = fabsf(m[i][3]);
            row = i;
        }
    }

    if (row < 0) {
        sProjKind = PROJ_ORTHO;
    } else {
        // Perspective: column 2 must be a * column 3 + b * (0, 0, 0, 1), so that z = a * w + b
        float a = m[row][2] / m[row][3];
        float scale = fabsf(m[0][2]) + fabsf(m[1][2]) + fabsf(m[2][2]) + 1.0e-6f;
        bool fits = true;

        for (i = 0; i < 3; i++) {
            if (fabsf(m[i][2] - a * m[i][3]) > 1.0e-3f * scale) {
                fits = false;
            }
        }
        sProjA = a;
        sProjB = m[3][2] - a * m[3][3];
        sProjKind = fits ? PROJ_PERSP : PROJ_CPU;
        if (!fits && !sLoggedCpuProj) {
            sLoggedCpuProj = true;
            gc_log("gfx: projection without z = a*w + b, drawn with CPU division (logged once)");
        }
    }
    sStateValid = false;
}

/* ============================================================================================== */
/* Render targets                                                                                 */
/* ============================================================================================== */

/* Scale and EFB area of the render target; the viewport and scissor follow at the next draw */
static void gx_set_target_scale(int scale, int w, int h) {
    sScale = scale;
    sTargetW = w;
    sTargetH = h;
    gfx_gx_state_lost();
}

/* Back to the frame: an off-screen pass ends (its image goes to RAM, the frame's pixels come back) */
static void gx_use_frame(void) {
    if (sCanvasKey != 0) {
        gfx_gx_flush();
        GFX_PROF_ENTER(GFX_PROF_FB);
        gfx_fb_canvas_end();
        GFX_PROF_LEAVE();
        sCanvasKey = 0;
        gx_set_target_scale(GFX_SCALE, GFX_EFB_WIDTH, GFX_EFB_HEIGHT);
    }
}

static bool gx_use_canvas(u32 key) {
    bool ok;

    // The same address with another pixel size or width is another image for gfx_fb.c (and another canvas width)
    if (sCanvasKey == key && sCanvasSiz == gGfxRdp.colorImageSiz && sTargetW == gGfxRdp.colorImageWidth) {
        return true;
    }
    gfx_gx_flush();
    GFX_PROF_ENTER(GFX_PROF_FB);
    ok = gfx_fb_canvas_begin(key, gGfxRdp.colorImageFmt, gGfxRdp.colorImageSiz, gGfxRdp.colorImageWidth);
    GFX_PROF_LEAVE();
    if (!ok) {
        return false;
    }
    sCanvasKey = key;
    sCanvasSiz = gGfxRdp.colorImageSiz;
    gx_set_target_scale(1, gGfxRdp.colorImageWidth, GFX_N64_HEIGHT);
    return true;
}

/* Render target for a draw under the current color image: the frame, a depth clear (`fill`: FILL mode into the z
 * image), or an off-screen image. Switching between the frame and off-screen images happens here, before the draw
 * binds its textures. */
static inline bool gx_target_same(bool fill) {
    return sTargetValid && gGfxRdp.colorImageAddr == sTargetCimg && gGfxRdp.zImageAddr == sTargetZimg &&
           fill == sTargetFill && gGfxRdp.colorImageSiz == sTargetSiz && gGfxRdp.colorImageWidth == sTargetWidth;
}

static Target gx_target_slow(bool fill) {
    u32 cimg = gGfxRdp.colorImageAddr;
    u32 zimg = gGfxRdp.zImageAddr;
    u32 key;
    bool isZ;

    if (gx_target_same(fill)) {
        return sTarget;
    }
    sTargetCimg = cimg;
    sTargetZimg = zimg;
    sTargetFill = fill;
    sTargetSiz = gGfxRdp.colorImageSiz;
    sTargetWidth = gGfxRdp.colorImageWidth;
    sTargetValid = true;

    key = gx_key((u32)gfx_addr(cimg));
    isZ = zimg != 0 && key == gx_key((u32)gfx_addr(zimg));
    if (sFrameKey == 0 && !isZ) {
        // The first color image of a task that is not the z-buffer is the frame
        sFrameKey = key;
        gfx_fb_set_frame(key, gGfxRdp.colorImageWidth);
    }

    if (key == sFrameKey) {
        gx_use_frame();
        sTarget = TARGET_FRAME;
    } else if (isZ && fill && sCanvasKey != key) {
        gx_use_frame();
        sTarget = TARGET_DEPTH;
    } else if (gx_use_canvas(key)) {
        // Colors drawn into the z-buffer's memory (PreRender captures, Lens of Truth) are not depth
        if (isZ) {
            sDepthColored = true;
        }
        sTarget = TARGET_CANVAS;
    } else {
        sTarget = TARGET_SKIP;
        if (sLogCount < LOG_LIMIT) {
            sLogCount++;
            gc_log("gfx: draws into off-screen color image %08X skipped (frame %08X)", key, sFrameKey);
        }
    }
    return sTarget;
}

static inline Target gx_target(bool fill) {
    return gx_target_same(fill) ? sTarget : gx_target_slow(fill);
}

/* Whether a rectangle sets each of its pixels without reading the color image: no memory reads in the blender
 * (IM_RD), no alpha compare dropping pixels */
static bool gx_rect_opaque(void) {
    u32 cycle = gGfxRdp.otherModeH & (3u << G_MDSFT_CYCLETYPE);

    if (cycle == G_CYC_FILL) {
        return true;
    }
    if ((gGfxRdp.otherModeL & (3u << G_MDSFT_ALPHACOMPARE)) != G_AC_NONE) {
        return false;
    }
    return cycle == G_CYC_COPY || !(gGfxRdp.otherModeL & RM_IM_RD);
}

/* Before a draw into the canvas covering N64 pixels [x0, x1) x [y0, y1) (clipped by the scissor here) */
static void gx_canvas_draw(float x0, float y0, float x1, float y1, bool opaque) {
    int sx0, sy0, sx1, sy1;
    int ix0 = (int)x0, iy0 = (int)y0;
    int ix1 = (int)ceilf(x1), iy1 = (int)ceilf(y1);

    gx_scissor_rect(&sx0, &sy0, &sx1, &sy1);
    ix0 = (ix0 > sx0) ? ix0 : sx0;
    iy0 = (iy0 > sy0) ? iy0 : sy0;
    ix1 = (ix1 < sx1) ? ix1 : sx1;
    iy1 = (iy1 < sy1) ? iy1 : sy1;
    // A rectangle that does not start and end on whole pixels only partly covers its edge pixels
    if (opaque && (x0 != (float)(int)x0 || y0 != (float)(int)y0 || x1 != (float)(int)x1 || y1 != (float)(int)y1)) {
        opaque = (float)ix0 >= x0 && (float)iy0 >= y0 && (float)ix1 <= x1 && (float)iy1 <= y1;
    }
    GFX_PROF_ENTER(GFX_PROF_FB);
    gfx_fb_canvas_draw(ix0, iy0, ix1, iy1, opaque);
    GFX_PROF_LEAVE();
    sStats.canvasDraws++;
}

/* ============================================================================================== */
/* Draw state                                                                                     */
/* ============================================================================================== */

static void gx_set_num_tex(int numTex) {
    if (numTex == sNumTex) {
        return;
    }
    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    if (numTex >= 1) {
        GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    }
    if (numTex >= 2) {
        GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
    }
    GX_SetNumTexGens(numTex);
    sNumTex = numTex;
}

/* sTex[index] changed */
static void gx_uv_update(int index) {
    const GfxTexBinding* b = &sTex[index];
    GxUv* u = &sUv[index];

    u->sScale = b->sShiftScale;
    u->sOffset = b->sOffset;
    u->tScale = b->tShiftScale;
    u->tOffset = b->tOffset;
    u->width = (float)b->width;
    u->height = (float)b->height;
    u->invWidth = (b->width != 0 && (b->width & (b->width - 1)) == 0) ? 1.0f / u->width : 0.0f;
    u->invHeight = (b->height != 0 && (b->height & (b->height - 1)) == 0) ? 1.0f / u->height : 0.0f;
}

/* gfx_tex_uv (gfx_internal.h) with the prepared sizes: the same expressions and floats */
static inline void gx_uv(const GxUv* u, float s, float t, float* ou, float* ov) {
    float ns = s * u->sScale - u->sOffset;
    float nt = t * u->tScale - u->tOffset;

    *ou = (u->invWidth != 0.0f) ? ns * u->invWidth : ns / u->width;
    *ov = (u->invHeight != 0.0f) ? nt * u->invHeight : nt / u->height;
}

static void gx_bind(int index, int tile) {
    GfxTexBinding* b = &sTex[index];
    int texMap = (index == 0) ? GX_TEXMAP0 : GX_TEXMAP1;
    bool ok;

    GFX_PROF_ENTER(GFX_PROF_BIND);
    ok = gfx_tex_bind(tile & 7, texMap, b) && b->valid;
    GFX_PROF_LEAVE();
    gx_uv_update(index);
    if (ok) {
        return;
    }
#ifdef GFX_TEX_FAIL_TRACE
    // Diagnostic build (GFX_CFLAGS += -DGFX_TEX_FAIL_TRACE): every distinct failing bind, with the display list
    // command (pc) and the return address of the innermost DL call, to find what draws it
    {
        extern uint32_t gGfxDiagPc[2];
        static uint32_t sSeen[256];
        static int sNumSeen;
        const GfxTile* t = &gGfxRdp.tiles[tile & 7];
        uint32_t key = gGfxDiagPc[0] ^ (gGfxRdp.combineLo * 31) ^ ((uint32_t)index << 30) ^ (t->tmem << 20);
        int i;

        for (i = 0; i < sNumSeen && sSeen[i] != key; i++) {
        }
        if (i == sNumSeen && sNumSeen < 256) {
            sSeen[sNumSeen++] = key;
            gc_log("gfx diag: bind fail TEXEL%d tile %d fmt %u siz %u tmem %03X line %u size %u..%u x %u..%u "
                   "timg %08X combine %06X %08X H %08X L %08X tex %d/%d pc %08X ret %08X", index, tile & 7, t->fmt,
                   t->siz, t->tmem, t->line, t->uls >> 2, t->lrs >> 2, t->ult >> 2, t->lrt >> 2,
                   (unsigned int)gGfxRdp.texImageAddr, (unsigned int)gGfxRdp.combineHi,
                   (unsigned int)gGfxRdp.combineLo, (unsigned int)gGfxRdp.otherModeH,
                   (unsigned int)gGfxRdp.otherModeL, gGfxRsp.textureOn, gGfxRsp.textureTile,
                   (unsigned int)gGfxDiagPc[0], (unsigned int)gGfxDiagPc[1]);
        }
    }
#endif
    // Unresolvable tile (nothing loaded): sample opaque white
    if (sLogBindCount < LOG_LIMIT) {
        const GfxTile* t = &gGfxRdp.tiles[tile & 7];

        sLogBindCount++;
        gc_log("gfx: TEXEL%d tile %d (fmt %u siz %u tmem %03X) not resolved, white instead (combine %06X %08X, "
               "mode H %08X)", index, tile & 7, t->fmt, t->siz, t->tmem, (unsigned int)gGfxRdp.combineHi,
               (unsigned int)gGfxRdp.combineLo, (unsigned int)gGfxRdp.otherModeH);
    }
    GX_LoadTexObj(&sDummyTex, texMap);
    memset(b, 0, sizeof(*b));
    b->width = 8;
    b->height = 4;
    b->sShiftScale = b->tShiftScale = 1.0f;
    gx_uv_update(index);
}

/* Make the RAM a tile was loaded from current, if the renderer drew there (VisMono and PreRender's coverage load
 * the frame they draw into). gfx_tex.c decodes tiles from RAM at bind time; the source is taken to be the last
 * G_SETTIMG image, from the tile's first row (G_LOADTILE) or its start (G_LOADBLOCK, image width 1), for as many
 * rows as the tile has, with one row of margin. */
static void gx_sync_tile_source(int tile) {
    const GfxTile* t = &gGfxRdp.tiles[tile & 7];
    u32 addr = gGfxRdp.texImageAddr;
    u32 stride = ((u32)gGfxRdp.texImageWidth << gGfxRdp.texImageSiz) >> 1; // bytes per image row
    u32 lineBytes = t->line * 8;
    u32 rowBytes = (stride > lineBytes) ? stride : lineBytes;
    u32 rows = (t->lrt >= t->ult) ? ((t->lrt - t->ult) >> 2) + 1 : 1;

    if (addr == 0) {
        return;
    }
    if (gGfxRdp.texImageWidth > 1) {
        addr += (t->ult >> 2) * stride;
    }
    GFX_PROF_ENTER(GFX_PROF_FB);
    gfx_fb_sync_ram((const void*)addr, (rows + 1) * rowBytes);
    GFX_PROF_LEAVE();
}

/* Bring GX up to date with the RSP/RDP state for a draw of `kind` reading tile `tile` (and tile + 1). */
static void gx_prepare_slow(GfxPrimKind kind, int tile) {
    u32 dirty = gGfxRdp.dirty;
    bool full = !sStateValid || (int)kind != sKind;
    VMode vmode;
    bool oldDecal = sInfo.decal;

    gfx_gx_flush();
    GFX_PROF_ENTER(GFX_PROF_PREP);

    if (full || (dirty & ~(GFX_DIRTY_SCISSOR | GFX_DIRTY_VIEWPORT | GFX_DIRTY_TEXTURES))) {
        bool used0 = sInfo.usesTexel0, used1 = sInfo.usesTexel1;

        GFX_PROF_ENTER(GFX_PROF_TEV);
        gfx_tev_apply(kind, &sInfo);
        GFX_PROF_LEAVE();
        if (full || (dirty & (GFX_DIRTY_TEXTURES | GFX_DIRTY_OTHERMODE)) || tile != sTexTile ||
            (sInfo.usesTexel0 && !used0) || (sInfo.usesTexel1 && !used1)) {
            sTexBound[0] = sTexBound[1] = false;
        }
        sKind = kind;
        sZPrim = (kind == GFX_PRIM_TRIANGLE) && (gGfxRdp.otherModeL & G_ZS_PRIM);
    } else if ((dirty & GFX_DIRTY_TEXTURES) || tile != sTexTile) {
        // Only tiles, loads or G_TEXTURE changed: gfx_tev_apply reads none of them (combiner, other modes, colors and
        // geometry mode are as last applied), the textures are bound again
        sTexBound[0] = sTexBound[1] = false;
    }

    // Textures the combiner reads; TEXEL1 is the next tile. Framebuffer contents they read go to RAM before
    // either is bound (writing RAM makes gfx_tex.c check its textures again).
    sTexTile = tile;
    if (tile == GX_TILE_IMAGE) {
        // gfx_gx_image_rect: the caller bound the textures
        sTexBound[0] = sTexBound[1] = true;
    }
    if (gfx_fb_pending()) {
        if (sInfo.usesTexel0 && !sTexBound[0]) {
            gx_sync_tile_source(tile);
        }
        if (sInfo.usesTexel1 && !sTexBound[1]) {
            gx_sync_tile_source(tile + 1);
        }
    }
    if (sInfo.usesTexel0 && !sTexBound[0]) {
        gx_bind(0, tile);
        sTexBound[0] = true;
    }
    if (sInfo.usesTexel1 && !sTexBound[1]) {
        gx_bind(1, tile + 1);
        sTexBound[1] = true;
    }
    gx_set_num_tex(sInfo.usesTexel1 ? 2 : (sInfo.usesTexel0 ? 1 : 0));

    if (full || (dirty & GFX_DIRTY_SCISSOR)) {
        gx_apply_scissor();
    }

    if (kind != GFX_PRIM_TRIANGLE) {
        vmode = VMODE_SCREEN;
    } else if (sProjKind == PROJ_PERSP && !sZPrim) {
        vmode = VMODE_PERSP;
    } else {
        vmode = VMODE_NDC;
    }
    sTriVMode = vmode;
    if (sZPrim) {
        // N64 NDC z that lands on the primitive depth through the N64 viewport's z mapping. Clamped to the
        // viewport's range: GX would otherwise clip the triangle on the primitive depth (G_SETPRIMDEPTH -1 is
        // just past the far plane), where the RSP clips on the vertices' own z.
        float sz, tz, ndc;

        gx_n64_viewport_z(&sz, &tz);
        ndc = (sz != 0.0f) ? (gx_prim_depth() * G_MAXZ - tz) / sz : 0.0f;
        sPrimNdcZ = (ndc < -1.0f) ? -1.0f : (ndc > 1.0f) ? 1.0f : ndc;
    }
    // Projection and viewport go through caches, so recomputing them is cheap
    if (full || vmode != sVMode || oldDecal != sInfo.decal || (dirty & GFX_DIRTY_VIEWPORT)) {
        gx_set_vmode(vmode);
    }

    gGfxRdp.dirty = 0;
    sStateValid = true;
    GFX_PROF_LEAVE();
}

static inline void gx_prepare(GfxPrimKind kind, int tile) {
    if (gGfxRdp.dirty != 0 || !sStateValid || (int)kind != sKind || (kind != GFX_PRIM_TRIANGLE && tile != sTexTile)) {
        gx_prepare_slow(kind, tile);
    }
}

/* My own GX state for FILL-mode rectangles and depth clears; the next normal draw re-applies everything. */
static void gx_set_fill_state(GXColor color, bool colorWrite, bool depthWrite) {
    gfx_gx_flush();
    GX_SetNumTevStages(1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevColor(GX_TEVREG0, color);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetNumIndStages(0);
    GX_SetTevDirect(GX_TEVSTAGE0);
    GX_SetFog(GX_FOG_NONE, 0, 1, 0.1f, 1, color);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_COPY);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(depthWrite ? GX_TRUE : GX_FALSE, GX_ALWAYS, depthWrite ? GX_TRUE : GX_FALSE);
    GX_SetColorUpdate(colorWrite ? GX_TRUE : GX_FALSE);
    GX_SetAlphaUpdate(colorWrite ? GX_TRUE : GX_FALSE);
    gx_set_num_tex(0);
    memset(&sInfo, 0, sizeof(sInfo));
    gx_set_vmode(VMODE_SCREEN);
    sStateValid = false;
    gfx_tev_invalidate();
}

/* ============================================================================================== */
/* Drawing                                                                                        */
/* ============================================================================================== */

void gfx_gx_flush(void) {
    BatchVtx* v = sBatch;
    int i;

    if (sBatchCount == 0) {
        return;
    }
    GFX_PROF_ENTER(GFX_PROF_FLUSH);
    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, sBatchCount);
    switch (sNumTex) {
        case 0:
            for (i = 0; i < sBatchCount; i++, v++) {
                GX_Position3f32(v->x, v->y, v->z);
                GX_Color1u32(v->color);
            }
            break;
        case 1:
            for (i = 0; i < sBatchCount; i++, v++) {
                GX_Position3f32(v->x, v->y, v->z);
                GX_Color1u32(v->color);
                GX_TexCoord2f32(v->u0, v->v0);
            }
            break;
        default:
            for (i = 0; i < sBatchCount; i++, v++) {
                GX_Position3f32(v->x, v->y, v->z);
                GX_Color1u32(v->color);
                GX_TexCoord2f32(v->u0, v->v0);
                GX_TexCoord2f32(v->u1, v->v1);
            }
            break;
    }
    GX_End();
    sStats.batches++;
    sBatchCount = 0;
    GFX_PROF_LEAVE();
}

static inline BatchVtx* gx_batch_alloc(int count) {
    BatchVtx* v;

    if (sBatchCount + count > BATCH_VERTS) {
        gfx_gx_flush();
    }
    v = &sBatch[sBatchCount];
    sBatchCount += count;
    return v;
}

#if GFX_VTX_CHECK
/* Check build (-DGFX_VTX_CHECK=1): every texture coordinate against gfx_tex_uv */
static u32 sChkUv, sChkUvBad;

static void gx_uv_check(const GfxTexBinding* b, float s, float t, float u, float v) {
    float ru, rv;

    gfx_tex_uv(b, s, t, &ru, &rv);
    sChkUv++;
    if (memcmp(&ru, &u, sizeof(u)) != 0 || memcmp(&rv, &v, sizeof(v)) != 0) {
        if (sChkUvBad++ < 16) {
            gc_log("gx: uv check: (%08X, %08X) size %ux%u: got %08X %08X, want %08X %08X", *(const u32*)&s,
                   *(const u32*)&t, b->width, b->height, *(const u32*)&u, *(const u32*)&v, *(const u32*)&ru,
                   *(const u32*)&rv);
        }
    }
}
#endif

static inline void gx_texcoords(BatchVtx* out, float s, float t) {
    if (sNumTex >= 1) {
        gx_uv(&sUv[0], s, t, &out->u0, &out->v0);
#if GFX_VTX_CHECK
        gx_uv_check(&sTex[0], s, t, out->u0, out->v0);
#endif
    }
    if (sNumTex >= 2) {
        gx_uv(&sUv[1], s, t, &out->u1, &out->v1);
#if GFX_VTX_CHECK
        gx_uv_check(&sTex[1], s, t, out->u1, out->v1);
#endif
    }
}

static inline bool gx_fits_persp(const GfxVtx* v) {
    return fabsf(v->z - (sProjA * v->w + sProjB)) <= PERSP_FIT_TOLERANCE * (fabsf(v->w) + 1.0f);
}

#if GFX_SHADOW_PROBE
/* Probe build (-DGFX_SHADOW_PROBE=1): the EFB color under the first actor shadows of each scene, before and after
 * the shadow is drawn (GX_PeekARGB after GX_DrawDone), with the inputs that make it. Shadows are the triangles drawn
 * with the blender's FOG_SHADE_A cycle, decal z and a black primitive color (SETUPDL_44). */
static int sProbeScene = -1;
static int sProbeLeft;
static unsigned int sProbeLastFrame;

static bool gx_probe_shadow(int* ex, int* ey, const GfxVtx* const vtx[3]) {
    u32 l = gGfxRdp.otherModeL;
    float sx = gGfxRsp.viewportScale[0] * 0.25f, sy = gGfxRsp.viewportScale[1] * 0.25f;
    float tx = gGfxRsp.viewportTrans[0] * 0.25f, ty = gGfxRsp.viewportTrans[1] * 0.25f;
    float x = 0.0f, y = 0.0f;
    int gs, scene, room, i;
    unsigned int frames, entrance;

    if (((l >> 30) & 3) != G_BL_CLR_FOG || ((l >> 26) & 3) != 2 || (l & 0x0C00) != 0x0C00 ||
        (gGfxRdp.primColor & 0xFFFFFF00) != 0) {
        return false;
    }
    gc_game_crash_info(&gs, &frames, &scene, &room, &entrance);
    if (gs != 3 || frames < 40) {
        // Play only, after the fade-in
        return false;
    }
    if (scene != sProbeScene) {
        sProbeScene = scene;
        sProbeLeft = 6;
        sProbeLastFrame = 0;
    }
    if (sProbeLeft <= 0 || (sProbeLastFrame != 0 && frames - sProbeLastFrame < 10)) {
        return false;
    }
    for (i = 0; i < 3; i++) {
        if (!(vtx[i]->w > 0.0f)) {
            return false;
        }
        x += vtx[i]->x / vtx[i]->w;
        y += vtx[i]->y / vtx[i]->w;
    }
    if (sx == 0.0f || sy == 0.0f) {
        sx = tx = GFX_N64_WIDTH / 2;
        sy = ty = GFX_N64_HEIGHT / 2;
    }
    *ex = (int)((tx + sx * x / 3.0f) * sScale);
    *ey = (int)((ty - sy * y / 3.0f) * sScale);
    if (*ex < 0 || *ey < 0 || *ex >= GFX_EFB_WIDTH || *ey >= GFX_EFB_HEIGHT) {
        return false;
    }
    sProbeLastFrame = frames;
    return true;
}

static void gx_probe_log(int ex, int ey, GXColor before, GXColor after, const GfxVtx* const vtx[3]) {
    int gs, scene, room;
    unsigned int frames, entrance;

    gc_game_crash_info(&gs, &frames, &scene, &room, &entrance);
    gc_log("probe shadow: scene %02X frame %u EFB (%d,%d) before %02X%02X%02X after %02X%02X%02X; prim %08X fog %08X "
           "env %08X; vtx rgba %08X %08X %08X; z/w %d %d %d (x1000); combine %06X %08X; mode H %08X L %08X; geo %08X",
           (unsigned int)scene, frames, ex, ey, before.r, before.g, before.b, after.r, after.g, after.b,
           (unsigned int)gGfxRdp.primColor, (unsigned int)gGfxRdp.fogColor, (unsigned int)gGfxRdp.envColor,
           ((u32)vtx[0]->r << 24) | ((u32)vtx[0]->g << 16) | ((u32)vtx[0]->b << 8) | vtx[0]->a,
           ((u32)vtx[1]->r << 24) | ((u32)vtx[1]->g << 16) | ((u32)vtx[1]->b << 8) | vtx[1]->a,
           ((u32)vtx[2]->r << 24) | ((u32)vtx[2]->g << 16) | ((u32)vtx[2]->b << 8) | vtx[2]->a,
           (int)(vtx[0]->z / vtx[0]->w * 1000.0f), (int)(vtx[1]->z / vtx[1]->w * 1000.0f),
           (int)(vtx[2]->z / vtx[2]->w * 1000.0f), (unsigned int)gGfxRdp.combineHi, (unsigned int)gGfxRdp.combineLo,
           (unsigned int)gGfxRdp.otherModeH, (unsigned int)gGfxRdp.otherModeL, (unsigned int)gGfxRsp.geometryMode);
}
#endif

static void gx_triangle(const GfxVtx* v0, const GfxVtx* v1, const GfxVtx* v2) {
    const GfxVtx* vtx[3] = { v0, v1, v2 };
    BatchVtx* out;
    bool cpu;
    int i;
    Target target = gx_target(false);
#if GFX_SHADOW_PROBE
    bool probe = false;
    int probeX = 0, probeY = 0;
    GXColor before = { 0, 0, 0, 0 }, after = { 0, 0, 0, 0 };
#endif

    if (target == TARGET_CANVAS) {
        // Any pixel inside the scissor, partly covered
        gx_canvas_draw(0.0f, 0.0f, 4096.0f, 4096.0f, false);
    } else if (target != TARGET_FRAME) {
        sStats.offscreen++;
        return;
    }
    gx_prepare(GFX_PRIM_TRIANGLE, gGfxRsp.textureTile);
    if (sScissorEmpty) {
        return;
    }
    if (target == TARGET_FRAME) {
        gfx_fb_frame_drawn(sScissorY0, sScissorY1);
    }

    cpu = (sTriVMode != VMODE_PERSP);
    if (!cpu && !(gx_fits_persp(v0) && gx_fits_persp(v1) && gx_fits_persp(v2))) {
        // z is off the fitted plane (non-affine modelview, forced matrix): divide this triangle on the CPU
        cpu = true;
        sStats.cpuTris++;
    }
    if (cpu && !(v0->w > 0.0f && v1->w > 0.0f && v2->w > 0.0f)) {
        // Behind the eye (or NaN): the CPU path does not clip
        sStats.droppedTris++;
        return;
    }
    if (sVMode != (cpu ? VMODE_NDC : VMODE_PERSP)) {
        gx_set_vmode(cpu ? VMODE_NDC : VMODE_PERSP);
    }
#if GFX_SHADOW_PROBE
    if (target == TARGET_FRAME && gx_probe_shadow(&probeX, &probeY, vtx)) {
        probe = true;
        gfx_gx_flush();
        GX_DrawDone();
        GX_PeekARGB((u16)probeX, (u16)probeY, &before);
        // Over black (a fade, a letterbox) the probe tells nothing: try again 10 frames later
        probe = before.r + before.g + before.b >= 24;
    }
#endif

    // (flat shading is applied by gfx_rsp: it keeps per-vertex alpha, as the microcode does)
    out = gx_batch_alloc(3);
    for (i = 0; i < 3; i++, out++) {
        const GfxVtx* v = vtx[i];

        if (!cpu) {
            out->x = v->x;
            out->y = v->y;
            out->z = -v->w;
        } else {
            float invW = 1.0f / v->w;
            float z = v->z * invW;

            out->x = v->x * invW;
            out->y = v->y * invW;
            // F3DZEX2 NoN clamps the screen z of vertices in front of the near plane to 0 (ndc -1); beyond
            // the far plane it clips, as GX does
            out->z = sZPrim ? sPrimNdcZ : (z > -1.0f) ? z : -1.0f;
        }
        out->color = ((u32)v->r << 24) | ((u32)v->g << 16) | ((u32)v->b << 8) | v->a;
        gx_texcoords(out, v->s, v->t);
    }
    sStats.tris++;
    if (target == TARGET_FRAME) {
        sColorClean = sDepthClean = false;
    }
#if GFX_SHADOW_PROBE
    if (probe) {
        gfx_gx_flush();
        GX_DrawDone();
        GX_PeekARGB((u16)probeX, (u16)probeY, &after);
        gx_probe_log(probeX, probeY, before, after, vtx);
        sProbeLeft--;
    }
#endif
}

void gfx_gx_triangle(const GfxVtx* v0, const GfxVtx* v1, const GfxVtx* v2) {
    GFX_PROF_ENTER(GFX_PROF_GXTRI);
    gx_triangle(v0, v1, v2);
    GFX_PROF_LEAVE();
}

/* Quad in N64 screen pixels as two triangles (VMODE_SCREEN). st[i] are the texel coordinates of the
 * corners UL, UR, LL, LR. */
static void gx_quad(float ulx, float uly, float lrx, float lry, float z, u32 color, const float st[4][2]) {
    static const u8 order[6] = { 0, 1, 2, 2, 1, 3 };
    float xs[4] = { ulx, lrx, ulx, lrx };
    float ys[4] = { uly, uly, lry, lry };
    BatchVtx* out = gx_batch_alloc(6);
    int i;

    for (i = 0; i < 6; i++, out++) {
        int c = order[i];

        out->x = xs[c];
        out->y = ys[c];
        out->z = z;
        out->color = color;
        if (st != NULL) {
            gx_texcoords(out, st[c][0], st[c][1]);
        } else {
            out->u0 = out->v0 = out->u1 = out->v1 = 0.0f;
        }
    }
}

/* Window depth of a rectangle: the primitive depth with G_ZS_PRIM, else 0 (rectangles have no z slope) */
static inline float gx_rect_depth(void) {
    return (gGfxRdp.otherModeL & G_ZS_PRIM) ? gx_prim_depth() : 0.0f;
}

/* gDPSetCombineLERP's words, as gbi.h packs them */
#define GX_N64_CC_HI(a0, c0, Aa0, Ac0, a1, c1)                                                                    \
    ((((a0) & 0xF) << 20) | (((c0) & 0x1F) << 15) | (((Aa0) & 7) << 12) | (((Ac0) & 7) << 9) | (((a1) & 0xF) << 5) | \
     ((c1) & 0x1F))
#define GX_N64_CC_LO(b0, d0, Ab0, Ad0, b1, Aa1, Ac1, d1, Ab1, Ad1)                                              \
    (((u32)((b0) & 0xF) << 28) | (((d0) & 7) << 15) | (((Ab0) & 7) << 12) | (((Ad0) & 7) << 9) |                \
     (((b1) & 0xF) << 24) | (((Aa1) & 7) << 21) | (((Ac1) & 7) << 18) | (((d1) & 7) << 6) | (((Ab1) & 7) << 3) | \
     ((Ad1) & 7))

/* z_vismono.c: (1 - 0) * TEXEL1_ALPHA + TEXEL0, alpha 1; then (PRIMITIVE - ENVIRONMENT) * COMBINED + ENVIRONMENT,
 * alpha PRIMITIVE */
#define VISMONO_CC_HI \
    GX_N64_CC_HI(G_CCMUX_1, G_CCMUX_TEXEL1_ALPHA, G_ACMUX_0, G_ACMUX_0, G_CCMUX_PRIMITIVE, G_CCMUX_COMBINED)
#define VISMONO_CC_LO                                                                                         \
    GX_N64_CC_LO(G_CCMUX_0, G_CCMUX_TEXEL0, G_ACMUX_0, G_ACMUX_1, G_CCMUX_ENVIRONMENT, G_ACMUX_0, G_ACMUX_0, \
                 G_CCMUX_ENVIRONMENT, G_ACMUX_0, G_ACMUX_PRIMITIVE)

static int sVisMonoNext = -1; /* VisMono: the row its next rectangle starts at; -1: no copy of the frame */
static GXTexObj sVisMonoTex;

/* Pixel (r, g, b) -> lerp(env, prim, (2 r + 4 g + b) / 7). Stages 0-2 add up the channels (texture swap tables RRR,
 * GGG, BBB) times konst weights, stage 3 interpolates; the blend takes prim alpha, as the 2nd cycle's blender. */
static void gx_vismono_state(void) {
    static const u8 swaps[3] = { GX_TEV_SWAP1, GX_TEV_SWAP2, GX_TEV_SWAP3 };
    static const u8 weights[3] = { 73, 146, 36 }; /* 2/7, 4/7, 1/7 of 255 */
    u32 prim = gGfxRdp.primColor, env = gGfxRdp.envColor;
    GXColor envColor = { env >> 24, env >> 16, env >> 8, env };
    GXColor primColor = { prim >> 24, prim >> 16, prim >> 8, prim };
    int i;

    GX_SetTevSwapModeTable(GX_TEV_SWAP1, GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA);
    GX_SetTevSwapModeTable(GX_TEV_SWAP2, GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA);
    GX_SetTevSwapModeTable(GX_TEV_SWAP3, GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA);
    for (i = 0; i < 3; i++) {
        GXColor k = { weights[i], weights[i], weights[i], 0 };

        GX_SetTevKColor(GX_KCOLOR0 + i, k);
        GX_SetTevDirect(GX_TEVSTAGE0 + i);
        GX_SetTevOrder(GX_TEVSTAGE0 + i, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
        GX_SetTevSwapMode(GX_TEVSTAGE0 + i, GX_TEV_SWAP0, swaps[i]);
        GX_SetTevKColorSel(GX_TEVSTAGE0 + i, GX_TEV_KCSEL_K0 + i);
        GX_SetTevColorIn(GX_TEVSTAGE0 + i, GX_CC_ZERO, GX_CC_TEXC, GX_CC_KONST, (i == 0) ? GX_CC_ZERO : GX_CC_CPREV);
        GX_SetTevAlphaIn(GX_TEVSTAGE0 + i, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
        GX_SetTevColorOp(GX_TEVSTAGE0 + i, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
        GX_SetTevAlphaOp(GX_TEVSTAGE0 + i, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    }
    GX_SetTevColor(GX_TEVREG1, envColor);
    GX_SetTevColor(GX_TEVREG2, primColor);
    GX_SetTevDirect(GX_TEVSTAGE3);
    GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevSwapMode(GX_TEVSTAGE3, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_C1, GX_CC_C2, GX_CC_CPREV, GX_CC_ZERO);
    GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A2);
    GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GX_SetNumTevStages(4);
    GX_SetNumIndStages(0);
    GX_SetFog(GX_FOG_NONE, 0, 1, 0.1f, 1, envColor);
    GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_COPY);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
    GX_SetZMode(GX_FALSE, GX_ALWAYS, GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_FALSE);
    GX_LoadTexObj(&sVisMonoTex, GX_TEXMAP0);
}

/*
 * VisMono_DesaturateDList's rectangles: rows [y, y + h) of the frame loaded as CI8 (two tiles one byte apart: the
 * high and low byte of each pixel, through VisMono_DesaturateTLUT's IA16 palette, I + A being the luminance) and
 * drawn back in place in 2-cycle mode. At GFX_SCALE the EFB samples each N64 pixel twice along s, at two different
 * bytes (vertical stripes), and every rectangle converts two textures: the rows are drawn from a GX copy of the frame
 * instead, through gx_vismono_state. Returns false (draw normally) for anything else.
 */
static bool gx_vismono(float ulx, float uly, float lrx, float lry, int tile, float s, float t, float dsdx, float dtdy,
                       bool flip) {
    const u32 cycleMask = 3u << G_MDSFT_CYCLETYPE;
    const u32 tlutMask = 3u << G_MDSFT_TEXTLUT;
    float st[4][2];
    int y0 = (int)uly;

    if (gGfxRdp.combineHi != VISMONO_CC_HI || gGfxRdp.combineLo != VISMONO_CC_LO || flip || tile != 0 ||
        (gGfxRdp.otherModeH & cycleMask) != G_CYC_2CYCLE || (gGfxRdp.otherModeH & tlutMask) != G_TT_IA16 ||
        dsdx != 2.0f || dtdy != 1.0f || s != 2.0f || t != 0.0f || ulx != 0.0f || uly != (float)y0 ||
        gGfxFb.frameWidth != GFX_N64_WIDTH || gGfxRdp.tiles[0].fmt != G_IM_FMT_CI ||
        gGfxRdp.tiles[0].siz != G_IM_SIZ_8b ||
        gx_key(gGfxRdp.texImageAddr) != sFrameKey + (u32)y0 * GFX_N64_WIDTH * 2) {
        return false;
    }
    // The rectangles go down the frame, each reading rows not drawn yet: one copy of the frame serves them all
    if (y0 != sVisMonoNext) {
        bool ok;

        GFX_PROF_ENTER(GFX_PROF_FB);
        ok = gfx_fb_frame_texture(&sVisMonoTex);
        GFX_PROF_LEAVE();
        if (!ok) {
            return false;
        }
    }
    sVisMonoNext = (int)ceilf(lry);

    gfx_gx_flush();
    gx_set_num_tex(1);
    memset(&sTex[0], 0, sizeof(sTex[0]));
    sTex[0].valid = true;
    sTex[0].width = GFX_N64_WIDTH;
    sTex[0].height = GFX_N64_HEIGHT;
    sTex[0].sShiftScale = sTex[0].tShiftScale = 1.0f;
    gx_uv_update(0);
    memset(&sInfo, 0, sizeof(sInfo));
    gx_set_vmode(VMODE_SCREEN);
    gx_apply_scissor();
    gGfxRdp.dirty &= ~GFX_DIRTY_SCISSOR;
    if (!sScissorEmpty) {
        gx_vismono_state();
        st[0][0] = 0.0f;
        st[0][1] = uly;
        st[1][0] = lrx;
        st[1][1] = uly;
        st[2][0] = 0.0f;
        st[2][1] = lry;
        st[3][0] = lrx;
        st[3][1] = lry;
        gx_quad(ulx, uly, lrx, lry, 0.0f, 0xFFFFFFFF, st);
        gfx_gx_flush();
        // Back to the swap tables gfx_tev.c expects (it never sets them)
        GX_SetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevSwapMode(GX_TEVSTAGE1, GX_TEV_SWAP0, GX_TEV_SWAP0);
        GX_SetTevSwapMode(GX_TEVSTAGE2, GX_TEV_SWAP0, GX_TEV_SWAP0);
        gfx_fb_frame_drawn(y0, sVisMonoNext);
        sColorClean = sDepthClean = false;
    }
    sStateValid = false;
    sKind = -1;
    sTexTile = -1;
    sTexBound[0] = sTexBound[1] = false;
    gfx_tev_invalidate();
    sStats.visMono++;
    return true;
}

void gfx_gx_texrect(float ulx, float uly, float lrx, float lry, int tile, float s, float t, float dsdx,
                    float dtdy, bool flip) {
    float st[4][2];
    float dx = lrx - ulx;
    float dy = lry - uly;
    Target target;

    if (lrx <= ulx || lry <= uly) {
        return;
    }
    target = gx_target(false);
    if (target == TARGET_CANVAS) {
        gx_canvas_draw(ulx, uly, lrx, lry, gx_rect_opaque());
    } else if (target != TARGET_FRAME) {
        sStats.offscreen++;
        return;
    } else if (gx_vismono(ulx, uly, lrx, lry, tile, s, t, dsdx, dtdy, flip)) {
        return;
    }
    gx_prepare(GFX_PRIM_TEXRECT, tile);
    if (sScissorEmpty) {
        return;
    }
    if (target == TARGET_FRAME) {
        gfx_fb_frame_drawn((int)uly, (int)ceilf(lry));
    }

    if (!flip) {
        st[0][0] = s;             st[0][1] = t;
        st[1][0] = s + dx * dsdx; st[1][1] = t;
        st[2][0] = s;             st[2][1] = t + dy * dtdy;
        st[3][0] = s + dx * dsdx; st[3][1] = t + dy * dtdy;
    } else {
        // G_TEXRECTFLIP: s advances down the rectangle, t across it
        st[0][0] = s;             st[0][1] = t;
        st[1][0] = s;             st[1][1] = t + dx * dtdy;
        st[2][0] = s + dy * dsdx; st[2][1] = t;
        st[3][0] = s + dy * dsdx; st[3][1] = t + dx * dtdy;
    }
    gx_quad(ulx, uly, lrx, lry, gx_rect_depth(), 0xFFFFFFFF, st);
    sStats.rects++;
    if (target == TARGET_FRAME) {
        sColorClean = sDepthClean = false;
    }
}

void gfx_gx_image_rect(float ulx, float uly, float lrx, float lry, const GfxTexBinding* b, float s, float t,
                       float dsdx, float dtdy) {
    float st[4][2];
    Target target;

    if (lrx <= ulx || lry <= uly || b == NULL || !b->valid) {
        return;
    }
    target = gx_target(false);
    if (target == TARGET_CANVAS) {
        gx_canvas_draw(ulx, uly, lrx, lry, gx_rect_opaque());
    } else if (target != TARGET_FRAME) {
        sStats.offscreen++;
        return;
    }
    // TEXEL0 is the caller's image (already in GX_TEXMAP0), TEXEL1 the white dummy. GX_TILE_IMAGE keeps
    // gx_prepare from binding tiles; the next draw from a tile binds both maps again.
    gfx_gx_flush();
    sTex[0] = *b;
    GX_LoadTexObj(&sDummyTex, GX_TEXMAP1);
    memset(&sTex[1], 0, sizeof(sTex[1]));
    sTex[1].width = 8;
    sTex[1].height = 4;
    sTex[1].sShiftScale = sTex[1].tShiftScale = 1.0f;
    gx_uv_update(0);
    gx_uv_update(1);
    gx_prepare(GFX_PRIM_TEXRECT, GX_TILE_IMAGE);
    if (sScissorEmpty) {
        return;
    }
    if (target == TARGET_FRAME) {
        gfx_fb_frame_drawn((int)uly, (int)ceilf(lry));
    }

    st[0][0] = s;                     st[0][1] = t;
    st[1][0] = s + (lrx - ulx) * dsdx; st[1][1] = t;
    st[2][0] = s;                     st[2][1] = t + (lry - uly) * dtdy;
    st[3][0] = s + (lrx - ulx) * dsdx; st[3][1] = t + (lry - uly) * dtdy;
    gx_quad(ulx, uly, lrx, lry, gx_rect_depth(), 0xFFFFFFFF, st);
    // The quad's texture coordinates were computed with this binding: draw it before anything else is bound
    gfx_gx_flush();
    sStats.rects++;
    if (target == TARGET_FRAME) {
        sColorClean = sDepthClean = false;
    }
}

/* FILL mode fill color as an RGBA8 color: the first of two RGBA5551 pixels, or RGBA8888 for 32-bit images */
static GXColor gx_fill_color(void) {
    u32 fill = gGfxRdp.fillColor;
    GXColor c;

    if (gGfxRdp.colorImageSiz == G_IM_SIZ_32b) {
        c.r = fill >> 24;
        c.g = fill >> 16;
        c.b = fill >> 8;
        c.a = fill;
    } else if (gGfxRdp.colorImageSiz == G_IM_SIZ_8b) {
        // Four 8-bit pixels: an intensity, which goes to red like the other draws into 8-bit images
        c.r = c.g = c.b = fill >> 24;
        c.a = 255;
    } else {
        u32 p = fill >> 16;
        u32 r = (p >> 11) & 0x1F, g = (p >> 6) & 0x1F, b = (p >> 1) & 0x1F;

        c.r = (r << 3) | (r >> 2);
        c.g = (g << 3) | (g >> 2);
        c.b = (b << 3) | (b >> 2);
        c.a = (p & 1) ? 255 : 0;
    }
    return c;
}

/* Whether a rectangle, clipped by the scissor, covers the whole N64 screen. Rectangles are exclusive of
 * their lower-right edge; one pixel of slack also accepts the inclusive FILL-mode form (0, 0, 319, 239). */
static bool gx_covers_screen(float ulx, float uly, float lrx, float lry) {
    float sx0 = gGfxRdp.scissorUlx * 0.25f, sy0 = gGfxRdp.scissorUly * 0.25f;
    float sx1 = gGfxRdp.scissorLrx * 0.25f, sy1 = gGfxRdp.scissorLry * 0.25f;

    return ulx <= 0.0f && uly <= 0.0f && lrx >= GFX_N64_WIDTH - 1 && lry >= GFX_N64_HEIGHT - 1 && sx0 <= 0.0f &&
           sy0 <= 0.0f && sx1 >= GFX_N64_WIDTH - 1 && sy1 >= GFX_N64_HEIGHT - 1;
}

static void gx_fill_depth(float ulx, float uly, float lrx, float lry) {
    static const GXColor black = { 0, 0, 0, 0 };
    bool full = gx_covers_screen(ulx, uly, lrx, lry);

    if (gGfxRdp.fillColor != 0xFFFCFFFC && !sLoggedFillZ) {
        sLoggedFillZ = true;
        gc_log("gfx: z-buffer filled with %08X, cleared to far instead (logged once)", gGfxRdp.fillColor);
    }
    sDepthFilled = true;
    sDepthColored = false;
    if (full && sDepthClean) {
        sStats.fillsSkipped++;
        return;
    }
    gx_set_fill_state(black, false, true);
    if (gGfxRdp.dirty & GFX_DIRTY_SCISSOR) {
        gx_apply_scissor();
        gGfxRdp.dirty &= ~GFX_DIRTY_SCISSOR;
    }
    if (!sScissorEmpty) {
        gx_quad(ulx, uly, lrx, lry, 1.0f, 0, NULL);
        gfx_gx_flush();
    }
    sStats.depthQuads++;
    if (full) {
        sDepthClean = true;
    }
}

/* FILL-mode rectangle into the frame (frame: the EFB clean-state logic applies) or the canvas */
static void gx_fill_color_rect(float ulx, float uly, float lrx, float lry, bool frame) {
    GXColor color = gx_fill_color();
    bool full = frame && gx_covers_screen(ulx, uly, lrx, lry);

    color.a = 255;
    if (full) {
        // The next copy clears the EFB to this color: frames usually start with the same fill
        sClearColor = color;
        if (sColorClean && gx_rgba(&sCleanColor) == gx_rgba(&color)) {
            sStats.fillsSkipped++;
            return;
        }
    }
    gx_set_fill_state(color, true, false);
    if (gGfxRdp.dirty & GFX_DIRTY_SCISSOR) {
        gx_apply_scissor();
        gGfxRdp.dirty &= ~GFX_DIRTY_SCISSOR;
    }
    if (!sScissorEmpty) {
        gx_quad(ulx, uly, lrx, lry, 0.0f, 0, NULL);
        gfx_gx_flush();
    }
    sStats.fillQuads++;
    if (frame) {
        sColorClean = full;
        sCleanColor = color;
    }
}

void gfx_gx_fillrect(float ulx, float uly, float lrx, float lry) {
    bool fillMode = (gGfxRdp.otherModeH & (3 << G_MDSFT_CYCLETYPE)) == G_CYC_FILL;
    Target target;

    if (lrx <= ulx || lry <= uly) {
        return;
    }
    target = gx_target(fillMode);
    if (fillMode) {
        if (target == TARGET_DEPTH) {
            gx_fill_depth(ulx, uly, lrx, lry);
        } else if (target == TARGET_FRAME) {
            // (a fill the clean EFB makes unnecessary still changes the frame's RAM on the N64)
            gfx_fb_frame_drawn((int)uly, (int)ceilf(lry));
            gx_fill_color_rect(ulx, uly, lrx, lry, true);
        } else if (target == TARGET_CANVAS) {
            gx_canvas_draw(ulx, uly, lrx, lry, true);
            gx_fill_color_rect(ulx, uly, lrx, lry, false);
        } else {
            sStats.offscreen++;
        }
        return;
    }

    if (target == TARGET_CANVAS) {
        gx_canvas_draw(ulx, uly, lrx, lry, gx_rect_opaque());
    } else if (target != TARGET_FRAME) {
        sStats.offscreen++;
        return;
    }
    gx_prepare(GFX_PRIM_FILLRECT, sTexTile < 0 ? 0 : sTexTile);
    if (sScissorEmpty) {
        return;
    }
    gx_quad(ulx, uly, lrx, lry, gx_rect_depth(), 0xFFFFFFFF, NULL);
    sStats.rects++;
    if (target == TARGET_FRAME) {
        gfx_fb_frame_drawn((int)uly, (int)ceilf(lry));
        sColorClean = sDepthClean = false;
    }
}

void gfx_gx_state_lost(void) {
    gfx_gx_flush();
    gx_base_state();
    sNumTex = -1;
    sVMode = VMODE_NONE;
    sGxProjValid = false;
    sGxVpValid = false;
    sStateValid = false;
    sKind = -1;
    sTexBound[0] = sTexBound[1] = false;
    gGfxRdp.dirty |= GFX_DIRTY_SCISSOR | GFX_DIRTY_VIEWPORT;
    gfx_tev_invalidate();
}

/* ============================================================================================== */
/* Tasks and frames                                                                               */
/* ============================================================================================== */

void gfx_gx_task_begin(void) {
    GFX_PROF_TASK_BEGIN();
    // The thread that writes the FIFO is the one GX suspends when the FIFO fills up
    if (GX_GetCurrentGXThread() != LWP_GetSelf()) {
        GX_SetCurrentGXThread();
    }
    sTaskStart = gettime();
    if (sGpuBusy) {
        // The last task's draws and display copy finish before this task changes textures or reads anything GX
        // wrote (gfx_tex.c and gfx_fb.c assume an idle GX at task start). Usually long done by now.
        u32 level;
        int i;

        GFX_PROF_ENTER(GFX_PROF_WAIT);
        GX_DrawDone();
        GFX_PROF_LEAVE();
        sGpuBusy = false;
        _CPU_ISR_Disable(level);
        for (i = 0; i < sSlotCount; i++) {
            sSlots[i].copying = false;
        }
        _CPU_ISR_Restore(level);
    }
    sFrameKey = 0;
    sTargetValid = false;
    sStateValid = false;
    sKind = -1;
    sTexTile = -1;
    sTexBound[0] = sTexBound[1] = false;
    sBatchCount = 0;
    sCanvasKey = 0;
    sScale = GFX_SCALE;
    sTargetW = GFX_EFB_WIDTH;
    sTargetH = GFX_EFB_HEIGHT;
    sDepthFilled = sDepthColored = false;
    sVisMonoNext = -1;
    gfx_fb_task_begin();
}

static void gx_log_stats(u64 now) {
    u32 ms = ticks_to_millisecs(now - sStatsStart);
    u32 tasks = (sStats.tasks != 0) ? sStats.tasks : 1;
    GfxFbStats fb;

    gc_log("gx: %u tasks in %u ms, %u frames; per task: %u tris (%u via CPU, %u dropped), %u rects, %u batches; "
           "fills: %u color, %u depth, %u skipped; %u off-screen draws, %u skipped; %u VisMono rects; task %u us avg, "
           "%u us max",
           sStats.tasks, ms, sStats.frames, sStats.tris / tasks, sStats.cpuTris / tasks, sStats.droppedTris / tasks,
           sStats.rects / tasks, sStats.batches / tasks, sStats.fillQuads, sStats.depthQuads, sStats.fillsSkipped,
           sStats.canvasDraws, sStats.offscreen, sStats.visMono, (u32)ticks_to_microsecs(sStats.taskTicks / tasks),
           (u32)ticks_to_microsecs(sStats.maxTaskTicks));
#if GFX_VTX_CHECK
    gc_log("gx: uv check: %u texture coordinates compared since boot, %u differ", sChkUv, sChkUvBad);
#endif
    gfx_fb_take_stats(&fb);
    if (fb.readbacks != 0 || fb.passes != 0 || fb.depthWrites != 0) {
        gc_log("gx: fb: %u frame readbacks (%u rows), %u off-screen passes, %u writes and %u loads of off-screen "
               "images, %u depth writes; %u us per task, %u us of it waiting for GX", fb.readbacks, fb.readbackRows,
               fb.passes, fb.copyOuts, fb.uploads, fb.depthWrites, (u32)ticks_to_microsecs(fb.ticks / tasks),
               (u32)ticks_to_microsecs(fb.waitTicks / tasks));
    }
    memset(&sStats, 0, sizeof(sStats));
    sStatsStart = now;
}

/* A slot that is neither on screen nor about to be (libogc's current framebuffer is the one latched at
 * the last retrace, the next one is latched at the coming retrace): the least recently used. With 4 slots that is
 * never the slot that left the screen at the last retrace, which a late retrace interrupt could leave the VI still
 * scanning out (at 60 fps the frame two swaps back, which holds the same N64 framebuffer, is that slot). Marked
 * busy, so the VI thread cannot present it. */
static int gx_pick_slot(u32 key) {
    void* current;
    void* next;
    u32 level;
    int slot = -1;
    int i;

    // Read libogc's framebuffers with interrupts off too: a retrace (current = next) followed by a present
    // between two unguarded reads would leave the slot on screen out of both
    _CPU_ISR_Disable(level);
    current = VIDEO_GetCurrentFramebuffer();
    next = VIDEO_GetNextFramebuffer();
    for (i = 0; i < sSlotCount; i++) {
        if (sSlots[i].busy || i == sPresented || sSlots[i].xfb == current || sSlots[i].xfb == next) {
            continue;
        }
        if (slot < 0 || sSlots[i].stamp < sSlots[slot].stamp) {
            slot = i;
        }
    }
    if (slot >= 0) {
        sSlots[slot].key = 0;
        sSlots[slot].busy = true;
    }
    _CPU_ISR_Restore(level);
    return slot;
}

void gfx_gx_task_end(void) {
    u32 level;
    u32 key = sFrameKey;
    u64 now;
    u64 ticks;
    int slot = -1;
    int i;

    GFX_PROF_ENTER(GFX_PROF_END);
    gfx_gx_flush();
    // An off-screen pass still open ends here; the z-buffer's RAM gets the frame's depth if the task cleared it
    // and drew no colors into it since
    gx_use_frame();
    GFX_PROF_ENTER(GFX_PROF_FB);
    gfx_fb_task_end(gfx_addr(gGfxRdp.zImageAddr), sDepthFilled && !sDepthColored);
    GFX_PROF_LEAVE();

    if (key != 0) {
        // With 3 XFBs one is always free; with 2, the previous frame stays on screen until the next retrace
        for (i = 0; (slot = gx_pick_slot(key)) < 0 && i < 4; i++) {
            VIDEO_WaitVSync();
        }
    }

    if (slot >= 0) {
        // The copy also clears the EFB (color and depth), which needs color and z updates enabled
        GX_SetColorUpdate(GX_TRUE);
        GX_SetAlphaUpdate(GX_TRUE);
        GX_SetZMode(GX_TRUE, GX_ALWAYS, GX_TRUE);
        GX_SetCopyClear(sClearColor, GX_MAX_Z24);
        GX_CopyDisp(sSlots[slot].xfb, GX_TRUE);
        // Written to the PE token register once the copy is in the XFB (gfx_gx_present waits for it)
        GX_SetDrawSync(++sCopyToken);
        sStateValid = false;
        gfx_tev_invalidate();
    }
#if GFX_LAZY_SYNC
    // No wait here: GX finishes the frame while the game runs, and the next task begins by waiting for it
    GX_Flush();
    sGpuBusy = true;
#else
    GFX_PROF_ENTER(GFX_PROF_WAIT);
    GX_DrawDone();
    GFX_PROF_LEAVE();
#endif

    if (slot >= 0) {
        sColorClean = sDepthClean = true;
        sCleanColor = sClearColor;
        _CPU_ISR_Disable(level);
        for (i = 0; i < sSlotCount; i++) {
            if (sSlots[i].key == key) {
                sSlots[i].key = 0;
            }
        }
        sSlots[slot].key = key;
        sSlots[slot].stamp = ++sStamp;
        sSlots[slot].copying = GFX_LAZY_SYNC;
        sSlots[slot].token = sCopyToken;
        sSlots[slot].busy = false;
        _CPU_ISR_Restore(level);
        sStats.frames++;
    } else if (key != 0 && sLogCount < LOG_LIMIT) {
        sLogCount++;
        gc_log("gfx: no free XFB for the frame in %08X", key);
    }

    now = gettime();
    ticks = now - sTaskStart;
    sStats.tasks++;
    sStats.taskTicks += ticks;
    if (ticks > sStats.maxTaskTicks) {
        sStats.maxTaskTicks = ticks;
    }
    if (ticks_to_millisecs(now - sStatsStart) >= STATS_INTERVAL_MS) {
        gx_log_stats(now);
    }
    GFX_PROF_LEAVE();
    GFX_PROF_TASK_END();
}

/* GX has written the PE token register with `token` or a later one (tokens increase; GX processes them in order) */
static inline bool gx_token_reached(u16 token) {
    return (s16)(GX_GetDrawSync() - token) >= 0;
}

/* VI thread: the copy into a slot about to be shown may still be running (gfx_gx_task_end does not wait for it).
 * It is nearly always done by the retrace after the game swaps; otherwise wait for its token, which is at most the
 * GX work queued at the end of the task. The wait sleeps between polls: this thread runs above every game thread
 * and the audio thread, and spinning would keep them off the CPU until GX is done (the frame is latched at the
 * next retrace anyway). */
#define COPY_WAIT_POLL_US 100
static void gx_wait_copy(int slot, u16 token) {
    u64 start = gettime();
    u32 level;

    while (!gx_token_reached(token)) {
        if (ticks_to_millisecs(gettime() - start) > 100) {
            if (!sLoggedCopyWait) {
                sLoggedCopyWait = true;
                gc_log("gfx: display copy not finished after 100 ms (token %u, GX at %u), shown anyway (logged once)",
                       token, GX_GetDrawSync());
            }
            break;
        }
        usleep(COPY_WAIT_POLL_US);
    }
    _CPU_ISR_Disable(level);
    if (sSlots[slot].token == token) {
        sSlots[slot].copying = false;
    }
    _CPU_ISR_Restore(level);
}

void gfx_gx_present(const void* n64Framebuffer) {
    u32 key = gx_key((u32)n64Framebuffer);
    u32 level;
    void* xfb = NULL;
    bool copying = false;
    u16 token = 0;
    int i;

    if (!sReady || key == 0) {
        return;
    }
    _CPU_ISR_Disable(level);
    for (i = 0; i < sSlotCount; i++) {
        if (sSlots[i].key == key && !sSlots[i].busy) {
            break;
        }
    }
    if (i < sSlotCount && i != sPresented) {
        sPresented = i;
        sSlots[i].stamp = ++sStamp;
        xfb = sSlots[i].xfb;
        copying = sSlots[i].copying;
        token = sSlots[i].token;
    }
    _CPU_ISR_Restore(level);

    if (xfb != NULL) {
        // (sPresented keeps gx_pick_slot away from this slot while it waits)
        if (copying) {
            gx_wait_copy(i, token);
        }
        gc_ogc_video_show_frame(xfb);
    } else if (i >= sSlotCount && sStamp != 0 && !sLoggedNoFrame) {
        // (before the first rendered frame this is the boot framebuffer: not interesting)
        sLoggedNoFrame = true;
        gc_log("gfx: present %08X: no frame was rendered into it (logged once)", key);
    }
}
