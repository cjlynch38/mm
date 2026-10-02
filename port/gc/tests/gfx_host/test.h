/**
 * Shared declarations of the gfx_rsp / gfx_s2dex host test: checks, fake N64 RAM, and what the mocks of GX,
 * gfx_rdp.c, the texture cache, gfx_fb.c and the bridge recorded.
 */
#ifndef GFX_HOST_TEST_H
#define GFX_HOST_TEST_H

#include <stdio.h>
#include "gfx_internal.h"

extern int gChecks, gFailures;

#define CHECK(cond, ...)                                     \
    do {                                                     \
        gChecks++;                                           \
        if (!(cond)) {                                       \
            gFailures++;                                     \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);      \
            printf(__VA_ARGS__);                             \
            printf("\n");                                    \
        }                                                    \
    } while (0)

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
#define MAX_BINDS 16
#define MAX_IMAGE_RECTS 64

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

typedef struct {
    const void* addr;
    uint8_t fmt, siz;
    uint16_t width, height, stride;
    const void* tlut;
    bool tlutIA, linear;
    int texMap;
} BindRec;

typedef struct {
    float ulx, uly, lrx, lry, s, t, dsdx, dtdy;
    GfxTexBinding b;
} ImageRectRec;

extern TriRec gTris[MAX_TRIS];
extern int gTriCount;
extern float gProj[4][4];
extern int gProjCount;
extern CmdRec gRdp[MAX_RDP];
extern int gRdpCount;
extern TexrectRec gTexrects[MAX_TEXRECTS];
extern int gTexrectCount;
extern BindRec gBinds[MAX_BINDS];
extern int gBindCount;
extern bool gBindFail; /* gfx_tex_bind_image fails */
extern ImageRectRec gImageRects[MAX_IMAGE_RECTS];
extern int gImageRectCount;
extern bool gFbReady;  /* what gfx_fb_ready returns */
extern bool gFbBindImage; /* gfx_fb_bind_image serves 320x240 images (a GPU texture of the frame or a capture) */
extern int gFbBindCount;  /* gfx_fb_bind_image calls, and the last one's image and filtering */
extern const void* gFbBindAddr;
extern bool gFbBindLinear;
extern GfxBindImageFn gFbBindFromRam; /* the last call's way back to RAM */
extern bool gFbBound;                 /* the last gfx_fb_bind_image call served the image... */
extern int gFbDoneCount;              /* ...gfx_fb_image_done calls... */
extern int gFbDoneRects;              /* ...and gImageRectCount when the served image was done with (-1: not yet) */
extern int gFbSyncCount; /* gfx_fb_sync_ram calls, and the last one's range */
extern const void* gFbSyncAddr;
extern uint32_t gFbSyncBytes;
extern int gLogCount;
extern char gLastLog[512];
extern char gPrevLog[512];    /* the log line before gLastLog */
extern int gNonFinite;        /* vertices, projections or rectangles with a NaN / infinite value that reached GX */
extern GfxTexStats gTexStats; /* what the gfx_tex_get_stats mock returns */

extern unsigned char gspS2DEX2_fifoTextStart[], gspF3DZEX2_NoN_PosLight_fifoTextStart[];
extern uintptr_t gGfxHostRamBias;

/* Clears the records; gGfxRdp as gfx_rdp_reset leaves it (full-screen scissor). The gfx_rdp_command mock keeps
 * the RDP state gfx_s2dex.c reads: texture image, tiles, color / z image, other modes, scissor. */
void mock_reset(void);

/* test_s2dex.c */
void test_s2dex(void);

#endif
