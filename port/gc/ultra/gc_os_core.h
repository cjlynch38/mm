/**
 * Private declarations shared by the libultra OS core of the GameCube shim (thread.c, mesg.c,
 * event.c, intmask.c, time.c, timer.c, globals.c, boot.c in port/gc/ultra/). Other shim files use
 * gc_ultra_internal.h only.
 */
#ifndef GC_OS_CORE_H
#define GC_OS_CORE_H

#include "gc_ultra_internal.h"
#include "stdbool.h"

/* ------------------------------------------------------------------------------------------ */
/* thread.c                                                                                     */
/* ------------------------------------------------------------------------------------------ */

/**
 * Block in gc_os_wait() on behalf of the calling thread. The caller holds the OS lock and must
 * re-check its wait condition afterwards (wakeups can be spurious). This is also the point where
 * a game thread honours osStopThread()/osDestroyThread() requests made by other threads: a stopped
 * thread stays here until osStartThread(), a destroyed one exits here and never returns.
 */
void __gcOsWaitLocked(void);

/** Called with the lock held at the start of every message-queue operation by a game thread:
 *  honours a pending osStopThread()/osDestroyThread() request for the calling thread. Cheap when
 *  no request is pending. */
void __gcOsCheckpointLocked(void);

/** osSetIntMask() state of one thread: the libultra mask and the gc_irq_disable() level saved
 *  when the mask went from enabled to OS_IM_NONE. */
typedef struct GcIntMaskState {
    OSIntMask mask;
    unsigned int irqLevel;
} GcIntMaskState;

/** The calling thread's osSetIntMask() state (shared fallback for non-game threads). */
GcIntMaskState* __gcGetIntMaskState(void);

/* ------------------------------------------------------------------------------------------ */
/* time.c                                                                                       */
/* ------------------------------------------------------------------------------------------ */

/** Make osGetTime() count from now (N64: the count register starts at boot). */
void __gcTimeInit(void);

/*
 * The N64 CPU counter runs at 46.875 MHz, the GameCube timebase at 40.5 MHz:
 * 46875000 / 40500000 = 125 / 108.
 */
#define GC_N64_CYCLES_PER_TICK_NUM 125
#define GC_N64_CYCLES_PER_TICK_DEN 108

/** N64 CPU cycles (OSTime) to GameCube timebase ticks, rounded up so timers never fire early. */
static inline u64 __gcCyclesToTicks(u64 cycles) {
    // Absurdly long durations (hundreds of years) would overflow the multiplication.
    if (cycles >= (0xFFFFFFFFFFFFFFFFULL / GC_N64_CYCLES_PER_TICK_DEN)) {
        return cycles;
    }
    return (cycles * GC_N64_CYCLES_PER_TICK_DEN + (GC_N64_CYCLES_PER_TICK_NUM - 1)) / GC_N64_CYCLES_PER_TICK_NUM;
}

/* ------------------------------------------------------------------------------------------ */
/* event.c                                                                                      */
/* ------------------------------------------------------------------------------------------ */

/** Reset button pressed: start the N64 PRE-NMI sequence (post OS_EVENT_PRENMI once). Registered
 *  with gc_set_reset_callback() by gc_ultra_boot(). Runs on a normal thread. */
void __gcOsResetPressed(void);

#endif
