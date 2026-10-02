/* Host stand-in for libogc's <ogc/lwp_watchdog.h> (gfx_fb host test): a timebase that advances on every read */
#ifndef GFX_FB_HOST_LWP_WATCHDOG_H
#define GFX_FB_HOST_LWP_WATCHDOG_H

#include <gccore.h>

u64 gettime(void);

#endif
