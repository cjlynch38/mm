/**
 * GameCube entry point. Brings up the platform (console, SD card, ROM), starts the game through
 * the libultra shim, then parks the main thread forever. On N64 the boot code never returns
 * either; all further work happens in the game's own threads.
 */
#include <gccore.h>
#include "gc_ogc.h"

int main(void) {
    gc_ogc_boot();

    gc_log("Starting the game");
    gc_ultra_boot();
    gc_log("main: game threads started (%u), parking the main thread", gc_ogc_thread_count());

    // Lowest priority, then suspended: the main thread never runs again and costs no CPU.
    LWP_SetThreadPriority(LWP_GetSelf(), LWP_PRIO_IDLE);
    for (;;) {
        LWP_SuspendThread(LWP_GetSelf());
    }
}
