/**
 * Internal interface of the libogc half of the platform layer (port/gc/ogc/).
 * Compiled with libogc headers only, never with the decomp's. main.c and the stand-alone bridge
 * test (port/gc/tests/bridge_test) use these functions to bring the platform up step by step;
 * the game side only sees gc_bridge.h.
 */
#ifndef GC_OGC_H
#define GC_OGC_H

#include "gc_bridge.h"

/* Files. The ROM is <root>/mmgcport/baserom.z64 on the SD card (sd:), on the disc in the drive
 * (dvd:: the disc image built by mkiso.py, or the Dolphin dev disc) or inside a disc image file
 * on the SD card (img:), see gc_ogc_storage_open_rom(). The log file and saves need the SD card;
 * without one, saves go to the memory card. */
#define GC_DIR "/mmgcport"
#define GC_ROM_FILE GC_DIR "/baserom.z64"
#define GC_SD_DIR "sd:" GC_DIR
#define GC_LOG_PATH GC_SD_DIR "/log.txt"
#define GC_SAVE_PATH GC_SD_DIR "/mm.fla"
/* Where a disc image on the SD card is looked for when the loader did not name one */
#define GC_SD_IMAGE_PATH GC_SD_DIR "/mm-gc.iso"

/* The disc image's game ID (port/gc/tools/mkiso.py); its first four letters and maker code also
 * own the memory card save file */
#define GC_CARD_GAME_CODE "GMME"
#define GC_CARD_COMPANY "00"
#define GC_CARD_FILE_NAME "mmgcport_flash"

/* The user's ROM: the compressed US 1.0 cartridge image in .z64 (big-endian) byte order */
#define GC_ROM_SIZE 0x2000000u
#define GC_ROM_MAGIC 0x80371240u
#define GC_ROM_CRC1 0x5354631Cu
#define GC_ROM_CRC2 0x03A2DEF0u

/* ROM range the audio code streams from every frame (Audiobank, Audioseq, Audiotable).
 * Preloaded into ARAM so those reads never touch the SD card. */
#define GC_ROM_RESIDENT_START 0x20700
#define GC_ROM_RESIDENT_END 0x5E06E0

/* The hot range, also preloaded into ARAM: link_animetion (read every gameplay frame), the item, map
 * and schedule statics, the yar archives (CmpDma), do_action/message/font statics and the message
 * data, ending with staff_message_data_static. The kanji font between the two ranges is not read by
 * the US game. */
#define GC_ROM_HOT_START 0x65C9E0
#define GC_ROM_HOT_END 0xA684D0

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
/** argv[0] as the loader passed it ("dvd:/" from the port's apploader; Swiss passes the path of
 *  the DOL or disc image it started), or NULL. */
const char* gc_ogc_boot_path(void);
/** Mount the disc in the DVD drive (the disc image or the Dolphin dev disc, both ISO9660) as "dvd:".
 *  Returns 0 on success. */
int gc_ogc_disc_mount(void);
/** Mount a disc image file (built by mkiso.py; e.g. "sd:/mmgcport/mm-gc.iso") read-only as "img:".
 *  Returns 0 on success. */
int gc_ogc_image_mount(const char* path);
/** Find and open the ROM: the source the program was booted from first (the disc, a disc image
 *  on SD, else the SD card), then the others. Call after gc_ogc_sd_mount(). Returns NULL on
 *  success, else a message for the user listing what was tried. */
const char* gc_ogc_storage_open_rom(void);
/** SD card if there is one, else the disc in the drive. Returns "sd:", "dvd:" or NULL (bridge test). */
const char* gc_ogc_storage_mount(void);
/** Mount the memory card in slot A, else slot B (not with a USB Gecko there), for saves without an
 *  SD card. 0 on success. */
int gc_ogc_card_mount(void);
int gc_ogc_card_mounted(void);
/** gc_save_load/gc_save_store on the memory card (same return values). */
int gc_ogc_card_save_load(void* dst, unsigned int size);
int gc_ogc_card_save_store(const void* src, unsigned int size);

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
/** Nonzero while the text console is on screen: before the renderer's first frame and after gc_halt. */
int gc_ogc_video_console_visible(void);

/* bridge_log.c */
/** Set up logging; also detects a USB Gecko in slot B, which then gets a copy of every line. */
void gc_ogc_log_init(void);
int gc_ogc_log_gecko(void);
/** Create (truncate) the log file on SD and start the log writer thread, which writes the lines logged so far and
 *  then every few seconds what was logged since. */
void gc_ogc_log_open_file(const char* path);
/** Write every line logged so far to the log file now (waits for the SD card). */
void gc_ogc_log_flush(void);
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
/** GC_ROM_CHECK builds: read the whole ROM back through gc_rom_read and log every 64 KB that differs from the
 *  build machine's ROM. */
void gc_ogc_rom_check(void);
void gc_ogc_rom_init(void);
/** Open and validate the ROM. Returns NULL on success, or a message for the user. */
const char* gc_ogc_rom_open(const char* path);
/** Copy [start, end) of the ROM into ARAM (a resident range, up to 4); gc_rom_read() then never reads
 *  it from the disc again. The first preload takes all free ARAM for the ROM, and its range gets a
 *  lock of its own (preload the audio data first: the audio thread then never waits for another
 *  thread's read); the later ranges share one. 0 on success. */
int gc_ogc_rom_preload(unsigned int start, unsigned int end);
/** Turn the ARAM left after the preloads into the file cache (whole dmadata files, LRU). 0 on success. */
int gc_ogc_rom_cache_init(void);
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
