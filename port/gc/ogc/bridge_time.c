/**
 * gc_time_ticks: the 64-bit PowerPC timebase, which runs at a quarter of the bus clock
 * (162 MHz / 4 = 40.5 MHz on GameCube). The libultra side converts it to N64 46.875 MHz cycles.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include "gc_ogc.h"

/* libogc's own definition of the timer clock (in kHz) must agree with the bridge's GC_TB_HZ */
_Static_assert(TB_TIMER_CLOCK == 40500, "unexpected GameCube timebase clock");
_Static_assert(TB_TIMER_CLOCK * 1000ull == GC_TB_HZ, "GC_TB_HZ disagrees with libogc");

unsigned long long gc_time_ticks(void) {
    return gettime();
}
