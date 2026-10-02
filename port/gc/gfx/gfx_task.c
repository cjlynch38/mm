/**
 * Renderer entry points declared in gc_bridge.h (gc_gfx_*): init, the per-task sequence that runs a
 * graphics task's display list through the RSP/RDP interpreters into GX, and presenting frames.
 * GC_RENDERER=0 builds (gc_options.h) keep the M2 text console as the display and skip the tasks.
 */
#include <gccore.h>
#include "gc_ogc.h"
#include "gc_options.h"
#include "gfx_internal.h"

/* gc_gfx_init() needs this much free MEM1 arena, so the renderer never starves the game:
 *   GX FIFO 256 KiB + texture cache (about 2 MiB, gfx_tex.c) + 3 XFBs of the video mode (614,400 bytes each
 *   at 640x480, 734,720 for PAL's 574 lines) + what the game's shim allocations take after boot (thread
 *   stacks and buffers, measured 1.2 MiB). */
#define GFX_MEM_FIXED (256 * 1024 + 2 * 1024 * 1024)
#define GFX_MEM_XFBS 3
#define GFX_MEM_GAME_RESERVE (1536 * 1024)

static bool sEnabled;

void gc_gfx_init(void) {
    unsigned int arenaFree;
    GXRModeObj* mode;
    unsigned int need;

    if (!GC_RENDERER) {
        gc_log("gfx: renderer off (GC_RENDERER=0): the text console stays the display");
        return;
    }
    arenaFree = (unsigned int)SYS_GetArena1Hi() - (unsigned int)SYS_GetArena1Lo();
    mode = (GXRModeObj*)gc_ogc_video_mode();
    need = GFX_MEM_FIXED + GFX_MEM_XFBS * ((mode != NULL) ? VIDEO_GetFrameBufferSize(mode) : 614400);
    if (arenaFree < need + GFX_MEM_GAME_RESERVE) {
        gc_log("gfx: renderer disabled: %u KB free in MEM1, it needs %u KB plus %u KB kept for the game", arenaFree / 1024,
               need / 1024, GFX_MEM_GAME_RESERVE / 1024);
        return;
    }

    gfx_gx_init();
    if (!gfx_gx_ready()) {
        return;
    }
    gfx_tex_init();
    gfx_tev_init();
    sEnabled = true;
    gc_log("gfx: renderer ready (%u KB of MEM1 left)",
           ((unsigned int)SYS_GetArena1Hi() - (unsigned int)SYS_GetArena1Lo()) / 1024);
}

int gc_gfx_enabled(void) {
    return sEnabled;
}

void gc_gfx_run_task(unsigned int dlist) {
    if (!sEnabled) {
        return;
    }
    gfx_gx_task_begin();
    gfx_rsp_reset();
    gfx_rdp_reset();
    gfx_tex_frame();
    gfx_rsp_run(dlist);
    gfx_gx_task_end();
    gfx_rsp_stats_frame();
}

void gc_gfx_present(const void* framebuffer) {
    if (sEnabled) {
        gfx_gx_present(framebuffer);
    }
}
