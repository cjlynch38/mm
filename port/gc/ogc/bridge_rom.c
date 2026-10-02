/**
 * ROM access: the user's unmodified compressed baserom.z64 on SD (or the Dolphin dev disc), read
 * by physical ROM offset.
 *
 * The N64 cartridge bytes are big-endian, as is the GameCube, so no conversion is needed.
 * DmaMgr, the game thread (CmpDma) and the audio thread all read concurrently:
 *  - The resident range (the audio data, streamed every frame) is copied at boot into ARAM, the
 *    GameCube's 16 MiB of auxiliary memory, which nothing else uses: MEM1 stays free for the
 *    renderer. The CPU cannot address ARAM, so a read DMAs the covering 32-byte aligned span into a
 *    bounce buffer and copies the requested bytes out. The bounce buffer has its own mutex, separate
 *    from the file mutex, so the audio thread never waits behind an SD/DVD read.
 *    With GC_ROM_RESIDENT_ARAM 0 the range is kept in MEM1 instead (5.75 MiB) and served by memcpy
 *    without any lock.
 *  - Everything else goes through a small cache of 64 KB blocks at 64 KB-aligned ROM offsets,
 *    filled with fseek/fread under one mutex. DmaMgr and Yaz0 read files front to back in small
 *    pieces, so a block serves many requests (read-ahead); several blocks let two readers
 *    interleave without evicting each other.
 * All data reaches the caller through CPU copies, so callers need no cache maintenance.
 * The SD driver only ever transfers into the 32-byte aligned cache blocks or resident buffer.
 */
#include <gccore.h>
#include <ogc/aram.h>
#include <ogc/arqueue.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "gc_ogc.h"

/* 1: the resident range lives in ARAM. 0: in MEM1 (the Milestone 2 layout). */
#ifndef GC_ROM_RESIDENT_ARAM
#define GC_ROM_RESIDENT_ARAM 1
#endif

/* 1: after the ARAM preload, read the whole range back through gc_rom_read's ARAM path in odd-sized
 * pieces and compare checksums (adds a fraction of a second to the boot) */
#ifndef GC_ROM_ARAM_VERIFY
#define GC_ROM_ARAM_VERIFY 0
#endif

/* Seconds between ARAM read statistics lines in the log; 0 disables them */
#ifndef GC_ROM_STATS_INTERVAL
#define GC_ROM_STATS_INTERVAL 20
#endif

#define CACHE_BLOCK_SIZE 0x10000
#define CACHE_BLOCKS 8

/* Chunk size for the resident preload. The ARAM preload stages two chunks in the cache blocks. */
#define PRELOAD_CHUNK 0x40000

#if GC_ROM_RESIDENT_ARAM
_Static_assert(2 * PRELOAD_CHUNK <= CACHE_BLOCKS * CACHE_BLOCK_SIZE, "ARAM preload staging needs two chunks");

/* ARAM DMA: the ARAM address, the MEM1 address and the length must all be multiples of 32 */
#define ARAM_ALIGN 32u
#define ARAM_BOUNCE_SIZE 0x10000u
/* AR_Alloc bookkeeping slots (ours, and room for later ARAM users) */
#define ARAM_BLOCK_SLOTS 8
/* ARQ owner tag of our requests */
#define ARAM_OWNER 0x524F4D00u
/* DMAs up to this many bytes are waited for by polling instead of sleeping (0: always sleep) */
#ifndef GC_ROM_ARAM_SPIN_MAX
#define GC_ROM_ARAM_SPIN_MAX 0
#endif
#if GC_ROM_STATS_INTERVAL != 0
#define ARAM_REPORT_TICKS secs_to_ticks(GC_ROM_STATS_INTERVAL)
#else
#define ARAM_REPORT_TICKS (~0ull)
#endif
#endif

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
static unsigned char* sCacheMem; /* the blocks' storage, contiguous */
static unsigned int sUseClock;

/* The resident range; empty (0, 0) until a preload succeeds */
static unsigned int sResidentStart;
static unsigned int sResidentEnd;

/* Statistics for gc_ogc_rom_print_stats (updated under the mutex) */
static unsigned int sFileReads;
static unsigned int sFileReadBytes;
static unsigned int sCacheMisses;
static u64 sFileReadTicks;

#if GC_ROM_RESIDENT_ARAM
typedef struct {
    unsigned int reads; /* resident pieces served (one per gc_rom_read call, normally) */
    unsigned int dmas;
    u64 bytes;    /* bytes copied to callers */
    u64 dmaBytes; /* bytes transferred from ARAM, alignment included */
    u64 ticks;    /* time spent in aram_read, mutex wait included */
} AramStats;

static mutex_t sAramMutex = LWP_MUTEX_NULL; /* the bounce buffer and the statistics */
static lwpq_t sAramQueue = LWP_TQUEUE_NULL; /* threads waiting for one of our DMAs */
static u32 sAramBlockLens[ARAM_BLOCK_SLOTS];
static unsigned int sAramBase;    /* ARAM address of the copy, 0 if none */
static unsigned int sAramSize;    /* bytes reserved there */
static unsigned int sAramRomBase; /* ROM offset stored at sAramBase (32-byte aligned) */
static unsigned char* sAramBounce;

static AramStats sAramStats;    /* since boot */
static AramStats sAramReported; /* sAramStats at the last statistics line */
static u64 sAramMaxTicks;       /* longest read since the last statistics line */
static u64 sAramReportTime;
#else
static const unsigned char* sResident;
#endif

void gc_ogc_rom_init(void) {
    if (sRomMutex == LWP_MUTEX_NULL && LWP_MutexInit(&sRomMutex, false) != 0) {
        gc_halt("gc_ogc_rom_init: cannot create the ROM mutex");
    }
#if GC_ROM_RESIDENT_ARAM
    if (sAramMutex == LWP_MUTEX_NULL && LWP_MutexInit(&sAramMutex, false) != 0) {
        gc_halt("gc_ogc_rom_init: cannot create the ARAM mutex");
    }
    if (sAramQueue == LWP_TQUEUE_NULL && LWP_InitQueue(&sAramQueue) != 0) {
        gc_halt("gc_ogc_rom_init: cannot create the ARAM wait queue");
    }
#endif
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

/* Empty every cache block. Caller holds the mutex. */
static void cache_reset(void) {
    int i;

    for (i = 0; i < CACHE_BLOCKS; i++) {
        sCache[i].base = ~0u;
        sCache[i].lastUse = 0;
    }
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

#if GC_ROM_RESIDENT_ARAM
/* ARQ completion, in the ARAM interrupt: the ARQ has already marked the request finished */
static void aram_dma_done(ARQRequest* req) {
    LWP_ThreadBroadcast(sAramQueue);
}

/* Queue one ARAM DMA. Through the ARQ rather than AR_StartDMA, so other ARQ users can share ARAM. */
static void aram_dma_start(ARQRequest* req, u32 dir, void* mram, unsigned int aram, unsigned int len) {
    ARQ_PostRequestAsync(req, ARAM_OWNER, dir, ARQ_PRIO_HI, aram, MEM_VIRTUAL_TO_PHYSICAL(mram), len,
                         aram_dma_done);
}

/* Wait for a DMA started with aram_dma_start; len is its length */
static void aram_dma_wait(ARQRequest* req, unsigned int len) {
    u32 level;

    if (len <= GC_ROM_ARAM_SPIN_MAX) {
        while (*(volatile u32*)&req->state != ARQ_TASK_FINISHED) {}
        return;
    }
    // As ARQ_PostRequest: with interrupts off, the completion cannot slip in between the check
    // and the sleep.
    _CPU_ISR_Disable(level);
    while (*(volatile u32*)&req->state != ARQ_TASK_FINISHED) {
        LWP_ThreadSleep(sAramQueue);
    }
    _CPU_ISR_Restore(level);
}

/* Log the statistics since the last line. Called without the ARAM mutex. */
static void aram_report(const AramStats* d, unsigned int maxUs, unsigned int windowMs) {
    unsigned int us = (unsigned int)ticks_to_microsecs(d->ticks);
    unsigned int permille = (windowMs != 0) ? us / windowMs : 0;

    gc_log("ROM: ARAM, last %u s: %u reads, %u KB (%u B avg), %u DMAs %u KB, %u us avg, %u us max, "
           "%u.%u%% of the time",
           windowMs / 1000, d->reads, (unsigned int)(d->bytes / 1024),
           (d->reads != 0) ? (unsigned int)(d->bytes / d->reads) : 0, d->dmas, (unsigned int)(d->dmaBytes / 1024),
           (d->reads != 0) ? us / d->reads : 0, maxUs, permille / 10, permille % 10);
}

/* Copy [offset, offset + size) of the resident range out of ARAM */
static void aram_read(unsigned int offset, unsigned char* dst, unsigned int size) {
    u64 t0 = gettime();
    ARQRequest req;
    AramStats delta;
    unsigned int maxUs = 0;
    unsigned int windowMs = 0;
    int report = 0;
    u64 now;
    u64 elapsed;

    LWP_MutexLock(sAramMutex);
    sAramStats.reads++;
    sAramStats.bytes += size;
    while (size != 0) {
        unsigned int spanStart = offset & ~(ARAM_ALIGN - 1);
        unsigned int head = offset - spanStart;
        unsigned int n = ARAM_BOUNCE_SIZE - head;
        unsigned int spanLen;

        if (n > size) {
            n = size;
        }
        spanLen = (head + n + ARAM_ALIGN - 1) & ~(ARAM_ALIGN - 1);
        // The DMA writes MEM1 behind the data cache: drop the bounce buffer's lines first.
        DCInvalidateRange(sAramBounce, spanLen);
        aram_dma_start(&req, ARQ_ARAMTOMRAM, sAramBounce, sAramBase + (spanStart - sAramRomBase), spanLen);
        aram_dma_wait(&req, spanLen);
        memcpy(dst, sAramBounce + head, n);
        sAramStats.dmas++;
        sAramStats.dmaBytes += spanLen;
        offset += n;
        dst += n;
        size -= n;
    }

    now = gettime();
    elapsed = now - t0;
    sAramStats.ticks += elapsed;
    if (elapsed > sAramMaxTicks) {
        sAramMaxTicks = elapsed;
    }
    if (now - sAramReportTime >= ARAM_REPORT_TICKS) {
        delta.reads = sAramStats.reads - sAramReported.reads;
        delta.dmas = sAramStats.dmas - sAramReported.dmas;
        delta.bytes = sAramStats.bytes - sAramReported.bytes;
        delta.dmaBytes = sAramStats.dmaBytes - sAramReported.dmaBytes;
        delta.ticks = sAramStats.ticks - sAramReported.ticks;
        maxUs = (unsigned int)ticks_to_microsecs(sAramMaxTicks);
        windowMs = (unsigned int)ticks_to_millisecs(now - sAramReportTime);
        // The first read only starts the first window
        report = (GC_ROM_STATS_INTERVAL != 0 && sAramReportTime != 0);
        sAramReported = sAramStats;
        sAramMaxTicks = 0;
        sAramReportTime = now;
    }
    LWP_MutexUnlock(sAramMutex);

    if (report) {
        aram_report(&delta, maxUs, windowMs);
    }
}
#endif

int gc_rom_read(unsigned int rom_offset, void* dst, unsigned int size) {
    unsigned char* out = dst;

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
        if (rom_offset >= sResidentStart && rom_offset < sResidentEnd) {
            n = sResidentEnd - rom_offset;
            if (n > size) {
                n = size;
            }
#if GC_ROM_RESIDENT_ARAM
            aram_read(rom_offset, out, n);
#else
            memcpy(out, sResident + (rom_offset - sResidentStart), n);
#endif
        } else {
            unsigned int limit = (rom_offset < sResidentStart) ? sResidentStart : sRomSize;
            int ret;

            n = limit - rom_offset;
            if (n > size) {
                n = size;
            }
            LWP_MutexLock(sRomMutex);
            ret = cached_read(rom_offset, out, n);
            LWP_MutexUnlock(sRomMutex);
            if (ret != 0) {
                return -1;
            }
        }
        rom_offset += n;
        out += n;
        size -= n;
    }
    return 0;
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

    if (sCacheMem == NULL) {
        sCacheMem = gc_mem_alloc(CACHE_BLOCKS * CACHE_BLOCK_SIZE, 32);
        if (sCacheMem == NULL) {
            fclose(file);
            return rom_error("no memory for the read cache");
        }
        for (i = 0; i < CACHE_BLOCKS; i++) {
            sCache[i].data = sCacheMem + i * CACHE_BLOCK_SIZE;
        }
    }

    LWP_MutexLock(sRomMutex);
    if (sRomFile != NULL) {
        fclose(sRomFile);
    }
    sRomFile = file;
    sRomSize = (unsigned int)size;
    sResidentStart = 0;
    sResidentEnd = 0;
    cache_reset();
    LWP_MutexUnlock(sRomMutex);

    gc_log("ROM: %.20s, %u MiB, code %.4s, CRC %08X %08X: OK", (const char*)header + 0x20, sRomSize >> 20,
           (const char*)header + 0x3B, crc1, crc2);
    return NULL;
}

/* Throughput for the logs, in 64 bits: bytes * 1000 overflows 32 bits from 4.3 MB on */
static unsigned int kb_per_second(unsigned int bytes, unsigned int ms) {
    return (ms != 0) ? (unsigned int)((u64)bytes * 1000 / 1024 / ms) : 0;
}

#if GC_ROM_RESIDENT_ARAM
/* Initialise ARAM if nobody has, reserve `size` bytes there (once) and allocate the bounce buffer */
static int aram_setup(unsigned int size) {
    const char* how = "was already initialised";

    if (!AR_CheckInit()) {
        // AR_Init keeps ARAM 0-0x3FFF for the OS and DSP; AR_Alloc hands out memory from 0x4000
        // and records each length in the array it is given (with NULL it writes through NULL, so
        // whoever initialises ARAM must pass one).
        AR_Init(sAramBlockLens, ARAM_BLOCK_SLOTS);
        how = "initialised here";
    }
    // After AR_Init, which clears the DMA callback that ARQ_Init installs. No-op if already done.
    ARQ_Init();

    if (sAramBase == 0 || sAramSize < size) {
        unsigned int base = AR_Alloc(size);

        if (base + size > AR_GetSize()) {
            AR_Free(NULL);
            gc_log("ROM: no room in ARAM for %u KB at %08X (%u KB of ARAM)", size / 1024, base, AR_GetSize() / 1024);
            return -1;
        }
        sAramBase = base;
        sAramSize = size;
    }
    if (sAramBounce == NULL) {
        sAramBounce = gc_mem_alloc(ARAM_BOUNCE_SIZE, ARAM_ALIGN);
        if (sAramBounce == NULL) {
            return -1;
        }
    }
    gc_log("ARAM: %u KB (%u KB internal), %s; ROM copy at %08X-%08X (%u KB), %u KB free above it; "
           "bounce buffer %u KB at %08X",
           AR_GetSize() / 1024, AR_GetInternalSize() / 1024, how, sAramBase, sAramBase + sAramSize, sAramSize / 1024,
           (AR_GetSize() - (sAramBase + sAramSize)) / 1024, ARAM_BOUNCE_SIZE / 1024, (unsigned int)sAramBounce);
    return 0;
}

#if GC_ROM_ARAM_VERIFY
static unsigned int checksum(unsigned int h, const unsigned char* p, unsigned int size) {
    while (size-- != 0) {
        h = h * 31 + *p++;
    }
    return h;
}

/* Read [start, end) back through aram_read in odd-sized pieces into an odd address, as callers
 * do, and compare with the checksum of the data that was loaded. Caller holds the ROM mutex
 * (the cache blocks are the destination). */
static int aram_verify(unsigned int start, unsigned int end, unsigned int expected) {
    unsigned char* buf = sCacheMem + 3;
    unsigned int offset = start;
    unsigned int h = 0;
    unsigned int pieces = 0;
    u64 t0 = gettime();

    while (offset < end) {
        unsigned int n = 1 + (pieces * 7919u) % (ARAM_BOUNCE_SIZE + 5000);

        if (n > end - offset) {
            n = end - offset;
        }
        aram_read(offset, buf, n);
        h = checksum(h, buf, n);
        offset += n;
        pieces++;
    }
    gc_log("ROM: ARAM verify %s: %u reads in %u ms, checksum %08X (expected %08X)", (h == expected) ? "OK" : "FAILED",
           pieces, (unsigned int)ticks_to_millisecs(gettime() - t0), h, expected);
    return (h == expected) ? 0 : -1;
}
#endif

static int preload_aram(unsigned int start, unsigned int end) {
    unsigned int romBase = start & ~(ARAM_ALIGN - 1);
    unsigned int romEnd = (end + ARAM_ALIGN - 1) & ~(ARAM_ALIGN - 1);
    unsigned char* stage[2];
    ARQRequest req;
    unsigned int offset;
    unsigned int pendingLen = 0;
    int cur = 0;
    int ret = 0;
    u64 t0;
    unsigned int ms;
#if GC_ROM_ARAM_VERIFY
    unsigned int sum = 0;
#endif

    if (aram_setup(romEnd - romBase) != 0) {
        return -1;
    }

    gc_log("ROM: loading %08X-%08X (%u KB) into ARAM...", start, end, (end - start) / 1024);
    t0 = gettime();
    // The cache blocks double as two staging buffers: the file fills one while the other is
    // DMA'd to ARAM. Holding the ROM mutex keeps cached reads out meanwhile.
    stage[0] = sCacheMem;
    stage[1] = sCacheMem + PRELOAD_CHUNK;
    LWP_MutexLock(sRomMutex);
    LWP_MutexLock(sAramMutex);
    sResidentStart = 0;
    sResidentEnd = 0;
    for (offset = romBase; offset < romEnd; offset += PRELOAD_CHUNK) {
        unsigned int n = romEnd - offset;
        unsigned int fileBytes;

        if (n > PRELOAD_CHUNK) {
            n = PRELOAD_CHUNK;
        }
        // Past the end of the ROM (only when end is an unaligned end of the file) the staging
        // bytes are stale; they land in ARAM but are never served.
        fileBytes = (n < sRomSize - offset) ? n : sRomSize - offset;
        if (file_read(offset, stage[cur], fileBytes) != 0) {
            ret = -1;
            break;
        }
#if GC_ROM_ARAM_VERIFY
        {
            unsigned int lo = (offset < start) ? start : offset;
            unsigned int hi = (offset + n > end) ? end : offset + n;

            sum = checksum(sum, stage[cur] + (lo - offset), hi - lo);
        }
#endif
        // The DMA reads MEM1 behind the data cache
        DCFlushRange(stage[cur], n);
        if (pendingLen != 0) {
            aram_dma_wait(&req, pendingLen);
        }
        aram_dma_start(&req, ARQ_MRAMTOARAM, stage[cur], sAramBase + (offset - romBase), n);
        pendingLen = n;
        cur ^= 1;
    }
    if (pendingLen != 0) {
        aram_dma_wait(&req, pendingLen);
    }
    if (ret == 0) {
        sAramRomBase = romBase;
        sResidentStart = start;
        sResidentEnd = end;
    }
    LWP_MutexUnlock(sAramMutex);
    if (ret == 0) {
        ms = (unsigned int)ticks_to_millisecs(gettime() - t0);
        gc_log("ROM: resident range loaded into ARAM in %u ms (%u KB/s)", ms, kb_per_second(end - start, ms));
#if GC_ROM_ARAM_VERIFY
        if (aram_verify(start, end, sum) != 0) {
            sResidentStart = 0;
            sResidentEnd = 0;
            ret = -1;
        }
#endif
    }
    cache_reset();
    LWP_MutexUnlock(sRomMutex);
    return ret;
}
#else
static int preload_mem1(unsigned int start, unsigned int end) {
    unsigned char* buffer;
    unsigned int offset;
    u64 t0;
    unsigned int ms;

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
    sResident = buffer;
    sResidentStart = start;
    sResidentEnd = end;
    LWP_MutexUnlock(sRomMutex);

    ms = (unsigned int)ticks_to_millisecs(gettime() - t0);
    gc_log("ROM: resident range loaded in %u ms (%u KB/s)", ms, kb_per_second(end - start, ms));
    return 0;
}
#endif

int gc_ogc_rom_preload(unsigned int start, unsigned int end) {
    if (sRomFile == NULL || start >= end || end > sRomSize) {
        gc_log("ROM: cannot preload %08X-%08X", start, end);
        return -1;
    }
#if GC_ROM_RESIDENT_ARAM
    return preload_aram(start, end);
#else
    return preload_mem1(start, end);
#endif
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

#if GC_ROM_RESIDENT_ARAM
    {
        AramStats s;

        LWP_MutexLock(sAramMutex);
        s = sAramStats;
        LWP_MutexUnlock(sAramMutex);
        gc_log("ROM: %u ARAM reads, %u KB, %u DMAs %u KB, %u us avg", s.reads, (unsigned int)(s.bytes / 1024), s.dmas,
               (unsigned int)(s.dmaBytes / 1024),
               (s.reads != 0) ? (unsigned int)(ticks_to_microsecs(s.ticks) / s.reads) : 0);
    }
#endif
}
