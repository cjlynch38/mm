/**
 * Raw read access to files on a FAT12/16/32 volume, for files that never change while the program runs (the ROM
 * or the disc image on the SD card, see storage.c). Plain C without libogc: the host test port/gc/tests/sd_host
 * builds it too.
 *
 *  - fat_map_mount finds the volume as FatFs (libogc's libfat) does: sector 0 is a FAT boot sector, else it is an
 *    MBR and the volume is in the first of its four partition entries whose sector holds a FAT boot sector.
 *  - fat_map_find walks the directories to a file (long or 8.3 names, case-insensitive).
 *  - fat_map_extents follows the file's cluster chain once and turns it into runs of consecutive sectors.
 *  - fat_map_read serves any byte range of the file from those runs: whole sectors straight into the
 *    destination, a partial first or last sector through a bounce buffer.
 * Sectors are 512 bytes (libfat on the GameCube supports no other size). Every sector is read through a callback.
 */
#ifndef FAT_MAP_H
#define FAT_MAP_H

#include <stdint.h>

#define FAT_MAP_SECTOR_SIZE 512u

/** Read `count` sectors from `sector` on into `dst`. 0 on success. */
typedef int (*FatMapRead)(void* ctx, uint32_t sector, uint32_t count, void* dst);

/** A run of a file: `count` sectors from disc sector `sector` hold the file from byte `offset` on */
typedef struct {
    uint32_t offset; /* a multiple of the cluster size */
    uint32_t sector;
    uint32_t count;
} FatExtent;

/** A mounted volume */
typedef struct {
    FatMapRead read;
    void* ctx;
    unsigned char* buf;      /* scratch, bufSectors sectors */
    uint32_t bufSectors;
    uint32_t bufStart;       /* first sector held in buf */
    uint32_t bufCount;       /* sectors held (0: none) */
    int type;                /* 12, 16 or 32 */
    uint32_t volume;         /* boot sector */
    uint32_t fat;            /* first sector of the first FAT (FatFs reads only that one) */
    uint32_t fatSectors;     /* sectors per FAT */
    uint32_t root;           /* FAT12/16: first sector of the root directory. FAT32: its first cluster. */
    uint32_t rootSectors;    /* FAT12/16 */
    uint32_t data;           /* first sector of cluster 2 */
    uint32_t clusterSectors;
    uint32_t entries;        /* FAT entries: clusters 2 to entries - 1 exist */
} FatMap;

/** Find the volume. `buf` is scratch of `bufSectors` sectors (at least 1; more speeds up reading the FAT) and
 *  stays in use until the last call with `m`. NULL on success, else why not. */
const char* fat_map_mount(FatMap* m, FatMapRead read, void* ctx, void* buf, uint32_t bufSectors);

/** Find the file at `path` ('/'-separated, from the root; '.' and '..' work). NULL on success, with its first
 *  cluster (0 for an empty file) and size; else why not (also for a directory). */
const char* fat_map_find(FatMap* m, const char* path, uint32_t* cluster, uint32_t* size);

/** Follow the cluster chain of a file of `size` bytes from `cluster` and store its runs in out[0..max) (out may be
 *  NULL to count them). *count gets the number of runs, also when they do not fit. NULL on success, else why not
 *  (a broken chain, or more than `max` runs while out is not NULL). */
const char* fat_map_extents(FatMap* m, uint32_t cluster, uint32_t size, FatExtent* out, uint32_t max,
                            uint32_t* count);

/** Read [offset, offset + size) of a file from its runs; the range must lie in the file. `bounce` is one sector of
 *  scratch. 0 on success, -1 if a read failed or the range is outside the runs. */
int fat_map_read(const FatExtent* ext, uint32_t count, FatMapRead read, void* ctx, uint32_t offset, void* dst,
                 uint32_t size, void* bounce);

/** Bytes per cluster of a mounted volume */
uint32_t fat_map_cluster_size(const FatMap* m);

#endif
