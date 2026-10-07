/**
 * Save data: the N64 flash image persisted as sd:/mmgcport/mm.fla.
 *
 * mm.fla, mm.fla.bak and mm.fla.tmp hold the three newest saves and are recycled: a store writes the new save over
 * mm.fla.tmp in place (it keeps its clusters) and renames the files round, so once they exist a store never
 * allocates clusters on the card. FatFs (libogc's libfat) looks for free clusters by reading the FAT from where it
 * last allocated (or, after truncating a file, from that file's old first cluster), which took the first save after
 * boot 37 s on a large, mostly full card; while it does, it holds the volume lock, and the old store also froze the
 * game, whose ROM reads waited for that lock (they no longer use libfat, see storage.c). A store still waits for the
 * lock while another libfat user searches, such as the log growing past the clusters it reused at boot (README,
 * "Saves"). save_rotation.c has the protocol and its invariant: whatever step a crash or power loss interrupts, a
 * complete save remains, and gc_save_load loads the newest one. It reads the mm.fla, .bak and .tmp that earlier
 * builds left.
 *
 * Without an SD card (booted from the disc), saves go to the memory card if one was mounted at boot
 * (storage.c, gc_ogc_card_save_load/store).
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "gc_ogc.h"
#include "save_rotation.h"

static mutex_t sSaveMutex = LWP_MUTEX_NULL;
static SaveSet sSaveSet;
static int sSaveSetReady;

void gc_ogc_save_init(void) {
    if (sSaveMutex == LWP_MUTEX_NULL && LWP_MutexInit(&sSaveMutex, false) != 0) {
        gc_halt("gc_ogc_save_init: cannot create the save mutex");
    }
}

static const SaveSet* save_set(void) {
    if (!sSaveSetReady) {
        save_set_init(&sSaveSet, GC_SAVE_PATH);
        sSaveSetReady = 1;
    }
    return &sSaveSet;
}

void gc_ogc_save_set_path(const char* path) {
    save_set_init(&sSaveSet, path);
    sSaveSetReady = 1;
}

/* ---------------------------------------------------------------------------------------------- */
/* The SD card's files, for save_rotation.c                                                       */
/* ---------------------------------------------------------------------------------------------- */

static void fs_stat(void* ctx, const char* path, SaveFileInfo* info) {
    struct stat st;

    if (stat(path, &st) == 0) {
        info->exists = 1;
        info->size = (long)st.st_size;
        info->cluster = (unsigned long)st.st_ino; // libfat: the first cluster
    }
}

static int fs_rename(void* ctx, const char* from, const char* to) {
    return rename(from, to);
}

/* Without O_CREAT/O_TRUNC libfat opens the file as it is: the write goes into its clusters */
static int fs_write(void* ctx, const char* path, const void* src, unsigned int size, int inPlace) {
    const unsigned char* p = src;
    unsigned int left = size;
    int fd = open(path, inPlace ? O_WRONLY : (O_WRONLY | O_CREAT | O_TRUNC), 0666);
    int ok;

    if (fd < 0) {
        gc_log("save: cannot open %s (errno %d)", path, errno);
        return -1;
    }
    while (left != 0) {
        ssize_t n = write(fd, p, left);

        if (n <= 0) {
            break;
        }
        p += n;
        left -= (unsigned int)n;
    }
    ok = (left == 0) && fsync(fd) == 0;
    if (!ok) {
        gc_log("save: writing %s failed (errno %d)", path, errno);
    }
    ok = (close(fd) == 0) && ok;
    return ok ? 0 : -1;
}

static void fs_log(void* ctx, const char* line) {
    gc_log("%s", line);
}

static const SaveFs sSdFs = { NULL, fs_stat, fs_rename, fs_write, fs_log };

/* ---------------------------------------------------------------------------------------------- */
/* Load and store                                                                                 */
/* ---------------------------------------------------------------------------------------------- */

int gc_save_load(void* dst, unsigned int size) {
    const SaveSet* set = save_set();
    const char* path;
    FILE* file;
    size_t got;
    long fileSize;
    struct stat st;
    int which;
    int ret = 0;

    if (!gc_ogc_sd_mounted()) {
        if (gc_ogc_card_mounted()) {
            LWP_MutexLock(sSaveMutex);
            ret = gc_ogc_card_save_load(dst, size);
            LWP_MutexUnlock(sSaveMutex);
            return ret;
        }
        gc_log("save: no SD card, starting without a save");
        return -1;
    }

    LWP_MutexLock(sSaveMutex);
    which = save_pick(&sSdFs, set, size);
    if (which < 0) {
        LWP_MutexUnlock(sSaveMutex);
        gc_log("save: no save file (%s)", set->path[SAVE_MAIN]);
        return 1;
    }
    path = set->path[which];
    if (which != SAVE_MAIN) {
        gc_log("save: loading %s: it holds the newest complete save", path);
    }
    fileSize = (stat(path, &st) == 0) ? (long)st.st_size : -1;

    file = fopen(path, "rb");
    if (file == NULL) {
        LWP_MutexUnlock(sSaveMutex);
        gc_log("save: cannot open %s (errno %d)", path, errno);
        return -1;
    }
    got = fread(dst, 1, size, file);
    if (ferror(file)) {
        gc_log("save: read error in %s (errno %d)", path, errno);
        ret = -1;
    }
    fclose(file);
    LWP_MutexUnlock(sSaveMutex);

    if (got < size) {
        // Missing bytes read as erased flash
        memset((unsigned char*)dst + got, 0xFF, size - got);
    }
    if (fileSize != (long)size) {
        gc_log("save: %s is %ld bytes, expected %u", path, fileSize, size);
    }
    if (ret == 0) {
        gc_log("save: loaded %s (%u bytes)", path, (unsigned int)got);
    }
    return ret;
}

int gc_save_store(const void* src, unsigned int size) {
    u64 t0 = gettime();
    int created = 0;
    int ret;

    if (!gc_ogc_sd_mounted()) {
        if (gc_ogc_card_mounted()) {
            LWP_MutexLock(sSaveMutex);
            ret = gc_ogc_card_save_store(src, size);
            LWP_MutexUnlock(sSaveMutex);
            return ret;
        }
        gc_log("save: no SD card, the save is not kept");
        return -1;
    }

    LWP_MutexLock(sSaveMutex);
    ret = save_store(&sSdFs, save_set(), src, size, &created);
    LWP_MutexUnlock(sSaveMutex);

    if (ret == 0) {
        gc_log("save: wrote %s (%u bytes, %s, %u ms)", save_set()->path[SAVE_MAIN], size,
               created ? "new file" : "in place", (unsigned int)ticks_to_millisecs(gettime() - t0));
    }
    return ret;
}
