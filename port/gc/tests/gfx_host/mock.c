/**
 * Mocks for the gfx_rsp host test: fake N64 RAM, the bridge (log, time), the GX backend, the RDP module
 * and texture cache entry points gfx_rsp.c calls, gGfxRdp, and the dummy microcode symbols. The GX mocks
 * count vertices or matrices that are not finite.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test.h"

#define RAM_BASE 0x80000000u
#define RAM_SIZE 0x01800000u

uintptr_t gGfxHostRamBias;
static uint8_t* sRam;
static uint32_t sRamTop;

unsigned char gspS2DEX2_fifoTextStart[16];
unsigned char gspF3DZEX2_NoN_PosLight_fifoTextStart[16];

GfxRdpState gGfxRdp;

TriRec gTris[MAX_TRIS];
int gTriCount;
float gProj[4][4];
int gProjCount;
CmdRec gRdp[MAX_RDP];
int gRdpCount;
TexrectRec gTexrects[MAX_TEXRECTS];
int gTexrectCount;
int gLogCount;
char gLastLog[512];
char gPrevLog[512];
int gNonFinite;
GfxTexStats gTexStats;

static unsigned long long sTicks;

void ram_init(void) {
    sRam = aligned_alloc(4096, RAM_SIZE);
    if (sRam == NULL) {
        fprintf(stderr, "cannot allocate fake RAM\n");
        exit(2);
    }
    gGfxHostRamBias = (uintptr_t)sRam - RAM_BASE;
    ram_reset();
}

void ram_reset(void) {
    memset(sRam, 0, 1 << 20); /* tests only use the first MiB */
    sRamTop = 0x1000;
}

uint32_t ram_alloc(uint32_t size) {
    uint32_t addr = RAM_BASE + sRamTop;

    sRamTop = (sRamTop + size + 15) & ~15u;
    if (sRamTop > (1u << 20)) {
        fprintf(stderr, "fake RAM exhausted\n");
        exit(2);
    }
    memset(sRam + (addr - RAM_BASE), 0, size);
    return addr;
}

uint8_t* ram_ptr(uint32_t addr) {
    return sRam + ((addr & 0x1FFFFFFF) - (RAM_BASE & 0x1FFFFFFF));
}

void wr16(uint32_t addr, uint16_t v) {
    uint8_t* p = ram_ptr(addr);

    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

void wr32(uint32_t addr, uint32_t v) {
    uint8_t* p = ram_ptr(addr);

    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void mock_reset(void) {
    gTriCount = 0;
    gProjCount = 0;
    gRdpCount = 0;
    gTexrectCount = 0;
    memset(&gGfxRdp, 0, sizeof(gGfxRdp));
}

/* Bridge */

void gc_log(const char* fmt, ...) {
    va_list ap;

    memcpy(gPrevLog, gLastLog, sizeof(gPrevLog));
    va_start(ap, fmt);
    vsnprintf(gLastLog, sizeof(gLastLog), fmt, ap);
    va_end(ap);
    gLogCount++;
    if (getenv("GFX_TEST_VERBOSE") != NULL) {
        printf("  [log] %s\n", gLastLog);
    }
}

void gc_halt(const char* fmt, ...) {
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    abort();
}

unsigned long long gc_time_ticks(void) {
    sTicks += 4050; /* 100 us per call */
    return sTicks;
}

/* GX backend and RDP module */

void gfx_gx_set_projection(const float m[4][4]) {
    int i;

    for (i = 0; i < 16; i++) {
        if (!isfinite(m[i >> 2][i & 3])) {
            gNonFinite++;
        }
    }
    memcpy(gProj, m, sizeof(gProj));
    gProjCount++;
}

static void check_finite(const GfxVtx* v) {
    if (!isfinite(v->x) || !isfinite(v->y) || !isfinite(v->z) || !isfinite(v->w) || !isfinite(v->s) ||
        !isfinite(v->t)) {
        gNonFinite++;
    }
}

void gfx_gx_triangle(const GfxVtx* v0, const GfxVtx* v1, const GfxVtx* v2) {
    check_finite(v0);
    check_finite(v1);
    check_finite(v2);
    if (gTriCount < MAX_TRIS) {
        gTris[gTriCount].v[0] = *v0;
        gTris[gTriCount].v[1] = *v1;
        gTris[gTriCount].v[2] = *v2;
    }
    gTriCount++;
}

void gfx_rdp_command(uint32_t w0, uint32_t w1) {
    if (gRdpCount < MAX_RDP) {
        gRdp[gRdpCount].w0 = w0;
        gRdp[gRdpCount].w1 = w1;
    }
    gRdpCount++;
}

void gfx_rdp_texrect(uint32_t w0, uint32_t w1, uint32_t half1, uint32_t half2, bool flip) {
    if (gTexrectCount < MAX_TEXRECTS) {
        TexrectRec* r = &gTexrects[gTexrectCount];

        r->w0 = w0;
        r->w1 = w1;
        r->half1 = half1;
        r->half2 = half2;
        r->flip = flip;
    }
    gTexrectCount++;
}

/* Texture cache */

void gfx_tex_get_stats(GfxTexStats* out) {
    *out = gTexStats;
}
