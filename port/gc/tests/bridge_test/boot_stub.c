/**
 * boot_test: runs the real platform entry point (port/gc/ogc/main.c) with this stand-in for the
 * libultra side's gc_ultra_boot. It checks the boot sequence (storage, ROM validation and
 * preload, memory map) and that main() parks below the game: the heartbeat thread runs at the
 * lowest game priority, so it only gets to run once main has stepped aside.
 */
#include <gccore.h>
#include <unistd.h>
#include "gc_ogc.h"

static void heartbeat_entry(void* arg) {
    int i;

    for (i = 0; i < 5; i++) {
        gc_log("boot_test: heartbeat %d at priority %d", i, GC_PRIO_GAME_MIN);
        usleep(200000);
    }
    gc_log("boot_test: RESULT PASS");
}

void gc_ultra_boot(void) {
    gc_thread_t t;

    gc_log("boot_test: stand-in gc_ultra_boot");
    gc_thread_create(&t, heartbeat_entry, NULL, 0x4000, GC_PRIO_GAME_MIN);
}
