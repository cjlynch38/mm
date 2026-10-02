/**
 * Internal interface of the libogc half of the platform layer (port/gc/ogc/).
 * Compiled with libogc headers only, never with the decomp's. main.c and the stand-alone bridge
 * test (port/gc/tests/bridge_test) use these functions to bring the platform up step by step;
 * the game side only sees gc_bridge.h.
 */
#ifndef GC_OGC_H
#define GC_OGC_H

#include "gc_bridge.h"

/* Files. The ROM is read from the storage root that gc_ogc_storage_mount() found ("sd:" on
 * hardware, "dvd:" for the Dolphin dev disc); the log file and saves need a writable SD card. */
#define GC_DIR "/mmgcport"
#define GC_ROM_FILE GC_DIR "/baserom.z64"
#define GC_SD_DIR "sd:" GC_DIR
#define GC_LOG_PATH GC_SD_DIR "/log.txt"
#define GC_SAVE_PATH GC_SD_DIR "/mm.fla"

/* The user's ROM: the compressed US 1.0 cartridge image in .z64 (big-endian) byte order */
#define GC_ROM_SIZE 0x2000000u
#define GC_ROM_MAGIC 0x80371240u
#define GC_ROM_CRC1 0x5354631Cu
#define GC_ROM_CRC2 0x03A2DEF0u

/* ROM range the audio code streams from every frame (Audiobank, Audioseq, Audiotable).
 * Preloaded into RAM so those reads never touch the SD card. */
#define GC_ROM_RESIDENT_START 0x20700
#define GC_ROM_RESIDENT_END 0x5E06E0

/* Priority of the bridge's reset callback thread; its press thread runs one above (LWP, 127 = highest) */
#define GC_PRIO_SERVICE_RESET 120

/* platform.c: boot sequence used by main() */

/** Video, console, bridge init, storage, log file, ROM open + validate + preload, memory map.
 *  Halts with an on-screen message if the storage or the ROM is missing or wrong. */
void gc_ogc_boot(void);
/** Create the bridge's locks and service threads. Call once, after gc_ogc_video_init(). */
void gc_ogc_init(void);
void gc_ogc_print_memory_map(void);

/* storage.c */

/** Mount the SD card as "sd:" (SD2SP2, then SD Gecko in slot A, then slot B).
 *  Returns a description of the device that worked, or NULL. */
const char* gc_ogc_sd_mount(void);
/** Nonzero once an SD card is mounted as sd: (the log file and saves need it). */
int gc_ogc_sd_mounted(void);
/** Mount the Dolphin dev disc (an ISO9660 image of the SD folder in the DVD drive) as "dvd:".
 *  Returns 0 on success. */
int gc_ogc_devdisc_mount(void);
/** SD card if there is one, else the Dolphin dev disc. Returns "sd:", "dvd:" or NULL. */
const char* gc_ogc_storage_mount(void);

/* bridge_video.c */
void gc_ogc_video_init(void);
/** Show the console framebuffer, unblanked (gc_halt uses it so the halt message is visible). After this,
 *  gc_ogc_video_show_frame() no longer changes the display. */
void gc_ogc_video_show_console(void);
/** The video mode picked by gc_ogc_video_init() (a GXRModeObj; NULL before). The renderer sizes its EFB
 *  copies and XFBs from it. */
struct _gx_rmodeobj* gc_ogc_video_mode(void);
/** Renderer (VI service thread): show XFB `xfb` from the next retrace on. The first call hands the
 *  display from the console to the renderer, which also makes osViBlack effective. Returns 0 without
 *  changing anything once gc_halt has brought the console back. */
int gc_ogc_video_show_frame(void* xfb);

/* bridge_log.c */
/** Set up logging; also detects a USB Gecko in slot B, which then gets a copy of every line. */
void gc_ogc_log_init(void);
int gc_ogc_log_gecko(void);
/** Create (truncate) the log file on SD and write the lines logged so far into it. */
void gc_ogc_log_open_file(const char* path);
/** Nonzero once gc_halt() has been called. */
int gc_ogc_halted(void);

/* bridge_thread.c */
/** Suspend every thread created with gc_thread_create() except the caller (used by gc_halt). */
void gc_ogc_stop_threads(void);
unsigned int gc_ogc_thread_count(void);

/* bridge_sync.c */
void gc_ogc_sync_init(void);

/* bridge_mem.c */
/** Bytes carved from the top of MEM1 so far, and the arena top before the first carve. */
unsigned int gc_ogc_mem_carved(void);
unsigned int gc_ogc_mem_top(void);

/* bridge_rom.c */
void gc_ogc_rom_init(void);
/** Open and validate the ROM. Returns NULL on success, or a message for the user. */
const char* gc_ogc_rom_open(const char* path);
/** Read [start, end) of the ROM into RAM; gc_rom_read() then serves it by memcpy. 0 on success. */
int gc_ogc_rom_preload(unsigned int start, unsigned int end);
void gc_ogc_rom_print_stats(void);

/* bridge_save.c */
void gc_ogc_save_init(void);
/** Redirect gc_save_load/store to another file (the bridge test uses this). */
void gc_ogc_save_set_path(const char* path);

/* bridge_pad.c */
void gc_ogc_pad_init(void);

/* bridge_reset.c */
void gc_ogc_reset_init(void);

#endif
