/**
 * gc_ultra_boot(): libultra start-up on the GameCube, then the game's own bootproc().
 *
 * On the N64, bootproc() (src/boot/boot_main.c) runs before any thread exists and ends with
 * osStartThread(&sIdleThread), which never returns. Here it runs on the caller's thread (libogc's
 * main thread, LWP priority 64, above every game thread except Fault), so the Idle thread only gets
 * the CPU once the caller blocks or lowers its priority, and osStartThread() returns.
 * bootproc() has nothing after that call, so returning is harmless.
 */
#include "gc_os_core.h"

void bootproc(void);

void gc_ultra_boot(void) {
    gc_log("ultra: boot");

    __gcTimeInit();
    __gcTimerInit();
    __gcViInit();
    gc_set_reset_callback(__gcOsResetPressed);

    gc_log("ultra: bootproc()");
    bootproc();
    gc_log("ultra: bootproc() returned, Idle thread started");
}
