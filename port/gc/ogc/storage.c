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
 * libiso9660 reports as st_ino). A ROM on the SD card (baserom.z64, or the disc image file) is read with raw sector
 * reads (gc_ogc_sd_raw_*, below), never through libfat, so that the game never waits for a libfat operation (the log
 * writer's or a save's). Everything else is plain stdio.
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
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fat_map.h"
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

/*
 * Every SD card command, libfat's and the raw reads' (below), goes through sSdLocked: it forwards to the adapter's
 * DISC_INTERFACE under sSdMutex, at most SD_LOCK_SECTORS sectors per hold. libogc's SD driver keeps its per-slot
 * state in globals and is not reentrant, so this lock is what lets raw reads run beside libfat. libfat itself holds
 * its volume lock for whole file operations, and finding a free cluster (for a new file, or a file that grows) can
 * keep one going for tens of seconds on a large, full card (FatFs reads the FAT from where it last allocated, or
 * from a truncated file's old first cluster); the raw reads never take that lock, only this one, which libfat
 * releases after each command. libogc 3's mutexes pass on priority: the log writer, at idle priority, runs at a
 * waiting reader's priority while it holds sSdMutex.
 */
#define SD_LOCK_SECTORS 64

static mutex_t sSdMutex = LWP_MUTEX_NULL;
static const DISC_INTERFACE* sSdIface; /* the adapter's interface */

static bool sd_locked_startup(void) {
    bool ok;

    LWP_MutexLock(sSdMutex);
    ok = sSdIface->startup();
    LWP_MutexUnlock(sSdMutex);
    return ok;
}

static bool sd_locked_is_inserted(void) {
    bool ok;

    LWP_MutexLock(sSdMutex);
    ok = sSdIface->isInserted();
    LWP_MutexUnlock(sSdMutex);
    return ok;
}

static bool sd_locked_read(sec_t sector, sec_t count, void* buffer) {
    unsigned char* dst = buffer;

    while (count != 0) {
        sec_t n = (count < SD_LOCK_SECTORS) ? count : SD_LOCK_SECTORS;
        bool ok;

        LWP_MutexLock(sSdMutex);
        ok = sSdIface->readSectors(sector, n, dst);
        LWP_MutexUnlock(sSdMutex);
        if (!ok) {
            return false;
        }
        sector += n;
        count -= n;
        dst += n * FAT_MAP_SECTOR_SIZE;
    }
    return true;
}

static bool sd_locked_write(sec_t sector, sec_t count, const void* buffer) {
    const unsigned char* src = buffer;

    while (count != 0) {
        sec_t n = (count < SD_LOCK_SECTORS) ? count : SD_LOCK_SECTORS;
        bool ok;

        LWP_MutexLock(sSdMutex);
        ok = sSdIface->writeSectors(sector, n, src);
        LWP_MutexUnlock(sSdMutex);
        if (!ok) {
            return false;
        }
        sector += n;
        count -= n;
        src += n * FAT_MAP_SECTOR_SIZE;
    }
    return true;
}

static bool sd_locked_clear_status(void) {
    bool ok;

    LWP_MutexLock(sSdMutex);
    ok = sSdIface->clearStatus();
    LWP_MutexUnlock(sSdMutex);
    return ok;
}

static bool sd_locked_shutdown(void) {
    bool ok;

    LWP_MutexLock(sSdMutex);
    ok = sSdIface->shutdown();
    LWP_MutexUnlock(sSdMutex);
    return ok;
}

/* ioType and features are the adapter's, filled in by gc_ogc_sd_mount */
static DISC_INTERFACE sSdLocked = {
    0,
    0,
    sd_locked_startup,
    sd_locked_is_inserted,
    sd_locked_read,
    sd_locked_write,
    sd_locked_clear_status,
    sd_locked_shutdown,
};

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

static int sd_lock_init(void) {
    if (sSdMutex == LWP_MUTEX_NULL && LWP_MutexInit(&sSdMutex, false) != 0) {
        sSdMutex = LWP_MUTEX_NULL;
        gc_log("SD: cannot create the SD card lock");
        return -1;
    }
    return 0;
}

/* Mount `iface` as sd: behind sSdLocked. 1 on success. */
static int sd_mount_locked(const DISC_INTERFACE* iface) {
    sSdIface = iface;
    sSdLocked.ioType = iface->ioType;
    sSdLocked.features = iface->features;
    return fatMountSimple("sd", &sSdLocked);
}

const char* gc_ogc_sd_mount(void) {
    int first = sd_preferred_device();
    int k;

    if (sSdMounted) {
        return (sSdDevice >= 0) ? sSdDevices[sSdDevice].name : "SD card";
    }
    if (sd_lock_init() != 0) {
        return NULL;
    }
    for (k = 0; k < SD_DEVICES; k++) {
        int i = (k == 0) ? first : (k <= first ? k - 1 : k);

        if (sSdDevices[i].iface == &__io_gcsdb && gc_ogc_log_gecko()) {
            // SD commands would reach the USB Gecko and show up as garbage in its log.
            gc_log("SD: slot B skipped (USB Gecko)");
            continue;
        }
        if (sd_mount_locked(sSdDevices[i].iface)) {
            sSdMounted = 1;
            sSdDevice = i;
            gc_log("SD: mounted the card in the %s as sd:", sSdDevices[i].name);
            return sSdDevices[i].name;
        }
        gc_log("SD: no card in the %s", sSdDevices[i].name);
    }
    return NULL;
}

int gc_ogc_sd_mount_iface(const struct DISC_INTERFACE_STRUCT* iface) {
    if (sSdMounted || sd_lock_init() != 0 || !sd_mount_locked(iface)) {
        return -1;
    }
    sSdMounted = 1;
    gc_log("SD: mounted a test volume as sd:");
    return 0;
}

int gc_ogc_sd_mounted(void) {
    return sSdMounted;
}

/* ---------------------------------------------------------------------------------------------- */
/* Raw reads of a file on the SD card                                                             */
/* ---------------------------------------------------------------------------------------------- */

/*
 * A file that never changes while the game runs (the ROM, or the disc image holding it) is mapped once to the runs
 * of sectors it occupies (fat_map.c: the volume's boot sector, the directories, the file's cluster chain), and then
 * read with sSdLocked's sector reads. Before the map is used, reads through it are compared with libfat's reads of
 * the same file: its start and end, ranges spread over it at odd offsets and sizes, and ranges across the first
 * runs' boundaries. On any difference, error or unsupported layout the file is read through libfat as before. The
 * boot log has one line per file saying which way it is read.
 */
#define SD_RAW_FILES 8
#define SD_RAW_MAX_EXTENTS 4096
#define SD_RAW_MAP_SECTORS 8
#define SD_RAW_CHECK_SIZE 0x1000
#define SD_RAW_CHECK_RANGES 24

struct GcSdRaw {
    char path[256];
    FatExtent* ext;
    unsigned int count;
    unsigned int size;
};

static GcSdRaw sRaw[SD_RAW_FILES];
static int sRawCount;
static mutex_t sRawMutex = LWP_MUTEX_NULL; /* sRawBounce */
static unsigned char sRawBounce[FAT_MAP_SECTOR_SIZE] __attribute__((aligned(32)));

static int raw_read_sectors(void* ctx, uint32_t sector, uint32_t count, void* dst) {
    return sd_locked_read(sector, count, dst) ? 0 : -1;
}

int gc_ogc_sd_raw_read(GcSdRaw* raw, unsigned int offset, void* dst, unsigned int size) {
    int ret;

    if (offset > raw->size || size > raw->size - offset) {
        return -1;
    }
    LWP_MutexLock(sRawMutex);
    ret = fat_map_read(raw->ext, raw->count, raw_read_sectors, NULL, offset, dst, size, sRawBounce);
    LWP_MutexUnlock(sRawMutex);
    return ret;
}

unsigned int gc_ogc_sd_raw_size(const GcSdRaw* raw) {
    return raw->size;
}

static unsigned int min_uint(unsigned int a, unsigned int b) {
    return (a < b) ? a : b;
}

/* Compare raw reads with libfat's reads of `file`. The number of ranges compared, or -1 with *why. */
static int raw_check(GcSdRaw* raw, FILE* file, const char** why) {
    unsigned int offsets[SD_RAW_CHECK_RANGES];
    unsigned int sizes[SD_RAW_CHECK_RANGES];
    unsigned char* mine = malloc(SD_RAW_CHECK_SIZE);
    unsigned char* theirs = malloc(SD_RAW_CHECK_SIZE);
    unsigned int size = raw->size;
    int count = 0;
    int i;

    if (mine == NULL || theirs == NULL) {
        free(mine);
        free(theirs);
        *why = "no memory for the check";
        return -1;
    }
    // Start and end
    offsets[count] = 0;
    sizes[count++] = min_uint(size, SD_RAW_CHECK_SIZE);
    offsets[count] = size - min_uint(size, SD_RAW_CHECK_SIZE);
    sizes[count++] = min_uint(size, SD_RAW_CHECK_SIZE);
    // Spread over the file, at odd offsets and sizes (partial first and last sectors)
    for (i = 1; i < 8; i++) {
        unsigned int at = (unsigned int)((u64)size * i / 8) + 37 * i;

        if (at < size) {
            offsets[count] = at;
            sizes[count++] = min_uint(size - at, 3000 + 111 * i);
        }
    }
    // Across run boundaries
    for (i = 1; i < (int)raw->count && count < SD_RAW_CHECK_RANGES; i++) {
        unsigned int at = raw->ext[i].offset - min_uint(raw->ext[i].offset, 700);

        offsets[count] = at;
        sizes[count++] = min_uint(size - at, 1400);
    }

    *why = NULL;
    for (i = 0; i < count && *why == NULL; i++) {
        if (sizes[i] == 0) {
            continue;
        }
        if (gc_ogc_sd_raw_read(raw, offsets[i], mine, sizes[i]) != 0) {
            *why = "a raw read failed";
        } else if (fseek(file, (long)offsets[i], SEEK_SET) != 0 || fread(theirs, 1, sizes[i], file) != sizes[i]) {
            clearerr(file);
            *why = "libfat cannot read the file";
        } else if (memcmp(mine, theirs, sizes[i]) != 0) {
            *why = "raw reads differ from libfat's";
        }
    }
    free(mine);
    free(theirs);
    return (*why == NULL) ? count : -1;
}

GcSdRaw* gc_ogc_sd_raw_open(const char* path, FILE* file) {
    u64 t0 = gettime();
    GcSdRaw* raw = &sRaw[sRawCount];
    unsigned char* scratch = NULL;
    const char* why = NULL;
    uint32_t cluster = 0;
    uint32_t size = 0;
    uint32_t count = 0;
    struct stat st;
    FatMap map;
    int checked = 0;
    int i;

    for (i = 0; i < sRawCount; i++) {
        if (strcmp(sRaw[i].path, path) == 0) {
            return &sRaw[i];
        }
    }
    memset(&map, 0, sizeof(map));
    if (!sSdMounted || strncmp(path, "sd:/", 4) != 0) {
        why = "not a file on the SD card";
    } else if (sRawCount == SD_RAW_FILES) {
        why = "too many raw files";
    } else if (sRawMutex == LWP_MUTEX_NULL && LWP_MutexInit(&sRawMutex, false) != 0) {
        sRawMutex = LWP_MUTEX_NULL;
        why = "cannot create the raw read lock";
    } else if ((scratch = malloc(SD_RAW_MAP_SECTORS * FAT_MAP_SECTOR_SIZE)) == NULL) {
        why = "no memory";
    } else if (fstat(fileno(file), &st) != 0) {
        why = "libfat cannot stat the file";
    }
    if (why == NULL) {
        why = fat_map_mount(&map, raw_read_sectors, NULL, scratch, SD_RAW_MAP_SECTORS);
    }
    if (why == NULL) {
        why = fat_map_find(&map, path + 3, &cluster, &size);
    }
    // libfat reports the first cluster as st_ino: the same file on the same volume
    if (why == NULL && (off_t)size != st.st_size) {
        why = "its size differs from libfat's";
    }
    if (why == NULL && st.st_ino != 0 && st.st_ino != cluster) {
        why = "its first cluster differs from libfat's";
    }
    if (why == NULL) {
        why = fat_map_extents(&map, cluster, size, NULL, 0, &count);
    }
    if (why == NULL && count > SD_RAW_MAX_EXTENTS) {
        why = "the file has too many fragments";
    }
    if (why == NULL && size == 0) {
        why = "the file is empty";
    }
    if (why == NULL && (raw->ext = gc_mem_alloc(count * sizeof(FatExtent), 32)) == NULL) {
        why = "no memory for the map";
    }
    if (why == NULL) {
        why = fat_map_extents(&map, cluster, size, raw->ext, count, &count);
    }
    if (why == NULL) {
        snprintf(raw->path, sizeof(raw->path), "%s", path);
        raw->count = count;
        raw->size = size;
        checked = raw_check(raw, file, &why);
    }
    free(scratch);
    if (why != NULL) {
        gc_log("SD: %s: read through libfat (no raw reads: %s)", path, why);
        return NULL;
    }
    sRawCount++;
    gc_log("SD: %s: raw sector reads, %u extent%s (FAT%d, %u-byte clusters, volume at sector %u); %d ranges match "
           "libfat (%u ms)",
           path, count, (count == 1) ? "" : "s", map.type, fat_map_cluster_size(&map), map.volume, checked,
           (unsigned int)ticks_to_millisecs(gettime() - t0));
    return raw;
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
static char sImagePath[256];
static int sImageMounted;
static int sImageRawTried;
static GcSdRaw* sImageRaw; /* raw reads of the image file, once gc_ogc_image_raw has set them up */

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

    if (sImageRaw != NULL) {
        u64 offset = (u64)sector * DISC_SECTOR_SIZE;
        unsigned int imageSize = gc_ogc_sd_raw_size(sImageRaw);
        unsigned int n = (offset < imageSize) ? min_uint(imageSize - (unsigned int)offset, size) : 0;

        if (n == 0 || gc_ogc_sd_raw_read(sImageRaw, (unsigned int)offset, buffer, n) == 0) {
            memset((unsigned char*)buffer + n, 0, size - n);
            return true;
        }
        gc_log("Image: raw read of sector %u failed; trying libfat", (unsigned int)sector);
    }
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
    snprintf(sImagePath, sizeof(sImagePath), "%s", path);
    gc_log("Image: %s (%.6s, '%s') mounted read-only as img:", path, (const char*)header,
           ISO9660_GetVolumeLabel("img"));
    return 0;
}

GcSdRaw* gc_ogc_image_raw(void) {
    if (sImageMounted && !sImageRawTried) {
        sImageRawTried = 1;
        sImageRaw = gc_ogc_sd_raw_open(sImagePath, sImageFile);
    }
    return sImageRaw;
}

/* Tests only: unmount sd: (not while img: is mounted on it) */
int gc_ogc_sd_unmount(void) {
    if (!sSdMounted || sImageMounted) {
        return -1;
    }
    fatUnmount("sd");
    sSdMounted = 0;
    sSdDevice = -1;
    // The raw maps were of that volume (their extent arrays are not returned)
    sRawCount = 0;
    gc_log("SD: unmounted sd:");
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
