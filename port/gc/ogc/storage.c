/**
 * Storage for the ROM, the log file and saves.
 *
 * On hardware this is the SD card, mounted as "sd:" through libogc's drivers: SD2SP2 in serial
 * port 2 first, then an SD Gecko in memory card slot A or B.
 *
 * Dolphin 2609 cannot emulate an SD card on the GameCube, so when no card answers, the platform
 * falls back to the Dolphin dev disc: port/gc/tools/run_dolphin.ps1 packs the SD folder into an
 * ISO9660 image (mkdevdisc.py) and inserts it as the DVD; it is mounted read-only as "dvd:" with
 * the same paths. The log file and saves need sd: and are skipped on dvd:.
 * (The dev-disc reader follows port/gc/tests/sd_probe, written with the harness.)
 */
#include <gccore.h>
#include <ogc/dvd.h>
#include <ogc/lwp_watchdog.h>
#include <fat.h>
#include <iso9660.h>
#include <sdcard/gcsd.h>
#include <string.h>
#include <unistd.h>
#include "gc_ogc.h"

/* DVD_Mount never returns on a console without a drive, hence the async mount with a timeout */
#define DVD_MOUNT_TIMEOUT_MS 8000
#define DVD_BOUNCE_SIZE 0x8000

static const struct {
    const char* name;
    const DISC_INTERFACE* iface;
} sSdDevices[] = {
    { "SD2SP2 (serial port 2)", &__io_gcsd2 },
    { "SD Gecko in memory card slot A", &__io_gcsda },
    { "SD Gecko in memory card slot B", &__io_gcsdb },
};

static int sSdMounted;
static int sDvdMounted;

const char* gc_ogc_sd_mount(void) {
    unsigned int i;

    for (i = 0; i < sizeof(sSdDevices) / sizeof(sSdDevices[0]); i++) {
        if (sSdDevices[i].iface == &__io_gcsdb && gc_ogc_log_gecko()) {
            // SD commands would reach the USB Gecko and show up as garbage in its log.
            gc_log("SD: slot B skipped (USB Gecko)");
            continue;
        }
        if (fatMountSimple("sd", sSdDevices[i].iface)) {
            sSdMounted = 1;
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
/* Dolphin dev disc                                                                               */
/* ---------------------------------------------------------------------------------------------- */

static volatile int sDvdMountDone;
static volatile s32 sDvdMountResult;
static dvdcmdblk sDvdMountBlock;
static unsigned char* sDvdBounce;

static void dvd_mount_callback(s32 result, dvdcmdblk* block) {
    sDvdMountResult = result;
    sDvdMountDone = 1;
}

static bool devdisc_startup(void) {
    return sDvdMounted;
}

static bool devdisc_is_inserted(void) {
    return sDvdMounted;
}

/* DI DMA needs 32-byte aligned buffers; libiso9660's own buffers may not be, so those reads go
 * through an aligned bounce buffer. libogc invalidates the data cache over DMA'd ranges. */
static bool devdisc_read_sectors(sec_t sector, sec_t numSectors, void* buffer) {
    dvdcmdblk block;
    s64 offset = (s64)sector << 11;
    u32 size = numSectors << 11;
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

static bool devdisc_write_sectors(sec_t sector, sec_t numSectors, const void* buffer) {
    return false;
}

static bool devdisc_clear_status(void) {
    return true;
}

static bool devdisc_shutdown(void) {
    return true;
}

static const DISC_INTERFACE sDevDisc = {
    DEVICE_TYPE_GAMECUBE_DVD,
    FEATURE_MEDIUM_CANREAD | FEATURE_GAMECUBE_DVD,
    devdisc_startup,
    devdisc_is_inserted,
    devdisc_read_sectors,
    devdisc_write_sectors,
    devdisc_clear_status,
    devdisc_shutdown,
};

int gc_ogc_devdisc_mount(void) {
    u64 start = gettime();
    dvddiskid* id;

    if (sDvdMounted) {
        return 0;
    }
    if (sDvdBounce == NULL) {
        sDvdBounce = gc_mem_alloc(DVD_BOUNCE_SIZE, 32);
        if (sDvdBounce == NULL) {
            return -1;
        }
    }

    DVD_Init();
    sDvdMountDone = 0;
    if (!DVD_MountAsync(&sDvdMountBlock, dvd_mount_callback)) {
        gc_log("DVD: mount request failed");
        return -1;
    }
    while (!sDvdMountDone && ticks_to_millisecs(gettime() - start) < DVD_MOUNT_TIMEOUT_MS) {
        usleep(10000);
    }
    if (!sDvdMountDone) {
        gc_log("DVD: no drive answer after %u ms", DVD_MOUNT_TIMEOUT_MS);
        return -1;
    }
    if (sDvdMountResult < 0) {
        gc_log("DVD: no disc (error %d, drive status %d)", (int)sDvdMountResult, (int)DVD_GetDriveStatus());
        return -1;
    }

    id = DVD_GetCurrentDiskID();
    sDvdMounted = 1;
    if (!ISO9660_Mount("dvd", &sDevDisc)) {
        sDvdMounted = 0;
        gc_log("DVD: disc %.4s%.2s is not a dev disc (no ISO9660 file system)", (const char*)id->gamename,
               (const char*)id->company);
        return -1;
    }
    gc_log("DVD: dev disc %.4s%.2s ('%s') mounted read-only as dvd:", (const char*)id->gamename,
           (const char*)id->company, ISO9660_GetVolumeLabel("dvd"));
    return 0;
}

const char* gc_ogc_storage_mount(void) {
    if (sSdMounted || gc_ogc_sd_mount() != NULL) {
        return "sd:";
    }
    if (sDvdMounted || gc_ogc_devdisc_mount() == 0) {
        return "dvd:";
    }
    return NULL;
}
