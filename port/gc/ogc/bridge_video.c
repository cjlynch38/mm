/**
 * Video: one external framebuffer in the TV's preferred mode, with libogc's text console drawn into
 * it. The console is the display until the GX renderer (port/gc/gfx) shows its first frame through
 * gc_ogc_video_show_frame(); from then on the renderer's XFBs are shown and gc_log stops writing to
 * the console (it still goes to the USB Gecko / SD log). gc_halt brings the console back for good
 * with gc_ogc_video_show_console(). The VI service thread of the libultra shim paces itself with
 * gc_video_wait_vsync().
 */
#include <gccore.h>
#include <ogc/machine/processor.h>
#include <stdio.h>
#include "gc_ogc.h"

static GXRModeObj* sMode;
static void* sXfb;
static int sBlack = -1;
static int sRendererOwns;  // the renderer has shown a frame: its XFBs are the display
static int sConsoleForced; // gc_halt showed the console: the renderer may not take the display back

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

struct _gx_rmodeobj* gc_ogc_video_mode(void) {
    return sMode;
}

void gc_video_wait_vsync(void) {
    VIDEO_WaitVSync();
}

// While the text console is the only thing on screen, the game's requests to blank the display are
// recorded but not applied: the N64 boots black and the scheduler keeps the screen black until its
// first framebuffer swap, which would hide the console. Once the renderer owns the display they are
// honoured (the recorded state is applied when it takes over).
#ifndef GC_HONOR_VI_BLACK
#define GC_HONOR_VI_BLACK 0
#endif

void gc_video_set_black(int black) {
    unsigned int level;

    black = (black != 0);
    _CPU_ISR_Disable(level);
    if (black != sBlack) {
        sBlack = black;
        if ((GC_HONOR_VI_BLACK || sRendererOwns) && !sConsoleForced) {
            VIDEO_SetBlack(black);
            VIDEO_Flush();
        }
    }
    _CPU_ISR_Restore(level);
}

int gc_ogc_video_show_frame(void* xfb) {
    unsigned int level;
    int first;

    _CPU_ISR_Disable(level);
    if (sConsoleForced || sMode == NULL) {
        _CPU_ISR_Restore(level);
        return 0;
    }
    first = !sRendererOwns;
    sRendererOwns = 1;
    VIDEO_SetNextFramebuffer(xfb);
    if (first) {
        VIDEO_SetBlack(sBlack > 0);
    }
    VIDEO_Flush();
    _CPU_ISR_Restore(level);

    if (first) {
        gc_log("video: the renderer owns the display now (console output continues in the log)");
    }
    return 1;
}

int gc_ogc_video_console_visible(void) {
    return !sRendererOwns || sConsoleForced;
}

void gc_ogc_video_show_console(void) {
    unsigned int level;

    if (sMode == NULL) {
        return;
    }
    // The game may have blanked the screen (osViBlack: the N64 boots black, and the PRE-NMI
    // sequence blanks it again), which would hide the console.
    _CPU_ISR_Disable(level);
    sConsoleForced = 1;
    VIDEO_SetNextFramebuffer(sXfb);
    VIDEO_SetBlack(false);
    sBlack = 0;
    VIDEO_Flush();
    _CPU_ISR_Restore(level);
}

int gc_video_refresh_hz(void) {
    unsigned int standard = (sMode != NULL) ? (sMode->viTVMode >> 2) : VIDEO_GetCurrentTvMode();

    return (standard == VI_PAL || standard == VI_DEBUG_PAL) ? 50 : 60;
}
