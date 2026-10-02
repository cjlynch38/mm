/**
 * libultra Video Interface API (osCreateViManager, osVi*) for the GameCube.
 *
 * On N64 the VI interrupt wakes the VI manager thread, which swaps the "next" VI context into
 * "current" (__osViSwapContext) and, every `retraceCount` retraces, posts the message registered
 * with osViSetEvent. Here a shim service thread at GC_PRIO_SERVICE_VI blocks in
 * gc_video_wait_vsync() and calls __gcViRetrace(), which does the same work from thread context.
 *
 * The contexts follow libultra exactly: setters write the next context, the retrace makes it
 * current and the new next context starts as a copy of it. sched.c relies on this to pace frames
 * (osViGetCurrentFramebuffer() only returns a swapped buffer after the following retrace). Nothing
 * from the N64 framebuffers is shown on screen in M2; modes, scales and features are only recorded.
 * All state is protected by the global OS lock.
 */
#include "gc_ultra_internal.h"
#include "stdbool.h"

#define GC_VI_THREAD_STACK_SIZE 0x4000

static __OSViContext sViContexts[2];
static __OSViContext* sViCurr = &sViContexts[0];
static __OSViContext* sViNext = &sViContexts[1];

static u32 sViAdditionalScanline;  // osViExtendVStart(), recorded only
static s32 sViServiceStarted;      // the VI service thread exists
static s32 sViMgrActive;           // osCreateViManager() has run
static u16 sViRetraceCountdown;    // retraces until the osViSetEvent message is posted
static s32 sViBlackApplied = -1;   // last value passed to gc_video_set_black(), -1 = never
static u32 sViRetraceTotal;        // retraces seen by the service thread (__osViIntrCount)
static gc_thread_t sViThread;

/** __osViSwapContext() without the register writes. Caller holds the OS lock. */
static void ViSwapContext(void) {
    __OSViContext* next = sViNext;

    sViNext = sViCurr;
    sViCurr = next;
    *sViNext = *sViCurr;
}

/** __osViInit(). Caller holds the OS lock. */
static void ViInitContexts(void) {
    bzero(sViContexts, sizeof(sViContexts));
    sViCurr = &sViContexts[0];
    sViNext = &sViContexts[1];
    sViNext->retraceCount = 1;
    sViCurr->retraceCount = 1;
    sViNext->buffer = (void*)K0BASE;
    sViCurr->buffer = (void*)K0BASE;

    if (osTvType == OS_TV_PAL) {
        sViNext->modep = &osViModePalLan1;
    } else if (osTvType == OS_TV_MPAL) {
        sViNext->modep = &osViModeMpalLan1;
    } else {
        sViNext->modep = &osViModeNtscLan1;
    }

    // The N64 starts with the screen blanked until the game calls osViBlack(false)
    sViNext->state = VI_STATE_BLACK;
    sViNext->features = sViNext->modep->comRegs.ctrl;

    ViSwapContext();
}

static void ViThreadMain(void* arg) {
    while (true) {
        gc_video_wait_vsync();
        __gcViRetrace();
    }
}

void __gcViInit(void) {
    s32 start;

    gc_os_lock();
    start = !sViServiceStarted;
    if (start) {
        sViServiceStarted = true;
        ViInitContexts();
    }
    gc_os_unlock();

    if (start && (gc_thread_create(&sViThread, ViThreadMain, NULL, GC_VI_THREAD_STACK_SIZE, GC_PRIO_SERVICE_VI) != 0)) {
        gc_halt("vi: cannot create the VI service thread");
    }
}

void __gcViRetrace(void) {
    s32 black = -1;

    // On N64 the VI interrupt raises OS_EVENT_VI, which wakes the VI manager. Nothing in the game
    // registers this event, but post it for anything that does.
    __gcPostEvent(OS_EVENT_VI);

    gc_os_lock();
    sViRetraceTotal++;

    // Like viMgrMain: no swaps and no retrace messages before osCreateViManager()
    if (sViMgrActive) {
        ViSwapContext();

        sViRetraceCountdown--;
        if (sViRetraceCountdown == 0) {
            if (sViCurr->mq != NULL) {
                // NOBLOCK: if the client's queue is full the message is dropped, as on N64
                __gcSendMesgLocked(sViCurr->mq, sViCurr->msg, false);
            }
            // libultra reloads 0 as-is, which makes the u16 counter wrap to 65536 retraces;
            // treat 0 as 1 instead (the game always uses 1).
            sViRetraceCountdown = (sViCurr->retraceCount != 0) ? sViCurr->retraceCount : 1;
        }

        // osViBlack() takes effect at the retrace that makes the context current
        black = (sViCurr->state & VI_STATE_BLACK) ? 1 : 0;
        if (black == sViBlackApplied) {
            black = -1;
        } else {
            sViBlackApplied = black;
        }
    }
    gc_os_unlock();

    if (black >= 0) {
        gc_video_set_black(black);
    }
}

void osCreateViManager(OSPri pri) {
    // gc_ultra_boot() normally starts the service thread already; `pri` is not used because the
    // service thread always runs above every game thread (the N64 VI manager uses priority 254).
    __gcViInit();

    gc_os_lock();
    if (!sViMgrActive) {
        sViMgrActive = true;
        sViRetraceCountdown = (sViCurr->retraceCount != 0) ? sViCurr->retraceCount : 1;
    }
    gc_os_unlock();
}

void osViSetEvent(OSMesgQueue* mq, OSMesg m, u32 retraceCount) {
    gc_os_lock();
    sViNext->mq = mq;
    sViNext->msg = m;
    sViNext->retraceCount = retraceCount;
    gc_os_unlock();
}

void osViSwapBuffer(void* frameBufPtr) {
    gc_os_lock();
    sViNext->buffer = frameBufPtr;
    sViNext->state |= VI_STATE_BUFFER_UPDATED;
    gc_os_unlock();
}

void* osViGetCurrentFramebuffer(void) {
    void* buffer;

    gc_os_lock();
    buffer = sViCurr->buffer;
    gc_os_unlock();
    return buffer;
}

void* osViGetNextFramebuffer(void) {
    void* buffer;

    gc_os_lock();
    buffer = sViNext->buffer;
    gc_os_unlock();
    return buffer;
}

void osViBlack(u8 active) {
    gc_os_lock();
    if (active) {
        sViNext->state |= VI_STATE_BLACK;
    } else {
        sViNext->state &= ~VI_STATE_BLACK;
    }
    gc_os_unlock();
}

void osViSetMode(OSViMode* modep) {
    gc_os_lock();
    sViNext->modep = modep;
    // Assignment, not OR, as in libultra: this also clears VI_STATE_BLACK
    sViNext->state = VI_STATE_MODE_UPDATED;
    sViNext->features = sViNext->modep->comRegs.ctrl;
    gc_os_unlock();
}

void osViSetSpecialFeatures(u32 func) {
    gc_os_lock();
    if (func & OS_VI_GAMMA_ON) {
        sViNext->features |= VI_CTRL_GAMMA_ON;
    }
    if (func & OS_VI_GAMMA_OFF) {
        sViNext->features &= ~VI_CTRL_GAMMA_ON;
    }
    if (func & OS_VI_GAMMA_DITHER_ON) {
        sViNext->features |= VI_CTRL_GAMMA_DITHER_ON;
    }
    if (func & OS_VI_GAMMA_DITHER_OFF) {
        sViNext->features &= ~VI_CTRL_GAMMA_DITHER_ON;
    }
    if (func & OS_VI_DIVOT_ON) {
        sViNext->features |= VI_CTRL_DIVOT_ON;
    }
    if (func & OS_VI_DIVOT_OFF) {
        sViNext->features &= ~VI_CTRL_DIVOT_ON;
    }
    if (func & OS_VI_DITHER_FILTER_ON) {
        sViNext->features |= VI_CTRL_DITHER_FILTER_ON;
        sViNext->features &= ~VI_CTRL_ANTIALIAS_MASK;
    }
    if (func & OS_VI_DITHER_FILTER_OFF) {
        sViNext->features &= ~VI_CTRL_DITHER_FILTER_ON;
        sViNext->features |= sViNext->modep->comRegs.ctrl & VI_CTRL_ANTIALIAS_MASK;
    }
    sViNext->state |= VI_STATE_CTRL_UPDATED;
    gc_os_unlock();
}

void osViSetXScale(f32 value) {
    u32 nomValue;

    gc_os_lock();
    sViNext->x.factor = value;
    sViNext->state |= VI_STATE_XSCALE_UPDATED;
    nomValue = sViNext->modep->comRegs.xScale & VI_SCALE_MASK;
    sViNext->x.scale = (u32)(sViNext->x.factor * nomValue) & VI_SCALE_MASK;
    gc_os_unlock();
}

void osViSetYScale(f32 value) {
    gc_os_lock();
    sViNext->y.factor = value;
    sViNext->state |= VI_STATE_YSCALE_UPDATED;
    gc_os_unlock();
}

void osViExtendVStart(u32 value) {
    sViAdditionalScanline = value;
}
