/**
 * RSP side of the N64 renderer: the display list interpreter for F3DZEX2 (gspF3DZEX2_NoN_PosLight_fifo,
 * "F3DZEX.NoN fifo 2.08I"), with matrices, the vertex pipeline (transform, clip codes, lighting with point
 * lights, texgen, fog), triangle rejection and culling, and statistics. RDP commands go to gfx_rdp.c.
 *
 * Behaviour follows the F3DEX2 disassembly at github.com/Mr-Wiseguy/f3dex2 (CC0), whose build options
 * for MM's microcode are CFG_G_BRANCH_W, CFG_NoN and CFG_POINT_LIGHTING. Points that differ from what
 * one might expect, or from some emulators:
 *   - G_BRANCH_Z ("G_BRANCH_W" in F3DZEX) compares the integer part of the vertex's clip w, not its
 *     screen z. MM's gsSPBranchLessZraw thresholds are distances (0x960, 0xFA0, 0x1770...).
 *   - G_TRI2 and G_QUAD draw the triangle in w1 first, then the one in w0.
 *   - Flat shading takes RGB from the triangle's first vertex; alpha (vertex alpha or fog) stays per vertex.
 *   - G_LOAD_UCODE reloads DMEM from 0x180 on with the new microcode's data. Coming back to F3DZEX2 resets
 *     the geometry mode (G_CLIPPING), fog factor, G_TEXTURE, lights and light count. Matrices, the
 *     viewport, segments and the display list stack live below 0x180 and survive.
 *   - Light directions are transformed into model space only when the light count is written or a
 *     matrix changes (G_MTX, G_POPMTX); loading a light with G_MOVEMEM does not refresh them. Colors
 *     and point light data are read when vertices are lit. Normalized directions are stored as
 *     floor(128 * d), saturated to s8 (127 along an axis, -128 against it).
 *   - Matrix products (G_MTX_MUL, MV * P) saturate to s15.16 like the microcode's mtx_multiply.
 *   - G_MW_PERSPNORM's word also overwrites the low half of the G_RDPHALF_1 value (they share DMEM).
 *   - In a G_VTX pair the first vertex (even index) is lit with the colc copy of each light color and
 *     the second with col.
 *   - Clip codes are the microcode's VCH/VCL tests: x >= w, x <= -w (also meaningful for w < 0), etc.
 *     Trivial rejection and G_CULLDL use x, y, far (z >= w) and w <= 0 (NoN: no near plane).
 *   - G_MW_FORCEMTX only marks the MVP as valid. G_MV_MATRIX writes the MVP directly; it stays in use
 *     until a G_MTX or G_POPMTX makes the microcode recompute MV * P.
 *
 * While S2DEX2 is loaded (G_LOAD_UCODE), the commands it shares with F3DEX2 run here (display lists, other modes,
 * segments, G_RDPHALF_*, RDP commands) as well as G_SELECT_DL and its status words; its own commands go to
 * gfx_s2dex.c. Its G_RDPHALF_0 is F3DEX2's G_TEXRECT opcode and works as one: the first half of a texture
 * rectangle, and the parameters of a G_SELECT_DL.
 *
 * Unlike the RSP, G_DL / G_BRANCH_Z to an address outside RAM is skipped (logged once) instead of running
 * garbage, so the rest of the frame still draws.
 *
 * Vertices stay in clip space; GX clips. The projection given to gfx_gx_set_projection() is the one the
 * current MVP was built with (or the forced MVP itself), sent at the G_VTX that first uses it, so that
 * vertices already in the buffer keep the projection they were transformed with.
 *
 * Lighting, texgen and fog use the microcode's fixed point steps where that is cheap (integer dot
 * products of s8 normals and s8 model space light directions); positions and matrices are float.
 *
 * Speed (results are bit-identical to the plain C code, which the host test and other CPUs still run):
 *   - On the GameCube, vertex positions, clip codes and matrices use the Gekko's paired singles: psq_l converts the
 *     s16 positions and the halves of s15.16 matrices to floats through the GQRs, and ps_madd evaluates the products
 *     in the order and with the fusing that GCC gives the C expressions.
 *   - G_VTX computes positions and clip codes only. A vertex's color (lighting), alpha (fog) and texture coordinates
 *     are computed when a triangle that uses it passes rejection and culling: most vertices belong only to rejected
 *     or culled triangles. G_VTX records the geometry mode bits, G_TEXTURE scale and fog they use (sAttr); before
 *     anything else they use changes, the vertices still waiting get their attributes (rsp_attrs_flush): a G_VTX
 *     with other recorded state or that rebuilds the lights, the modelview under point lights, G_MODIFYVTX of the
 *     vertex, microcode switches.
 *   - -DGFX_VTX_CHECK=1 runs the original eager pipeline next to it and logs every difference.
 */
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "gfx_internal.h"
#include "gfx_prof.h"

// Dummy microcode symbols (port/gc/game/ucode_dummies.c). G_LOAD_UCODE is identified by its text address.
extern unsigned char gspS2DEX2_fifoTextStart[], gspF3DZEX2_NoN_PosLight_fifoTextStart[];

#ifdef GFX_HOST_TEST
// Host test (port/gc/tests/gfx_host): N64 address 0x80000000 + x lives at host address 0x80000000 + x + bias
extern uintptr_t gGfxHostRamBias;
#define RAM_BIAS gGfxHostRamBias
#else
#define RAM_BIAS ((uintptr_t)0)
#endif

#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x01800000u // GameCube MEM1

/* Paired-single code paths (Gekko) */
#if defined(__PPC__) && defined(GEKKO) && !defined(GFX_HOST_TEST)
#define RSP_PAIRED 1
#else
#define RSP_PAIRED 0
#endif
/* GQR values (load and store halves alike): type in bits 0-2 of each half, scale in bits 8-13. The GQRs are global:
 * libogc's thread switches keep each thread's paired-single halves but not its GQRs (it only relies on GQR0 = 0,
 * float, which nothing here changes). The asm below sets the GQRs it uses right before using them, and always to
 * these values, so other code that uses GQR2-GQR5 must use the same values (or switch to GQR6-GQR7):
 * GQR2 = s16, GQR3 = u16 / 65536, GQR4 = s16 / 32 (texture coordinates), GQR5 = float. */
#define RSP_GQR_S16 0x00070007u
#define RSP_GQR_U16_FRAC 0x10051005u
#define RSP_GQR_S16_ST 0x05070507u

#ifndef GFX_VTX_CHECK
#define GFX_VTX_CHECK 0
#endif

#define VTX_COUNT 32
#define MTX_STACK_MAX 32
#define MTX_STACK_N64 16 // the game's 1 KiB DRAM matrix stack (SP_DRAM_STACK_SIZE8 / sizeof(Mtx))
#define DL_STACK_MAX 32
#define DL_STACK_N64 18  // F3DEX2 keeps 18 return addresses in DMEM
#define LIGHT_SIZE 24    // DMEM light slot: 16 bytes of Light + 8 bytes of transformed direction
#define LIGHT_SLOTS 10   // lookat X, lookat Y, then up to 7 lights and the ambient light
#define LIGHT_MAIN 2     // first slot of lightBufferMain
#define MAX_LIGHTS 7
#define CMD_LIMIT (1u << 22) // runaway guard per task

typedef struct {
    uint8_t col[2][3]; // [0] = colc (first vertex of a pair), [1] = col (second vertex)
    uint8_t kc, kl, kq; // point light attenuation; kc != 0 marks a point light
    float pos[3];       // point light position (world space, s16)
} RspLight;

typedef struct {
    float mv[4][4];
    float p[4][4];
    float mvp[4][4];
    float pUsed[4][4]; // projection the MVP was last computed with
    float mvStack[MTX_STACK_MAX][4][4];
    int mvDepth;
    bool mvpValid;
    bool lightsValid;
    bool mvpForced; // the MVP was written by G_MV_MATRIX / G_MW_MATRIX instead of computed
    uint32_t pVer, pUsedVer, forcedVer;
    // Projection last given to gfx_gx_set_projection() in this task
    bool gxProjSent;
    uint32_t gxProjKey;
    float gxProj[4][4];
    // Lights: DMEM image of lightBufferLookat + lightBufferMain (as written by G_MV_LIGHT / G_MW_LIGHTCOL),
    // transformed s8 directions per slot, and the parsed lights
    uint8_t lightRaw[LIGHT_SLOTS * LIGHT_SIZE];
    int8_t lightDir[LIGHT_SLOTS][3];
    RspLight lights[MAX_LIGHTS + 1];
    uint8_t numLights18; // numLightsx18: number of lights (not counting ambient) * 24
    int numLights;
    bool lightsParsed;
    bool hasPointLight;
    uint8_t viewportRaw[16];
    uint16_t texScale[2]; // G_TEXTURE s, t scale (0.16 fixed point)
    uint32_t rdpHalf1;
    uint32_t texrectW0, texrectW1;
    bool texrectSet;
    uint16_t perspNorm;
    bool s2dex;  // S2DEX2 is loaded (until F3DZEX2 comes back)
    bool vpFlip; // viewport mirrors one axis: screen space winding is reversed
} RspState;

typedef struct {
    uint32_t tasks, cmds, vtx, tris, drawn, clipped, culled, rects;
    uint64_t ticks, maxTicks;
    uint64_t lastLog;
} RspStats;

enum {
    LOG_BAD_DL,
    LOG_BAD_VTX,
    LOG_VTX_RANGE,
    LOG_TRI_INDEX,
    LOG_BAD_MTX,
    LOG_MTX_N64_DEPTH,
    LOG_MTX_OVERFLOW,
    LOG_DL_N64_DEPTH,
    LOG_DL_OVERFLOW,
    LOG_RUNAWAY,
    LOG_PC_RANGE,
    LOG_LINE3D,
    LOG_DMA_IO,
    LOG_SPECIAL,
    LOG_MOVEMEM,
    LOG_MOVEMEM_RANGE,
    LOG_MOVEWORD,
    LOG_MOVEWORD_OFS,
    LOG_MODIFYVTX,
    LOG_MODIFYVTX_W,
    LOG_UCODE,
    LOG_S2DEX,
    LOG_HALF2,
    LOG_NUMLIGHTS,
    LOG_COUNT
};

uint32_t gGfxSegments[16];
GfxRspState gGfxRsp;

static RspState sRsp;
static GfxVtx sVtx[VTX_COUNT] __attribute__((aligned(32)));
static RspStats sStats;
static uint8_t sLogged[LOG_COUNT];
static uint32_t sLoggedOp[8];
static uint32_t sLoggedS2dexOp[8];

#define LOG_ONCE(id, ...)          \
    do {                           \
        if (!sLogged[id]) {        \
            sLogged[id] = 1;       \
            gc_log(__VA_ARGS__);   \
        }                          \
    } while (0)

/* ============================================================================================== */
/* Optional per-phase profile (gfx_prof.h, -DGFX_PROF=1)                                          */
/* ============================================================================================== */

#if GFX_PROF && !defined(GFX_HOST_TEST)
static const char* const sProfNames[GFX_PROF_COUNT] = {
    "other", "dl", "vtx", "tri", "gxtri", "prep", "tev", "bind", "flush", "rdp", "rect", "fb", "end", "wait", "s2d",
    "attr", "vsetup",
};
static uint32_t sProfTicks[GFX_PROF_COUNT];
static uint32_t sProfCalls[GFX_PROF_COUNT];
static uint8_t sProfStack[32];
static int sProfDepth;
static uint8_t sProfCur;
static uint32_t sProfLast;
static uint32_t sProfTasks;
/* Vertex work: lit vertices (directional, positional), light evaluations (directional, point), fog, texgen */
static uint32_t sProfVtxLit, sProfVtxPos, sProfDirEvals, sProfPointEvals, sProfVtxFog, sProfVtxTexgen;
#define PROF_COUNT(var, n) ((var) += (n))
/* Texture command sequences (image, tiles, loads) between triangles, and how many repeat the previous one exactly
 * (from the same texture image state): those draws could keep the textures already bound */
static uint32_t sProfTexHash, sProfTexPrevHash, sProfTexSeqs, sProfTexSame;
static bool sProfTexOpen;
static uint32_t sProfOps[256]; /* display list commands by opcode */

static inline void prof_tex_cmd(uint32_t w0, uint32_t w1) {
    if (!sProfTexOpen) {
        sProfTexOpen = true;
        sProfTexHash = 0x811C9DC5u ^ gGfxRdp.texImageAddr ^ ((uint32_t)gGfxRdp.texImageWidth << 16) ^
                       ((uint32_t)gGfxRdp.texImageFmt << 8) ^ gGfxRdp.texImageSiz;
    }
    sProfTexHash = ((sProfTexHash ^ w0) * 0x9E3779B1u) ^ w1;
    sProfTexHash *= 0x85EBCA6Bu;
}

static inline void prof_tex_draw(void) {
    if (sProfTexOpen) {
        sProfTexOpen = false;
        sProfTexSeqs++;
        sProfTexSame += (sProfTexHash == sProfTexPrevHash);
        sProfTexPrevHash = sProfTexHash;
    }
}

static inline uint32_t prof_now(void) {
    uint32_t t;

    __asm__ volatile("mftb %0" : "=r"(t));
    return t;
}

void gfx_prof_enter(int phase) {
    uint32_t now = prof_now();

    sProfTicks[sProfCur] += now - sProfLast;
    sProfLast = now;
    if (sProfDepth < (int)sizeof(sProfStack)) {
        sProfStack[sProfDepth] = sProfCur;
    }
    sProfDepth++;
    sProfCur = (uint8_t)phase;
    sProfCalls[phase]++;
}

void gfx_prof_leave(void) {
    uint32_t now = prof_now();

    sProfTicks[sProfCur] += now - sProfLast;
    sProfLast = now;
    if (sProfDepth > 0) {
        sProfDepth--;
        sProfCur = (sProfDepth < (int)sizeof(sProfStack)) ? sProfStack[sProfDepth] : GFX_PROF_OTHER;
    }
}

void gfx_prof_task_begin(void) {
    sProfLast = prof_now();
    sProfCur = GFX_PROF_OTHER;
    sProfDepth = 0;
}

void gfx_prof_task_end(void) {
    uint32_t now = prof_now();

    sProfTicks[sProfCur] += now - sProfLast;
    sProfLast = now;
    sProfTasks++;
}

static void prof_log(uint32_t ms) {
    char line[512];
    char calls[512];
    int len = 0, clen = 0;
    int i;

    if (sProfTasks == 0) {
        return;
    }
    for (i = 0; i < GFX_PROF_COUNT; i++) {
        len += snprintf(line + len, sizeof(line) - len, "%s%s %u", (i == 0) ? "" : ", ", sProfNames[i],
                        (unsigned int)((uint64_t)sProfTicks[i] * 1000000 / GC_TB_HZ / sProfTasks));
        clen += snprintf(calls + clen, sizeof(calls) - clen, "%s%s %u", (i == 0) ? "" : ", ", sProfNames[i],
                         (unsigned int)(sProfCalls[i] / sProfTasks));
        if (len >= (int)sizeof(line) || clen >= (int)sizeof(calls)) {
            break;
        }
    }
    gc_log("gfx_prof: %u tasks in %u ms; per task us: %s", (unsigned int)sProfTasks, (unsigned int)ms, line);
    gc_log("gfx_prof: calls per task: %s", calls);
    gc_log("gfx_prof: per task: %u lit vtx (%u positional), %u directional and %u point light evaluations, %u fog vtx, "
           "%u texgen vtx", (unsigned int)(sProfVtxLit / sProfTasks), (unsigned int)(sProfVtxPos / sProfTasks),
           (unsigned int)(sProfDirEvals / sProfTasks), (unsigned int)(sProfPointEvals / sProfTasks),
           (unsigned int)(sProfVtxFog / sProfTasks), (unsigned int)(sProfVtxTexgen / sProfTasks));
    gc_log("gfx_prof: per task: %u texture command sequences before triangles, %u repeat the previous one",
           (unsigned int)(sProfTexSeqs / sProfTasks), (unsigned int)(sProfTexSame / sProfTasks));
    sProfTexSeqs = sProfTexSame = 0;
    {
        // The most frequent display list commands
        int k;

        clen = 0;
        for (k = 0; k < 12; k++) {
            int best = -1, op;

            for (op = 0; op < 256; op++) {
                if (sProfOps[op] != 0 && (best < 0 || sProfOps[op] > sProfOps[best])) {
                    best = op;
                }
            }
            if (best < 0) {
                break;
            }
            clen += snprintf(calls + clen, sizeof(calls) - clen, "%s%02X %u", (k == 0) ? "" : ", ", best,
                             (unsigned int)(sProfOps[best] / sProfTasks));
            sProfOps[best] = 0;
            if (clen >= (int)sizeof(calls)) {
                break;
            }
        }
        gc_log("gfx_prof: commands per task by opcode: %s", calls);
        memset(sProfOps, 0, sizeof(sProfOps));
    }
    memset(sProfTicks, 0, sizeof(sProfTicks));
    memset(sProfCalls, 0, sizeof(sProfCalls));
    sProfTasks = 0;
    sProfVtxLit = sProfVtxPos = sProfDirEvals = sProfPointEvals = sProfVtxFog = sProfVtxTexgen = 0;
}
#else
#define PROF_COUNT(var, n) ((void)0)
#endif

/* ============================================================================================== */
/* Helpers                                                                                        */
/* ============================================================================================== */

// Display lists and N64 structures are big endian, the GameCube's own byte order
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
static inline uint16_t rd16(const uint8_t* p) {
    uint16_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
static inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}
#else
static inline uint16_t rd16(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}
static inline uint32_t rd32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
#endif

static inline int32_t sat16(int32_t v) {
    return (v < -32768) ? -32768 : (v > 32767) ? 32767 : v;
}

static inline float clampf(float v, float lo, float hi) {
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

// floor() without a libm call. Floats of magnitude >= 2^23 are already integers (and may not fit an int32).
static inline float floorf_fast(float x) {
    float t;

    if (!(x > -8388608.0f && x < 8388608.0f)) {
        return x;
    }
    t = (float)(int32_t)x;
    return (t > x) ? t - 1.0f : t;
}

/* clampf(floorf_fast(x), -32768, 32767) without integer conversions (they go through memory on the Gekko). Below
 * 2^22, (x + 1.5 * 2^23) - 1.5 * 2^23 is x rounded to an integer (to nearest); floor is one less if that is above
 * x. NaN stays NaN, as through floorf_fast and clampf. */
static inline float floor_clamp16(float x) {
    float r;

    if (x >= 32767.0f) {
        return 32767.0f;
    }
    if (x < -32768.0f) {
        return -32768.0f;
    }
    r = (x + 12582912.0f) - 12582912.0f;
    return (r > x) ? r - 1.0f : r;
}

static inline float rsqrtf_fast(float x) {
#if defined(__PPC__)
    float e;
    __asm__("frsqrte %0,%1" : "=f"(e) : "f"(x));
    e = e * (1.5f - 0.5f * x * e * e);
    e = e * (1.5f - 0.5f * x * e * e);
    return e;
#else
    return 1.0f / sqrtf(x);
#endif
}

/* A normalized direction component (|v| <= 128) as the microcode stores it: the top byte of a s0.15 value,
 * i.e. floor(v), saturated to the s8 range (an axis gives 127, its opposite -128) */
static inline int8_t to_s8_floor(float v) {
    float r;

    if (!(v > -128.0f)) {
        return -128; // also NaN
    }
    if (v >= 127.0f) {
        return 127;
    }
    // floor(v) as floor_clamp16 computes it (one float to integer conversion instead of three)
    r = (v + 12582912.0f) - 12582912.0f;
    if (r > v) {
        r -= 1.0f;
    }
    return (int8_t)(int32_t)r;
}

/* ============================================================================================== */
/* Addresses                                                                                      */
/* ============================================================================================== */

/* Segmented / KSEG address -> 0x80000000-based N64 address (0 stays 0). Segment bases may be physical or
 * KSEG0; the sum is masked to 29 bits either way. */
static inline uint32_t rsp_resolve(uint32_t addr) {
    if (addr == 0) {
        return 0;
    }
    if (addr & 0x80000000) {
        return RAM_BASE | (addr & 0x1FFFFFFF);
    }
    return RAM_BASE | ((gGfxSegments[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF)) & 0x1FFFFFFF);
}

void* gfx_addr(uint32_t addr) {
    uint32_t a = rsp_resolve(addr);

    return (a != 0) ? (void*)((uintptr_t)a + RAM_BIAS) : NULL;
}

/* CPU pointer to `size` bytes at a display list address, or NULL if they are not all in MEM1 */
static const uint8_t* rsp_ptr(uint32_t addr, uint32_t size) {
    uint32_t a = rsp_resolve(addr);

    if (a == 0 || size > RAM_SIZE || a - RAM_BASE > RAM_SIZE - size) {
        return NULL;
    }
    return (const uint8_t*)((uintptr_t)a + RAM_BIAS);
}

static inline uint32_t symbol_phys(const void* sym) {
    return (uint32_t)((uintptr_t)sym - RAM_BIAS) & 0x1FFFFFFF;
}

/* Vertex attributes still to compute (see "Vertices") */
static void rsp_attrs_flush(void);
static void rsp_attrs_before_mv_change(void);

#if GFX_VTX_CHECK
/* Check build: comparisons with the original code and the differences found */
static uint32_t sChkPos, sChkAttr, sChkBad;
static uint32_t sChkMtx __attribute__((unused)); /* paired-single matrices (GameCube only) */
static int sChkLogged;

/* For the host test's check build (port/gc/tests/gfx_host) */
uint32_t gfx_rsp_vtx_check_failures(void);
uint32_t gfx_rsp_vtx_check_failures(void) {
    return sChkBad;
}
#endif

/* ============================================================================================== */
/* Matrices                                                                                       */
/* ============================================================================================== */

#if !RSP_PAIRED || GFX_VTX_CHECK
/* out = a * b (row vectors: v * a * b). Like the microcode's mtx_multiply, elements saturate to the s15.16
 * range, which also keeps repeated G_MTX_MUL products finite. */
static void mtx_mul_c(float out[4][4], const float a[4][4], const float b[4][4]) {
    float t[4][4];
    int i, j;

    for (i = 0; i < 4; i++) {
        float a0 = a[i][0], a1 = a[i][1], a2 = a[i][2], a3 = a[i][3];

        for (j = 0; j < 4; j++) {
            float v = a0 * b[0][j] + a1 * b[1][j] + a2 * b[2][j] + a3 * b[3][j];

            t[i][j] = (v < -32768.0f) ? -32768.0f : (v > 32767.998f) ? 32767.998f : v;
        }
    }
    memcpy(out, t, sizeof(t));
}
#endif

/* N64 Mtx: 16 integer halves, then 16 fraction halves, row major; element = (int << 16 | frac) / 65536 */
static void mtx_from_raw_c(float m[4][4], const uint8_t* raw) {
    int i;

    for (i = 0; i < 16; i++) {
        int32_t v = (int32_t)(((uint32_t)rd16(raw + 2 * i) << 16) | rd16(raw + 32 + 2 * i));

        m[i >> 2][i & 3] = (float)v * (1.0f / 65536.0f);
    }
}

#if RSP_PAIRED
static const float sSatPair[2][2] ATTRIBUTE_ALIGN(8) = { { -32768.0f, -32768.0f }, { 32767.998f, 32767.998f } };

/* mtx_mul_c with paired singles. GCC evaluates each element as fma(a3, b3j, fma(a2, b2j, fma(a0, b0j, a1 * b1j)))
 * (the product a1 * b1j rounded, the others fused), and so does this, two columns at a time. The saturation selects
 * with ps_sel on v - lo and hi - v: products of saturated elements are always finite. b is read whole first and
 * each row of a before that row is written, so out may be a or b. */
static void mtx_mul_ps(float out[4][4], const float a[4][4], const float b[4][4]) {
    const float* pa = &a[0][0];
    float* po = &out[0][0];
    uint32_t i;

    __asm__ volatile("mtspr 917, %[f32]\n\t"
                     "psq_l 0, 0(%[b]), 0, 5\n\t"
                     "psq_l 1, 8(%[b]), 0, 5\n\t"
                     "psq_l 2, 16(%[b]), 0, 5\n\t"
                     "psq_l 3, 24(%[b]), 0, 5\n\t"
                     "psq_l 4, 32(%[b]), 0, 5\n\t"
                     "psq_l 5, 40(%[b]), 0, 5\n\t"
                     "psq_l 6, 48(%[b]), 0, 5\n\t"
                     "psq_l 7, 56(%[b]), 0, 5\n\t"
                     "psq_l 8, 0(%[sat]), 0, 5\n\t"
                     "psq_l 9, 8(%[sat]), 0, 5\n\t"
                     "li %[i], 4\n\t"
                     "mtctr %[i]\n"
                     "1:\n\t"
                     "psq_l 10, 0(%[a]), 0, 5\n\t"
                     "psq_l 11, 8(%[a]), 0, 5\n\t"
                     "ps_muls1 12, 2, 10\n\t"
                     "ps_muls1 13, 3, 10\n\t"
                     "ps_madds0 12, 0, 10, 12\n\t"
                     "ps_madds0 13, 1, 10, 13\n\t"
                     "ps_madds0 12, 4, 11, 12\n\t"
                     "ps_madds0 13, 5, 11, 13\n\t"
                     "ps_madds1 12, 6, 11, 12\n\t"
                     "ps_madds1 13, 7, 11, 13\n\t"
                     "ps_sub 10, 12, 8\n\t"
                     "ps_sub 11, 13, 8\n\t"
                     "ps_sel 12, 10, 12, 8\n\t"
                     "ps_sel 13, 11, 13, 8\n\t"
                     "ps_sub 10, 9, 12\n\t"
                     "ps_sub 11, 9, 13\n\t"
                     "ps_sel 12, 10, 12, 9\n\t"
                     "ps_sel 13, 11, 13, 9\n\t"
                     "psq_st 12, 0(%[o]), 0, 5\n\t"
                     "psq_st 13, 8(%[o]), 0, 5\n\t"
                     "addi %[a], %[a], 16\n\t"
                     "addi %[o], %[o], 16\n\t"
                     "bdnz 1b"
                     : [a] "+b"(pa), [o] "+b"(po), [i] "=&r"(i)
                     : [b] "b"(&b[0][0]), [sat] "b"(&sSatPair[0][0]), [f32] "r"(0)
                     : "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "fr12",
                       "fr13", "ctr", "memory");
}

/* mtx_from_raw_c with paired singles: the integer halves load as s16, the fractions as u16 / 65536, and their sum
 * rounds once, to the same float as the s15.16 value converted whole. (Matrices are 8-byte aligned: the RSP's DMA
 * needs that. Anything else takes the C path.) */
static void mtx_from_raw_ps(float m[4][4], const uint8_t* raw) {
    if ((uintptr_t)raw & 3) {
        mtx_from_raw_c(m, raw);
        return;
    }
    __asm__ volatile("mtspr 914, %[s16]\n\t"
                     "mtspr 915, %[u16]\n\t"
                     "mtspr 917, %[f32]\n\t"
                     "psq_l 0, 0(%[r]), 0, 2\n\t"
                     "psq_l 1, 32(%[r]), 0, 3\n\t"
                     "psq_l 2, 4(%[r]), 0, 2\n\t"
                     "psq_l 3, 36(%[r]), 0, 3\n\t"
                     "psq_l 4, 8(%[r]), 0, 2\n\t"
                     "psq_l 5, 40(%[r]), 0, 3\n\t"
                     "psq_l 6, 12(%[r]), 0, 2\n\t"
                     "psq_l 7, 44(%[r]), 0, 3\n\t"
                     "ps_add 0, 0, 1\n\t"
                     "ps_add 2, 2, 3\n\t"
                     "ps_add 4, 4, 5\n\t"
                     "ps_add 6, 6, 7\n\t"
                     "psq_st 0, 0(%[m]), 0, 5\n\t"
                     "psq_st 2, 8(%[m]), 0, 5\n\t"
                     "psq_st 4, 16(%[m]), 0, 5\n\t"
                     "psq_st 6, 24(%[m]), 0, 5\n\t"
                     "psq_l 0, 16(%[r]), 0, 2\n\t"
                     "psq_l 1, 48(%[r]), 0, 3\n\t"
                     "psq_l 2, 20(%[r]), 0, 2\n\t"
                     "psq_l 3, 52(%[r]), 0, 3\n\t"
                     "psq_l 4, 24(%[r]), 0, 2\n\t"
                     "psq_l 5, 56(%[r]), 0, 3\n\t"
                     "psq_l 6, 28(%[r]), 0, 2\n\t"
                     "psq_l 7, 60(%[r]), 0, 3\n\t"
                     "ps_add 0, 0, 1\n\t"
                     "ps_add 2, 2, 3\n\t"
                     "ps_add 4, 4, 5\n\t"
                     "ps_add 6, 6, 7\n\t"
                     "psq_st 0, 32(%[m]), 0, 5\n\t"
                     "psq_st 2, 40(%[m]), 0, 5\n\t"
                     "psq_st 4, 48(%[m]), 0, 5\n\t"
                     "psq_st 6, 56(%[m]), 0, 5"
                     :
                     : [r] "b"(raw), [m] "b"(&m[0][0]), [s16] "r"(RSP_GQR_S16), [u16] "r"(RSP_GQR_U16_FRAC),
                       [f32] "r"(0)
                     : "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "memory");
}

#if GFX_VTX_CHECK
static void mtx_chk_report(const char* what, const float got[4][4], const float want[4][4]) {
    sChkMtx++;
    if (memcmp(got, want, 64) != 0) {
        sChkBad++;
        if (sChkLogged < 16) {
            sChkLogged++;
            gc_log("gfx_rsp: vtx check: %s differs (first row %08X %08X %08X %08X, want %08X %08X %08X %08X)", what,
                   *(const uint32_t*)&got[0][0], *(const uint32_t*)&got[0][1], *(const uint32_t*)&got[0][2],
                   *(const uint32_t*)&got[0][3], *(const uint32_t*)&want[0][0], *(const uint32_t*)&want[0][1],
                   *(const uint32_t*)&want[0][2], *(const uint32_t*)&want[0][3]);
        }
    }
}

static void mtx_mul(float out[4][4], const float a[4][4], const float b[4][4]) {
    float want[4][4];

    mtx_mul_c(want, a, b);
    mtx_mul_ps(out, a, b);
    mtx_chk_report("matrix product", (const float(*)[4])out, (const float(*)[4])want);
}

static void mtx_from_raw(float m[4][4], const uint8_t* raw) {
    float want[4][4];

    mtx_from_raw_c(want, raw);
    mtx_from_raw_ps(m, raw);
    mtx_chk_report("matrix", (const float(*)[4])m, (const float(*)[4])want);
}
#else
#define mtx_mul mtx_mul_ps
#define mtx_from_raw mtx_from_raw_ps
#endif
#else
#define mtx_mul mtx_mul_c
#define mtx_from_raw mtx_from_raw_c
#endif

static void mtx_to_raw(uint8_t raw[64], const float m[4][4]) {
    int i;

    for (i = 0; i < 16; i++) {
        double d = (double)m[i >> 2][i & 3] * 65536.0;
        int32_t v;

        d = (d > 2147483647.0) ? 2147483647.0 : (d < -2147483648.0) ? -2147483648.0 : d;
        v = (int32_t)((d >= 0.0) ? d + 0.5 : d - 0.5);
        raw[2 * i] = (uint8_t)(v >> 24);
        raw[2 * i + 1] = (uint8_t)(v >> 16);
        raw[32 + 2 * i] = (uint8_t)(v >> 8);
        raw[32 + 2 * i + 1] = (uint8_t)v;
    }
}

/* A DMEM write into a matrix: whole matrices go straight in; partial writes (G_MOVEMEM with an offset,
 * G_MW_MATRIX) round-trip through the fixed point layout. */
static void mtx_write(float m[4][4], uint32_t ofs, const uint8_t* src, uint32_t len) {
    uint8_t raw[64] __attribute__((aligned(8)));

    if (ofs == 0 && len >= 64) {
        mtx_from_raw(m, src);
        return;
    }
    if (ofs >= 64) {
        return;
    }
    if (len > 64 - ofs) {
        len = 64 - ofs;
    }
    mtx_to_raw(raw, m);
    memcpy(raw + ofs, src, len);
    mtx_from_raw(m, raw);
}

static void rsp_mtx(uint32_t w0, uint32_t w1) {
    const uint8_t* src = rsp_ptr(w1, 64);
    uint32_t params = w0 & 0xFF; // F3DEX2 stores G_MTX_PUSH inverted: bit 0 clear = push
    float m[4][4];

    if (src == NULL) {
        LOG_ONCE(LOG_BAD_MTX, "gfx_rsp: G_MTX from bad address %08X (logged once)", (unsigned int)w1);
        return;
    }
    mtx_from_raw(m, src);
    if (params & G_MTX_PROJECTION) {
        if (params & G_MTX_LOAD) {
            memcpy(sRsp.p, m, sizeof(m));
        } else {
            mtx_mul(sRsp.p, m, sRsp.p);
        }
        sRsp.pVer++;
    } else {
        rsp_attrs_before_mv_change();
        if (!(params & G_MTX_PUSH)) {
            if (sRsp.mvDepth >= MTX_STACK_N64) {
                LOG_ONCE(LOG_MTX_N64_DEPTH, "gfx_rsp: matrix stack deeper than %d (overflows on N64; logged once)",
                         MTX_STACK_N64);
            }
            if (sRsp.mvDepth < MTX_STACK_MAX) {
                memcpy(sRsp.mvStack[sRsp.mvDepth], sRsp.mv, sizeof(sRsp.mv));
                sRsp.mvDepth++;
            } else {
                LOG_ONCE(LOG_MTX_OVERFLOW, "gfx_rsp: matrix stack overflow, push dropped (logged once)");
            }
        }
        if (params & G_MTX_LOAD) {
            memcpy(sRsp.mv, m, sizeof(m));
        } else {
            mtx_mul(sRsp.mv, m, sRsp.mv);
        }
    }
    sRsp.mvpValid = false;
    sRsp.lightsValid = false;
}

static void rsp_popmtx(uint32_t w1) {
    uint32_t n = w1 / 64; // bytes popped off the DRAM stack, which stops at its base

    if (n == 0 || sRsp.mvDepth == 0) {
        return;
    }
    if (n > (uint32_t)sRsp.mvDepth) {
        n = (uint32_t)sRsp.mvDepth;
    }
    rsp_attrs_before_mv_change();
    sRsp.mvDepth -= (int)n;
    memcpy(sRsp.mv, sRsp.mvStack[sRsp.mvDepth], sizeof(sRsp.mv));
    sRsp.mvpValid = false;
    sRsp.lightsValid = false;
}

/* Before a vertex load: recompute MV * P if a matrix changed (as the microcode does), then hand GX the
 * projection these vertices are transformed with if it differs from the last one. */
static void rsp_prepare_mvp(void) {
    const float(*proj)[4];
    uint32_t key;

    if (!sRsp.mvpValid) {
        mtx_mul(sRsp.mvp, sRsp.mv, sRsp.p);
        sRsp.mvpValid = true;
        sRsp.mvpForced = false;
        if (sRsp.pUsedVer != sRsp.pVer) {
            memcpy(sRsp.pUsed, sRsp.p, sizeof(sRsp.p));
            sRsp.pUsedVer = sRsp.pVer;
        }
    }
    if (sRsp.mvpForced) {
        // A forced combined matrix: GX gets it as the projection (modelview identity)
        proj = (const float(*)[4])sRsp.mvp;
        key = (sRsp.forcedVer << 1) | 1;
    } else {
        proj = (const float(*)[4])sRsp.pUsed;
        key = sRsp.pUsedVer << 1;
    }
    if (!sRsp.gxProjSent || key != sRsp.gxProjKey) {
        sRsp.gxProjKey = key;
        if (!sRsp.gxProjSent || memcmp(proj, sRsp.gxProj, sizeof(sRsp.gxProj)) != 0) {
            memcpy(sRsp.gxProj, proj, sizeof(sRsp.gxProj));
            sRsp.gxProjSent = true;
            gfx_gx_set_projection((const float(*)[4])sRsp.gxProj);
        }
    }
}

/* ============================================================================================== */
/* Lights                                                                                         */
/* ============================================================================================== */

/* Model space direction of a light or lookat slot: the s8 direction times the 3x3 modelview (transposed
 * relative to the vertex transform), normalized and stored as the microcode does: floor(128 * d / |d|),
 * saturated to s8. (The microcode normalizes in fixed point, with |M d| truncated to an integer before
 * the reciprocal square root; under a small-scale modelview that leaves directions unnormalized. This
 * normalizes in float, as emulators do.) */
static void rsp_light_dir(int slot) {
    const uint8_t* l = &sRsp.lightRaw[slot * LIGHT_SIZE];
    float dx = (float)(int8_t)l[8], dy = (float)(int8_t)l[9], dz = (float)(int8_t)l[10];
    const float(*m)[4] = (const float(*)[4])sRsp.mv;
    float x = m[0][0] * dx + m[0][1] * dy + m[0][2] * dz;
    float y = m[1][0] * dx + m[1][1] * dy + m[1][2] * dz;
    float z = m[2][0] * dx + m[2][1] * dy + m[2][2] * dz;
    float len2 = x * x + y * y + z * z;
    int8_t* out = sRsp.lightDir[slot];

    if (len2 > 0.0f) {
        float s = 128.0f * rsqrtf_fast(len2);

        out[0] = to_s8_floor(x * s);
        out[1] = to_s8_floor(y * s);
        out[2] = to_s8_floor(z * s);
    } else {
        out[0] = out[1] = out[2] = 0;
    }
}

static void rsp_prepare_lights(void) {
    int n = sRsp.numLights18 / LIGHT_SIZE;
    int i;

    if (n > MAX_LIGHTS) {
        LOG_ONCE(LOG_NUMLIGHTS, "gfx_rsp: %d lights, using %d (logged once)", n, MAX_LIGHTS);
        n = MAX_LIGHTS;
    }
    if (!sRsp.lightsValid) {
        // Lookat X, lookat Y and every light (point lights too: their bytes are transformed but unused)
        for (i = 0; i < LIGHT_MAIN + n; i++) {
            rsp_light_dir(i);
        }
        sRsp.lightsValid = true;
    }
    if (!sRsp.lightsParsed || sRsp.numLights != n) {
        sRsp.numLights = n;
        sRsp.hasPointLight = false;
        for (i = 0; i <= n; i++) {
            const uint8_t* l = &sRsp.lightRaw[(LIGHT_MAIN + i) * LIGHT_SIZE];
            RspLight* lt = &sRsp.lights[i];
            int c;

            for (c = 0; c < 3; c++) {
                lt->col[0][c] = l[4 + c]; // colc
                lt->col[1][c] = l[c];     // col
            }
            lt->kc = l[3];
            lt->kl = l[7];
            lt->kq = l[14];
            lt->pos[0] = (float)(int16_t)rd16(l + 8);
            lt->pos[1] = (float)(int16_t)rd16(l + 10);
            lt->pos[2] = (float)(int16_t)rd16(l + 12);
            if (i < n && lt->kc != 0) {
                sRsp.hasPointLight = true;
            }
        }
        sRsp.lightsParsed = true;
    }
}

/* Directional light intensity: 2 * (n . d) for s8 normal and direction, clamped to [0, 0x7FFF] (0x7FFF = 1) */
static inline int32_t rsp_dir_intensity(int nx, int ny, int nz, const int8_t* d) {
    int32_t i = 2 * (nx * d[0] + ny * d[1] + nz * d[2]);

    return (i <= 0) ? 0 : (i > 0x7FFF) ? 0x7FFF : i;
}

/* Point light intensity (F3DZEX2 PosLight, as in MM's microcode), in [0, 0x7FFF]:
 *   L  = light position - floor(vertex * MV)       (world space, saturated to s16 like the microcode's vsub)
 *   K  = Lx^2 + Ly^2 + 2 Lz^2                      (the microcode counts z twice; saturates near 2^31)
 *   Lm = floor(MV3x3 * L)                          (model space, not normalized: carries MV's scale)
 *   V  = clamp(n/128 . clamp(4 Lm / sqrt(K), -1, 1), 0, 1)
 *   attenuation = 65536 / (4096 kc + 32768 + 2 kl len + kq * 32 * (min(16 len, 32767)^2 >> 16)),
 *   len = (int)sqrt(K); intensity = V * attenuation, rounded as vmulf does
 * fn is the s8 normal as floats. */
static int32_t rsp_point_intensity(const RspLight* lt, const float* wp, const float* fn) {
    const float(*m)[4] = (const float(*)[4])sRsp.mv;
    float lx = clampf(lt->pos[0] - wp[0], -32768.0f, 32767.0f);
    float ly = clampf(lt->pos[1] - wp[1], -32768.0f, 32767.0f);
    float lz = clampf(lt->pos[2] - wp[2], -32768.0f, 32767.0f);
    float k = lx * lx + ly * ly + 2.0f * lz * lz;
    float mx, my, mz, inv, s, v, att;
    int32_t len, q, acc, intensity;

    if (k <= 0.0f) {
        return 0;
    }
    if (k > 2147483647.0f) {
        k = 2147483647.0f;
    }
    mx = floor_clamp16(m[0][0] * lx + m[0][1] * ly + m[0][2] * lz);
    my = floor_clamp16(m[1][0] * lx + m[1][1] * ly + m[1][2] * lz);
    mz = floor_clamp16(m[2][0] * lx + m[2][1] * ly + m[2][2] * lz);
    inv = rsqrtf_fast(k);
    s = 4.0f * inv;
    v = (fn[0] * clampf(mx * s, -1.0f, 1.0f) + fn[1] * clampf(my * s, -1.0f, 1.0f) +
         fn[2] * clampf(mz * s, -1.0f, 1.0f)) *
        (1.0f / 128.0f);
    if (v <= 0.0f) {
        return 0;
    }
    len = (int32_t)(k * inv + 0.01f); // sqrt(K), truncated; the bias keeps perfect squares exact
    if (len > 32767) {
        len = 32767;
    }
    q = (len * 16 > 32767) ? 32767 : len * 16;
    acc = (int32_t)lt->kc * 4096 + 32768 + 2 * (int32_t)lt->kl * len + ((q * q) >> 16) * (int32_t)lt->kq * 32;
    att = 65536.0f / (float)acc;
    if (att > 32767.0f / 32768.0f) {
        att = 32767.0f / 32768.0f;
    }
    intensity = (int32_t)(clampf(v, 0.0f, 32767.0f / 32768.0f) * att * 32768.0f + 0.5f);
    return (intensity > 0x7FFF) ? 0x7FFF : intensity;
}

/* Lit color of one vertex. Colors accumulate as the microcode's vmulf/vmacf do: color += lc * I / 32768,
 * with the truncation that makes a component above 128 lose 1 per accumulation pass. Directional
 * lighting is one pass; positional lighting stores the color after each light. */
static inline void rsp_light_directional(GfxVtx* d, int nx, int ny, int nz, int odd) {
    const int n = sRsp.numLights;
    const RspLight* amb = &sRsp.lights[n];
    int32_t r = amb->col[odd][0], g = amb->col[odd][1], b = amb->col[odd][2];
    int32_t sr = 0, sg = 0, sb = 0;
    int l;

    for (l = 0; l < n; l++) {
        int32_t in = rsp_dir_intensity(nx, ny, nz, sRsp.lightDir[LIGHT_MAIN + l]);

        if (in != 0) {
            const uint8_t* c = sRsp.lights[l].col[odd];

            sr += c[0] * in;
            sg += c[1] * in;
            sb += c[2] * in;
        }
    }
    r += (sr + 128 - r) >> 15;
    g += (sg + 128 - g) >> 15;
    b += (sb + 128 - b) >> 15;
    d->r = (uint8_t)((r > 255) ? 255 : r);
    d->g = (uint8_t)((g > 255) ? 255 : g);
    d->b = (uint8_t)((b > 255) ? 255 : b);
}

static void rsp_light_vertex(GfxVtx* d, int nx, int ny, int nz, int odd, bool positional, const float* wp) {
    const int n = sRsp.numLights;
    const RspLight* amb = &sRsp.lights[n];
    int32_t r = amb->col[odd][0], g = amb->col[odd][1], b = amb->col[odd][2];
    int l;

    if (!positional) {
        rsp_light_directional(d, nx, ny, nz, odd);
        return;
    } else {
        // The normal as floats once for all point lights (exact: s8 values)
        const float fn[3] = { (float)nx, (float)ny, (float)nz };

        for (l = n - 1; l >= 0; l--) {
            const RspLight* lt = &sRsp.lights[l];
            const uint8_t* c = lt->col[odd];
            int32_t in = (lt->kc != 0) ? rsp_point_intensity(lt, wp, fn)
                                       : rsp_dir_intensity(nx, ny, nz, sRsp.lightDir[LIGHT_MAIN + l]);

            r += (c[0] * in + 128 - r) >> 15;
            g += (c[1] * in + 128 - g) >> 15;
            b += (c[2] * in + 128 - b) >> 15;
            r = (r > 255) ? 255 : r;
            g = (g > 255) ? 255 : g;
            b = (b > 255) ? 255 : b;
        }
    }
    d->r = (uint8_t)((r > 255) ? 255 : r);
    d->g = (uint8_t)((g > 255) ? 255 : g);
    d->b = (uint8_t)((b > 255) ? 255 : b);
}

/* Texgen coordinate (before the G_TEXTURE scale) from the normal and a lookat direction:
 *   spherical: 0x4000 * (1 + D)
 *   linear:    0x8000 * (0.5 + 0.26885 D + 0.23115 D^3), the microcode's cubic approximation of acos(-D)/pi
 * where D = n . lookat as an s0.15 fraction. */
static int32_t rsp_texgen(int nx, int ny, int nz, const int8_t* d, bool linear) {
    int32_t dot = sat16(2 * (nx * d[0] + ny * d[1] + nz * d[2]));
    int32_t x, x2, c;

    if (!linear) {
        return 0x4000 + (dot >> 1);
    }
    x = dot >> 1;
    x2 = sat16((x * x * 2 + 0x8000) >> 16);
    c = sat16((int32_t)(((int64_t)x * (0x7FFF + 0x6CB3) * 2 + 0x8000) >> 16));
    return sat16((int32_t)((((int64_t)0x4000 << 16) + (int64_t)x * 0x44D3 * 2 + (int64_t)x2 * c * 2) >> 16));
}

/* ============================================================================================== */
/* Vertices                                                                                       */
/* ============================================================================================== */

/* Clip codes as the microcode's VCH/VCL compare x, y, z against +-w; correct for w < 0 too */
static inline uint8_t rsp_clip_codes(float x, float y, float z, float w) {
    uint8_t c = 0;

    if (x <= -w) {
        c |= GFX_CLIP_NEG_X;
    }
    if (x >= w) {
        c |= GFX_CLIP_POS_X;
    }
    if (y <= -w) {
        c |= GFX_CLIP_NEG_Y;
    }
    if (y >= w) {
        c |= GFX_CLIP_POS_Y;
    }
    if (w <= 0.0f) {
        c |= GFX_CLIP_NEG_W;
    }
    if (z >= w) {
        c |= GFX_CLIP_FAR;
    }
    return c;
}

/* What a vertex's attributes (color, alpha or fog, texture coordinates) depend on besides its own data, the lights
 * (sRsp.lights, lightDir) and, with point lights, the modelview. Set at each G_VTX; nothing in it changes while
 * vertices wait for their attributes (rsp_attrs_flush runs first). */
#define ATTR_LIT 0x01
#define ATTR_FOG 0x02
#define ATTR_TEXGEN 0x04
#define ATTR_TEXGEN_LINEAR 0x08
#define ATTR_POSITIONAL 0x10
#define ATTR_WORLD 0x20 /* positional with a point light: the vertex's world position */

typedef struct {
    uint32_t flags;
    int32_t scaleS, scaleT;
    int16_t fogMultiplier, fogOffset;
    float fm, fo; /* the fog multiplier and offset as floats */
} RspAttrState;

static RspAttrState sAttr;
static uint32_t sPending;                 /* bit i: sVtx[i] still needs its attributes */
static uint32_t sVtxOdd;                  /* bit i: sVtx[i] was the second of a pair in its G_VTX */
static const uint8_t* sVtxIn[VTX_COUNT];  /* the N64 Vtx each sVtx came from (RAM does not change during a task) */

#if GFX_VTX_CHECK
/* Check build: what the original eager pipeline computed for each vertex */
static GfxVtx sRefVtx[VTX_COUNT];

static void chk_report(const char* what, int idx, const GfxVtx* got, const GfxVtx* want) {
    sChkBad++;
    if (sChkLogged < 16) {
        sChkLogged++;
        gc_log("gfx_rsp: vtx check: %s differs at %d: got %08X %08X %08X %08X clip %02X rgba %02X%02X%02X%02X st %08X "
               "%08X, want %08X %08X %08X %08X clip %02X rgba %02X%02X%02X%02X st %08X %08X", what, idx,
               *(const uint32_t*)&got->x, *(const uint32_t*)&got->y, *(const uint32_t*)&got->z,
               *(const uint32_t*)&got->w, got->clip, got->r, got->g, got->b, got->a, *(const uint32_t*)&got->s,
               *(const uint32_t*)&got->t, *(const uint32_t*)&want->x, *(const uint32_t*)&want->y,
               *(const uint32_t*)&want->z, *(const uint32_t*)&want->w, want->clip, want->r, want->g, want->b,
               want->a, *(const uint32_t*)&want->s, *(const uint32_t*)&want->t);
    }
}
#endif

/* Lighting with G_LIGHTING_POSITIONAL or texgen, which most vertices do not use (kept out of rsp_vertex_attrs) */
static __attribute__((noinline)) void rsp_vertex_light_full(GfxVtx* d, const uint8_t* in, int idx, uint32_t flags,
                                                            int32_t* s, int32_t* t) {
    int nx = (int8_t)in[12], ny = (int8_t)in[13], nz = (int8_t)in[14];
    float wp[3] = { 0.0f, 0.0f, 0.0f }; // vertex in world space (point lights)

    if (flags & ATTR_WORLD) {
        const float(*mv)[4] = (const float(*)[4])sRsp.mv;
        float x = (float)(int16_t)rd16(in);
        float y = (float)(int16_t)rd16(in + 2);
        float z = (float)(int16_t)rd16(in + 4);

        wp[0] = floor_clamp16(x * mv[0][0] + y * mv[1][0] + z * mv[2][0] + mv[3][0]);
        wp[1] = floor_clamp16(x * mv[0][1] + y * mv[1][1] + z * mv[2][1] + mv[3][1]);
        wp[2] = floor_clamp16(x * mv[0][2] + y * mv[1][2] + z * mv[2][2] + mv[3][2]);
    }
    rsp_light_vertex(d, nx, ny, nz, (int)((sVtxOdd >> idx) & 1), (flags & ATTR_POSITIONAL) != 0, wp);
    if (flags & ATTR_TEXGEN) {
        *s = rsp_texgen(nx, ny, nz, sRsp.lightDir[0], (flags & ATTR_TEXGEN_LINEAR) != 0);
        *t = rsp_texgen(nx, ny, nz, sRsp.lightDir[1], (flags & ATTR_TEXGEN_LINEAR) != 0);
    }
}

/* Color, alpha and texture coordinates of sVtx[idx] (the attribute half of the original G_VTX loop) */
static inline void rsp_vertex_attrs(int idx) {
    GfxVtx* d = &sVtx[idx];
    const uint8_t* in = sVtxIn[idx];
    const uint32_t flags = sAttr.flags;
    int32_t s = (int16_t)rd16(in + 8);
    int32_t t = (int16_t)rd16(in + 10);

    if (flags & ATTR_LIT) {
        if (flags & (ATTR_POSITIONAL | ATTR_TEXGEN)) {
            rsp_vertex_light_full(d, in, idx, flags, &s, &t);
        } else {
            rsp_light_directional(d, (int8_t)in[12], (int8_t)in[13], (int8_t)in[14], (int)((sVtxOdd >> idx) & 1));
        }
    } else {
        d->r = in[12];
        d->g = in[13];
        d->b = in[14];
    }

    if (flags & ATTR_FOG) {
        // alpha = clamp(floor(z / w * multiplier) + offset, 0, 255); 1/w saturates for w near or below 0
        const float cz = d->z, cw = d->w;
        const float fm = sAttr.fm, fo = sAttr.fo;
        float iw = (cw > (1.0f / 32768.0f)) ? 1.0f / cw : 32768.0f;
        float f = cz * iw * fm + fo;

        d->a = (f > 0.0f) ? ((f < 255.0f) ? (uint8_t)(int32_t)f : 255) : 0;
    } else {
        d->a = in[15];
    }

    // s10.5 coordinate * 0.16 scale, truncated like the microcode's vmudm, then in texels
#if RSP_PAIRED
    {
        // Both results fit an s16 (a s16 times a u16, shifted by 16): psq_l reads them back as floats, divided by
        // 32 through the GQR's scale, which is exact, as (float)v * (1 / 32) is
        int16_t st[2] __attribute__((aligned(8)));

        st[0] = (int16_t)((s * sAttr.scaleS) >> 16);
        st[1] = (int16_t)((t * sAttr.scaleT) >> 16);
        // (GQR4 and GQR5 are set by rsp_attrs_compute, the only caller)
        __asm__ volatile("psq_l 0, 0(%[st]), 0, 4\n\t"
                         "psq_st 0, %[ofs](%[d]), 0, 5"
                         :
                         : [st] "b"(st), [d] "b"(d), [ofs] "i"(offsetof(GfxVtx, s))
                         : "fr0", "memory");
    }
#else
    d->s = (float)((s * sAttr.scaleS) >> 16) * (1.0f / 32.0f);
    d->t = (float)((t * sAttr.scaleT) >> 16) * (1.0f / 32.0f);
#endif
#if GFX_VTX_CHECK
    {
        const GfxVtx* r = &sRefVtx[idx];

        sChkAttr++;
        if (d->r != r->r || d->g != r->g || d->b != r->b || d->a != r->a || memcmp(&d->s, &r->s, 8) != 0) {
            chk_report("attributes", idx, d, r);
        }
    }
#endif
}

/* Attributes of the vertices in `mask` that still wait for them */
static void rsp_attrs_compute(uint32_t mask) {
    mask &= sPending;
    sPending &= ~mask;
    GFX_PROF_ENTER(GFX_PROF_ATTR);
#if RSP_PAIRED
    // GQRs of rsp_vertex_attrs (texture coordinates): s16 / 32 and float. Nothing in between changes them.
    __asm__ volatile("mtspr 916, %[gqr]\n\t"
                     "mtspr 917, %[f32]"
                     :
                     : [gqr] "r"(RSP_GQR_S16_ST), [f32] "r"(0));
#endif
    while (mask != 0) {
        int idx = __builtin_ctz(mask);

        mask &= mask - 1;
        rsp_vertex_attrs(idx);
    }
    GFX_PROF_LEAVE();
}

/* Every waiting vertex gets its attributes now: their inputs are about to change */
static void rsp_attrs_flush(void) {
    if (sPending != 0) {
        rsp_attrs_compute(sPending);
    }
}

/* The modelview is about to change: waiting vertices lit by point lights need the current one */
static void rsp_attrs_before_mv_change(void) {
    if (sPending != 0 && (sAttr.flags & ATTR_WORLD)) {
        rsp_attrs_compute(sPending);
    }
}

/* rsp_prepare_lights would change the light state the waiting vertices use */
static bool rsp_lights_stale(void) {
    int n = sRsp.numLights18 / LIGHT_SIZE;

    if (n > MAX_LIGHTS) {
        n = MAX_LIGHTS;
    }
    return !sRsp.lightsValid || !sRsp.lightsParsed || sRsp.numLights != n;
}

/* Positions and clip codes of n (>= 1) vertices with the current MVP */
static void rsp_positions_c(GfxVtx* d, const uint8_t* in, int n) {
    const float(*m)[4] = (const float(*)[4])sRsp.mvp;
    int i;

    for (i = 0; i < n; i++, in += 16, d++) {
        float x = (float)(int16_t)rd16(in);
        float y = (float)(int16_t)rd16(in + 2);
        float z = (float)(int16_t)rd16(in + 4);

        d->x = x * m[0][0] + y * m[1][0] + z * m[2][0] + m[3][0];
        d->y = x * m[0][1] + y * m[1][1] + z * m[2][1] + m[3][1];
        d->z = x * m[0][2] + y * m[1][2] + z * m[2][2] + m[3][2];
        d->w = x * m[0][3] + y * m[1][3] + z * m[2][3] + m[3][3];
        d->clip = rsp_clip_codes(d->x, d->y, d->z, d->w);
    }
}

#if RSP_PAIRED
static void rsp_positions(GfxVtx* d, const uint8_t* in, int n) {
    uint32_t t0, t1, t2;

    if ((uintptr_t)in & 3) {
        // Vertices are 8-byte aligned (the RSP's DMA needs that); anything else takes the C path
        rsp_positions_c(d, in, n);
        return;
    }

    /* Per vertex: (x, y) and (z, 1) from the s16 coordinates; (cx, cy) and (cz, cw) as y * row 1, then + x * row 0
     * (fused), + z * row 2 (fused), + row 3, which is how GCC evaluates x * m0 + y * m1 + z * m2 + m3. Clip codes from
     * six compares (w <= 0 as w <= -w) and one mfcr: LT|EQ and GT|EQ of each field are combined by shifts. */
    __asm__ volatile("mtspr 914, %[s16]\n\t"
                     "mtspr 917, %[f32]\n\t"
                     "psq_l 0, 0(%[m]), 0, 5\n\t"
                     "psq_l 1, 8(%[m]), 0, 5\n\t"
                     "psq_l 2, 16(%[m]), 0, 5\n\t"
                     "psq_l 3, 24(%[m]), 0, 5\n\t"
                     "psq_l 4, 32(%[m]), 0, 5\n\t"
                     "psq_l 5, 40(%[m]), 0, 5\n\t"
                     "psq_l 6, 48(%[m]), 0, 5\n\t"
                     "psq_l 7, 56(%[m]), 0, 5\n\t"
                     "mtctr %[n]\n"
                     "1:\n\t"
                     "psq_l 8, 0(%[in]), 0, 2\n\t"
                     "psq_l 9, 4(%[in]), 1, 2\n\t"
                     "ps_muls1 10, 2, 8\n\t"
                     "ps_muls1 11, 3, 8\n\t"
                     "ps_madds0 10, 0, 8, 10\n\t"
                     "ps_madds0 11, 1, 8, 11\n\t"
                     "ps_madds0 10, 4, 9, 10\n\t"
                     "ps_madds0 11, 5, 9, 11\n\t"
                     "ps_add 10, 10, 6\n\t"
                     "ps_add 11, 11, 7\n\t"
                     "psq_st 10, 0(%[d]), 0, 5\n\t"
                     "psq_st 11, 8(%[d]), 0, 5\n\t"
                     "ps_merge11 12, 11, 11\n\t"
                     "ps_neg 13, 12\n\t"
                     "fcmpu 0, 10, 13\n\t"     /* x : -w  NEG_X */
                     "fcmpu 1, 10, 12\n\t"     /* x : w   POS_X */
                     "fcmpu 2, 11, 12\n\t"     /* z : w   FAR */
                     "ps_cmpu1 5, 10, 13\n\t"  /* y : -w  NEG_Y */
                     "ps_cmpu1 6, 10, 12\n\t"  /* y : w   POS_Y */
                     "fcmpu 7, 12, 13\n\t"     /* w : -w  NEG_W */
                     "mfcr %[t0]\n\t"
                     "slwi %[t1], %[t0], 2\n\t"
                     "slwi %[t2], %[t0], 1\n\t"
                     "or %[t1], %[t1], %[t0]\n\t" /* LT | EQ at each field's LT bit */
                     "or %[t2], %[t2], %[t0]\n\t" /* GT | EQ at each field's GT bit */
                     "rlwinm %[t0], %[t1], 1, 31, 31\n\t"
                     "rlwimi %[t0], %[t2], 7, 30, 30\n\t"
                     "rlwimi %[t0], %[t1], 23, 29, 29\n\t"
                     "rlwimi %[t0], %[t2], 29, 28, 28\n\t"
                     "rlwimi %[t0], %[t1], 1, 27, 27\n\t"
                     "rlwimi %[t0], %[t2], 15, 26, 26\n\t"
                     "stb %[t0], %[clipofs](%[d])\n\t"
                     "addi %[in], %[in], 16\n\t"
                     "addi %[d], %[d], %[stride]\n\t"
                     "bdnz 1b"
                     : [in] "+b"(in), [d] "+b"(d), [t0] "=&r"(t0), [t1] "=&r"(t1), [t2] "=&r"(t2)
                     : [m] "b"(&sRsp.mvp[0][0]), [n] "r"(n), [s16] "r"(RSP_GQR_S16), [f32] "r"(0),
                       [clipofs] "i"(offsetof(GfxVtx, clip)), [stride] "i"(sizeof(GfxVtx))
                     : "fr0", "fr1", "fr2", "fr3", "fr4", "fr5", "fr6", "fr7", "fr8", "fr9", "fr10", "fr11", "fr12",
                       "fr13", "cr0", "cr1", "cr2", "cr5", "cr6", "cr7", "ctr", "memory");
}
#else
#define rsp_positions rsp_positions_c
#endif

#if GFX_VTX_CHECK
/* The original G_VTX loop, into sRefVtx (check build) */
static void rsp_vertices_ref(int v0, int n, const uint8_t* in) {
    const uint32_t gm = gGfxRsp.geometryMode;
    const bool lit = (gm & G_LIGHTING) != 0;
    const bool fog = (gm & G_FOG) != 0;
    const bool texgen = lit && (gm & G_TEXTURE_GEN);
    const bool texgenLinear = (gm & G_TEXTURE_GEN_LINEAR) != 0;
    const bool positional = lit && (gm & G_LIGHTING_POSITIONAL);
    const int32_t scaleS = sRsp.texScale[0], scaleT = sRsp.texScale[1];
    const float fm = (float)gGfxRsp.fogMultiplier, fo = (float)gGfxRsp.fogOffset;
    const bool needWorld = positional && sRsp.hasPointLight;
    float m00, m01, m02, m03, m10, m11, m12, m13, m20, m21, m22, m23, m30, m31, m32, m33;
    float wp[3] = { 0.0f, 0.0f, 0.0f };
    GfxVtx* d;
    int i;

    m00 = sRsp.mvp[0][0], m01 = sRsp.mvp[0][1], m02 = sRsp.mvp[0][2], m03 = sRsp.mvp[0][3];
    m10 = sRsp.mvp[1][0], m11 = sRsp.mvp[1][1], m12 = sRsp.mvp[1][2], m13 = sRsp.mvp[1][3];
    m20 = sRsp.mvp[2][0], m21 = sRsp.mvp[2][1], m22 = sRsp.mvp[2][2], m23 = sRsp.mvp[2][3];
    m30 = sRsp.mvp[3][0], m31 = sRsp.mvp[3][1], m32 = sRsp.mvp[3][2], m33 = sRsp.mvp[3][3];

    d = &sRefVtx[v0];
    for (i = 0; i < n; i++, in += 16, d++) {
        float x = (float)(int16_t)rd16(in);
        float y = (float)(int16_t)rd16(in + 2);
        float z = (float)(int16_t)rd16(in + 4);
        float cx = x * m00 + y * m10 + z * m20 + m30;
        float cy = x * m01 + y * m11 + z * m21 + m31;
        float cz = x * m02 + y * m12 + z * m22 + m32;
        float cw = x * m03 + y * m13 + z * m23 + m33;
        int32_t s = (int16_t)rd16(in + 8);
        int32_t t = (int16_t)rd16(in + 10);

        d->x = cx;
        d->y = cy;
        d->z = cz;
        d->w = cw;
        d->clip = rsp_clip_codes(cx, cy, cz, cw);

        if (lit) {
            int nx = (int8_t)in[12], ny = (int8_t)in[13], nz = (int8_t)in[14];

            if (needWorld) {
                const float(*mv)[4] = (const float(*)[4])sRsp.mv;

                wp[0] = clampf(floorf_fast(x * mv[0][0] + y * mv[1][0] + z * mv[2][0] + mv[3][0]), -32768.0f, 32767.0f);
                wp[1] = clampf(floorf_fast(x * mv[0][1] + y * mv[1][1] + z * mv[2][1] + mv[3][1]), -32768.0f, 32767.0f);
                wp[2] = clampf(floorf_fast(x * mv[0][2] + y * mv[1][2] + z * mv[2][2] + mv[3][2]), -32768.0f, 32767.0f);
            }
            rsp_light_vertex(d, nx, ny, nz, i & 1, positional, wp);
            if (texgen) {
                s = rsp_texgen(nx, ny, nz, sRsp.lightDir[0], texgenLinear);
                t = rsp_texgen(nx, ny, nz, sRsp.lightDir[1], texgenLinear);
            }
        } else {
            d->r = in[12];
            d->g = in[13];
            d->b = in[14];
        }

        if (fog) {
            // alpha = clamp(floor(z / w * multiplier) + offset, 0, 255); 1/w saturates for w near or below 0
            float iw = (cw > (1.0f / 32768.0f)) ? 1.0f / cw : 32768.0f;
            float f = cz * iw * fm + fo;

            d->a = (f > 0.0f) ? ((f < 255.0f) ? (uint8_t)(int32_t)f : 255) : 0;
        } else {
            d->a = in[15];
        }

        // s10.5 coordinate * 0.16 scale, truncated like the microcode's vmudm, then in texels
        d->s = (float)((s * scaleS) >> 16) * (1.0f / 32.0f);
        d->t = (float)((t * scaleT) >> 16) * (1.0f / 32.0f);
    }
}
#endif

/* G_VTX: positions and clip codes now; attributes when a triangle that is drawn uses the vertex */
static void rsp_vertices(uint32_t w0, uint32_t w1) {
    int n = (int)((w0 >> 12) & 0xFF);
    int v0 = (int)((w0 & 0xFF) >> 1) - n;
    const uint8_t* in;
    const uint32_t gm = gGfxRsp.geometryMode;
    const bool lit = (gm & G_LIGHTING) != 0;
    RspAttrState attr;
    uint32_t mask;
    int i;

    if (n == 0) {
        return;
    }
    if (v0 < 0 || v0 + n > VTX_COUNT) {
        LOG_ONCE(LOG_VTX_RANGE, "gfx_rsp: G_VTX of %d at %d is outside the vertex buffer (logged once)", n, v0);
        if (v0 < 0 || v0 >= VTX_COUNT) {
            return;
        }
        n = VTX_COUNT - v0;
    }
    in = rsp_ptr(w1, (uint32_t)n * 16);
    if (in == NULL) {
        LOG_ONCE(LOG_BAD_VTX, "gfx_rsp: G_VTX from bad address %08X (logged once)", (unsigned int)w1);
        return;
    }
    mask = (n == 32) ? 0xFFFFFFFFu : (((1u << n) - 1) << v0);

    // Vertices overwritten here never need their attributes; the others get them before the lights change
    sPending &= ~mask;
    attr.flags = (lit ? ATTR_LIT : 0) | ((gm & G_FOG) ? ATTR_FOG : 0) |
                 ((lit && (gm & G_TEXTURE_GEN)) ? ATTR_TEXGEN : 0) | ((gm & G_TEXTURE_GEN_LINEAR) ? ATTR_TEXGEN_LINEAR : 0) |
                 ((lit && (gm & G_LIGHTING_POSITIONAL)) ? ATTR_POSITIONAL : 0);
    attr.scaleS = sRsp.texScale[0];
    attr.scaleT = sRsp.texScale[1];
    attr.fogMultiplier = gGfxRsp.fogMultiplier;
    attr.fogOffset = gGfxRsp.fogOffset;
    attr.fm = (float)attr.fogMultiplier;
    attr.fo = (float)attr.fogOffset;
    if (sPending != 0 && ((lit && rsp_lights_stale()) || attr.flags != (sAttr.flags & ~ATTR_WORLD) ||
                          attr.scaleS != sAttr.scaleS || attr.scaleT != sAttr.scaleT ||
                          attr.fogMultiplier != sAttr.fogMultiplier || attr.fogOffset != sAttr.fogOffset)) {
        rsp_attrs_flush();
    }

    GFX_PROF_ENTER(GFX_PROF_VSETUP);
    rsp_prepare_mvp();
    if (lit) {
        rsp_prepare_lights();
        if ((attr.flags & ATTR_POSITIONAL) && sRsp.hasPointLight) {
            attr.flags |= ATTR_WORLD;
        }
    }
    GFX_PROF_LEAVE();
    sAttr = attr;
    sRsp.vpFlip = (gGfxRsp.viewportScale[0] < 0) != (gGfxRsp.viewportScale[1] < 0);
#if GFX_PROF && !defined(GFX_HOST_TEST)
    if (lit) {
        int l, points = 0;

        for (l = 0; l < sRsp.numLights; l++) {
            points += ((attr.flags & ATTR_POSITIONAL) && sRsp.lights[l].kc != 0);
        }
        PROF_COUNT(sProfVtxLit, n);
        PROF_COUNT(sProfVtxPos, (attr.flags & ATTR_WORLD) ? n : 0);
        PROF_COUNT(sProfDirEvals, n * (sRsp.numLights - points));
        PROF_COUNT(sProfPointEvals, n * points);
        PROF_COUNT(sProfVtxTexgen, (attr.flags & ATTR_TEXGEN) ? n : 0);
    }
    PROF_COUNT(sProfVtxFog, (attr.flags & ATTR_FOG) ? n : 0);
#endif

    rsp_positions(&sVtx[v0], in, n);
    for (i = 0; i < n; i++) {
        sVtxIn[v0 + i] = in + 16 * i;
    }
    // The second vertex of each pair of the load (light colors: col instead of colc)
    sVtxOdd = (sVtxOdd & ~mask) | ((((v0 & 1) ? 0x55555555u : 0xAAAAAAAAu)) & mask);
    sPending |= mask;
#if GFX_VTX_CHECK
    rsp_vertices_ref(v0, n, in);
    for (i = v0; i < v0 + n; i++) {
        sChkPos++;
        if (memcmp(&sVtx[i].x, &sRefVtx[i].x, 16) != 0 || sVtx[i].clip != sRefVtx[i].clip) {
            chk_report("position", i, &sVtx[i], &sRefVtx[i]);
        }
    }
#endif
    sStats.vtx += (uint32_t)n;
}

/* G_MODIFYVTX: writes into the processed vertex (ST is already scaled; screen values are converted back
 * to clip space through the current viewport). Clip codes are left alone, as on the RSP. */
static void rsp_modify_vertex(uint32_t w0, uint32_t w1) {
    int idx = (int)((w0 & 0xFFF) >> 1);
    uint32_t where = (w0 >> 16) & 0xFF;
    GfxVtx* v;

    if (idx >= VTX_COUNT) {
        LOG_ONCE(LOG_TRI_INDEX, "gfx_rsp: vertex index %d out of range (logged once)", idx);
        return;
    }
    // Its attributes as G_VTX left them, before the write
    rsp_attrs_compute(1u << idx);
    v = &sVtx[idx];
    switch (where) {
        case G_MWO_POINT_RGBA:
            v->r = (uint8_t)(w1 >> 24);
            v->g = (uint8_t)(w1 >> 16);
            v->b = (uint8_t)(w1 >> 8);
            v->a = (uint8_t)w1;
            break;
        case G_MWO_POINT_ST:
            v->s = (float)(int16_t)(w1 >> 16) * (1.0f / 32.0f);
            v->t = (float)(int16_t)w1 * (1.0f / 32.0f);
            break;
        case G_MWO_POINT_XYSCREEN:
        case G_MWO_POINT_ZSCREEN:
            if (v->w <= 0.0f) {
                LOG_ONCE(LOG_MODIFYVTX_W, "gfx_rsp: G_MODIFYVTX screen position of a vertex behind the eye (logged once)");
            } else if (where == G_MWO_POINT_XYSCREEN) {
                float sx = gGfxRsp.viewportScale[0], sy = gGfxRsp.viewportScale[1];

                // Screen x = x/w * vscale + vtrans and y = -y/w * vscale + vtrans, in quarter pixels
                if (sx != 0.0f && sy != 0.0f) {
                    v->x = ((float)(int16_t)(w1 >> 16) - gGfxRsp.viewportTrans[0]) / sx * v->w;
                    v->y = (gGfxRsp.viewportTrans[1] - (float)(int16_t)w1) / sy * v->w;
                }
            } else {
                float sz = gGfxRsp.viewportScale[2];

                // Screen z (16.16) = z/w * vscale + vtrans
                if (sz != 0.0f) {
                    v->z = ((float)(int32_t)w1 * (1.0f / 65536.0f) - gGfxRsp.viewportTrans[2]) / sz * v->w;
                }
            }
            break;
        default:
            LOG_ONCE(LOG_MODIFYVTX, "gfx_rsp: G_MODIFYVTX at offset %02X ignored (logged once)", (unsigned int)where);
            break;
    }
}

/* ============================================================================================== */
/* Triangles                                                                                      */
/* ============================================================================================== */

/* One triangle from the low 24 bits of a command word (vertex index * 2 per byte) */
static void rsp_triangle(uint32_t word) {
    uint32_t i0 = ((word >> 16) & 0xFF) >> 1;
    uint32_t i1 = ((word >> 8) & 0xFF) >> 1;
    uint32_t i2 = (word & 0xFF) >> 1;
    const GfxVtx *a, *b, *c;
    uint32_t cull;
    float det;

    sStats.tris++;
    if (i0 >= VTX_COUNT || i1 >= VTX_COUNT || i2 >= VTX_COUNT) {
        LOG_ONCE(LOG_TRI_INDEX, "gfx_rsp: triangle vertex index out of range (%08X; logged once)", (unsigned int)word);
        return;
    }
    a = &sVtx[i0];
    b = &sVtx[i1];
    c = &sVtx[i2];

    if (a->clip & b->clip & c->clip) {
        sStats.clipped++;
        return;
    }

    // Orientation from the homogeneous determinant det[x y w]: the sign of the screen space area for any
    // w signs, without dividing. Positive = counterclockwise with y up = front face.
    det = a->x * (b->y * c->w - c->y * b->w) - a->y * (b->x * c->w - c->x * b->w) +
          a->w * (b->x * c->y - c->x * b->y);
    if (sRsp.vpFlip) {
        det = -det;
    }
    cull = gGfxRsp.geometryMode & G_CULL_BOTH;
    if (det == 0.0f || (cull == G_CULL_BACK && det < 0.0f) || (cull == G_CULL_FRONT && det > 0.0f) ||
        cull == G_CULL_BOTH) {
        sStats.culled++;
        return;
    }

    sStats.drawn++;
    if (sPending & ((1u << i0) | (1u << i1) | (1u << i2))) {
        rsp_attrs_compute((1u << i0) | (1u << i1) | (1u << i2));
    }
    if (!(gGfxRsp.geometryMode & G_SHADING_SMOOTH)) {
        // Flat shading: RGB of the first vertex, alpha of each vertex
        GfxVtx fb = *b, fc = *c;

        fb.r = fc.r = a->r;
        fb.g = fc.g = a->g;
        fb.b = fc.b = a->b;
        gfx_gx_triangle(a, &fb, &fc);
    } else {
        gfx_gx_triangle(a, b, c);
    }
}

/* G_CULLDL: true if all vertices from first to last share a clip code */
static bool rsp_cull_dl(uint32_t w0, uint32_t w1) {
    uint32_t first = (w0 & 0xFFF) >> 1;
    uint32_t last = (w1 & 0xFFF) >> 1;
    uint8_t codes = GFX_CLIP_NEG_X | GFX_CLIP_POS_X | GFX_CLIP_NEG_Y | GFX_CLIP_POS_Y | GFX_CLIP_NEG_W | GFX_CLIP_FAR;
    uint32_t i;

    if (first >= VTX_COUNT) {
        LOG_ONCE(LOG_TRI_INDEX, "gfx_rsp: G_CULLDL vertex index out of range (logged once)");
        return false;
    }
    if (last >= VTX_COUNT) {
        last = VTX_COUNT - 1;
    }
    if (last < first) {
        last = first;
    }
    for (i = first; i <= last && codes != 0; i++) {
        codes &= sVtx[i].clip;
    }
    return codes != 0;
}

/* Integer part of the clip w as the microcode stores it (s16) */
static inline int32_t rsp_w_int(float w) {
    int32_t i;

    if (w >= 32767.0f) {
        return 32767;
    }
    if (!(w > -32768.0f)) {
        return -32768;
    }
    i = (int32_t)w;
    return ((float)i > w) ? i - 1 : i;
}

/* ============================================================================================== */
/* MOVEMEM / MOVEWORD                                                                             */
/* ============================================================================================== */

static void rsp_parse_viewport(void) {
    int i;
    bool changed = false;

    for (i = 0; i < 4; i++) {
        int16_t sc = (int16_t)rd16(sRsp.viewportRaw + 2 * i);
        int16_t tr = (int16_t)rd16(sRsp.viewportRaw + 8 + 2 * i);

        if (sc != gGfxRsp.viewportScale[i] || tr != gGfxRsp.viewportTrans[i]) {
            gGfxRsp.viewportScale[i] = sc;
            gGfxRsp.viewportTrans[i] = tr;
            changed = true;
        }
    }
    if (changed) {
        gGfxRdp.dirty |= GFX_DIRTY_VIEWPORT;
    }
}

/* Copy into a DMEM image, clipped to its size */
static void rsp_write_image(uint8_t* image, uint32_t size, uint32_t ofs, const uint8_t* src, uint32_t len) {
    if (ofs >= size) {
        LOG_ONCE(LOG_MOVEMEM_RANGE, "gfx_rsp: DMEM write past a table ignored (logged once)");
        return;
    }
    if (len > size - ofs) {
        len = size - ofs;
    }
    memcpy(image + ofs, src, len);
}

static void rsp_movemem(uint32_t w0, uint32_t w1) {
    uint32_t index = w0 & 0xFE;
    uint32_t ofs = ((w0 >> 8) & 0xFF) * 8;
    uint32_t len = ((w0 >> 19) & 0x1F) * 8 + 8;
    const uint8_t* src = rsp_ptr(w1, len);

    if (src == NULL) {
        LOG_ONCE(LOG_MOVEMEM, "gfx_rsp: G_MOVEMEM %u from bad address %08X (logged once)", (unsigned int)index,
                 (unsigned int)w1);
        return;
    }
    switch (index) {
        case G_MV_VIEWPORT:
            rsp_write_image(sRsp.viewportRaw, sizeof(sRsp.viewportRaw), ofs, src, len);
            rsp_parse_viewport();
            break;
        case G_MV_LIGHT:
            rsp_write_image(sRsp.lightRaw, sizeof(sRsp.lightRaw), ofs, src, len);
            sRsp.lightsParsed = false;
            break;
        case G_MV_MMTX:
            // Raw load: the microcode does not invalidate the MVP or the light directions here
            rsp_attrs_before_mv_change();
            mtx_write(sRsp.mv, ofs, src, len);
            break;
        case G_MV_PMTX:
            mtx_write(sRsp.p, ofs, src, len);
            sRsp.pVer++;
            break;
        case G_MV_MATRIX:
            // Into the MVP itself (gSPForceMatrix); used as is while mvpValid stays set
            mtx_write(sRsp.mvp, ofs, src, len);
            sRsp.mvpForced = true;
            sRsp.forcedVer++;
            break;
        case 0:
        case 4:
            // G_MTX multiply temporaries: no lasting effect
            break;
        default:
            LOG_ONCE(LOG_MOVEMEM, "gfx_rsp: G_MOVEMEM index %u ignored (logged once)", (unsigned int)index);
            break;
    }
}

static void rsp_moveword(uint32_t w0, uint32_t w1) {
    uint32_t index = (w0 >> 16) & 0xFF;
    uint32_t ofs = w0 & 0xFFFF;
    uint8_t bytes[4] = { (uint8_t)(w1 >> 24), (uint8_t)(w1 >> 16), (uint8_t)(w1 >> 8), (uint8_t)w1 };

    switch (index) {
        case G_MW_MATRIX:
            mtx_write(sRsp.mvp, ofs, bytes, 4);
            sRsp.mvpForced = true;
            sRsp.forcedVer++;
            break;
        case G_MW_NUMLIGHT:
            // The word lands on [pad, pad, lightsValid, numLightsx18]: NUML(n) also invalidates the lights
            if (ofs == 0) {
                sRsp.numLights18 = (uint8_t)w1;
                sRsp.lightsValid = ((w1 >> 8) & 0xFF) != 0;
                sRsp.lightsParsed = false;
            } else {
                LOG_ONCE(LOG_MOVEWORD_OFS, "gfx_rsp: G_MOVEWORD %u offset %X ignored (logged once)",
                         (unsigned int)index, (unsigned int)ofs);
            }
            break;
        case G_MW_CLIP:
            // Clip ratio: only the microcode's own polygon clipping uses it
            break;
        case G_MW_SEGMENT:
            // Past the table (or unaligned) the RSP would overwrite the display list stack, not a segment
            if (ofs > 0x3C || (ofs & 3)) {
                LOG_ONCE(LOG_MOVEWORD_OFS, "gfx_rsp: G_MW_SEGMENT offset %X ignored (logged once)", (unsigned int)ofs);
                break;
            }
            gGfxSegments[ofs >> 2] = w1;
            break;
        case G_MW_FOG:
            gGfxRsp.fogMultiplier = (int16_t)(w1 >> 16);
            gGfxRsp.fogOffset = (int16_t)w1;
            break;
        case G_MW_LIGHTCOL:
            // Writes col or colc of a light, including the byte after it (kc / kl)
            rsp_write_image(sRsp.lightRaw, sizeof(sRsp.lightRaw), LIGHT_MAIN * LIGHT_SIZE + ofs, bytes, 4);
            sRsp.lightsParsed = false;
            break;
        case G_MW_FORCEMTX:
            // The word lands on [pad, mvpValid, pad, pad]
            if (ofs == 0) {
                sRsp.mvpValid = ((w1 >> 16) & 0xFF) != 0;
            } else {
                LOG_ONCE(LOG_MOVEWORD_OFS, "gfx_rsp: G_MOVEWORD %u offset %X ignored (logged once)",
                         (unsigned int)index, (unsigned int)ofs);
            }
            break;
        case G_MW_PERSPNORM:
            // Only scales w before the RSP's reciprocal (precision); no effect in float. The word written
            // starts 2 bytes before perspNorm: its high half lands in the low half of the G_RDPHALF_1 value.
            sRsp.perspNorm = (uint16_t)w1;
            sRsp.rdpHalf1 = (sRsp.rdpHalf1 & 0xFFFF0000u) | (w1 >> 16);
            break;
        default:
            LOG_ONCE(LOG_MOVEWORD, "gfx_rsp: G_MOVEWORD index %u ignored (logged once)", (unsigned int)index);
            break;
    }
}

/* ============================================================================================== */
/* Microcode switches                                                                             */
/* ============================================================================================== */

/* F3DZEX2 loaded again: DMEM from 0x180 on comes back from its data image */
static void rsp_reload_f3dzex2(void) {
    gGfxRsp.geometryMode = G_CLIPPING;
    gGfxRsp.textureOn = G_OFF;
    gGfxRsp.textureTile = 0;
    gGfxRsp.textureLevels = 0;
    gGfxRsp.textureScaleS = gGfxRsp.textureScaleT = 0.0f;
    gGfxRsp.fogMultiplier = gGfxRsp.fogOffset = 0;
    sRsp.texScale[0] = sRsp.texScale[1] = 0;
    sRsp.numLights18 = 0;
    sRsp.mvpValid = true;
    sRsp.lightsValid = true;
    sRsp.lightsParsed = false;
    memset(sRsp.lightRaw, 0, sizeof(sRsp.lightRaw));
    memset(sRsp.lightDir, 0, sizeof(sRsp.lightDir));
    gGfxRdp.dirty |= GFX_DIRTY_GEOMETRY | GFX_DIRTY_TEXTURES;
}

static void rsp_load_ucode(uint32_t w1) {
    // The microcode DMAs from this address as is (physical or KSEG0, never segmented)
    uint32_t text = w1 & 0x1FFFFFFF;

    // Geometry mode, fog, G_TEXTURE and the lights are about to be reset
    rsp_attrs_flush();
    if (text == symbol_phys(gspS2DEX2_fifoTextStart)) {
        sRsp.s2dex = true;
        gfx_s2dex_load();
        // S2DEX2 has no geometry mode or G_TEXTURE: its triangles (rotated sprites) are unshaded, without z, tile 0
        gGfxRsp.geometryMode = 0;
        gGfxRsp.textureTile = G_TX_RENDERTILE;
        gGfxRdp.dirty |= GFX_DIRTY_GEOMETRY | GFX_DIRTY_TEXTURES;
        return;
    }
    if (text != symbol_phys(gspF3DZEX2_NoN_PosLight_fifoTextStart)) {
        LOG_ONCE(LOG_UCODE, "gfx_rsp: G_LOAD_UCODE of unknown microcode %08X, treated as F3DZEX2 (logged once)",
                 (unsigned int)w1);
    }
    if (sRsp.s2dex) {
        // S2DEX2's sprite triangles set their own projection on GX: send this one again with the next vertices
        sRsp.gxProjSent = false;
    }
    sRsp.s2dex = false;
    rsp_reload_f3dzex2();
}

/* ============================================================================================== */
/* Display list interpreter                                                                       */
/* ============================================================================================== */

static void log_unknown(uint32_t* bitmap, const char* what, uint32_t w0, uint32_t w1) {
    uint32_t op = w0 >> 24;

    if (!(bitmap[op >> 5] & (1u << (op & 31)))) {
        bitmap[op >> 5] |= 1u << (op & 31);
        gc_log("gfx_rsp: %s %02X (%08X %08X) ignored (logged once)", what, (unsigned int)op, (unsigned int)w0,
               (unsigned int)w1);
    }
}

/* G_DL, and S2DEX2's G_SELECT_DL: call (push the return address) or branch to a display list */
static void rsp_call_dl(uint32_t* stack, int* sp, uint32_t* pc, uint32_t addr, bool push) {
    // A bad pointer would make the RSP run garbage; skip the call so the rest of the frame draws
    if (rsp_ptr(addr, 8) == NULL) {
        LOG_ONCE(LOG_BAD_DL, "gfx_rsp: DL call/branch to bad address %08X skipped (logged once)", (unsigned int)addr);
        return;
    }
    if (push) {
        if (*sp == DL_STACK_N64) {
            LOG_ONCE(LOG_DL_N64_DEPTH, "gfx_rsp: display lists nested deeper than %d (overflows on N64; logged once)",
                     DL_STACK_N64);
        }
        if (*sp < DL_STACK_MAX) {
            stack[(*sp)++] = *pc;
        } else {
            LOG_ONCE(LOG_DL_OVERFLOW, "gfx_rsp: display list stack overflow, call made a branch (logged once)");
        }
    }
    *pc = rsp_resolve(addr);
}

/* A command while S2DEX2 is loaded. Returns true if it was handled here; false sends it on to the F3DEX2
 * interpreter, which runs the commands both microcodes share. */
static bool rsp_s2dex_command(uint32_t* stack, int* sp, uint32_t* pc, uint32_t w0, uint32_t w1) {
    uint32_t op = w0 >> 24;
    uint32_t dl;
    bool push;

    switch (op) {
        case G_DL:
        case G_ENDDL:
        case G_LOAD_UCODE:
        case G_RDPHALF_1:
        case G_RDPHALF_2:
        case G_TEXRECT: // G_RDPHALF_0
        case G_TEXRECTFLIP:
        case G_SPNOOP:
        case G_NOOP:
        case G_SETOTHERMODE_H:
        case G_SETOTHERMODE_L:
            return false;
        case G_MOVEWORD:
            switch ((w0 >> 16) & 0xFF) {
                case G_MW_SEGMENT:
                    return false;
                case G_MW_FOG: // G_MW_GENSTAT in S2DEX2
                    gfx_s2dex_set_status(w0 & 0xFFFF, w1);
                    return true;
                default:
                    log_unknown(sLoggedS2dexOp, "S2DEX2 moveword", w0, w1);
                    return true;
            }
        case 0x04: // G_SELECT_DL, after the G_RDPHALF_0 that holds its address, status id and flag
            if (gfx_s2dex_select_dl(sRsp.texrectW0, sRsp.texrectW1, w0, w1, &dl, &push)) {
                rsp_call_dl(stack, sp, pc, dl, push);
            }
            sRsp.texrectSet = false;
            return true;
        default:
            if (op >= G_RDPLOADSYNC) {
                return false;
            }
            if (!gfx_s2dex_command(w0, w1)) {
                LOG_ONCE(LOG_S2DEX, "gfx_rsp: unknown S2DEX2 commands are skipped (logged once)");
                log_unknown(sLoggedS2dexOp, "S2DEX2 command", w0, w1);
            }
            return true;
    }
}

void gfx_rsp_reset(void) {
    // DMEM as the microcode's data image initializes it at task start
    memset(&sRsp, 0, sizeof(sRsp));
    sRsp.mvpValid = true;
    sRsp.lightsValid = true;
    sRsp.perspNorm = 0xFFFF;
    memset(gGfxSegments, 0, sizeof(gGfxSegments));
    memset(&gGfxRsp, 0, sizeof(gGfxRsp));
    gGfxRsp.geometryMode = G_CLIPPING;
    memset(sVtx, 0, sizeof(sVtx));
    sPending = 0;
    sVtxOdd = 0;
    memset(&sAttr, 0, sizeof(sAttr));
    gfx_s2dex_reset();
}

#ifdef GFX_TEX_FAIL_TRACE
uint32_t gGfxDiagPc[2]; /* command being run, and the return address of the innermost DL call */
#endif

void gfx_rsp_run(uint32_t dlAddr) {
    uint32_t stack[DL_STACK_MAX];
    int sp = 0;
    uint32_t pc = rsp_resolve(dlAddr);
    uint32_t cmds = 0;
    uint64_t start = gc_time_ticks();
    uint64_t elapsed;

    GFX_PROF_ENTER(GFX_PROF_DL);
    while (pc != 0) {
        const uint8_t* p;
        uint32_t w0, w1, op;

        if (pc - RAM_BASE > RAM_SIZE - 8) {
            LOG_ONCE(LOG_PC_RANGE, "gfx_rsp: display list at %08X is outside RAM, task stopped (logged once)",
                     (unsigned int)pc);
            break;
        }
        if (++cmds > CMD_LIMIT) {
            LOG_ONCE(LOG_RUNAWAY, "gfx_rsp: more than %u commands in a task, stopped (logged once)", CMD_LIMIT);
            break;
        }
        p = (const uint8_t*)((uintptr_t)pc + RAM_BIAS);
        w0 = rd32(p);
        w1 = rd32(p + 4);
#ifdef GFX_TEX_FAIL_TRACE
        gGfxDiagPc[0] = pc;
        gGfxDiagPc[1] = (sp > 0) ? stack[sp - 1] : 0;
#endif
        pc += 8;
        op = w0 >> 24;
#if GFX_PROF && !defined(GFX_HOST_TEST)
        sProfOps[op]++;
#endif

        if (sRsp.s2dex) {
            bool done;

            GFX_PROF_ENTER(GFX_PROF_S2D);
            done = rsp_s2dex_command(stack, &sp, &pc, w0, w1);
            GFX_PROF_LEAVE();
            if (done) {
                continue;
            }
        }

        switch (op) {
            case G_NOOP:
            case G_SPNOOP:
                break;

            case G_VTX:
                GFX_PROF_ENTER(GFX_PROF_VTX);
                rsp_vertices(w0, w1);
                GFX_PROF_LEAVE();
                break;

            case G_MODIFYVTX:
                rsp_modify_vertex(w0, w1);
                break;

            case G_CULLDL:
                if (rsp_cull_dl(w0, w1)) {
                    pc = (sp > 0) ? stack[--sp] : 0; // like G_ENDDL
                }
                break;

            case G_BRANCH_Z: {
                // F3DZEX's G_BRANCH_W: branch to the G_RDPHALF_1 address if (s16)w_int - w1 < 0
                uint32_t idx = (w0 & 0xFFF) >> 1;

                if (idx >= VTX_COUNT) {
                    LOG_ONCE(LOG_TRI_INDEX, "gfx_rsp: G_BRANCH_Z vertex index out of range (logged once)");
                } else if ((int32_t)((uint32_t)rsp_w_int(sVtx[idx].w) - w1) < 0) {
                    if (rsp_ptr(sRsp.rdpHalf1, 8) != NULL) {
                        pc = rsp_resolve(sRsp.rdpHalf1);
                    } else {
                        LOG_ONCE(LOG_BAD_DL, "gfx_rsp: DL call/branch to bad address %08X skipped (logged once)",
                                 (unsigned int)sRsp.rdpHalf1);
                    }
                }
                break;
            }

            case G_TRI1:
#if GFX_PROF && !defined(GFX_HOST_TEST)
                prof_tex_draw();
#endif
                GFX_PROF_ENTER(GFX_PROF_TRI);
                rsp_triangle(w0);
                GFX_PROF_LEAVE();
                break;

            case G_TRI2:
            case G_QUAD:
                // The microcode draws the second triangle first
#if GFX_PROF && !defined(GFX_HOST_TEST)
                prof_tex_draw();
#endif
                GFX_PROF_ENTER(GFX_PROF_TRI);
                rsp_triangle(w1);
                rsp_triangle(w0);
                GFX_PROF_LEAVE();
                break;

            case G_LINE3D:
                LOG_ONCE(LOG_LINE3D, "gfx_rsp: G_LINE3D is a no-op in F3DZEX2 (logged once)");
                break;

            case G_DMA_IO:
                LOG_ONCE(LOG_DMA_IO, "gfx_rsp: G_DMA_IO ignored (logged once)");
                break;

            case G_SPECIAL_1:
            case G_SPECIAL_2:
            case G_SPECIAL_3:
                LOG_ONCE(LOG_SPECIAL, "gfx_rsp: G_SPECIAL_%u is a no-op (logged once)", (unsigned int)(G_SPECIAL_1 - op + 1));
                break;

            case G_TEXTURE: {
                uint8_t on = ((w0 >> 1) & 0x7F) ? G_ON : G_OFF;
                uint8_t tile = (uint8_t)((w0 >> 8) & 7);
                uint8_t levels = (uint8_t)((w0 >> 11) & 7);

                if (on != gGfxRsp.textureOn || tile != gGfxRsp.textureTile || levels != gGfxRsp.textureLevels ||
                    (w1 >> 16) != sRsp.texScale[0] || (w1 & 0xFFFF) != sRsp.texScale[1]) {
                    gGfxRsp.textureOn = on;
                    gGfxRsp.textureTile = tile;
                    gGfxRsp.textureLevels = levels;
                    sRsp.texScale[0] = (uint16_t)(w1 >> 16);
                    sRsp.texScale[1] = (uint16_t)w1;
                    gGfxRsp.textureScaleS = sRsp.texScale[0] * (1.0f / 65536.0f);
                    gGfxRsp.textureScaleT = sRsp.texScale[1] * (1.0f / 65536.0f);
                    gGfxRdp.dirty |= GFX_DIRTY_TEXTURES;
                }
                break;
            }

            case G_POPMTX:
                rsp_popmtx(w1);
                break;

            case G_GEOMETRYMODE: {
                // The clear mask is the whole w0, command byte included
                uint32_t mode = (gGfxRsp.geometryMode & w0) | w1;

                if (mode != gGfxRsp.geometryMode) {
                    gGfxRsp.geometryMode = mode;
                    gGfxRdp.dirty |= GFX_DIRTY_GEOMETRY;
                }
                break;
            }

            case G_MTX:
                rsp_mtx(w0, w1);
                break;

            case G_MOVEWORD:
                rsp_moveword(w0, w1);
                break;

            case G_MOVEMEM:
                rsp_movemem(w0, w1);
                break;

            case G_LOAD_UCODE:
                rsp_load_ucode(w1);
                break;

            case G_DL:
                rsp_call_dl(stack, &sp, &pc, w1, !((w0 >> 16) & G_DL_NOPUSH));
                break;

            case G_ENDDL:
                pc = (sp > 0) ? stack[--sp] : 0;
                break;

            case G_RDPHALF_1:
                sRsp.rdpHalf1 = w1;
                break;

            case G_TEXRECT:
            case G_TEXRECTFLIP:
                // Sent to the RDP with the G_RDPHALF_1 and G_RDPHALF_2 words that follow
                sRsp.texrectW0 = w0;
                sRsp.texrectW1 = w1;
                sRsp.texrectSet = true;
                break;

            case G_RDPHALF_2:
                if (sRsp.texrectSet) {
                    GFX_PROF_ENTER(GFX_PROF_RECT);
                    gfx_rdp_texrect(sRsp.texrectW0, sRsp.texrectW1, sRsp.rdpHalf1, w1,
                                    (sRsp.texrectW0 >> 24) == G_TEXRECTFLIP);
                    GFX_PROF_LEAVE();
                    sStats.rects++;
                } else {
                    LOG_ONCE(LOG_HALF2, "gfx_rsp: G_RDPHALF_2 without a texture rectangle (logged once)");
                }
                break;

            case G_SETOTHERMODE_H:
            case G_SETOTHERMODE_L:
                GFX_PROF_ENTER(GFX_PROF_RDP);
                gfx_rdp_command(w0, w1);
                GFX_PROF_LEAVE();
                break;

            default:
                if (op >= G_RDPLOADSYNC) {
#if GFX_PROF && !defined(GFX_HOST_TEST)
                    if (op == G_SETTIMG || op == G_SETTILE || op == G_SETTILESIZE || op == G_LOADBLOCK ||
                        op == G_LOADTILE || op == G_LOADTLUT) {
                        prof_tex_cmd(w0, w1);
                    }
#endif
                    GFX_PROF_ENTER(op == G_FILLRECT ? GFX_PROF_RECT : GFX_PROF_RDP);
                    gfx_rdp_command(w0, w1);
                    GFX_PROF_LEAVE();
                    if (op == G_FILLRECT) {
                        sStats.rects++;
                    } else if (op == G_SETCIMG || op == G_SETZIMG) {
                        gfx_s2dex_note_image(w0, w1);
                    } else if (op == G_LOADTLUT) {
                        gfx_s2dex_note_tlut(w0, w1);
                    }
                } else {
                    log_unknown(sLoggedOp, "unknown opcode", w0, w1);
                }
                break;
        }
    }
    GFX_PROF_LEAVE();

    elapsed = gc_time_ticks() - start;
    sStats.tasks++;
    sStats.cmds += cmds;
    sStats.ticks += elapsed;
    if (elapsed > sStats.maxTicks) {
        sStats.maxTicks = elapsed;
    }
}

/* Texture cache counters (gfx_tex.c counts since boot): activity since the last line, current occupancy */
static void rsp_log_tex_stats(void) {
    static GfxTexStats sPrev;
    GfxTexStats s;

    gfx_tex_get_stats(&s);
    gc_log("gfx_tex: %u binds (%u hits, %u misses, %u reconverts, %u slow, %u failed), %u evictions, %u syncs; "
           "%u textures, %u of %u KB",
           (unsigned int)(s.binds - sPrev.binds), (unsigned int)(s.hits - sPrev.hits),
           (unsigned int)(s.misses - sPrev.misses), (unsigned int)(s.reconverts - sPrev.reconverts),
           (unsigned int)(s.slow - sPrev.slow), (unsigned int)(s.failures - sPrev.failures),
           (unsigned int)(s.evictions - sPrev.evictions), (unsigned int)(s.syncs - sPrev.syncs),
           (unsigned int)s.entries, (unsigned int)(s.bytesUsed / 1024), (unsigned int)(s.bytesTotal / 1024));
    sPrev = s;
}

/* S2DEX2 activity since the last line, if there was any */
static void rsp_log_s2dex_stats(void) {
    static GfxS2dexStats sPrev;
    GfxS2dexStats s;

    gfx_s2dex_get_stats(&s);
    if (memcmp(&s, &sPrev, sizeof(s)) != 0) {
        gc_log("gfx_s2dex: %u backgrounds in %u rects (%u from render targets), %u not drawn, %u sprites",
               (unsigned int)(s.bgs - sPrev.bgs), (unsigned int)(s.rects - sPrev.rects),
               (unsigned int)(s.targetBgs - sPrev.targetBgs), (unsigned int)(s.skipped - sPrev.skipped),
               (unsigned int)(s.objs - sPrev.objs));
        sPrev = s;
    }
}

void gfx_rsp_stats_frame(void) {
    uint64_t now = gc_time_ticks();
    uint32_t n;

    if (sStats.lastLog == 0) {
        sStats.lastLog = now;
    }
    if (now - sStats.lastLog < 3 * GC_TB_HZ) {
        return;
    }
    n = sStats.tasks;
    if (n != 0) {
        gc_log("gfx_rsp: %u tasks in %u ms; per task %u cmds, %u vtx, %u tris (%u drawn, %u clipped, %u culled), "
               "%u rects; %u us avg, %u us max",
               (unsigned int)n, (unsigned int)((now - sStats.lastLog) * 1000 / GC_TB_HZ),
               (unsigned int)(sStats.cmds / n), (unsigned int)(sStats.vtx / n), (unsigned int)(sStats.tris / n),
               (unsigned int)(sStats.drawn / n), (unsigned int)(sStats.clipped / n),
               (unsigned int)(sStats.culled / n), (unsigned int)(sStats.rects / n),
               (unsigned int)(sStats.ticks * 1000000 / GC_TB_HZ / n),
               (unsigned int)(sStats.maxTicks * 1000000 / GC_TB_HZ));
        rsp_log_tex_stats();
        rsp_log_s2dex_stats();
#if GFX_PROF && !defined(GFX_HOST_TEST)
        prof_log((uint32_t)((now - sStats.lastLog) * 1000 / GC_TB_HZ));
#endif
#if GFX_VTX_CHECK && !defined(GFX_HOST_TEST)
        gc_log("gfx_rsp: vtx check: %u positions, %u attribute sets, %u matrices compared since boot, %u differ",
               (unsigned int)sChkPos, (unsigned int)sChkAttr, (unsigned int)sChkMtx, (unsigned int)sChkBad);
#endif
    }
    memset(&sStats, 0, sizeof(sStats));
    sStats.lastLog = now;
}
