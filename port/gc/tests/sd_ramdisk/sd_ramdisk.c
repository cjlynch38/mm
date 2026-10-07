/**
 * sd_ramdisk: test DOL for the SD card code of port/gc/ogc on the real libfat, in Dolphin (which has no GameCube SD
 * card). Volumes in RAM are mounted as sd: through storage.c's SD lock, the way an SD adapter is.
 *
 * A 4 MB FAT16 volume behind an MBR:
 *  - raw reads (gc_ogc_sd_raw_open/read) of files that libfat wrote, fragmented by interleaved writes, with long
 *    names, 8.3 names (libfat's own entries, numeric tails and case flags) and a subdirectory: the whole file and
 *    random ranges against the data, also while another thread writes a file through libfat;
 *  - the libfat behaviour the save rotation relies on: a rename fails onto an existing name, opening without
 *    O_TRUNC writes in place (same size, same first cluster), stat's st_ino is the first cluster;
 *  - saves (gc_save_store/load): the rotation, stores in place that allocate nothing (free clusters and the files'
 *    first clusters unchanged), a rotation interrupted after its first rename, and the files of earlier builds.
 * A sparse 1.6 GB FAT32 volume laid out like the user's card (MBR, volume at sector 8192, 4 KB clusters, a long
 * stretch of used clusters, cluster numbers above 0xFFFF, garbage in the FAT entries' reserved bits):
 *  - raw reads of fragmented files that libfat wrote there, and of a file whose chain jumps the used stretch;
 *  - FatFs's search for free clusters, with sector reads slowed to about the card's speed: a log opened the way
 *    bridge_log.c opens it (fopen "w", which truncates) restarts the search at its old first cluster and reads the
 *    FAT through the whole used stretch once it grows past its first clusters; raw reads meanwhile stay fast while
 *    a libfat read waits for the scan. remove() before fopen() avoids the scan as long as FSINFO has a next-free hint;
 *  - saves in place: the files' first clusters (above 0xFFFF) and the free clusters unchanged.
 * The summary line is "sd_ramdisk: RESULT PASS" or "sd_ramdisk: RESULT FAIL".
 *
 *   make -C port/gc/tests/sd_ramdisk
 *   port/gc/tools/run_dolphin.sh port/gc/tests/sd_ramdisk/build/sd_ramdisk.dol -Seconds 30 -NoSd
 */
#include <gccore.h>
#include <ogc/disc_io.h>
#include <ogc/lwp_watchdog.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include "gc_ogc.h"

#define SS 512u
#define RD_SECTORS 8192u
#define RD_VOLUME 64u /* the FAT16 volume's first sector, behind the MBR */
#define RD_FAT_SECTORS 32u
#define SAVE_SIZE 0x20000u
#define DIR "sd:/mmgcport"

static int sPassed;
static int sFailed;
static unsigned char* sDisk;
static volatile unsigned int sDiskReads;
static volatile unsigned int sDiskWrites;

static void check(int ok, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char* fmt, ...) {
    char msg[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if (ok) {
        sPassed++;
    } else {
        sFailed++;
    }
    gc_log("  %s %s", ok ? "pass" : "FAIL", msg);
}

/* ---------------------------------------------------------------------------------------------- */
/* The RAM disk                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

static bool rd_startup(void) {
    return sDisk != NULL;
}

static bool rd_inserted(void) {
    return sDisk != NULL;
}

static bool rd_read(sec_t sector, sec_t count, void* buffer) {
    if (sector >= RD_SECTORS || count > RD_SECTORS - sector) {
        return false;
    }
    memcpy(buffer, sDisk + sector * SS, count * SS);
    sDiskReads++;
    return true;
}

static bool rd_write(sec_t sector, sec_t count, const void* buffer) {
    if (sector >= RD_SECTORS || count > RD_SECTORS - sector) {
        return false;
    }
    memcpy(sDisk + sector * SS, buffer, count * SS);
    sDiskWrites++;
    return true;
}

static bool rd_true(void) {
    return true;
}

static const DISC_INTERFACE sRamDisk = {
    ('R' << 24) | ('A' << 16) | ('M' << 8) | 'D',
    FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE,
    rd_startup,
    rd_inserted,
    rd_read,
    rd_write,
    rd_true,
    rd_true,
};

static void put16(unsigned char* p, unsigned int v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void put32(unsigned char* p, unsigned int v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}

static unsigned int get32(const unsigned char* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned int)p[3] << 24);
}

/* An MBR with one FAT16 partition from RD_VOLUME on: 512-byte clusters, 2 FATs, 512 root entries */
static void format_ramdisk(void) {
    unsigned char* mbr = sDisk;
    unsigned char* bs = sDisk + RD_VOLUME * SS;
    unsigned int total = RD_SECTORS - RD_VOLUME;
    int i;

    memset(sDisk, 0, RD_SECTORS * SS);
    mbr[0] = 0xFA;
    mbr[1] = 0x33;
    mbr[2] = 0xC0;
    mbr[446 + 4] = 0x0E;
    put32(mbr + 446 + 8, RD_VOLUME);
    put32(mbr + 446 + 12, total);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    bs[0] = 0xEB;
    bs[1] = 0x3C;
    bs[2] = 0x90;
    memcpy(bs + 3, "MSWIN4.1", 8);
    put16(bs + 11, SS);
    bs[13] = 1;
    put16(bs + 14, 1);
    bs[16] = 2;
    put16(bs + 17, 512);
    put16(bs + 19, total);
    bs[21] = 0xF8;
    put16(bs + 22, RD_FAT_SECTORS);
    put16(bs + 24, 63);
    put16(bs + 26, 255);
    put32(bs + 28, RD_VOLUME);
    bs[36] = 0x80;
    bs[38] = 0x29;
    put32(bs + 39, 0x12345678);
    memcpy(bs + 43, "RAMDISK    ", 11);
    memcpy(bs + 54, "FAT16   ", 8);
    bs[510] = 0x55;
    bs[511] = 0xAA;
    for (i = 0; i < 2; i++) {
        unsigned char* fat = bs + (1 + i * RD_FAT_SECTORS) * SS;

        put16(fat, 0xFFF8);
        put16(fat + 2, 0xFFFF);
    }
}

/* ---------------------------------------------------------------------------------------------- */
/* Files                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

static unsigned char content(unsigned int seed, unsigned int n) {
    unsigned int x = (n + seed * 7919u) * 2654435761u;

    x ^= x >> 15;
    x *= 0x2C1B3C6Du;
    x ^= x >> 12;
    return (unsigned char)((x >> 24) ^ (n >> 16));
}

static void fill(unsigned char* buf, unsigned int seed, unsigned int offset, unsigned int size) {
    unsigned int i;

    for (i = 0; i < size; i++) {
        buf[i] = content(seed, offset + i);
    }
}

typedef struct {
    const char* path;
    unsigned int seed;
    unsigned int size;
    unsigned int written;
    int fd;
} TestFile;

/* Write the files in turns of `chunk` bytes, so libfat interleaves their clusters */
static void write_interleaved(TestFile* files, int count, unsigned int chunk) {
    unsigned char* buf = malloc(chunk);
    int left = count;
    int i;

    for (i = 0; i < count; i++) {
        files[i].written = 0;
        files[i].fd = open(files[i].path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        check(files[i].fd >= 0, "create %s", files[i].path);
    }
    while (left > 0) {
        left = 0;
        for (i = 0; i < count; i++) {
            TestFile* f = &files[i];
            unsigned int n = f->size - f->written;

            if (n == 0 || f->fd < 0) {
                continue;
            }
            n = (n < chunk) ? n : chunk;
            fill(buf, f->seed, f->written, n);
            if (write(f->fd, buf, n) != (int)n) {
                check(0, "write to %s at %u", f->path, f->written);
                close(f->fd);
                f->fd = -1;
                continue;
            }
            fsync(f->fd);
            f->written += n;
            left += (f->written < f->size);
        }
    }
    for (i = 0; i < count; i++) {
        if (files[i].fd >= 0) {
            close(files[i].fd);
        }
    }
    free(buf);
}

static unsigned int sRandom = 0x9E3779B9u;
static unsigned int rnd(void) {
    sRandom ^= sRandom << 13;
    sRandom ^= sRandom >> 17;
    sRandom ^= sRandom << 5;
    return sRandom;
}

/* Raw reads of a file: the whole file, then random ranges into odd addresses */
static GcSdRaw* check_raw(const char* path, unsigned int seed, unsigned int size, int ranges) {
    FILE* file = fopen(path, "rb");
    unsigned char* got = malloc(size + 64);
    unsigned char* want = malloc(size);
    GcSdRaw* raw = NULL;
    int bad = 0;
    int i;

    if (file == NULL || got == NULL || want == NULL) {
        check(0, "%s: cannot open it or no memory", path);
        goto done;
    }
    setvbuf(file, NULL, _IONBF, 0);
    raw = gc_ogc_sd_raw_open(path, file);
    check(raw != NULL, "%s: raw reads set up", path);
    if (raw == NULL) {
        goto done;
    }
    check(gc_ogc_sd_raw_size(raw) == size, "%s: raw size %u, expected %u", path, gc_ogc_sd_raw_size(raw), size);
    fill(want, seed, 0, size);
    check(gc_ogc_sd_raw_read(raw, 0, got, size) == 0 && memcmp(got, want, size) == 0, "%s: whole file (%u bytes)",
          path, size);
    for (i = 0; i < ranges; i++) {
        unsigned int off = rnd() % size;
        unsigned int len = 1 + rnd() % ((size - off < 20000) ? size - off : 20000);
        unsigned int shift = rnd() % 32;

        if (gc_ogc_sd_raw_read(raw, off, got + shift, len) != 0 || memcmp(got + shift, want + off, len) != 0) {
            bad++;
        }
    }
    check(bad == 0, "%s: %d random ranges, %d wrong", path, ranges, bad);
    check(gc_ogc_sd_raw_read(raw, size, got, 1) != 0, "%s: a read past the end is refused", path);
done:
    if (file != NULL) {
        fclose(file);
    }
    free(got);
    free(want);
    return raw;
}

/* ---------------------------------------------------------------------------------------------- */
/* Raw reads                                                                                      */
/* ---------------------------------------------------------------------------------------------- */

static volatile int sWriterDone;
static volatile int sWriterOk;

#define LOG_CHUNK 2048u
#define LOG_CHUNKS 150u

/* Appends to a file through libfat, with a sync after every chunk, while the main thread reads raw */
static void* writer_entry(void* arg) {
    unsigned char* buf = malloc(LOG_CHUNK);
    int fd = open(DIR "/writer.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    unsigned int i;

    sWriterOk = (fd >= 0 && buf != NULL);
    for (i = 0; i < LOG_CHUNKS && sWriterOk; i++) {
        fill(buf, 99, i * LOG_CHUNK, LOG_CHUNK);
        sWriterOk = write(fd, buf, LOG_CHUNK) == (int)LOG_CHUNK && fsync(fd) == 0;
        usleep(200);
    }
    if (fd >= 0) {
        close(fd);
    }
    free(buf);
    sWriterDone = 1;
    return NULL;
}

static void test_raw(void) {
    TestFile files[] = {
        { DIR "/Mm-Gc Test Image.iso", 1, 1200 * 1024 + 77, 0, -1 },
        { DIR "/other.bin", 2, 400 * 1024, 0, -1 },
        { DIR "/sub dir/a much longer file name than thirteen characters.bin", 3, 70 * 1024 + 1, 0, -1 },
        { DIR "/Long name 1.bin", 4, 9000, 0, -1 },
    };
    GcSdRaw* image;
    lwp_t writer;
    unsigned char* got;
    unsigned char* want;
    int bad = 0;
    int reads = 0;
    FILE* f;

    gc_log("raw reads:");
    mkdir(DIR, 0777);
    mkdir(DIR "/sub dir", 0777);
    write_interleaved(files, 4, 3000);
    image = check_raw(files[0].path, files[0].seed, files[0].size, 200);
    check_raw(files[1].path, files[1].seed, files[1].size, 100);
    check_raw(files[2].path, files[2].seed, files[2].size, 50);
    // libfat's 8.3 name of a long name, and the long name
    check_raw(DIR "/LONGNA~1.BIN", files[3].seed, files[3].size, 20);
    check_raw(files[3].path, files[3].seed, files[3].size, 20);
    // An empty file is read through libfat
    f = fopen(DIR "/empty.bin", "wb");
    if (f != NULL) {
        fclose(f);
        f = fopen(DIR "/empty.bin", "rb");
        check(f != NULL && gc_ogc_sd_raw_open(DIR "/empty.bin", f) == NULL, "an empty file gets no raw reads");
        if (f != NULL) {
            fclose(f);
        }
    }

    // Raw reads while another thread writes through libfat
    if (image == NULL) {
        return;
    }
    got = malloc(20000 + 32);
    want = malloc(20000);
    sWriterDone = 0;
    LWP_CreateThread(&writer, writer_entry, NULL, NULL, 0x8000, 40);
    while (!sWriterDone || reads < 100) {
        unsigned int off = rnd() % files[0].size;
        unsigned int len = 1 + rnd() % ((files[0].size - off < 20000) ? files[0].size - off : 20000);

        fill(want, files[0].seed, off, len);
        if (gc_ogc_sd_raw_read(image, off, got + 3, len) != 0 || memcmp(got + 3, want, len) != 0) {
            bad++;
        }
        reads++;
        usleep(100);
    }
    check(sWriterOk, "the writer thread wrote %u KB through libfat", LOG_CHUNKS * LOG_CHUNK / 1024);
    check(bad == 0, "%d raw reads beside it, %d wrong", reads, bad);
    f = fopen(DIR "/writer.log", "rb");
    if (f != NULL) {
        unsigned int i;

        bad = 0;
        for (i = 0; i < LOG_CHUNKS; i++) {
            if (fread(got, 1, LOG_CHUNK, f) != LOG_CHUNK) {
                bad++;
                break;
            }
            fill(want, 99, i * LOG_CHUNK, LOG_CHUNK);
            bad += memcmp(got, want, LOG_CHUNK) != 0;
        }
        fclose(f);
        check(bad == 0, "the writer's file reads back through libfat");
    }
    free(got);
    free(want);
}

/* ---------------------------------------------------------------------------------------------- */
/* Saves                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

static long size_of(const char* path) {
    struct stat st;

    return (stat(path, &st) == 0) ? (long)st.st_size : -1;
}

static unsigned long first_cluster(const char* path) {
    struct stat st;

    return (stat(path, &st) == 0) ? (unsigned long)st.st_ino : 0;
}

static unsigned long free_blocks(void) {
    struct statvfs vfs;

    return (statvfs("sd:/", &vfs) == 0) ? (unsigned long)vfs.f_bfree : 0;
}

static int write_file(const char* path, unsigned int seed, unsigned int size, int flags) {
    unsigned char* buf = malloc(size);
    int fd = open(path, flags, 0666);
    int ok;

    fill(buf, seed, 0, size);
    ok = fd >= 0 && write(fd, buf, size) == (int)size && fsync(fd) == 0;
    if (fd >= 0) {
        close(fd);
    }
    free(buf);
    return ok;
}

static int load_is(unsigned int seed) {
    unsigned char* buf = malloc(SAVE_SIZE);
    unsigned char* want = malloc(SAVE_SIZE);
    int ok;

    fill(want, seed, 0, SAVE_SIZE);
    ok = gc_save_load(buf, SAVE_SIZE) == 0 && memcmp(buf, want, SAVE_SIZE) == 0;
    free(buf);
    free(want);
    return ok;
}

static int store(unsigned int seed) {
    unsigned char* buf = malloc(SAVE_SIZE);
    int ok;

    fill(buf, seed, 0, SAVE_SIZE);
    ok = gc_save_store(buf, SAVE_SIZE) == 0;
    free(buf);
    return ok;
}

static void test_libfat_semantics(void) {
    unsigned long cluster;

    gc_log("libfat behaviour the saves rely on:");
    check(write_file(DIR "/sem.a", 50, 4000, O_WRONLY | O_CREAT | O_TRUNC) &&
              write_file(DIR "/sem.b", 51, 4000, O_WRONLY | O_CREAT | O_TRUNC),
          "two files written");
    check(rename(DIR "/sem.a", DIR "/sem.b") != 0 && size_of(DIR "/sem.a") == 4000,
          "a rename onto an existing name fails and changes nothing");
    cluster = first_cluster(DIR "/sem.a");
    check(cluster >= 2, "stat gives the first cluster as st_ino (%lu)", cluster);
    check(write_file(DIR "/sem.a", 52, 100, O_WRONLY) && size_of(DIR "/sem.a") == 4000 &&
              first_cluster(DIR "/sem.a") == cluster,
          "open without O_TRUNC writes in place: same size and first cluster");
    check(rename(DIR "/sem.a", DIR "/sem.c") == 0 && first_cluster(DIR "/sem.c") == cluster,
          "a rename keeps the first cluster");
    remove(DIR "/sem.b");
    remove(DIR "/sem.c");
}

static void test_saves(void) {
    static const char* const names[3] = { DIR "/rd.fla", DIR "/rd.fla.bak", DIR "/rd.fla.tmp" };
    unsigned long before[3];
    unsigned long after[3];
    unsigned long freeBefore;
    unsigned int v;
    int i;

    gc_log("saves:");
    gc_ogc_save_set_path(DIR "/rd.fla");
    {
        unsigned char* buf = malloc(SAVE_SIZE);

        check(gc_save_load(buf, SAVE_SIZE) == 1, "no save yet");
        free(buf);
    }
    for (v = 1; v <= 3; v++) {
        check(store(v) && load_is(v), "store and load %u", v);
    }
    check(size_of(names[0]) == SAVE_SIZE && size_of(names[1]) == SAVE_SIZE && size_of(names[2]) == SAVE_SIZE &&
              size_of(DIR "/rd.fla.spare") < 0,
          "after 3 stores: mm.fla, .bak, .tmp, no .spare");
    for (i = 0; i < 3; i++) {
        before[i] = first_cluster(names[i]);
    }
    freeBefore = free_blocks();
    for (v = 4; v <= 6; v++) {
        check(store(v) && load_is(v), "store and load %u", v);
    }
    for (i = 0; i < 3; i++) {
        after[i] = first_cluster(names[i]);
    }
    // Three stores rotate the three files once round
    check(after[0] == before[0] && after[1] == before[1] && after[2] == before[2] && before[0] != 0,
          "stores 4-6 kept the files' clusters (first clusters %lu %lu %lu, then %lu %lu %lu)", before[0], before[1],
          before[2], after[0], after[1], after[2]);
    check(free_blocks() == freeBefore && freeBefore != 0, "stores 4-6 allocated nothing (%lu free clusters, then %lu)",
          freeBefore, free_blocks());

    // A power cut after the first rename of a store: .bak renamed to .spare, the new save complete in .tmp
    check(rename(names[1], DIR "/rd.fla.spare") == 0 && write_file(names[2], 7, SAVE_SIZE, O_WRONLY),
          "set up a rotation cut after its first rename");
    check(load_is(7), "load takes the new save from .tmp while .spare exists");
    check(store(8) && load_is(8) && size_of(DIR "/rd.fla.spare") < 0 && size_of(names[1]) == SAVE_SIZE &&
              size_of(names[2]) == SAVE_SIZE,
          "the next store finishes the renames and stores 8");
    check(free_blocks() == freeBefore, "and allocates nothing");
    check(remove(names[0]) == 0 && load_is(7), "with mm.fla deleted, load takes .bak (7)");

    // Files of an earlier build: mm.fla and mm.fla.bak
    gc_log("saves of an earlier build:");
    gc_ogc_save_set_path(DIR "/old.fla");
    check(write_file(DIR "/old.fla", 20, SAVE_SIZE, O_WRONLY | O_CREAT | O_TRUNC) &&
              write_file(DIR "/old.fla.bak", 19, SAVE_SIZE, O_WRONLY | O_CREAT | O_TRUNC),
          "write mm.fla and .bak as an earlier build left them");
    check(load_is(20), "load takes their mm.fla");
    check(store(21) && load_is(21) && size_of(DIR "/old.fla.tmp") == SAVE_SIZE, "the first store adds .tmp");
    freeBefore = free_blocks();
    check(store(22) && store(23) && load_is(23), "two more stores");
    check(free_blocks() == freeBefore, "they allocate nothing");
    gc_ogc_save_set_path(GC_SAVE_PATH);
}

/* ---------------------------------------------------------------------------------------------- */
/* The FAT32 RAM disk                                                                             */
/* ---------------------------------------------------------------------------------------------- */

/*
 * Laid out like the user's 128 GB card where it matters: an MBR, the volume at sector 8192, 4 KB clusters, a long
 * stretch of used clusters after a small hole. Only written sectors are stored, in 4 KB pages; the others are
 * generated. The FATs mark cluster 2 (the root directory) and F32_STRETCH clusters from F32_STRETCH_FIRST on (one
 * chain without a directory entry, standing in for the card's other files) as used, and every other cluster as free,
 * odd ones with garbage in the reserved top 4 bits (FatFs keeps those bits when it allocates, so libfat's chains
 * carry them). Everything else reads as zeros. Reads can be slowed to about the card's speed (sSlowUs per sector).
 */
#define F32_VOLUME 8192u
#define F32_RESERVED 32u
#define F32_CLUSTER_SECTORS 8u
#define F32_CLUSTERS 400000u
#define F32_FAT_SECTORS 3128u /* (F32_CLUSTERS + 2) * 4 bytes, rounded up so that clusters are 4 KB-aligned */
#define F32_FAT (F32_VOLUME + F32_RESERVED)
#define F32_DATA (F32_FAT + 2 * F32_FAT_SECTORS)
#define F32_VOLUME_SECTORS (F32_RESERVED + 2 * F32_FAT_SECTORS + F32_CLUSTERS * F32_CLUSTER_SECTORS)
#define F32_SECTORS (F32_VOLUME + F32_VOLUME_SECTORS)
#define F32_STRETCH_FIRST 7u /* clusters 3-6 are free: the hole */
#define F32_STRETCH 250000u
#define F32_STRETCH_END (F32_STRETCH_FIRST + F32_STRETCH)
#define F32_STRETCH_FAT_SECTORS (F32_STRETCH * 4 / SS)
/* FAT sectors read by a search through the stretch, at least (FAT pages still in libfat's cache are not read) */
#define F32_SEARCHED (F32_STRETCH_FAT_SECTORS * 9 / 10)
#define F32_PAGE_SECTORS 8u
#define F32_PAGES 1024u  /* 4 MB of written sectors */
#define F32_SLOTS 2048u  /* hash table of the stored pages, a power of two */
#define F32_SLOW_US 400u /* per sector read: about 1.2 MB/s, the card's speed in the console logs */

static unsigned char* sPages;
static unsigned int sPagesUsed;
static unsigned int sSlotKey[F32_SLOTS]; /* page number + 1; 0: a free slot */
static unsigned short sSlotPage[F32_SLOTS];
static int sDiskFull;
static volatile unsigned int sFatReads; /* FAT sectors read from the disk */
static volatile unsigned int sSlowUs;
static unsigned int f32_entry(unsigned int n) {
    if (n < 3) {
        return (n == 0) ? 0x0FFFFFF8u : 0x0FFFFFFFu;
    }
    if (n >= F32_STRETCH_FIRST && n < F32_STRETCH_END) {
        return (n + 1 < F32_STRETCH_END) ? n + 1 : 0x0FFFFFFFu;
    }
    if (n >= F32_CLUSTERS + 2) {
        return 0;
    }
    return (n & 1) ? 0xA0000000u : 0; // free
}

static void f32_generate(unsigned int sector, unsigned char* dst) {
    unsigned int first;
    unsigned int i;

    memset(dst, 0, SS);
    if (sector >= F32_FAT && sector < F32_DATA) {
        first = ((sector - F32_FAT) % F32_FAT_SECTORS) * (SS / 4);
        for (i = 0; i < SS / 4; i++) {
            put32(dst + i * 4, f32_entry(first + i));
        }
    }
}

/* Stored page `index` (sectors index * F32_PAGE_SECTORS on); `create` stores it, from the generated sectors */
static unsigned char* f32_page(unsigned int index, int create) {
    unsigned int slot = (index * 2654435761u) & (F32_SLOTS - 1);
    unsigned char* page;
    unsigned int i;

    while (sSlotKey[slot] != 0) {
        if (sSlotKey[slot] == index + 1) {
            return sPages + sSlotPage[slot] * F32_PAGE_SECTORS * SS;
        }
        slot = (slot + 1) & (F32_SLOTS - 1);
    }
    if (!create) {
        return NULL;
    }
    if (sPagesUsed == F32_PAGES) {
        sDiskFull = 1;
        return NULL;
    }
    sSlotKey[slot] = index + 1;
    sSlotPage[slot] = (unsigned short)sPagesUsed;
    page = sPages + sPagesUsed++ * F32_PAGE_SECTORS * SS;
    for (i = 0; i < F32_PAGE_SECTORS; i++) {
        f32_generate(index * F32_PAGE_SECTORS + i, page + i * SS);
    }
    return page;
}

static bool rd32_present(void) {
    return sPages != NULL;
}

static bool rd32_read(sec_t sector, sec_t count, void* buffer) {
    unsigned char* dst = buffer;

    if (sector >= F32_SECTORS || count > F32_SECTORS - sector) {
        return false;
    }
    if (sSlowUs != 0) {
        usleep(count * sSlowUs);
    }
    for (; count != 0; count--, sector++, dst += SS) {
        const unsigned char* page = f32_page(sector / F32_PAGE_SECTORS, 0);

        if (page != NULL) {
            memcpy(dst, page + (sector % F32_PAGE_SECTORS) * SS, SS);
        } else {
            f32_generate(sector, dst);
        }
        if (sector >= F32_FAT && sector < F32_DATA) {
            sFatReads++;
        }
    }
    sDiskReads++;
    return true;
}

static bool rd32_write(sec_t sector, sec_t count, const void* buffer) {
    const unsigned char* src = buffer;

    if (sector >= F32_SECTORS || count > F32_SECTORS - sector) {
        return false;
    }
    for (; count != 0; count--, sector++, src += SS) {
        unsigned char* page = f32_page(sector / F32_PAGE_SECTORS, 1);

        if (page == NULL) {
            return false;
        }
        memcpy(page + (sector % F32_PAGE_SECTORS) * SS, src, SS);
    }
    sDiskWrites++;
    return true;
}

static const DISC_INTERFACE sRamDisk32 = {
    ('R' << 24) | ('D' << 16) | ('3' << 8) | '2',
    FEATURE_MEDIUM_CANREAD | FEATURE_MEDIUM_CANWRITE,
    rd32_present,
    rd32_present,
    rd32_read,
    rd32_write,
    rd_true,
    rd_true,
};

static void f32_put(unsigned int sector, const unsigned char* src) {
    unsigned char* page = f32_page(sector / F32_PAGE_SECTORS, 1);

    if (page != NULL) {
        memcpy(page + (sector % F32_PAGE_SECTORS) * SS, src, SS);
    }
}

/* A FAT entry as the disk holds it, reserved bits included (no libfat call may be under way) */
static unsigned int f32_fat_entry(unsigned int cluster) {
    unsigned int sector = F32_FAT + cluster / (SS / 4);
    const unsigned char* page = f32_page(sector / F32_PAGE_SECTORS, 0);
    unsigned char buf[SS];

    if (page != NULL) {
        memcpy(buf, page + (sector % F32_PAGE_SECTORS) * SS, SS);
    } else {
        f32_generate(sector, buf);
    }
    return get32(buf + (cluster % (SS / 4)) * 4);
}

/* FSINFO's next-free hint (FSI_Nxt_Free), read or set directly on the disk while it is not mounted */
static unsigned int f32_next_free(void) {
    const unsigned char* page = f32_page((F32_VOLUME + 1) / F32_PAGE_SECTORS, 0);

    return (page != NULL) ? get32(page + ((F32_VOLUME + 1) % F32_PAGE_SECTORS) * SS + 492) : 0;
}

static void f32_set_next_free(unsigned int cluster) {
    unsigned char* page = f32_page((F32_VOLUME + 1) / F32_PAGE_SECTORS, 0);

    if (page != NULL) {
        put32(page + ((F32_VOLUME + 1) % F32_PAGE_SECTORS) * SS + 492, cluster);
    }
}

/* The MBR, the boot sector and FSINFO (with their backups at 6 and 7). FSINFO: the right free count, next free 2. */
static void format_fat32(void) {
    unsigned char s[SS];

    memset(sSlotKey, 0, sizeof(sSlotKey));
    sPagesUsed = 0;
    sDiskFull = 0;

    memset(s, 0, SS);
    s[0] = 0xFA;
    s[1] = 0x33;
    s[2] = 0xC0;
    s[446 + 4] = 0x0C;
    put32(s + 446 + 8, F32_VOLUME);
    put32(s + 446 + 12, F32_VOLUME_SECTORS);
    s[510] = 0x55;
    s[511] = 0xAA;
    f32_put(0, s);

    memset(s, 0, SS);
    s[0] = 0xEB;
    s[1] = 0x58;
    s[2] = 0x90;
    memcpy(s + 3, "MSWIN4.1", 8);
    put16(s + 11, SS);
    s[13] = F32_CLUSTER_SECTORS;
    put16(s + 14, F32_RESERVED);
    s[16] = 2;
    s[21] = 0xF8;
    put16(s + 24, 63);
    put16(s + 26, 255);
    put32(s + 28, F32_VOLUME);
    put32(s + 32, F32_VOLUME_SECTORS);
    put32(s + 36, F32_FAT_SECTORS);
    put32(s + 44, 2);
    put16(s + 48, 1);
    put16(s + 50, 6);
    s[64] = 0x80;
    s[66] = 0x29;
    put32(s + 67, 0x2468ACE0);
    memcpy(s + 71, "RAMDISK32  ", 11);
    memcpy(s + 82, "FAT32   ", 8);
    s[510] = 0x55;
    s[511] = 0xAA;
    f32_put(F32_VOLUME, s);
    f32_put(F32_VOLUME + 6, s);

    memset(s, 0, SS);
    put32(s, 0x41615252);
    put32(s + 484, 0x61417272);
    put32(s + 488, F32_CLUSTERS - 1 - F32_STRETCH);
    put32(s + 492, 2);
    put32(s + 508, 0xAA550000);
    f32_put(F32_VOLUME + 1, s);
    f32_put(F32_VOLUME + 7, s);
}

/* Unmount and mount the volume again: FatFs reads FSINFO afresh, as after a reboot */
static int f32_remount(void) {
    int ok = gc_ogc_sd_unmount() == 0 && gc_ogc_sd_mount_iface(&sRamDisk32) == 0;

    check(ok, "unmount and mount the FAT32 volume again, as a reboot does");
    return ok;
}

/* ---------------------------------------------------------------------------------------------- */
/* FatFs's search for free clusters                                                               */
/* ---------------------------------------------------------------------------------------------- */

#define SCAN_LOG DIR "/log-scan.txt"
#define SCAN_LOG_SIZE 0x10000u /* 16 clusters */
#define SCAN_RAW_SIZE 4096u

/*
 * libfat's cache holds 4 pages of 32 KB. Each write of the log that allocates a cluster also reads back 3 of them
 * (the FAT page, its mirror in the second FAT, FSINFO's page), which the fsync before it pushed out, so the FAT
 * sectors read in total grow with the log. A search shows as one write that reads many more.
 */
typedef struct {
    FILE* file;
    unsigned int seed;
    volatile int done;
    int ok;
    volatile int inWrite;            /* in fwrite/fflush/fsync of a chunk */
    volatile unsigned int writeFat0; /* sFatReads when that began */
    unsigned int slowestUs;          /* the longest chunk */
    unsigned int slowestFat;         /* FAT sectors read during it */
} LogWriter;

typedef struct {
    int ok;
    unsigned int fatReads;   /* FAT sectors read from the open to the last write */
    unsigned int ms;         /* the time that took */
    unsigned int slowestUs;  /* the longest write of a chunk: libfat's volume lock held in one go */
    unsigned int slowestFat; /* FAT sectors read during it */
    unsigned int rawReads;   /* raw reads meanwhile */
    unsigned int rawWrong;
    unsigned int rawSlowest; /* us */
    int libfatWaited;        /* us a libfat read waited during a long write; -1: none was made */
} LogRun;

/* bridge_log.c's log writer: fwrite, fflush and fsync per chunk */
static void* log_writer_entry(void* arg) {
    LogWriter* w = arg;
    unsigned char* buf = malloc(LOG_CHUNK);
    unsigned int at;

    w->ok = (buf != NULL);
    for (at = 0; at < SCAN_LOG_SIZE && w->ok; at += LOG_CHUNK) {
        u64 t0;
        unsigned int us;

        fill(buf, w->seed, at, LOG_CHUNK);
        w->writeFat0 = sFatReads;
        w->inWrite = 1;
        t0 = gettime();
        w->ok = fwrite(buf, 1, LOG_CHUNK, w->file) == LOG_CHUNK && fflush(w->file) == 0 &&
                fsync(fileno(w->file)) == 0;
        us = (unsigned int)ticks_to_microsecs(gettime() - t0);
        w->inWrite = 0;
        if (us > w->slowestUs) {
            w->slowestUs = us;
            w->slowestFat = sFatReads - w->writeFat0;
        }
    }
    free(buf);
    w->done = 1;
    return NULL;
}

/*
 * Write SCAN_LOG as bridge_log.c does: fopen(path, "w") (after remove(path) if `removeFirst`), then a writer thread
 * at idle priority, with sector reads slowed to the card's speed. Meanwhile the main thread reads `rawFile` (if not
 * NULL) through `raw`, and once a write has read more FAT than any write without a search does, reads 4 KB of `fd`
 * (if not -1) through libfat.
 */
static void log_run(int removeFirst, unsigned int seed, GcSdRaw* raw, const TestFile* rawFile, int fd, LogRun* run) {
    unsigned char* got = malloc(SCAN_RAW_SIZE);
    unsigned char* want = malloc(SCAN_RAW_SIZE);
    unsigned int fat0 = sFatReads;
    u64 t0 = gettime();
    LogWriter w;
    lwp_t thread;

    memset(run, 0, sizeof(*run));
    run->libfatWaited = -1;
    if (removeFirst) {
        remove(SCAN_LOG);
    }
    memset(&w, 0, sizeof(w));
    w.file = fopen(SCAN_LOG, "w");
    w.seed = seed;
    if (w.file == NULL || got == NULL || want == NULL ||
        LWP_CreateThread(&thread, log_writer_entry, &w, NULL, 0x8000, GC_PRIO_IDLE) != 0) {
        check(0, "open %s and start its writer", SCAN_LOG);
        if (w.file != NULL) {
            fclose(w.file);
        }
        free(got);
        free(want);
        return;
    }
    sSlowUs = F32_SLOW_US;
    while (!w.done) {
        if (raw != NULL) {
            unsigned int off = rnd() % (rawFile->size - SCAN_RAW_SIZE);
            u64 r0 = gettime();
            unsigned int us;

            if (gc_ogc_sd_raw_read(raw, off, got, SCAN_RAW_SIZE) != 0) {
                run->rawWrong++;
            }
            us = (unsigned int)ticks_to_microsecs(gettime() - r0);
            run->rawSlowest = (us > run->rawSlowest) ? us : run->rawSlowest;
            fill(want, rawFile->seed, off, SCAN_RAW_SIZE);
            run->rawWrong += memcmp(got, want, SCAN_RAW_SIZE) != 0;
            run->rawReads++;
        }
        // A write that has read more than 3 pages of FAT is searching
        if (fd >= 0 && run->libfatWaited < 0 && w.inWrite && sFatReads - w.writeFat0 > 3 * 64) {
            u64 r0 = gettime();

            if (read(fd, got, SCAN_RAW_SIZE) == (int)SCAN_RAW_SIZE) {
                run->libfatWaited = (int)ticks_to_microsecs(gettime() - r0);
            }
        }
        usleep(1000);
    }
    LWP_JoinThread(thread, NULL);
    sSlowUs = 0;
    run->ms = (unsigned int)ticks_to_millisecs(gettime() - t0);
    run->fatReads = sFatReads - fat0;
    run->slowestUs = w.slowestUs;
    run->slowestFat = w.slowestFat;
    run->ok = (fclose(w.file) == 0) && w.ok;
    free(got);
    free(want);
}

static void test_fat32(void) {
    static const char* const names[3] = { DIR "/f32.fla", DIR "/f32.fla.bak", DIR "/f32.fla.tmp" };
    TestFile files[] = {
        { DIR "/MM-GC FAT32 Image.iso", 11, 700 * 1024 + 3, 0, -1 },
        { DIR "/fat32 sub dir/a long name on the FAT32 volume, with a comma.bin", 12, 150 * 1024, 0, -1 },
        { DIR "/F32SHORT.BIN", 13, 40 * 1024 + 9, 0, -1 },
    };
    unsigned long before[3];
    unsigned long after[3];
    unsigned long freeBefore;
    unsigned long cluster;
    unsigned int next;
    unsigned int v;
    GcSdRaw* raw = NULL;
    LogRun a;
    LogRun b;
    LogRun c;
    FILE* f;
    int fd;
    int ok;
    int i;

    gc_log("FAT32 volume (sparse; MBR, volume at sector %u, 4 KB clusters, clusters %u-%u used):", F32_VOLUME,
           F32_STRETCH_FIRST, F32_STRETCH_END - 1);
    sPages = gc_mem_alloc(F32_PAGES * F32_PAGE_SECTORS * SS, 32);
    if (sPages == NULL) {
        check(0, "memory for the FAT32 RAM disk");
        return;
    }
    format_fat32();
    ok = gc_ogc_sd_unmount() == 0 && gc_ogc_sd_mount_iface(&sRamDisk32) == 0;
    check(ok, "unmount the FAT16 volume, mount the FAT32 one as sd:");
    if (!ok) {
        return;
    }

    // A log of an earlier session: its first clusters in the hole, the rest past the stretch (as on the card)
    mkdir(DIR, 0777);
    log_run(0, 21, NULL, NULL, -1, &a);
    cluster = first_cluster(SCAN_LOG);
    check(a.ok && cluster == 4 && a.slowestFat >= F32_SEARCHED,
          "a new %u KB log from cluster %lu on: its 4th cluster took a search through the used stretch (one write "
          "read %u FAT sectors in %u ms)",
          SCAN_LOG_SIZE / 1024, cluster, a.slowestFat, a.slowestUs / 1000);

    gc_log("raw reads on FAT32:");
    mkdir(DIR "/fat32 sub dir", 0777);
    write_interleaved(files, 3, 3000);
    for (i = 0; i < 3; i++) {
        check_raw(files[i].path, files[i].seed, files[i].size, 50);
    }
    cluster = first_cluster(files[0].path);
    check(cluster > 0xFFFF, "the files' clusters are above 0xFFFF (first cluster %lu)", cluster);
    {
        unsigned int at = (unsigned int)cluster;
        unsigned int marked = 0;
        unsigned int n;

        for (n = 0; n < 1000 && at >= 2 && at < F32_CLUSTERS + 2; n++) {
            unsigned int entry = f32_fat_entry(at);

            marked += (entry >> 28) == 0xA;
            at = entry & 0x0FFFFFFF;
        }
        check(marked != 0 && at >= 0x0FFFFFF8, "%s: %u of its %u FAT entries keep garbage in the reserved bits",
              files[0].path, marked, n);
    }

    gc_log("the log opened as bridge_log.c opens it (fopen \"w\"), after a reboot:");
    if (!f32_remount()) {
        return;
    }
    f = fopen(files[0].path, "rb");
    if (f != NULL) {
        setvbuf(f, NULL, _IONBF, 0);
        raw = gc_ogc_sd_raw_open(files[0].path, f);
    }
    fd = open(files[1].path, O_RDONLY);
    log_run(0, 22, raw, &files[0], fd, &a);
    if (fd >= 0) {
        close(fd);
    }
    if (f != NULL) {
        fclose(f);
    }
    cluster = first_cluster(SCAN_LOG);
    check(a.ok && cluster == 4, "fopen \"w\" truncated the log, and FatFs reused its old first cluster (%lu)", cluster);
    check(a.slowestFat >= F32_SEARCHED,
          "growing past those 3 clusters searched the FAT through the used stretch: one write read %u FAT sectors (the "
          "stretch takes %u) and held libfat's lock %u ms",
          a.slowestFat, F32_STRETCH_FAT_SECTORS, a.slowestUs / 1000);
    check(a.libfatWaited >= 200000, "a libfat read meanwhile waited %d ms for it", a.libfatWaited / 1000);
    check(raw != NULL && a.rawReads >= 20 && a.rawWrong == 0 && a.rawSlowest * 5 < a.slowestUs,
          "%u raw reads meanwhile, %u wrong, the slowest %u us", a.rawReads, a.rawWrong, a.rawSlowest);

    gc_log("the log removed before fopen \"w\", after a reboot:");
    if (!f32_remount()) {
        return;
    }
    log_run(1, 23, NULL, NULL, -1, &b);
    cluster = first_cluster(SCAN_LOG);
    check(b.ok && cluster >= F32_STRETCH_END, "the new log starts at FSINFO's next-free hint, past the stretch (%lu)",
          cluster);
    check(b.slowestFat <= 3 * 64 && b.slowestUs * 4 < a.slowestUs,
          "no search: the longest write read %u FAT sectors in %u ms (fopen \"w\" alone: %u in %u ms); %u in all",
          b.slowestFat, b.slowestUs / 1000, a.slowestFat, a.slowestUs / 1000, b.fatReads);

    gc_log("the same with no next-free hint in FSINFO:");
    ok = gc_ogc_sd_unmount() == 0;
    if (ok) {
        f32_set_next_free(0xFFFFFFFFu);
        ok = gc_ogc_sd_mount_iface(&sRamDisk32) == 0;
    }
    check(ok, "FSINFO's next free set to 0xFFFFFFFF (unknown), the volume mounted again");
    if (!ok) {
        return;
    }
    log_run(1, 24, NULL, NULL, -1, &c);
    cluster = first_cluster(SCAN_LOG);
    check(c.ok && cluster == 4 && c.slowestFat >= F32_SEARCHED,
          "FatFs searches from cluster 2: the log takes the hole again and its growth searches the stretch (first "
          "cluster %lu; one write read %u FAT sectors in %u ms)",
          cluster, c.slowestFat, c.slowestUs / 1000);
    if (!f32_remount()) {
        return;
    }
    next = f32_next_free();
    check(next >= F32_STRETCH_END && next < F32_CLUSTERS + 2,
          "FatFs wrote a next-free hint to FSINFO again (%u), so the next boot needs no search", next);
    // Its chain jumps the stretch
    check_raw(SCAN_LOG, 24, SCAN_LOG_SIZE, 20);

    gc_log("saves on FAT32:");
    gc_ogc_save_set_path(names[0]);
    for (v = 31; v <= 33; v++) {
        check(store(v) && load_is(v), "store and load %u", v);
    }
    for (i = 0; i < 3; i++) {
        before[i] = first_cluster(names[i]);
    }
    freeBefore = free_blocks();
    for (v = 34; v <= 36; v++) {
        check(store(v) && load_is(v), "store and load %u", v);
    }
    for (i = 0; i < 3; i++) {
        after[i] = first_cluster(names[i]);
    }
    check(after[0] == before[0] && after[1] == before[1] && after[2] == before[2] && before[0] > 0xFFFF &&
              before[1] > 0xFFFF && before[2] > 0xFFFF,
          "stores 4-6 kept the files' clusters (first clusters %lu %lu %lu, then %lu %lu %lu)", before[0], before[1],
          before[2], after[0], after[1], after[2]);
    check(free_blocks() == freeBefore && freeBefore != 0, "stores 4-6 allocated nothing (%lu free clusters, then %lu)",
          freeBefore, free_blocks());
    gc_ogc_save_set_path(GC_SAVE_PATH);
    check(!sDiskFull, "the RAM disk had room (%u of %u pages stored)", sPagesUsed, F32_PAGES);
}

int main(void) {
    u64 t0;

    gc_ogc_video_init();
    gc_ogc_init();
    gc_log("sd_ramdisk: libfat on a RAM disk mounted as sd:, built " __DATE__ " " __TIME__);
    sDisk = gc_mem_alloc(RD_SECTORS * SS, 32);
    if (sDisk == NULL) {
        gc_halt("sd_ramdisk: no memory for the RAM disk");
    }
    format_ramdisk();
    check(gc_ogc_sd_mount_iface(&sRamDisk) == 0 && gc_ogc_sd_mounted(), "mount the RAM disk as sd:");
    t0 = gettime();
    test_raw();
    test_libfat_semantics();
    test_saves();
    test_fat32();
    gc_log("sd_ramdisk: %u disk reads, %u writes, %u ms", sDiskReads, sDiskWrites,
           (unsigned int)ticks_to_millisecs(gettime() - t0));
    gc_log("sd_ramdisk: %d passed, %d failed", sPassed, sFailed);
    gc_log("sd_ramdisk: RESULT %s", sFailed == 0 ? "PASS" : "FAIL");
    gc_ogc_log_flush();
    for (;;) {
        VIDEO_WaitVSync();
    }
}
