/**
 * Save data: the N64 flash image persisted as sd:/mmgcport/mm.fla.
 *
 * A store never overwrites the save in place:
 *   1. write mm.fla.tmp completely and fsync it;
 *   2. replace mm.fla.bak with the current mm.fla (rename);
 *   3. rename mm.fla.tmp to mm.fla.
 * libogc's libfat is FatFs, whose rename and unlink commit the directory change to the card
 * before returning, and rename fails if the target exists (hence step 2's order).
 * Whatever step a crash or power loss interrupts, a complete save remains: mm.fla, else
 * mm.fla.bak (the previous save), else a mm.fla.tmp of the full size (written in step 1).
 * gc_save_load tries them in that order.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "gc_ogc.h"

#define SAVE_PATH_MAX 96

static mutex_t sSaveMutex = LWP_MUTEX_NULL;
static char sSavePath[SAVE_PATH_MAX] = GC_SAVE_PATH;
static char sTmpPath[SAVE_PATH_MAX] = GC_SAVE_PATH ".tmp";
static char sBakPath[SAVE_PATH_MAX] = GC_SAVE_PATH ".bak";

void gc_ogc_save_init(void) {
    if (sSaveMutex == LWP_MUTEX_NULL && LWP_MutexInit(&sSaveMutex, false) != 0) {
        gc_halt("gc_ogc_save_init: cannot create the save mutex");
    }
}

void gc_ogc_save_set_path(const char* path) {
    snprintf(sSavePath, sizeof(sSavePath), "%s", path);
    snprintf(sTmpPath, sizeof(sTmpPath), "%s.tmp", path);
    snprintf(sBakPath, sizeof(sBakPath), "%s.bak", path);
}

static long file_size(const char* path) {
    struct stat st;

    return (stat(path, &st) == 0) ? (long)st.st_size : -1;
}

int gc_save_load(void* dst, unsigned int size) {
    const char* path;
    FILE* file;
    size_t got;
    long fileSize;
    int ret = 0;

    if (!gc_ogc_sd_mounted()) {
        gc_log("save: no SD card, starting without a save");
        return -1;
    }

    LWP_MutexLock(sSaveMutex);
    path = sSavePath;
    fileSize = file_size(path);
    if (fileSize < 0) {
        path = sBakPath;
        fileSize = file_size(path);
    }
    if (fileSize < 0 && file_size(sTmpPath) == (long)size) {
        path = sTmpPath;
        fileSize = size;
    }
    if (fileSize < 0) {
        LWP_MutexUnlock(sSaveMutex);
        gc_log("save: no save file (%s)", sSavePath);
        return 1;
    }
    if (path != sSavePath) {
        gc_log("save: %s is missing, recovering from %s", sSavePath, path);
    }

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
    FILE* file;
    int ok;

    if (!gc_ogc_sd_mounted()) {
        gc_log("save: no SD card, the save is not kept");
        return -1;
    }

    LWP_MutexLock(sSaveMutex);
    file = fopen(sTmpPath, "wb");
    if (file == NULL) {
        LWP_MutexUnlock(sSaveMutex);
        gc_log("save: cannot create %s (errno %d)", sTmpPath, errno);
        return -1;
    }
    ok = fwrite(src, 1, size, file) == size;
    ok = ok && fflush(file) == 0;
    ok = ok && fsync(fileno(file)) == 0;
    ok = (fclose(file) == 0) && ok;
    if (!ok) {
        int err = errno;

        remove(sTmpPath);
        LWP_MutexUnlock(sSaveMutex);
        gc_log("save: writing %s failed (errno %d)", sTmpPath, err);
        return -1;
    }

    if (file_size(sSavePath) >= 0) {
        remove(sBakPath);
        if (rename(sSavePath, sBakPath) != 0) {
            int err = errno;

            LWP_MutexUnlock(sSaveMutex);
            gc_log("save: cannot rename %s to %s (errno %d)", sSavePath, sBakPath, err);
            return -1;
        }
    }
    if (rename(sTmpPath, sSavePath) != 0) {
        int err = errno;

        LWP_MutexUnlock(sSaveMutex);
        gc_log("save: cannot rename %s to %s (errno %d)", sTmpPath, sSavePath, err);
        return -1;
    }
    LWP_MutexUnlock(sSaveMutex);

    gc_log("save: wrote %s (%u bytes, %u ms)", sSavePath, size, (unsigned int)ticks_to_millisecs(gettime() - t0));
    return 0;
}
