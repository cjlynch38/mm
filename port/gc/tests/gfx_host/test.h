/**
 * Shared declarations of the gfx_rsp host test: fake N64 RAM, and what the mocks of GX, gfx_rdp.c and
 * the bridge recorded.
 */
#ifndef GFX_HOST_TEST_H
#define GFX_HOST_TEST_H

#include "gfx_internal.h"

/* Fake RAM: N64 addresses 0x80000000.. (24 MiB) */
void ram_init(void);
void ram_reset(void);
uint32_t ram_alloc(uint32_t size); /* 16-byte aligned, zeroed; returns an N64 (KSEG0) address */
uint8_t* ram_ptr(uint32_t addr);
void wr16(uint32_t addr, uint16_t v);
void wr32(uint32_t addr, uint32_t v);

/* Mock records */
#define MAX_TRIS 512
#define MAX_RDP 512
#define MAX_TEXRECTS 16

typedef struct {
    GfxVtx v[3];
} TriRec;

typedef struct {
    uint32_t w0, w1;
} CmdRec;

typedef struct {
    uint32_t w0, w1, half1, half2;
    bool flip;
} TexrectRec;

extern TriRec gTris[MAX_TRIS];
extern int gTriCount;
extern float gProj[4][4];
extern int gProjCount;
extern CmdRec gRdp[MAX_RDP];
extern int gRdpCount;
extern TexrectRec gTexrects[MAX_TEXRECTS];
extern int gTexrectCount;
extern int gLogCount;
extern char gLastLog[512];
extern char gPrevLog[512];  /* the log line before gLastLog */
extern int gNonFinite;      /* vertices or projections with a NaN / infinite value that reached GX */
extern GfxTexStats gTexStats; /* what the gfx_tex_get_stats mock returns */

extern unsigned char gspS2DEX2_fifoTextStart[], gspF3DZEX2_NoN_PosLight_fifoTextStart[];
extern uintptr_t gGfxHostRamBias;

void mock_reset(void);

#endif
