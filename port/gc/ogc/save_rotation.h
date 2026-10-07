/**
 * Crash-safe saves in a recycled set of files (bridge_save.c keeps the 128 KB flash image in them on the SD card).
 * Plain C without libogc: every file operation goes through SaveFs, so the host test port/gc/tests/sd_host runs
 * the same code on a model of the card that can lose power after any step. See save_rotation.c for the protocol.
 */
#ifndef SAVE_ROTATION_H
#define SAVE_ROTATION_H

#define SAVE_PATH_MAX 96

/* The files of a save path P: P, P.bak, P.tmp, P.spare */
enum { SAVE_MAIN, SAVE_BAK, SAVE_TMP, SAVE_SPARE, SAVE_FILES };

typedef struct {
    int exists;
    long size;
    unsigned long cluster; /* first cluster (st_ino on libfat); 0 if the file is empty or it is unknown */
} SaveFileInfo;

typedef struct {
    void* ctx;
    /** Fill *info (exists 0 if there is no such file) */
    void (*stat)(void* ctx, const char* path, SaveFileInfo* info);
    /** Rename; fails if `to` exists. 0 on success. */
    int (*rename)(void* ctx, const char* from, const char* to);
    /** Write `size` bytes from offset 0 and sync them to the card. inPlace: the file exists and is overwritten without
     *  being truncated, so it keeps its clusters; else it is created (or truncated). 0 on success. */
    int (*write)(void* ctx, const char* path, const void* src, unsigned int size, int inPlace);
    void (*log)(void* ctx, const char* line);
} SaveFs;

typedef struct {
    char path[SAVE_FILES][SAVE_PATH_MAX];
} SaveSet;

void save_set_init(SaveSet* set, const char* path);

/** The file holding the newest complete save of `size` bytes (SAVE_MAIN to SAVE_SPARE), or -1 if there is none.
 *  Changes nothing. */
int save_pick(const SaveFs* fs, const SaveSet* set, unsigned int size);

/** Store a save of `size` bytes. *created is set to 1 if a file had to be created (clusters allocated), else 0.
 *  0 on success. On failure the files still hold the previous save (save_pick finds it), and the next store
 *  carries on from wherever this one stopped. */
int save_store(const SaveFs* fs, const SaveSet* set, const void* src, unsigned int size, int* created);

#endif
