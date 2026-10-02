/**
 * libultra Audio Interface API (osAiSetFrequency, osAiGetLength, osAiSetNextBuffer) with audio
 * output stubbed (Milestone 2; the game runs the audio manager at R_AUDIOMGR_DEBUG_LEVEL 1).
 *
 * Replaces both src/libultra/io/ai*.c and the game's own src/audio/lib/aisetnextbuf.c.
 */
#include "gc_ultra_internal.h"

/**
 * On N64 the result is the rate the DAC really runs at (VI clock / divider, 32006 Hz for a
 * 32000 Hz request on NTSC). The GameCube AI runs at exactly 32000 Hz, which is what M4 will
 * output, so the requested rate is returned unchanged.
 */
s32 osAiSetFrequency(u32 frequency) {
    return frequency;
}

/** Bytes left in the current AI DMA: no buffer is ever playing. */
u32 osAiGetLength(void) {
    return 0;
}

/** Accept and drop the buffer (0 = queued; -1 would mean the AI FIFO is full). */
s32 osAiSetNextBuffer(void* buf, u32 size) {
    return 0;
}
