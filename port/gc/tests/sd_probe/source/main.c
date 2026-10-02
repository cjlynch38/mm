// sd_probe: storage and logging probe for the GameCube dev loop.
//
// Tries every way the port can reach the ROM: the libogc SD drivers (SD2SP2 in
// serial port 2, SD Gecko in slot A or B) and, when no SD card answers, the
// Dolphin dev disc (an ISO9660 image of the SD folder inserted as the DVD, see
// port/gc/tools/mkdevdisc.py). It then lists /mmgcport, dumps the first 64
// bytes of baserom.z64, times a sequential read and, on a writable volume,
// writes /mmgcport/probe_log.txt.
//
// Every log line also goes to a USB Gecko in slot B when one answers. Dolphin
// 2609 emulates the USB Gecko (a TCP server on port 55020) but not an SD card
// on the GameCube EXI bus, so in Dolphin the Gecko stream is the only log that
// leaves the emulator; run_dolphin.ps1 captures it.
#include <gccore.h>
#include <ogc/libversion.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/usbgecko.h>
#include <sdcard/gcsd.h>
#include <fat.h>
#include <iso9660.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define GECKO_CHAN 1 // EXI channel 1 = memory card slot B
#define ROM_DIR "mmgcport"
#define ROM_NAME "baserom.z64"
#define LOG_NAME "probe_log.txt"
#define TIMED_READ_SIZE (4 * 1024 * 1024)
#define TIMED_READ_CHUNK (64 * 1024)
#define DVD_MOUNT_TIMEOUT_MS 8000
#define ARRAY_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

static void* sXfb;
static GXRModeObj* sRmode;
static bool sGecko;
static char sLogBuf[16 * 1024];
static size_t sLogLen;
static u8 sReadBuf[TIMED_READ_CHUNK] ATTRIBUTE_ALIGN(32);

static void Probe_Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

static void Probe_Log(const char* fmt, ...) {
    char line[256];
    va_list args;
    int len;

    va_start(args, fmt);
    len = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (len < 0) {
        return;
    }
    if ((size_t)len >= sizeof(line)) {
        len = sizeof(line) - 1;
    }

    fputs(line, stdout);
    if (sGecko) {
        usb_sendbuffer_safe(GECKO_CHAN, line, len);
    }
    if (sLogLen + len < sizeof(sLogBuf)) {
        memcpy(sLogBuf + sLogLen, line, len);
        sLogLen += len;
    }
}

static u32 Probe_MsSince(u64 start) {
    return ticks_to_millisecs(gettime() - start);
}

static void Probe_VideoInit(void) {
    VIDEO_Init();
    sRmode = VIDEO_GetPreferredMode(NULL);
    sXfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(sRmode));
    console_init(sXfb, 20, 20, sRmode->fbWidth, sRmode->xfbHeight, sRmode->fbWidth * VI_DISPLAY_PIX_SZ);
    VIDEO_Configure(sRmode);
    VIDEO_SetNextFramebuffer(sXfb);
    VIDEO_SetBlack(false);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (sRmode->viTVMode & VI_NON_INTERLACE) {
        VIDEO_WaitVSync();
    }
}

// --- SD ---------------------------------------------------------------------

typedef struct {
    const DISC_INTERFACE* iface;
    const char* desc;
} SdSlot;

static const SdSlot sSdSlots[] = {
    { &__io_gcsd2, "SD2SP2 (serial port 2)" },
    { &__io_gcsda, "SD Gecko in slot A" },
    { &__io_gcsdb, "SD Gecko in slot B" },
};

// Mounts the first SD card that answers as "sd:"; reports every slot.
static bool Probe_MountSd(void) {
    bool mounted = false;
    size_t i;

    for (i = 0; i < ARRAY_COUNT(sSdSlots); i++) {
        const SdSlot* slot = &sSdSlots[i];
        u64 start = gettime();
        bool present;

        if (slot->iface == &__io_gcsdb && sGecko) {
            // SD commands would land in the Gecko's FIFO as garbage log bytes.
            Probe_Log("  %-24s skipped (USB Gecko in slot B)\n", slot->desc);
            continue;
        }

        present = slot->iface->startup() && slot->iface->isInserted();
        if (!present) {
            Probe_Log("  %-24s no card (%u ms)\n", slot->desc, Probe_MsSince(start));
            continue;
        }
        if (mounted) {
            Probe_Log("  %-24s card present, not mounted (sd: taken)\n", slot->desc);
            continue;
        }
        mounted = fatMountSimple("sd", slot->iface);
        Probe_Log("  %-24s card present, mount %s (%u ms)\n", slot->desc, mounted ? "ok as sd:" : "FAILED",
                  Probe_MsSince(start));
    }
    return mounted;
}

// --- Dolphin dev disc ---------------------------------------------------------

static volatile bool sDvdMountDone;
static volatile s32 sDvdMountResult;
static bool sDvdReady;
static dvdcmdblk sDvdMountBlock;
static u8 sDvdBounce[0x8000] ATTRIBUTE_ALIGN(32);

static void Probe_DvdMountCallback(s32 result, dvdcmdblk* block) {
    (void)block;
    sDvdMountResult = result;
    sDvdMountDone = true;
}

static bool DevDisc_Startup(void) {
    return sDvdReady;
}

static bool DevDisc_IsInserted(void) {
    return sDvdReady;
}

// libogc's __io_gcdvd reads straight into the caller's buffer, but DI DMA needs
// 32-byte alignment and libiso9660's buffers live inside a malloc'd struct, so
// misaligned reads bounce through an aligned buffer.
static bool DevDisc_ReadSectors(sec_t sector, sec_t numSectors, void* buffer) {
    dvdcmdblk block;
    s64 offset = (s64)sector << 11;
    u32 len = numSectors << 11;
    u8* dst = buffer;

    if (((u32)dst & 31) == 0) {
        return DVD_ReadPrio(&block, dst, len, offset, 2) > 0;
    }
    while (len != 0) {
        u32 chunk = (len < sizeof(sDvdBounce)) ? len : sizeof(sDvdBounce);

        if (DVD_ReadPrio(&block, sDvdBounce, chunk, offset, 2) <= 0) {
            return false;
        }
        memcpy(dst, sDvdBounce, chunk);
        dst += chunk;
        offset += chunk;
        len -= chunk;
    }
    return true;
}

static bool DevDisc_WriteSectors(sec_t sector, sec_t numSectors, const void* buffer) {
    (void)sector;
    (void)numSectors;
    (void)buffer;
    return false;
}

static bool DevDisc_ClearStatus(void) {
    return true;
}

static bool DevDisc_Shutdown(void) {
    return true;
}

static const DISC_INTERFACE sDevDisc = {
    DEVICE_TYPE_GAMECUBE_DVD,
    FEATURE_MEDIUM_CANREAD | FEATURE_GAMECUBE_DVD,
    DevDisc_Startup,
    DevDisc_IsInserted,
    DevDisc_ReadSectors,
    DevDisc_WriteSectors,
    DevDisc_ClearStatus,
    DevDisc_Shutdown,
};

// DVD_Mount() blocks forever on a console without a drive, so mount
// asynchronously with a timeout and only then hand the disc to libiso9660.
static bool Probe_MountDevDisc(void) {
    u64 start = gettime();
    dvddiskid* id;
    const char* label;
    int labelLen;

    DVD_Init();
    sDvdMountDone = false;
    if (!DVD_MountAsync(&sDvdMountBlock, Probe_DvdMountCallback)) {
        Probe_Log("  DVD mount request failed\n");
        return false;
    }
    while (!sDvdMountDone && Probe_MsSince(start) < DVD_MOUNT_TIMEOUT_MS) {
        VIDEO_WaitVSync();
    }
    if (!sDvdMountDone) {
        Probe_Log("  DVD: no answer after %u ms\n", Probe_MsSince(start));
        return false;
    }
    if (sDvdMountResult < 0) {
        Probe_Log("  DVD: mount error %d, drive status %d (%u ms)\n", (int)sDvdMountResult,
                  (int)DVD_GetDriveStatus(), Probe_MsSince(start));
        return false;
    }
    id = DVD_GetCurrentDiskID();
    Probe_Log("  DVD: disc %.4s%.2s mounted (%u ms)\n", (const char*)id->gamename, (const char*)id->company,
              Probe_MsSince(start));

    sDvdReady = true;
    if (!ISO9660_Mount("dvd", &sDevDisc)) {
        Probe_Log("  DVD: no ISO9660 filesystem (not a dev disc?)\n");
        return false;
    }
    label = ISO9660_GetVolumeLabel("dvd");
    labelLen = (label != NULL) ? (int)strlen(label) : 0;
    while (labelLen > 0 && label[labelLen - 1] == ' ') {
        labelLen--;
    }
    Probe_Log("  DVD: ISO9660 volume '%.*s' mounted as dvd:\n", labelLen, label != NULL ? label : "");
    return true;
}

// --- Probes -------------------------------------------------------------------

static void Probe_ListDir(const char* path) {
    DIR* dir = opendir(path);
    struct dirent* ent;
    int count = 0;

    if (dir == NULL) {
        Probe_Log("  %s: cannot open\n", path);
        return;
    }
    Probe_Log("  %s:\n", path);
    while ((ent = readdir(dir)) != NULL && count < 32) {
        char full[512];
        struct stat st;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        snprintf(full, sizeof(full), "%s/%s", path, ent->d_name);
        if (stat(full, &st) == 0 && !S_ISDIR(st.st_mode)) {
            Probe_Log("    %-32s %10lu\n", ent->d_name, (unsigned long)st.st_size);
        } else {
            Probe_Log("    %s/\n", ent->d_name);
        }
        count++;
    }
    closedir(dir);
}

static void Probe_ReadRom(const char* root) {
    char path[128];
    u8 head[64];
    FILE* f;
    long size;
    size_t got;
    size_t total;
    u64 start;
    u32 ms;
    int i;

    snprintf(path, sizeof(path), "%s/" ROM_DIR "/" ROM_NAME, root);
    f = fopen(path, "rb");
    if (f == NULL) {
        Probe_Log("  %s: not found\n", path);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    got = fread(head, 1, sizeof(head), f);
    Probe_Log("  %s: %ld bytes, first %u:\n", path, size, (unsigned)got);
    for (i = 0; i + 16 <= (int)got; i += 16) {
        Probe_Log("    %02X: %02X %02X %02X %02X %02X %02X %02X %02X  %02X %02X %02X %02X %02X %02X %02X %02X\n",
                  i, head[i + 0], head[i + 1], head[i + 2], head[i + 3], head[i + 4], head[i + 5], head[i + 6],
                  head[i + 7], head[i + 8], head[i + 9], head[i + 10], head[i + 11], head[i + 12], head[i + 13],
                  head[i + 14], head[i + 15]);
    }
    if (got == sizeof(head)) {
        u32 magic = ((u32)head[0] << 24) | ((u32)head[1] << 16) | ((u32)head[2] << 8) | head[3];
        const char* order = (magic == 0x80371240)   ? "z64 (big-endian, ok)"
                            : (magic == 0x37804012) ? "v64 (byte-swapped, rejected by the port)"
                            : (magic == 0x40123780) ? "n64 (little-endian, rejected by the port)"
                                                    : "unknown";

        Probe_Log("  magic %08X = %s\n", (unsigned)magic, order);
        Probe_Log("  title '%.20s'  code %.4s  crc %02X%02X%02X%02X/%02X%02X%02X%02X\n", (const char*)&head[0x20],
                  (const char*)&head[0x3B], head[0x10], head[0x11], head[0x12], head[0x13], head[0x14], head[0x15],
                  head[0x16], head[0x17]);
    }

    fseek(f, 0, SEEK_SET);
    total = 0;
    start = gettime();
    while (total < TIMED_READ_SIZE) {
        got = fread(sReadBuf, 1, sizeof(sReadBuf), f);
        if (got == 0) {
            break;
        }
        total += got;
    }
    ms = Probe_MsSince(start);
    Probe_Log("  sequential read: %u KiB in %u ms (%u KiB/s)\n", (unsigned)(total / 1024), ms,
              ms != 0 ? (unsigned)((u64)total * 1000 / 1024 / ms) : 0);
    fclose(f);
}

static void Probe_WriteLog(void) {
    const char* path = "sd:/" ROM_DIR "/" LOG_NAME;
    FILE* f;
    size_t written;

    mkdir("sd:/" ROM_DIR, 0777);
    Probe_Log("sd_probe: writing %s\n", path);
    f = fopen(path, "w");
    if (f == NULL) {
        Probe_Log("  open for writing FAILED\n");
        return;
    }
    written = fwrite(sLogBuf, 1, sLogLen, f);
    if (fclose(f) != 0 || written != sLogLen) {
        Probe_Log("  write FAILED (%u of %u bytes)\n", (unsigned)written, (unsigned)sLogLen);
        return;
    }
    fatUnmount("sd");
    Probe_Log("  wrote %u bytes, sd: unmounted\n", (unsigned)written);
}

int main(void) {
    const char* root = NULL;

    Probe_VideoInit();
    PAD_Init();
    printf("\x1b[2;0H");

    sGecko = usb_isgeckoalive(GECKO_CHAN);
    Probe_Log("sd_probe: mmgcport storage probe, libogc %d.%d.%d\n", _V_MAJOR_, _V_MINOR_, _V_PATCH_);
    Probe_Log("USB Gecko in slot B: %s\n", sGecko ? "yes (log mirrored)" : "no");

    Probe_Log("SD cards:\n");
    if (Probe_MountSd()) {
        root = "sd:";
    } else {
        Probe_Log("Dolphin dev disc:\n");
        if (Probe_MountDevDisc()) {
            root = "dvd:";
        }
    }

    if (root == NULL) {
        Probe_Log("sd_probe: RESULT no storage found\n");
    } else {
        char dir[32];

        Probe_Log("Contents:\n");
        Probe_ListDir(root[0] == 's' ? "sd:/" : "dvd:/");
        snprintf(dir, sizeof(dir), "%s/" ROM_DIR, root);
        Probe_ListDir(dir);
        Probe_Log("ROM:\n");
        Probe_ReadRom(root);
        Probe_Log("sd_probe: RESULT storage %s ok\n", root);
        if (root[0] == 's') {
            Probe_WriteLog();
        } else {
            Probe_Log("sd_probe: dvd: is read-only, %s not written\n", LOG_NAME);
        }
    }
    Probe_Log("sd_probe: done\n");

    printf("\nPress START to exit\n");
    while (true) {
        PAD_ScanPads();
        if (PAD_ButtonsDown(0) & PAD_BUTTON_START) {
            break;
        }
        VIDEO_WaitVSync();
    }
    exit(0);
}
