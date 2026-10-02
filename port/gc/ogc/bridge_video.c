/**
 * Video for M2: one external framebuffer in the TV's preferred mode, with libogc's text console
 * drawn into it. That console is the M2 "display" (gc_log output); the game's own framebuffers
 * are not shown yet. The VI service thread of the libultra shim paces itself with
 * gc_video_wait_vsync().
 */
#include <gccore.h>
#include <stdio.h>
#include "gc_ogc.h"

static GXRModeObj* sMode;
static void* sXfb;
static int sBlack = -1;

void gc_ogc_video_init(void) {
    VIDEO_Init();
    sMode = VIDEO_GetPreferredMode(NULL);
    // The console writes pixels with the CPU; the uncached alias makes them visible to VI at once.
    sXfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(sMode));
    console_init(sXfb, 20, 20, sMode->fbWidth, sMode->xfbHeight, sMode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(sMode);
    VIDEO_SetNextFramebuffer(sXfb);
    VIDEO_SetBlack(false);
    sBlack = 0;
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (sMode->viTVMode & VI_NON_INTERLACE) {
        VIDEO_WaitVSync();
    }
    // Start two rows down, clear of the overscan area on CRTs.
    printf("\x1b[2;0H");
}

void gc_video_wait_vsync(void) {
    VIDEO_WaitVSync();
}

// While the text console is the only thing on screen (until the GX renderer exists), the game's
// requests to blank the display are recorded but not applied: the N64 boots black and the scheduler
// keeps the screen black until its first framebuffer swap, which would hide the console.
#ifndef GC_HONOR_VI_BLACK
#define GC_HONOR_VI_BLACK 0
#endif

void gc_video_set_black(int black) {
    black = (black != 0);
    if (black != sBlack) {
        sBlack = black;
        if (GC_HONOR_VI_BLACK) {
            VIDEO_SetBlack(black);
            VIDEO_Flush();
        }
    }
}

void gc_ogc_video_show_console(void) {
    if (sMode == NULL) {
        return;
    }
    // The game may have blanked the screen (osViBlack: the N64 boots black, and the PRE-NMI
    // sequence blanks it again), which would hide the console.
    VIDEO_SetNextFramebuffer(sXfb);
    VIDEO_SetBlack(false);
    sBlack = 0;
    VIDEO_Flush();
}

int gc_video_refresh_hz(void) {
    unsigned int standard = (sMode != NULL) ? (sMode->viTVMode >> 2) : VIDEO_GetCurrentTvMode();

    return (standard == VI_PAL || standard == VI_DEBUG_PAL) ? 50 : 60;
}
