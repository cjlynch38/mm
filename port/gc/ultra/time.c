/**
 * libultra time: osGetTime and osGetCount in N64 CPU counter units (46.875 MHz), derived from the
 * GameCube timebase (40.5 MHz). OSTime values are stored in save files and converted with
 * OS_CYCLES_TO_USEC, so the N64 rate must be kept.
 */
#include "gc_os_core.h"

// Timebase value that osGetTime() reports as 0
static u64 sTimeBase;

void __gcTimeInit(void) {
    sTimeBase = gc_time_ticks();
}

OSTime osGetTime(void) {
    u64 ticks = gc_time_ticks() - sTimeBase;

    return ticks * GC_N64_CYCLES_PER_TICK_NUM / GC_N64_CYCLES_PER_TICK_DEN;
}

u32 osGetCount(void) {
    return (u32)osGetTime();
}
