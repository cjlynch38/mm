/**
 * Host test of port/gc/ogc/fat_map.c: the raw FAT reader behind the SD card ROM reads (storage.c).
 *
 * Reads the images and manifest made by mkfat.py through a file-backed mock of the SD card's sector reads and
 * checks, for every image and with a 1-sector and an 8-sector scratch buffer:
 *  - the volume is found (behind an MBR or not) with the right FAT type and cluster size, and the images that must
 *    be refused (exFAT, 4 KB sectors, no FAT volume) are refused for the right reason;
 *  - every file is found by its path and by every alias (case, 8.3 name, '.', '..', backslashes, trailing dot);
 *  - its runs match the builder's count, tile the file and lie in the image, and too small an array is refused;
 *  - the whole file and many random ranges (odd offsets, sizes and destination addresses) read back byte-exactly,
 *    with one read call per run for a whole-file read;
 *  - entries that must not be found are not (deleted, orphan or broken long names, a volume label, entries after the
 *    end marker, directories, missing names), and read errors are reported.
 *
 *   test_fat_map <manifest>      (the images are next to it)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fat_map.h"

#define SS FAT_MAP_SECTOR_SIZE
#define MAX_FILES 512
#define MAX_ALIASES 16
#define RANDOM_READS 12

typedef struct {
    int fd;
    uint64_t sectors;
    unsigned int calls;
    int failAfter; /* >= 0: reads fail after this many calls */
} Disc;

typedef struct {
    uint32_t size;
    uint32_t seed;
    uint32_t runs;
    char* path;
    char* aliases[MAX_ALIASES];
    int aliasCount;
} FileEntry;

static int sFailures;
static int sChecks;
static unsigned char sBlock[65536];
static const char* sContext = "";

static void check(int ok, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char* fmt, ...) {
    va_list args;

    sChecks++;
    if (ok) {
        return;
    }
    sFailures++;
    if (sFailures <= 60) {
        fprintf(stderr, "FAIL [%s] ", sContext);
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
        fprintf(stderr, "\n");
    }
}

static int disc_read(void* ctx, uint32_t sector, uint32_t count, void* dst) {
    Disc* d = ctx;
    size_t want = (size_t)count * SS;
    size_t done = 0;

    if (d->failAfter >= 0 && (int)d->calls >= d->failAfter) {
        return -1;
    }
    d->calls++;
    if (count == 0 || (uint64_t)sector + count > d->sectors) {
        return -1;
    }
    while (done < want) {
        ssize_t n = pread(d->fd, (unsigned char*)dst + done, want - done, (off_t)sector * SS + (off_t)done);

        if (n <= 0) {
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

/* The generator of mkfat.py */
static void make_block(void) {
    uint32_t x = 0x2545F491u;
    int i;

    for (i = 0; i < 65536; i++) {
        x = x * 1103515245u + 12345u;
        sBlock[i] = (unsigned char)(x >> 24);
    }
}

static void expected(uint32_t seed, uint32_t offset, unsigned char* out, uint32_t size) {
    uint32_t rot = (uint32_t)(((uint64_t)seed * 7919) % 65536);
    uint32_t i;

    for (i = 0; i < size; i++) {
        uint32_t n = offset + i;

        out[i] = sBlock[(n + rot) & 0xFFFF] ^ (unsigned char)(((n >> 16) + seed) & 0xFF);
    }
}

static uint32_t sRandom = 12345;
static uint32_t rnd(void) {
    sRandom ^= sRandom << 13;
    sRandom ^= sRandom >> 17;
    sRandom ^= sRandom << 5;
    return sRandom;
}

static char* trim(char* s) {
    size_t n = strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) {
        s[--n] = '\0';
    }
    return s;
}

/* Runs: start at 0, tile the file, whole clusters, inside the image */
static void check_runs(const FatMap* m, const Disc* d, const FatExtent* ext, uint32_t count, uint32_t size) {
    uint32_t cb = fat_map_cluster_size(m);
    uint64_t at = 0;
    uint32_t i;

    for (i = 0; i < count; i++) {
        check(ext[i].offset == at, "run %u starts at %u, expected %llu", i, ext[i].offset, (unsigned long long)at);
        check(ext[i].count != 0 && ext[i].count % m->clusterSectors == 0, "run %u has %u sectors", i, ext[i].count);
        check(ext[i].sector >= m->data && (uint64_t)ext[i].sector + ext[i].count <= d->sectors,
              "run %u at sector %u+%u is outside the data area", i, ext[i].sector, ext[i].count);
        if (i > 0) {
            check(ext[i].sector != ext[i - 1].sector + ext[i - 1].count, "runs %u and %u should be one", i - 1, i);
        }
        at += (uint64_t)ext[i].count * SS;
    }
    check(at >= size && at - size < cb, "runs cover %llu bytes for a file of %u", (unsigned long long)at, size);
}

static void test_file(FatMap* m, Disc* d, const FileEntry* f, int thorough) {
    uint32_t cluster = 0;
    uint32_t size = 0;
    uint32_t count = 0;
    uint32_t count2 = 0;
    FatExtent* ext;
    unsigned char* got;
    unsigned char* want;
    unsigned char bounce[SS];
    const char* why;
    unsigned int calls;
    int i;

    why = fat_map_find(m, f->path, &cluster, &size);
    check(why == NULL, "%s: not found (%s)", f->path, why ? why : "");
    if (why != NULL) {
        return;
    }
    check(size == f->size, "%s: size %u, expected %u", f->path, size, f->size);
    for (i = 0; i < f->aliasCount; i++) {
        uint32_t c2 = 0;
        uint32_t s2 = 0;

        why = fat_map_find(m, f->aliases[i], &c2, &s2);
        check(why == NULL && c2 == cluster && s2 == size, "alias %s of %s: %s (cluster %u/%u, size %u/%u)",
              f->aliases[i], f->path, why ? why : "found", c2, cluster, s2, size);
    }

    why = fat_map_extents(m, cluster, size, NULL, 0, &count);
    check(why == NULL && count == f->runs, "%s: %u runs (%s), expected %u", f->path, count, why ? why : "ok", f->runs);
    ext = calloc(count + 1, sizeof(FatExtent));
    why = fat_map_extents(m, cluster, size, ext, count, &count2);
    check(why == NULL && count2 == count, "%s: storing the runs: %s (%u)", f->path, why ? why : "ok", count2);
    if (count > 1) {
        why = fat_map_extents(m, cluster, size, ext, count - 1, &count2);
        check(why != NULL && strstr(why, "fragments") != NULL && count2 == count,
              "%s: %u runs into an array of %u: %s", f->path, count, count - 1, why ? why : "accepted");
        fat_map_extents(m, cluster, size, ext, count, &count2);
    }
    check_runs(m, d, ext, count, size);

    // The whole file: one call per run, plus one for a partial last sector
    got = malloc(size + 8);
    want = malloc(size + 8);
    expected(f->seed, 0, want, size);
    calls = d->calls;
    check(fat_map_read(ext, count, disc_read, d, 0, got, size, bounce) == 0, "%s: whole-file read failed", f->path);
    check(memcmp(got, want, size) == 0, "%s: whole-file read differs", f->path);
    check(d->calls - calls <= count + 1, "%s: whole-file read took %u calls for %u runs", f->path, d->calls - calls,
          count);

    // Random ranges into an odd destination
    for (i = 0; i < (thorough ? RANDOM_READS : 2) && size != 0; i++) {
        uint32_t off = rnd() % size;
        uint32_t max = size - off;
        uint32_t len = 1 + rnd() % (max < 20000 ? max : 20000);
        unsigned int shift = rnd() % 8;

        if (i == 0) {
            off = (size > SS) ? SS - 1 : 0; // across the first sector boundary
            len = (size - off < 3) ? size - off : 3;
        }
        memset(got, 0xA5, size + 8);
        check(fat_map_read(ext, count, disc_read, d, off, got + shift, len, bounce) == 0, "%s: read %u+%u failed",
              f->path, off, len);
        check(memcmp(got + shift, want + off, len) == 0, "%s: read %u+%u differs", f->path, off, len);
        check(got[shift + len] == 0xA5 && (shift == 0 || got[shift - 1] == 0xA5), "%s: read %u+%u wrote outside",
              f->path, off, len);
    }
    // Runs that cross a fragment boundary
    for (i = 1; i < (int)count && i < 6; i++) {
        uint32_t off = (ext[i].offset > 700) ? ext[i].offset - 700 : 0;
        uint32_t len = (size - off < 1400) ? size - off : 1400;

        check(fat_map_read(ext, count, disc_read, d, off, got, len, bounce) == 0 && memcmp(got, want + off, len) == 0,
              "%s: read across run %u failed or differs", f->path, i);
    }
    // Past the runs
    if (count != 0) {
        uint32_t end = ext[count - 1].offset + ext[count - 1].count * SS;

        check(fat_map_read(ext, count, disc_read, d, end, got, 1, bounce) != 0, "%s: a read past the runs worked",
              f->path);
    }
    // A failing disc
    if (count != 0 && size > 2 * SS) {
        int saved = d->failAfter;

        d->failAfter = (int)d->calls;
        check(fat_map_read(ext, count, disc_read, d, 1, got, size - 1, bounce) != 0, "%s: a read error was not reported",
              f->path);
        d->failAfter = saved;
    }
    free(got);
    free(want);
    free(ext);
}

static void test_image(const char* dir, char** lines, int lineCount, int* at) {
    char line[1024];
    char name[256];
    char kind[16];
    char words[256] = "";
    char path[1024];
    int fatType = 0;
    unsigned int clusterBytes = 0;
    FileEntry files[MAX_FILES];
    char* missing[2048];
    int fileCount = 0;
    int missingCount = 0;
    int bufSectors;
    struct stat st;
    Disc d;
    int i;

    snprintf(line, sizeof(line), "%s", lines[*at]);
    if (sscanf(line, "image %255s %15s", name, kind) != 2) {
        check(0, "bad manifest line: %s", line);
        (*at)++;
        return;
    }
    if (strcmp(kind, "ok") == 0) {
        sscanf(line, "image %*s ok %d %u", &fatType, &clusterBytes);
    } else {
        const char* w = strstr(line, " fail ");

        snprintf(words, sizeof(words), "%s", w ? w + 6 : "");
    }
    for ((*at)++; *at < lineCount && strncmp(lines[*at], "image ", 6) != 0; (*at)++) {
        char* l = lines[*at];

        if (strncmp(l, "file ", 5) == 0 && fileCount < MAX_FILES) {
            FileEntry* f = &files[fileCount++];
            int skip = 0;

            memset(f, 0, sizeof(*f));
            sscanf(l, "file %u %u %u %n", &f->size, &f->seed, &f->runs, &skip);
            f->path = l + skip;
        } else if (strncmp(l, "alias ", 6) == 0 && fileCount > 0 && files[fileCount - 1].aliasCount < MAX_ALIASES) {
            FileEntry* f = &files[fileCount - 1];

            f->aliases[f->aliasCount++] = l + 6;
        } else if (strncmp(l, "missing ", 8) == 0 && missingCount < 2048) {
            missing[missingCount++] = l + 8;
        }
    }

    snprintf(path, sizeof(path), "%s/%s", dir, name);
    d.fd = open(path, O_RDONLY);
    if (d.fd < 0 || fstat(d.fd, &st) != 0) {
        check(0, "cannot open %s (errno %d)", path, errno);
        return;
    }
    d.sectors = (uint64_t)st.st_size / SS;
    d.failAfter = -1;
    d.calls = 0;

    for (bufSectors = 1; bufSectors <= 8; bufSectors += 7) {
        unsigned char* buf = malloc((size_t)bufSectors * SS);
        char context[300];
        const char* why;
        FatMap m;

        snprintf(context, sizeof(context), "%s, %d-sector buffer", name, bufSectors);
        sContext = context;
        why = fat_map_mount(&m, disc_read, &d, buf, (uint32_t)bufSectors);
        if (words[0] != '\0') {
            check(why != NULL && strstr(why, words) != NULL, "mounted, or the wrong reason: %s (expected '%s')",
                  why ? why : "mounted", words);
            free(buf);
            continue;
        }
        check(why == NULL, "mount failed: %s", why ? why : "");
        if (why != NULL) {
            free(buf);
            continue;
        }
        check(m.type == fatType, "FAT%d, expected FAT%d", m.type, fatType);
        check(fat_map_cluster_size(&m) == clusterBytes, "clusters of %u bytes, expected %u",
              fat_map_cluster_size(&m), clusterBytes);
        for (i = 0; i < fileCount; i++) {
            test_file(&m, &d, &files[i], bufSectors == 8);
        }
        for (i = 0; i < missingCount; i++) {
            uint32_t c = 0;
            uint32_t s = 0;

            why = fat_map_find(&m, missing[i], &c, &s);
            check(why != NULL, "%s was found (cluster %u, size %u)", missing[i], c, s);
        }
        // A disc that fails while the FAT is read
        if (bufSectors == 1 && fileCount > 0) {
            uint32_t c = 0;
            uint32_t s = 0;
            uint32_t n = 0;
            FatExtent ext[4];

            d.failAfter = (int)d.calls;
            why = fat_map_find(&m, "/mmgcport/baserom.z64", &c, &s);
            if (why == NULL) {
                why = fat_map_extents(&m, c, s, ext, 4, &n);
            }
            check(why != NULL, "a failing disc went unnoticed");
            d.failAfter = -1;
        }
        printf("  %-22s %2d-sector buffer: %s, %d files, %d missing names\n", name, bufSectors,
               (m.type != 0) ? "ok" : "?", fileCount, missingCount);
        free(buf);
    }
    close(d.fd);
}

int main(int argc, char** argv) {
    char dir[1024];
    char* slash;
    char** lines = NULL;
    int lineCount = 0;
    int cap = 0;
    char buf[2048];
    FILE* manifest;
    int at = 0;

    if (argc != 2) {
        fprintf(stderr, "usage: test_fat_map <manifest>\n");
        return 2;
    }
    manifest = fopen(argv[1], "r");
    if (manifest == NULL) {
        fprintf(stderr, "cannot open %s\n", argv[1]);
        return 2;
    }
    while (fgets(buf, sizeof(buf), manifest) != NULL) {
        if (lineCount == cap) {
            cap = cap ? cap * 2 : 1024;
            lines = realloc(lines, (size_t)cap * sizeof(char*));
        }
        lines[lineCount++] = strdup(trim(buf));
    }
    fclose(manifest);
    snprintf(dir, sizeof(dir), "%s", argv[1]);
    slash = strrchr(dir, '/');
    if (slash != NULL) {
        *slash = '\0';
    } else {
        snprintf(dir, sizeof(dir), ".");
    }

    make_block();
    while (at < lineCount) {
        if (strncmp(lines[at], "image ", 6) == 0) {
            test_image(dir, lines, lineCount, &at);
        } else {
            at++;
        }
    }
    for (at = 0; at < lineCount; at++) {
        free(lines[at]);
    }
    free(lines);
    printf("fat_map: %d checks, %d failed\n", sChecks, sFailures);
    return sFailures != 0;
}
