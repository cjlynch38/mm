/**
 * Storage for the ROM, the log file and saves.
 *
 * Devices:
 *  - sd:  the SD card, mounted through libfat: SD2SP2 in serial port 2 first, then an SD Gecko in
 *         memory card slot A or B. The log file and saves go there whenever it exists.
 *  - dvd: the disc in the DVD drive, mounted read-only with libiso9660. Both discs this port uses
 *         are GameCube discs that are also ISO9660 volumes: the bootable disc image built from the
 *         user's ROM by port/gc/tools/mkiso.py (`make -f Makefile.gc iso`), and the Dolphin dev disc
 *         (mkdevdisc.py, a copy of the SD folder; Dolphin 2609 cannot emulate a GameCube SD card).
 *  - img: the same disc image as a file on the SD card, when Swiss started it from there. Swiss
 *         emulates the DVD drive only for programs built with Nintendo's SDK (its patches find SDK
 *         functions by signature), never for libogc programs, so the image file is mounted with
 *         libiso9660 through a small DISC_INTERFACE that reads the file with libfat.
 *  - the memory card in slot A (else B): saves, when there is no SD card.
 *
 * Where the program came from decides where the ROM is looked for first (gc_ogc_storage_open_rom).
 * Loaders pass it in libogc's argv: the port's apploader passes "dvd:/" (booted from the disc in
 * the drive); Swiss passes the path of what it started ("sd:/.../mm-gc.iso", "carda:/...",
 * "dvd:/..." on an optical drive emulator, "sd:/.../mm-gc.dol"). Without argv (Dolphin -e, other
 * loaders) the program was a DOL.
 *   disc:  dvd:/mmgcport/baserom.z64, then the SD card
 *   image: img:/mmgcport/baserom.z64 inside the image file, then the SD card, then dvd:
 *   DOL:   the SD card, then dvd: (the dev disc in Dolphin, or the game disc)
 * where "the SD card" is sd:/mmgcport/baserom.z64, else the ROM inside a disc image at
 * sd:/mmgcport/mm-gc.iso (for loaders that do not pass the image's path, e.g. Swiss's BS2 boot).
 *
 * Reads from dvd: go through bridge_rom.c's direct DVD path (DVD_ReadPrio at the file's sector, which
 * libiso9660 reports as st_ino); everything else is plain stdio.
 * (The dev-disc reader follows port/gc/tests/sd_probe, written with the harness.)
 */
#include <gccore.h>
#include <gctypes.h>
#include <ogc/card.h>
#include <ogc/dvd.h>
#include <ogc/lwp_watchdog.h>
#include <errno.h>
#include <fat.h>
#include <iso9660.h>
#include <sdcard/gcsd.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "gc_ogc.h"

/* DVD_Mount never returns on a console without a drive, hence the async mount with a timeout */
#define DVD_MOUNT_TIMEOUT_MS 8000
#define DVD_BOUNCE_SIZE 0x8000
#define DISC_SECTOR_SIZE 0x800

/* Reasons for the halt message when no ROM is found */
#define WHY_SIZE 768

/* ---------------------------------------------------------------------------------------------- */
/* Boot path                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

enum {
    BOOT_DOL,   /* a DOL, or no argv */
    BOOT_DISC,  /* the disc in the DVD drive (our apploader, or Swiss on an optical drive emulator) */
    BOOT_IMAGE, /* a disc image file on an SD card (Swiss) */
};

const char* gc_ogc_boot_path(void) {
    if (__system_argv == NULL || __system_argv->argvMagic != ARGV_MAGIC || __system_argv->argc < 1 ||
        __system_argv->argv == NULL || __system_argv->argv[0] == NULL || __system_argv->argv[0][0] == '\0') {
        return NULL;
    }
    return __system_argv->argv[0];
}

static int ends_with(const char* s, const char* suffix) {
    size_t n = strlen(s);
    size_t m = strlen(suffix);

    return n >= m && strcasecmp(s + n - m, suffix) == 0;
}

static int boot_kind(void) {
    const char* path = gc_ogc_boot_path();

    if (path == NULL) {
        return BOOT_DOL;
    }
    if (strncmp(path, "dvd:", 4) == 0) {
        return BOOT_DISC;
    }
    if (ends_with(path, ".iso") || ends_with(path, ".gcm")) {
        return BOOT_IMAGE;
    }
    return BOOT_DOL;
}

/* ---------------------------------------------------------------------------------------------- */
/* SD card                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

static const struct {
    const char* name;
    const char* swissPrefix; /* how Swiss names the device in argv */
    const DISC_INTERFACE* iface;
} sSdDevices[] = {
    { "SD2SP2 (serial port 2)", "sd:", &__io_gcsd2 },
    { "SD Gecko in memory card slot A", "carda:", &__io_gcsda },
    { "SD Gecko in memory card slot B", "cardb:", &__io_gcsdb },
};
#define SD_DEVICES (int)(sizeof(sSdDevices) / sizeof(sSdDevices[0]))

static int sSdMounted;
static int sSdDevice = -1;

/* The SD device the boot path names, so a program started from an SD Gecko finds its card first */
static int sd_preferred_device(void) {
    const char* path = gc_ogc_boot_path();
    int i;

    if (path != NULL) {
        for (i = 0; i < SD_DEVICES; i++) {
            if (strncmp(path, sSdDevices[i].swissPrefix, strlen(sSdDevices[i].swissPrefix)) == 0) {
                return i;
            }
        }
    }
    return 0;
}

const char* gc_ogc_sd_mount(void) {
    int first = sd_preferred_device();
    int k;

    if (sSdMounted) {
        return sSdDevices[sSdDevice].name;
    }
    for (k = 0; k < SD_DEVICES; k++) {
        int i = (k == 0) ? first : (k <= first ? k - 1 : k);

        if (sSdDevices[i].iface == &__io_gcsdb && gc_ogc_log_gecko()) {
            // SD commands would reach the USB Gecko and show up as garbage in its log.
            gc_log("SD: slot B skipped (USB Gecko)");
            continue;
        }
        if (fatMountSimple("sd", sSdDevices[i].iface)) {
            sSdMounted = 1;
            sSdDevice = i;
            gc_log("SD: mounted the card in the %s as sd:", sSdDevices[i].name);
            return sSdDevices[i].name;
        }
        gc_log("SD: no card in the %s", sSdDevices[i].name);
    }
    return NULL;
}

int gc_ogc_sd_mounted(void) {
    return sSdMounted;
}

/* ---------------------------------------------------------------------------------------------- */
/* The disc in the DVD drive (dvd:)                                                               */
/* ---------------------------------------------------------------------------------------------- */

static int sDvdMounted;
static volatile int sDvdMountDone;
static volatile s32 sDvdMountResult;
static dvdcmdblk sDvdMountBlock;
static unsigned char* sDvdBounce;
static char sDvdWhy[128] = "not tried";

static void dvd_mount_callback(s32 result, dvdcmdblk* block) {
    sDvdMountResult = result;
    sDvdMountDone = 1;
}

static bool dvd_startup(void) {
    return sDvdMounted;
}

static bool dvd_is_inserted(void) {
    return sDvdMounted;
}

/* DI DMA needs 32-byte aligned buffers; libiso9660's own buffers may not be, so those reads go
 * through an aligned bounce buffer. libogc invalidates the data cache over DMA'd ranges. */
static bool dvd_read_sectors(sec_t sector, sec_t numSectors, void* buffer) {
    dvdcmdblk block;
    s64 offset = (s64)sector * DISC_SECTOR_SIZE;
    u32 size = numSectors * DISC_SECTOR_SIZE;
    unsigned char* dst = buffer;

    if (((u32)dst & 31) == 0) {
        return DVD_ReadPrio(&block, dst, size, offset, 2) > 0;
    }
    while (size != 0) {
        u32 n = (size < DVD_BOUNCE_SIZE) ? size : DVD_BOUNCE_SIZE;

        if (DVD_ReadPrio(&block, sDvdBounce, n, offset, 2) <= 0) {
            return false;
        }
        memcpy(dst, sDvdBounce, n);
        dst += n;
        offset += n;
        size -= n;
    }
    return true;
}

static bool read_only_write(sec_t sector, sec_t numSectors, const void* buffer) {
    return false;
}

static bool always_true(void) {
    return true;
}

static const DISC_INTERFACE sDvdDisc = {
    DEVICE_TYPE_GAMECUBE_DVD,
    FEATURE_MEDIUM_CANREAD | FEATURE_GAMECUBE_DVD,
    dvd_startup,
    dvd_is_inserted,
    dvd_read_sectors,
    read_only_write,
    always_true,
    always_true,
};

static void set_why(char* why, size_t size, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void set_why(char* why, size_t size, const char* fmt, ...) {
    va_list args;

    va_start(args, fmt);
    vsnprintf(why, size, fmt, args);
    va_end(args);
}

int gc_ogc_disc_mount(void) {
    u64 start = gettime();
    dvddiskid* id;

    if (sDvdMounted) {
        return 0;
    }
    if (sDvdBounce == NULL) {
        sDvdBounce = gc_mem_alloc(DVD_BOUNCE_SIZE, 32);
        if (sDvdBounce == NULL) {
            set_why(sDvdWhy, sizeof(sDvdWhy), "no memory");
            return -1;
        }
    }

    DVD_Init();
    sDvdMountDone = 0;
    if (!DVD_MountAsync(&sDvdMountBlock, dvd_mount_callback)) {
        set_why(sDvdWhy, sizeof(sDvdWhy), "the drive refused the mount request");
        gc_log("DVD: %s", sDvdWhy);
        return -1;
    }
    while (!sDvdMountDone && ticks_to_millisecs(gettime() - start) < DVD_MOUNT_TIMEOUT_MS) {
        usleep(10000);
    }
    if (!sDvdMountDone) {
        set_why(sDvdWhy, sizeof(sDvdWhy), "no drive answer after %u ms", DVD_MOUNT_TIMEOUT_MS);
        gc_log("DVD: %s", sDvdWhy);
        return -1;
    }
    if (sDvdMountResult < 0) {
        set_why(sDvdWhy, sizeof(sDvdWhy), "no disc (error %d, drive status %d)", (int)sDvdMountResult,
                (int)DVD_GetDriveStatus());
        gc_log("DVD: %s", sDvdWhy);
        return -1;
    }

    id = DVD_GetCurrentDiskID();
    sDvdMounted = 1;
    if (!ISO9660_Mount("dvd", &sDvdDisc)) {
        sDvdMounted = 0;
        set_why(sDvdWhy, sizeof(sDvdWhy), "disc %.4s%.2s is not one of this port's discs (no ISO9660 file system)",
                (const char*)id->gamename, (const char*)id->company);
        gc_log("DVD: %s", sDvdWhy);
        return -1;
    }
    gc_log("DVD: disc %.4s%.2s ('%s') mounted read-only as dvd: (%u ms)", (const char*)id->gamename,
           (const char*)id->company, ISO9660_GetVolumeLabel("dvd"),
           (unsigned int)ticks_to_millisecs(gettime() - start));
    return 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* A disc image file on the SD card (img:)                                                        */
/* ---------------------------------------------------------------------------------------------- */

static FILE* sImageFile;
static int sImageMounted;

static bool image_startup(void) {
    return sImageFile != NULL;
}

static bool image_is_inserted(void) {
    return sImageFile != NULL;
}

/* libiso9660 reads 16 sectors at a time; past the end of the file it gets zeros */
static bool image_read_sectors(sec_t sector, sec_t numSectors, void* buffer) {
    size_t size = (size_t)numSectors * DISC_SECTOR_SIZE;
    size_t got;

    if (sImageFile == NULL || fseek(sImageFile, (long)sector * DISC_SECTOR_SIZE, SEEK_SET) != 0) {
        return false;
    }
    got = fread(buffer, 1, size, sImageFile);
    if (got < size) {
        if (ferror(sImageFile)) {
            clearerr(sImageFile);
            return false;
        }
        memset((unsigned char*)buffer + got, 0, size - got);
    }
    return true;
}

static const DISC_INTERFACE sImageDisc = {
    ('I' << 24) | ('M' << 16) | ('G' << 8) | 'F',
    FEATURE_MEDIUM_CANREAD,
    image_startup,
    image_is_inserted,
    image_read_sectors,
    read_only_write,
    always_true,
    always_true,
};

static char sImageWhy[192] = "not tried";

int gc_ogc_image_mount(const char* path) {
    unsigned char header[0x20];

    if (sImageMounted) {
        return 0;
    }
    sImageFile = fopen(path, "rb");
    if (sImageFile == NULL) {
        set_why(sImageWhy, sizeof(sImageWhy), "cannot open %s (errno %d)", path, errno);
        return -1;
    }
    // Unbuffered: each 32 KB read of libiso9660 then reaches libfat as one read (one multi-block SD
    // transfer) instead of a series of stdio buffer refills.
    setvbuf(sImageFile, NULL, _IONBF, 0);
    if (fread(header, 1, sizeof(header), sImageFile) != sizeof(header) || header[0x1C] != 0xC2 ||
        header[0x1D] != 0x33 || header[0x1E] != 0x9F || header[0x1F] != 0x3D) {
        fclose(sImageFile);
        sImageFile = NULL;
        set_why(sImageWhy, sizeof(sImageWhy), "%s is not a GameCube disc image", path);
        return -1;
    }
    if (!ISO9660_Mount("img", &sImageDisc)) {
        fclose(sImageFile);
        sImageFile = NULL;
        set_why(sImageWhy, sizeof(sImageWhy),
                "%s (%.6s) has no ISO9660 file system: build it with make -f Makefile.gc iso", path,
                (const char*)header);
        return -1;
    }
    sImageMounted = 1;
    gc_log("Image: %s (%.6s, '%s') mounted read-only as img:", path, (const char*)header,
           ISO9660_GetVolumeLabel("img"));
    return 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* ROM                                                                                            */
/* ---------------------------------------------------------------------------------------------- */

static char sWhy[WHY_SIZE];

static void add_why(const char* source, const char* reason) {
    size_t len = strlen(sWhy);

    snprintf(sWhy + len, sizeof(sWhy) - len, "\n - %s: %s", source, reason);
}

/* Open <root>/mmgcport/baserom.z64 if it exists. 0 on success. */
static int try_rom(const char* root, const char* source) {
    char path[64];
    struct stat st;
    const char* error;

    snprintf(path, sizeof(path), "%s%s", root, GC_ROM_FILE);
    if (stat(path, &st) != 0) {
        char reason[96];

        snprintf(reason, sizeof(reason), "no %s", path);
        add_why(source, reason);
        gc_log("ROM: %s: %s", source, reason);
        return -1;
    }
    error = gc_ogc_rom_open(path);
    if (error != NULL) {
        add_why(source, error);
        gc_log("ROM: %s", error);
        return -1;
    }
    gc_log("ROM: using %s (%s)", path, source);
    return 0;
}

/* sd:/mmgcport/baserom.z64, else the disc image at its default place on the card */
static int try_sd(void) {
    struct stat st;

    if (!sSdMounted) {
        add_why("SD card", "none found (SD2SP2, SD Gecko in slot A or B)");
        return -1;
    }
    if (try_rom("sd:", "SD card") == 0) {
        return 0;
    }
    if (sImageMounted || stat(GC_SD_IMAGE_PATH, &st) != 0) {
        return -1;
    }
    if (gc_ogc_image_mount(GC_SD_IMAGE_PATH) != 0) {
        add_why("disc image", sImageWhy);
        gc_log("Image: %s", sImageWhy);
        return -1;
    }
    return try_rom("img:", "disc image on the SD card");
}

static int try_disc(void) {
    if (gc_ogc_disc_mount() != 0) {
        add_why("DVD drive", sDvdWhy);
        return -1;
    }
    return try_rom("dvd:", "disc in the DVD drive");
}

/* The disc image Swiss started: it names SD adapters sd:, carda: and cardb:, and the card is
 * mounted here as sd: whichever adapter holds it */
static int try_image(void) {
    const char* bootPath = gc_ogc_boot_path();
    const char* colon = strchr(bootPath, ':');
    char path[256];

    if (colon == NULL || colon[1] != '/') {
        add_why("disc image", "the boot path is not a file path");
        return -1;
    }
    if (!sSdMounted) {
        add_why("disc image", "no SD card is mounted");
        return -1;
    }
    snprintf(path, sizeof(path), "sd:%s", colon + 1);
    if (gc_ogc_image_mount(path) != 0) {
        add_why("disc image", sImageWhy);
        gc_log("Image: %s", sImageWhy);
        return -1;
    }
    return try_rom("img:", "disc image on the SD card");
}

const char* gc_ogc_storage_open_rom(void) {
    static char sMessage[WHY_SIZE + 256];
    const char* path = gc_ogc_boot_path();
    int kind = boot_kind();

    sWhy[0] = '\0';
    gc_log("Boot: %s%s", (path != NULL) ? path : "a DOL (no argv)",
           (kind == BOOT_DISC)    ? ", the disc in the DVD drive"
           : (kind == BOOT_IMAGE) ? ", a disc image file"
                                  : "");
    switch (kind) {
        case BOOT_DISC:
            if (try_disc() == 0 || try_sd() == 0) {
                return NULL;
            }
            break;
        case BOOT_IMAGE:
            if (try_image() == 0 || try_sd() == 0 || try_disc() == 0) {
                return NULL;
            }
            break;
        default:
            if (try_sd() == 0 || try_disc() == 0) {
                return NULL;
            }
            break;
    }
    snprintf(sMessage, sizeof(sMessage),
             "No usable Majora's Mask ROM was found.%s\n\nPut your ROM (USA 1.0, .z64) at SD:" GC_ROM_FILE
             ", or boot the disc image built with make -f Makefile.gc iso.",
             sWhy);
    return sMessage;
}

/* The bridge test's entry point: the SD card, else the disc in the drive. "sd:", "dvd:" or NULL. */
const char* gc_ogc_storage_mount(void) {
    if (sSdMounted || gc_ogc_sd_mount() != NULL) {
        return "sd:";
    }
    if (sDvdMounted || gc_ogc_disc_mount() == 0) {
        return "dvd:";
    }
    return NULL;
}

/* ---------------------------------------------------------------------------------------------- */
/* Memory card saves                                                                              */
/* ---------------------------------------------------------------------------------------------- */

/*
 * One file on the memory card holds the 128 KB flash image: a header sector (the comment the IPL's
 * memory card screen shows, and a tag), then the image, one card sector (8 KB) per 8 KB of flash.
 * MM keeps every save twice in flash (main and backup copies in separate 8 KB-aligned areas) and
 * checks both with checksums. A store writes only the sectors that changed since the card was last
 * read or written, in order, so a power cut during a store damages at most the copy being written
 * and the game falls back to the other one. The card is never formatted and no other file is
 * touched; without room for the file the save is not kept (logged).
 */
#define CARD_TAG "MMGCPORT FLASH 1"
#define CARD_COMMENT_TITLE "Majora's Mask (GC port)"
#define CARD_COMMENT_TEXT "Flash save, 128 KB"
/* Largest card sector handled (official cards use 8 KB) */
#define CARD_SECTOR_MAX 0x4000

static int sCardInited;
static int sCardChannel = -1;
static volatile int sCardDetached;
static u32 sCardSectorSize;
static unsigned char* sCardWorkArea;
static unsigned char* sCardSector;
static unsigned char* sCardShadow; /* what the card's data sectors hold */
static unsigned int sCardShadowSize;
static int sCardShadowValid;
static int sCardHeaderOk; /* the file's header sector has the comment and the tag */

static void card_detach_callback(s32 chn, s32 result) {
    sCardDetached = 1;
}

static const char* card_slot_name(int chn) {
    return (chn == CARD_SLOTA) ? "slot A" : "slot B";
}

static int card_mount_channel(int chn) {
    s32 ret;
    u32 blocks = 0;
    u16 freeBlocks = 0;

    ret = CARD_Mount(chn, sCardWorkArea, card_detach_callback);
    if (ret < 0 && ret != CARD_ERROR_ENCODING) {
        if (ret != CARD_ERROR_NOCARD) {
            gc_log("save: memory card in %s: mount error %d", card_slot_name(chn), (int)ret);
        }
        return -1;
    }
    if (CARD_GetSectorSize(chn, &sCardSectorSize) < 0 || sCardSectorSize < CARD_READSIZE ||
        sCardSectorSize > CARD_SECTOR_MAX) {
        gc_log("save: memory card in %s: unsupported sector size %u", card_slot_name(chn),
               (unsigned int)sCardSectorSize);
        CARD_Unmount(chn);
        return -1;
    }
    CARD_GetBlockCount(chn, &blocks);
    CARD_GetFreeBlocks(chn, &freeBlocks);
    sCardChannel = chn;
    sCardDetached = 0;
    sCardShadowValid = 0;
    sCardHeaderOk = 0;
    gc_log("save: memory card in %s: %u blocks of %u KB, %u free; saves go to its file '%s' (%u blocks)",
           card_slot_name(chn), (unsigned int)blocks, (unsigned int)(sCardSectorSize / 1024), (unsigned int)freeBlocks,
           GC_CARD_FILE_NAME, (unsigned int)(1 + (0x20000 + sCardSectorSize - 1) / sCardSectorSize));
    return 0;
}

int gc_ogc_card_mount(void) {
    int chn;

    if (sCardChannel >= 0) {
        return 0;
    }
    if (sCardWorkArea == NULL) {
        sCardWorkArea = gc_mem_alloc(CARD_WORKAREA_SIZE, 32);
        sCardSector = gc_mem_alloc(CARD_SECTOR_MAX, 32);
        if (sCardWorkArea == NULL || sCardSector == NULL) {
            gc_log("save: no memory for the memory card");
            return -1;
        }
    }
    if (!sCardInited) {
        CARD_Init(GC_CARD_GAME_CODE, GC_CARD_COMPANY);
        sCardInited = 1;
    }
    for (chn = CARD_SLOTA; chn <= CARD_SLOTB; chn++) {
        if (chn == CARD_SLOTB && gc_ogc_log_gecko()) {
            continue; // the USB Gecko
        }
        if (card_mount_channel(chn) == 0) {
            return 0;
        }
    }
    gc_log("save: no memory card in slot A or B");
    return -1;
}

int gc_ogc_card_mounted(void) {
    return sCardChannel >= 0;
}

/* A card was pulled since the last access: mount whatever is in the slot now */
static int card_check(void) {
    int chn = sCardChannel;

    if (chn < 0) {
        return -1;
    }
    if (!sCardDetached) {
        return 0;
    }
    gc_log("save: the memory card in %s was removed; mounting it again", card_slot_name(chn));
    CARD_Unmount(chn);
    sCardChannel = -1;
    return card_mount_channel(chn);
}

static int card_shadow_alloc(unsigned int size) {
    if (sCardShadow == NULL) {
        sCardShadow = gc_mem_alloc(size, 32);
        sCardShadowSize = size;
    }
    return (sCardShadow != NULL && sCardShadowSize >= size) ? 0 : -1;
}

int gc_ogc_card_save_load(void* dst, unsigned int size) {
    card_file file;
    unsigned int s = sCardSectorSize;
    unsigned int offset;
    unsigned int dataSize;
    s32 ret;

    if (card_check() != 0) {
        gc_log("save: no memory card, starting without a save");
        return -1;
    }
    ret = CARD_Open(sCardChannel, GC_CARD_FILE_NAME, &file);
    if (ret == CARD_ERROR_NOFILE) {
        gc_log("save: no save on the memory card in %s", card_slot_name(sCardChannel));
        sCardShadowValid = 0;
        return 1;
    }
    if (ret < 0) {
        gc_log("save: cannot open the save on the memory card (error %d)", (int)ret);
        return -1;
    }

    dataSize = ((unsigned int)file.len > s) ? (unsigned int)file.len - s : 0;
    ret = CARD_Read(&file, sCardSector, s, 0);
    if (ret >= 0) {
        sCardHeaderOk = (memcmp(sCardSector + 64, CARD_TAG, sizeof(CARD_TAG) - 1) == 0);
        if (!sCardHeaderOk) {
            gc_log("save: the save file on the memory card has no '" CARD_TAG "' tag; reading it anyway");
        }
    }
    for (offset = 0; ret >= 0 && offset < size; offset += s) {
        unsigned int n = (size - offset < s) ? size - offset : s;

        if (offset >= dataSize) {
            memset((unsigned char*)dst + offset, 0xFF, n); // missing bytes read as erased flash
            continue;
        }
        ret = CARD_Read(&file, sCardSector, s, s + offset);
        if (ret >= 0) {
            memcpy((unsigned char*)dst + offset, sCardSector, n);
        }
    }
    CARD_Close(&file);
    if (ret < 0) {
        gc_log("save: reading the memory card failed (error %d)", (int)ret);
        sCardShadowValid = 0;
        return -1;
    }
    if (card_shadow_alloc(size) == 0) {
        memcpy(sCardShadow, dst, size);
        sCardShadowValid = (dataSize >= size);
    }
    gc_log("save: loaded %u bytes from the memory card in %s", size, card_slot_name(sCardChannel));
    return 0;
}

int gc_ogc_card_save_store(const void* src, unsigned int size) {
    u64 t0 = gettime();
    card_file file;
    card_stat status;
    unsigned int s;
    unsigned int dataSectors;
    unsigned int offset;
    unsigned int written = 0;
    int created = 0;
    s32 ret;

    if (card_check() != 0) {
        return -1;
    }
    s = sCardSectorSize;
    dataSectors = (size + s - 1) / s;

    ret = CARD_Open(sCardChannel, GC_CARD_FILE_NAME, &file);
    if (ret == CARD_ERROR_NOFILE) {
        ret = CARD_Create(sCardChannel, GC_CARD_FILE_NAME, (1 + dataSectors) * s, &file);
        if (ret < 0) {
            gc_log("save: cannot create the save on the memory card in %s (error %d%s); the save is not kept",
                   card_slot_name(sCardChannel), (int)ret,
                   (ret == CARD_ERROR_INSSPACE || ret == CARD_ERROR_NOENT) ? ": not enough free blocks" : "");
            return -1;
        }
        created = 1;
        sCardShadowValid = 0;
        // CARD_Create leaves the handle's length at 0: open the new file like an existing one
        CARD_Close(&file);
        ret = CARD_Open(sCardChannel, GC_CARD_FILE_NAME, &file);
        if (ret < 0) {
            gc_log("save: cannot open the new save file on the memory card (error %d)", (int)ret);
            return -1;
        }
    } else if (ret < 0) {
        gc_log("save: cannot open the save on the memory card (error %d)", (int)ret);
        return -1;
    }
    if ((unsigned int)file.len < (1 + dataSectors) * s) {
        CARD_Close(&file);
        gc_log("save: the save file on the memory card is %d bytes, too small; the save is not kept", (int)file.len);
        return -1;
    }

    // A new file (or one whose header a failed store left out) gets its header sector first
    if (created || !sCardHeaderOk) {
        memset(sCardSector, 0, s);
        memcpy(sCardSector, CARD_COMMENT_TITLE, sizeof(CARD_COMMENT_TITLE) - 1);
        memcpy(sCardSector + 32, CARD_COMMENT_TEXT, sizeof(CARD_COMMENT_TEXT) - 1);
        memcpy(sCardSector + 64, CARD_TAG, sizeof(CARD_TAG) - 1);
        memcpy(sCardSector + 80, &size, sizeof(size));
        ret = CARD_Write(&file, sCardSector, s, 0);
        if (ret >= 0) {
            ret = CARD_GetStatus(sCardChannel, file.filenum, &status);
        }
        if (ret >= 0) {
            status.banner_fmt = CARD_BANNER_NONE;
            status.icon_addr = (u32)-1;
            status.icon_fmt = 0;
            status.icon_speed = 0;
            status.comment_addr = 0;
            ret = CARD_SetStatus(sCardChannel, file.filenum, &status);
        }
        sCardHeaderOk = (ret >= 0);
    }
    for (offset = 0; ret >= 0 && offset < size; offset += s) {
        unsigned int n = (size - offset < s) ? size - offset : s;

        if (sCardShadowValid && memcmp(sCardShadow + offset, (const unsigned char*)src + offset, n) == 0) {
            continue;
        }
        memcpy(sCardSector, (const unsigned char*)src + offset, n);
        memset(sCardSector + n, 0xFF, s - n);
        ret = CARD_Write(&file, sCardSector, s, s + offset);
        written++;
    }
    CARD_Close(&file);
    if (ret < 0) {
        sCardShadowValid = 0;
        gc_log("save: writing the memory card in %s failed (error %d)", card_slot_name(sCardChannel), (int)ret);
        return -1;
    }
    if (card_shadow_alloc(size) == 0) {
        memcpy(sCardShadow, src, size);
        sCardShadowValid = 1;
    }
    gc_log("save: wrote %u of %u sectors to the memory card in %s%s (%u ms)", written, dataSectors,
           card_slot_name(sCardChannel), created ? ", new file" : "",
           (unsigned int)ticks_to_millisecs(gettime() - t0));
    return 0;
}
