/**
 * ROM access: the user's unmodified compressed baserom.z64 on SD, read by physical ROM offset.
 *
 * The N64 cartridge bytes are big-endian, as is the GameCube, so no conversion is needed.
 * DmaMgr, the game thread (CmpDma) and the audio thread all read concurrently:
 *  - The resident range (the audio data, streamed every frame) is preloaded into RAM at boot and
 *    served by memcpy without any lock. It never changes after the preload.
 *  - Everything else goes through a small cache of 64 KB blocks at 64 KB-aligned ROM offsets,
 *    filled with fseek/fread under one mutex. DmaMgr and Yaz0 read files front to back in small
 *    pieces, so a block serves many requests (read-ahead); several blocks let two readers
 *    interleave without evicting each other.
 * All data reaches the caller through CPU copies, so no cache maintenance is needed for it.
 * The SD driver only ever transfers into the 32-byte aligned cache blocks or resident buffer.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "gc_ogc.h"

#define CACHE_BLOCK_SIZE 0x10000
#define CACHE_BLOCKS 8

/* Chunk size for the resident preload */
#define PRELOAD_CHUNK 0x40000

typedef struct {
    unsigned int base; /* ROM offset of the block, or ~0 if empty */
    unsigned int size; /* valid bytes (less than CACHE_BLOCK_SIZE only for the last block) */
    unsigned int lastUse;
    unsigned char* data;
} CacheBlock;

static mutex_t sRomMutex = LWP_MUTEX_NULL;
static FILE* sRomFile;
static unsigned int sRomSize;
static CacheBlock sCache[CACHE_BLOCKS];
static unsigned int sUseClock;

static const unsigned char* sResident;
static unsigned int sResidentStart;
static unsigned int sResidentEnd;

/* Statistics for gc_ogc_rom_print_stats (updated under the mutex) */
static unsigned int sFileReads;
static unsigned int sFileReadBytes;
static unsigned int sCacheMisses;
static u64 sFileReadTicks;

void gc_ogc_rom_init(void) {
    if (sRomMutex == LWP_MUTEX_NULL && LWP_MutexInit(&sRomMutex, false) != 0) {
        gc_halt("gc_ogc_rom_init: cannot create the ROM mutex");
    }
}

static unsigned int be32(const unsigned char* p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

/* Read straight from the file. Caller holds the mutex; [offset, offset + size) lies in the file. */
static int file_read(unsigned int offset, void* dst, unsigned int size) {
    u64 start = gettime();
    size_t got;

    if (fseek(sRomFile, offset, SEEK_SET) != 0) {
        gc_log("ROM: seek to %08X failed (errno %d)", offset, errno);
        return -1;
    }
    got = fread(dst, 1, size, sRomFile);
    sFileReads++;
    sFileReadBytes += got;
    sFileReadTicks += gettime() - start;
    if (got != size) {
        gc_log("ROM: read of %u bytes at %08X returned %u (errno %d)", size, offset, (unsigned int)got, errno);
        clearerr(sRomFile);
        return -1;
    }
    return 0;
}

static CacheBlock* cache_lookup(unsigned int base) {
    CacheBlock* victim = &sCache[0];
    int i;

    for (i = 0; i < CACHE_BLOCKS; i++) {
        if (sCache[i].base == base) {
            sCache[i].lastUse = ++sUseClock;
            return &sCache[i];
        }
        if (sCache[i].lastUse < victim->lastUse) {
            victim = &sCache[i];
        }
    }

    sCacheMisses++;
    victim->base = ~0u;
    victim->size = sRomSize - base;
    if (victim->size > CACHE_BLOCK_SIZE) {
        victim->size = CACHE_BLOCK_SIZE;
    }
    if (file_read(base, victim->data, victim->size) != 0) {
        victim->lastUse = 0;
        return NULL;
    }
    victim->base = base;
    victim->lastUse = ++sUseClock;
    return victim;
}

/* Copy [offset, offset + size) from the file through the cache. Caller holds the mutex. */
static int cached_read(unsigned int offset, unsigned char* dst, unsigned int size) {
    while (size != 0) {
        unsigned int base = offset & ~(CACHE_BLOCK_SIZE - 1);
        unsigned int inBlock = offset - base;
        CacheBlock* block = cache_lookup(base);
        unsigned int n;

        if (block == NULL) {
            return -1;
        }
        n = block->size - inBlock;
        if (n > size) {
            n = size;
        }
        memcpy(dst, block->data + inBlock, n);
        offset += n;
        dst += n;
        size -= n;
    }
    return 0;
}

int gc_rom_read(unsigned int rom_offset, void* dst, unsigned int size) {
    unsigned char* out = dst;
    int locked = 0;
    int ret = 0;

    if (sRomFile == NULL) {
        gc_log("gc_rom_read(%08X, %u): no ROM is open", rom_offset, size);
        return -1;
    }

    while (size != 0) {
        unsigned int n;

        if (rom_offset >= sRomSize) {
            memset(out, 0, size);
            break;
        }
        if (sResident != NULL && rom_offset >= sResidentStart && rom_offset < sResidentEnd) {
            n = sResidentEnd - rom_offset;
            if (n > size) {
                n = size;
            }
            memcpy(out, sResident + (rom_offset - sResidentStart), n);
        } else {
            unsigned int limit = sRomSize;

            if (sResident != NULL && rom_offset < sResidentStart && sResidentStart < limit) {
                limit = sResidentStart;
            }
            n = limit - rom_offset;
            if (n > size) {
                n = size;
            }
            if (!locked) {
                LWP_MutexLock(sRomMutex);
                locked = 1;
            }
            if (cached_read(rom_offset, out, n) != 0) {
                ret = -1;
                break;
            }
        }
        rom_offset += n;
        out += n;
        size -= n;
    }

    if (locked) {
        LWP_MutexUnlock(sRomMutex);
    }
    return ret;
}

unsigned int gc_rom_size(void) {
    return sRomSize;
}

static const char* rom_error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static const char* rom_error(const char* fmt, ...) {
    static char sMessage[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(sMessage, sizeof(sMessage), fmt, args);
    va_end(args);
    return sMessage;
}

const char* gc_ogc_rom_open(const char* path) {
    unsigned char header[0x40];
    FILE* file;
    long size;
    unsigned int magic;
    unsigned int crc1;
    unsigned int crc2;
    int i;

    gc_ogc_rom_init();
    gc_log("ROM: opening %s", path);
    file = fopen(path, "rb");
    if (file == NULL) {
        return rom_error("cannot open %s (errno %d). Copy your Majora's Mask (USA) N64 ROM in .z64 format "
                         "there.",
                         path, errno);
    }
    // Unbuffered: fread then goes straight into the caller's (aligned) buffer.
    setvbuf(file, NULL, _IONBF, 0);

    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0 ||
        fread(header, 1, sizeof(header), file) != sizeof(header)) {
        fclose(file);
        return rom_error("cannot read %s (errno %d)", path, errno);
    }

    magic = be32(header);
    if (magic == 0x37804012) {
        fclose(file);
        return rom_error("%s is byte-swapped (.v64). Convert it to .z64 (big-endian) byte order.", path);
    }
    if (magic == 0x40123780) {
        fclose(file);
        return rom_error("%s is little-endian (.n64). Convert it to .z64 (big-endian) byte order.", path);
    }
    if (magic != GC_ROM_MAGIC) {
        fclose(file);
        return rom_error("%s is not an N64 ROM (first word %08X).", path, magic);
    }
    if (memcmp(header + 0x3B, "NZSE", 4) != 0) {
        fclose(file);
        return rom_error("%s is not Majora's Mask (USA): game code '%c%c%c%c', expected 'NZSE'.", path,
                         header[0x3B], header[0x3C], header[0x3D], header[0x3E]);
    }
    if ((unsigned long)size != GC_ROM_SIZE) {
        fclose(file);
        return rom_error("%s is %ld bytes; expected %u (the original compressed 32 MiB ROM).", path, size,
                         GC_ROM_SIZE);
    }
    crc1 = be32(header + 0x10);
    crc2 = be32(header + 0x14);
    if (crc1 != GC_ROM_CRC1 || crc2 != GC_ROM_CRC2) {
        fclose(file);
        return rom_error("%s has checksums %08X %08X; expected %08X %08X (USA 1.0).", path, crc1, crc2,
                         GC_ROM_CRC1, GC_ROM_CRC2);
    }

    if (sCache[0].data == NULL) {
        unsigned char* data = gc_mem_alloc(CACHE_BLOCKS * CACHE_BLOCK_SIZE, 32);

        if (data == NULL) {
            fclose(file);
            return rom_error("no memory for the read cache");
        }
        for (i = 0; i < CACHE_BLOCKS; i++) {
            sCache[i].data = data + i * CACHE_BLOCK_SIZE;
        }
    }

    LWP_MutexLock(sRomMutex);
    if (sRomFile != NULL) {
        fclose(sRomFile);
    }
    sRomFile = file;
    sRomSize = (unsigned int)size;
    sResident = NULL;
    for (i = 0; i < CACHE_BLOCKS; i++) {
        sCache[i].base = ~0u;
        sCache[i].lastUse = 0;
    }
    LWP_MutexUnlock(sRomMutex);

    gc_log("ROM: %.20s, %u MiB, code %.4s, CRC %08X %08X: OK", (const char*)header + 0x20, sRomSize >> 20,
           (const char*)header + 0x3B, crc1, crc2);
    return NULL;
}

/* Throughput for the logs, in 64 bits: bytes * 1000 overflows 32 bits from 4.3 MB on */
static unsigned int kb_per_second(unsigned int bytes, unsigned int ms) {
    return (ms != 0) ? (unsigned int)((u64)bytes * 1000 / 1024 / ms) : 0;
}

int gc_ogc_rom_preload(unsigned int start, unsigned int end) {
    unsigned char* buffer;
    unsigned int offset;
    u64 t0;
    unsigned int ms;

    if (sRomFile == NULL || start >= end || end > sRomSize) {
        gc_log("ROM: cannot preload %08X-%08X", start, end);
        return -1;
    }
    buffer = gc_mem_alloc(end - start, 32);
    if (buffer == NULL) {
        return -1;
    }

    gc_log("ROM: loading %08X-%08X (%u KB) into RAM...", start, end, (end - start) / 1024);
    t0 = gettime();
    LWP_MutexLock(sRomMutex);
    for (offset = start; offset < end; offset += PRELOAD_CHUNK) {
        unsigned int n = end - offset;

        if (n > PRELOAD_CHUNK) {
            n = PRELOAD_CHUNK;
        }
        if (file_read(offset, buffer + (offset - start), n) != 0) {
            LWP_MutexUnlock(sRomMutex);
            return -1;
        }
    }
    sResidentStart = start;
    sResidentEnd = end;
    sResident = buffer;
    LWP_MutexUnlock(sRomMutex);

    ms = (unsigned int)ticks_to_millisecs(gettime() - t0);
    gc_log("ROM: resident range loaded in %u ms (%u KB/s)", ms, kb_per_second(end - start, ms));
    return 0;
}

void gc_ogc_rom_print_stats(void) {
    unsigned int reads;
    unsigned int bytes;
    unsigned int misses;
    unsigned int ms;

    LWP_MutexLock(sRomMutex);
    reads = sFileReads;
    bytes = sFileReadBytes;
    misses = sCacheMisses;
    ms = (unsigned int)ticks_to_millisecs(sFileReadTicks);
    LWP_MutexUnlock(sRomMutex);
    gc_log("ROM: %u file reads, %u KB, %u ms reading (%u KB/s), %u cache misses", reads, bytes / 1024, ms,
           kb_per_second(bytes, ms), misses);
}
