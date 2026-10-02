/**
 * Platform bring-up before the game starts: video and console, the bridge's locks and service
 * threads, storage (SD card, or the Dolphin dev disc), the log file, the ROM (opened, validated,
 * audio range preloaded), the GX renderer and a memory map. main() runs it all through
 * gc_ogc_boot(); the bridge test calls the steps itself.
 */
#include <gccore.h>
#include <ogc/libversion.h>
#include <stdio.h>
#include "gc_ogc.h"

/* Program image bounds from libogc's linker script (libogc_common.ld). Weak, so a custom link
 * script without them still links; they then read as NULL. */
extern unsigned char __bss_start[] __attribute__((weak));
extern unsigned char __bss_end[] __attribute__((weak));
extern unsigned char __Arena1Lo[] __attribute__((weak));

/* Game-visible memory must stay below this address (see gc_mem_alloc) */
#define GAME_MEMORY_LIMIT 0x81000000u

void gc_ogc_init(void) {
    gc_ogc_log_init();
    gc_ogc_sync_init();
    gc_ogc_pad_init();
    gc_ogc_rom_init();
    gc_ogc_save_init();
    gc_ogc_reset_init();
}

void gc_ogc_print_memory_map(void) {
    unsigned int imageEnd = (unsigned int)__bss_end;
    unsigned int arenaLo = (unsigned int)SYS_GetArena1Lo();
    unsigned int arenaHi = (unsigned int)SYS_GetArena1Hi();

    gc_log("Memory map (MEM1 80000000-81800000):");
    if (__bss_end != NULL) {
        gc_log("  program image  80003100-%08X (bss from %08X)", imageEnd, (unsigned int)__bss_start);
    }
    if (__Arena1Lo != NULL) {
        gc_log("  arena start    %08X (end of image)", (unsigned int)__Arena1Lo);
    }
    gc_log("  arena lo       %08X (XFB and malloc heap below)", arenaLo);
    gc_log("  arena hi       %08X (shim buffers above: %u KB up to %08X)", arenaHi, gc_ogc_mem_carved() / 1024,
           gc_ogc_mem_top());
    gc_log("  free MEM1      %u KB", (arenaHi - arenaLo) / 1024);
    if (imageEnd >= GAME_MEMORY_LIMIT) {
        gc_log("  WARNING: the program image ends above %08X", GAME_MEMORY_LIMIT);
    }
}

void gc_ogc_boot(void) {
    char romPath[64];
    const char* root;
    const char* error;

    gc_ogc_video_init();
    gc_ogc_init();
    gc_log("Majora's Mask for GameCube (M2 bring-up), built " __DATE__ " " __TIME__ ", libogc %d.%d.%d", _V_MAJOR_,
           _V_MINOR_, _V_PATCH_);
    gc_log("Video: %d Hz; USB Gecko in slot B: %s", gc_video_refresh_hz(), gc_ogc_log_gecko() ? "yes" : "no");

    root = gc_ogc_storage_mount();
    if (root == NULL) {
        gc_halt("No SD card found (tried SD2SP2, then SD Gecko in slots A and B). Put your ROM at "
                "SD:" GC_ROM_FILE ".");
    }
    if (gc_ogc_sd_mounted()) {
        gc_ogc_log_open_file(GC_LOG_PATH);
    } else {
        gc_log("No SD card: no log file, and saves are not kept");
    }

    snprintf(romPath, sizeof(romPath), "%s%s", root, GC_ROM_FILE);
    error = gc_ogc_rom_open(romPath);
    if (error != NULL) {
        gc_halt("%s", error);
    }
    if (gc_ogc_rom_preload(GC_ROM_RESIDENT_START, GC_ROM_RESIDENT_END) != 0) {
        gc_halt("Could not load the audio data from the ROM into RAM (see above).");
    }
    // After the ROM preload, so the renderer only takes memory that is really left; the console stays
    // the display until the renderer shows its first frame.
    gc_gfx_init();
    gc_ogc_print_memory_map();
}
