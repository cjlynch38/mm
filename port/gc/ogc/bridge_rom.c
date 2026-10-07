/**
 * ROM access: the user's unmodified compressed baserom.z64 (on SD, on the disc in the drive or in a
 * disc image file, see storage.c), read by physical ROM offset. gc_rom_read() runs in the calling
 * thread (osEPiStartDma) and is synchronous.
 *
 * The N64 cartridge bytes are big-endian, as is the GameCube, so no conversion is needed.
 * Three threads read: the DmaMgr thread (every DmaMgr request; Yaz0 streams compressed files in
 * 1 KB pieces), the game thread (CmpDma archive pieces) and the audio thread (banks, sequences and
 * samples, every frame). Four layers serve them:
 *
 *  1. Resident ranges (gc_ogc_rom_preload): copied into ARAM at boot and never evicted: the audio
 *     data, and the hot range (link_animetion, read every gameplay frame, the item/map statics,
 *     the yar archives and the message data). A read DMAs the covering 32-byte aligned span into a
 *     bounce buffer and copies the requested bytes out. Only that buffer's lane mutex is taken, so
 *     a resident read (all the audio thread does) never waits behind the disc. The first range
 *     (the audio data) has a lane of its own: the audio thread never waits for another thread's
 *     resident read either (the graph thread, the lowest game priority, reads the yar archives;
 *     holding the audio thread's lock it could be kept off the CPU by DmaMgr's decompression).
 *  2. Block cache in MEM1: CACHE_BLOCKS blocks of up to 64 KB, each holding a ROM range
 *     [base, base + size). Every other read is copied from a block; DmaMgr and Yaz0 read front to
 *     back in small pieces, so one block serves many requests.
 *  3. File cache (gc_ogc_rom_cache_init): the rest of ARAM holds whole files of the ROM's dmadata
 *     table, the least recently used evicted first. Files are stored in FC_PAGE_SIZE pages, so a
 *     file of any size fits without fragmentation; pages are handed out next-fit, so a file's
 *     pages are mostly consecutive and move in few DMAs. A block miss inside a cached file
 *     refills the block from ARAM.
 *  4. The disc. A block miss inside a file the file cache takes reads the whole file (one seek, one
 *     sequential read) into blocks and copies it to ARAM; any other miss reads the 64 KB-aligned
 *     block. On dvd: (the disc image or the Dolphin dev disc) the ROM's sectors are read with
 *     DVD_ReadPrio straight into the block (libiso9660 would split every read into 32 KB pieces
 *     through its own buffer); anything else with fseek/fread, unbuffered, so 32-byte aligned
 *     buffers get the data by DMA.
 *
 * Locks and ARAM rules:
 *  - sRomMutex covers the disc, the block cache, the file table, the file cache (tables and
 *    pages), our share of ARAM and changes to the resident range table. It is held for whole disc
 *    reads. The audio thread never takes it.
 *  - sAramMutex[lane] covers that lane's bounce buffer. It is held only around one resident read's
 *    ARAM DMAs, never while the disc is read. Lock order: sRomMutex, then the lanes in order (only
 *    a re-open takes them all, to wait for resident reads in flight).
 *  - Every ARAM DMA goes through the ARQ, so other ARQ users can share ARAM. MEM1 buffer, ARAM
 *    address and length are all multiples of 32 bytes. Before ARAM->MEM1 the destination's cache
 *    lines are invalidated; before MEM1->ARAM the source's lines are flushed.
 *  - Resident reads post ARQ_PRIO_HI requests (at most one bounce buffer: 64 KB on the audio lane,
 *    16 KB on the other; one DMA each). File-cache transfers post ARQ_PRIO_LO requests, which the
 *    ARQ splits into 4 KB chunks and serves high-priority requests in between, so an audio read's
 *    DMA waits at most for one 4 KB chunk or one 16 KB read of the other lane. The ARQ completes
 *    requests of one priority in order: a page written to ARAM is complete before any later read
 *    of that page, even after the page changed owner.
 *  - Waiting for a DMA: the completion callback (ARAM interrupt) broadcasts a thread queue; the
 *    waiter checks its request's state with interrupts off before every sleep.
 *  - Statistics are updated with interrupts off (a few additions per call) and logged every
 *    GC_ROM_STATS_INTERVAL seconds by the last thread that read past the resident ranges, never by
 *    the audio thread.
 *
 * All data reaches the caller through CPU copies, so callers need no cache maintenance.
 */
#include <gccore.h>
#include <ogc/aram.h>
#include <ogc/arqueue.h>
#include <ogc/dvd.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gc_ogc.h"

/* 1: resident ranges live in ARAM, and the file cache can exist. 0: resident ranges in MEM1, no
 * file cache (the Milestone 2 layout). */
#ifndef GC_ROM_RESIDENT_ARAM
#define GC_ROM_RESIDENT_ARAM 1
#endif

/* 1: gc_ogc_rom_cache_init() creates the ARAM file cache. 0: it does nothing. */
#ifndef GC_ROM_FILE_CACHE
#define GC_ROM_FILE_CACHE 1
#endif

/* 1: when the ROM is on dvd: (the disc image or the dev disc), read its sectors with DVD_ReadPrio.
 * 0: always fread. */
#ifndef GC_ROM_DVD_DIRECT
#define GC_ROM_DVD_DIRECT 1
#endif

/* Largest file the file cache takes; bigger ones are read in 64 KB blocks */
#ifndef GC_ROM_CACHE_MAX_FILE
#define GC_ROM_CACHE_MAX_FILE 0x80000
#endif

/* Bytes of ARAM left for other users when the ROM takes ARAM (it takes the rest at the first preload) */
#ifndef GC_ROM_ARAM_RESERVE
#define GC_ROM_ARAM_RESERVE 0
#endif

/* 1: after a preload, read the range back through the resident path in odd-sized pieces and compare
 * checksums (adds a fraction of a second to the boot) */
#ifndef GC_ROM_ARAM_VERIFY
#define GC_ROM_ARAM_VERIFY 0
#endif

/* Seconds between statistics lines in the log; 0 disables them */
#ifndef GC_ROM_STATS_INTERVAL
#define GC_ROM_STATS_INTERVAL 20
#endif

/* Read tracing for measurements (each line is an SD write on hardware, so keep it 0 there):
 * 1: one line per run of reads of one file by one thread that went past the resident ranges,
 *    and the most-read resident files in every statistics window; 2: also runs of resident files. */
#ifndef GC_ROM_TRACE
#define GC_ROM_TRACE 0
#endif

/* 1: gc_ogc_rom_check() reads the whole ROM back after the boot and compares it with checksums of the build
 * machine's ROM (make -f Makefile.gc GC_ROM_CHECK=1, which generates the table) */
#ifndef GC_ROM_CHECK
#define GC_ROM_CHECK 0
#endif

#define CACHE_BLOCK_SIZE 0x10000
#define CACHE_BLOCKS 8

/* Chunk size for preloads. The ARAM preload stages two chunks in the cache blocks. */
#define PRELOAD_CHUNK 0x40000

/* Resident ranges at most */
#define RES_MAX 4

/* The dmadata table: 16-byte entries {vromStart, vromEnd, romStart, romEnd}, ended by a zero entry */
#define DMADATA_ROM 0x1A500u
#define DMADATA_SIZE 0x6200u
#define DMADATA_MAX (DMADATA_SIZE / 16)

/* Physical start/end of a dmadata entry that has no data in the ROM */
#define DMA_NO_DATA 0xFFFFFFFFu

/* The dev disc's ISO9660 sector size: libiso9660 reports a file's first sector as st_ino */
#define ISO_SECTOR_SIZE 0x800u

/* Calls slower than these are counted in the statistics (milliseconds) */
#define SLOW_MS_1 2
#define SLOW_MS_2 16
#define SLOW_MS_3 50

#if GC_ROM_RESIDENT_ARAM
_Static_assert(2 * PRELOAD_CHUNK <= CACHE_BLOCKS * CACHE_BLOCK_SIZE, "ARAM preload staging needs two chunks");

/* ARAM DMA: the ARAM address, the MEM1 address and the length must all be multiples of 32 */
#define ARAM_ALIGN 32u
/* Resident read lanes (bounce buffer + mutex): 0 for the first resident range (the audio data), 1
 * for the others. The others' reads are DmaMgr/CmpDma pieces of at most 8 KB, so a smaller buffer. */
#define ARAM_LANES 2
#define ARAM_BOUNCE_SIZE 0x10000u
#define ARAM_BOUNCE_SIZE_OTHER 0x4000u
/* AR_Alloc bookkeeping slots (ours, and room for later ARAM users) */
#define ARAM_BLOCK_SLOTS 8
/* ARQ owner tags of our requests: resident reads and preloads, file cache transfers */
#define ARAM_OWNER 0x524F4D00u
#define FC_OWNER 0x524F4D01u
/* DMAs up to this many bytes are waited for by polling instead of sleeping (0: always sleep) */
#ifndef GC_ROM_ARAM_SPIN_MAX
#define GC_ROM_ARAM_SPIN_MAX 0
#endif
#else
#undef GC_ROM_FILE_CACHE
#define GC_ROM_FILE_CACHE 0
#endif

#if GC_ROM_FILE_CACHE
/* File cache page size; a 64 KB block maps onto at most FC_BLOCK_PAGES pages */
#define FC_PAGE_SIZE 0x800u
#define FC_BLOCK_PAGES (CACHE_BLOCK_SIZE / FC_PAGE_SIZE)
/* "No page" / "no file" in the u16 links */
#define FC_NONE 0xFFFFu
/* Below this many pages the file cache is not worth having */
#define FC_MIN_PAGES 64u
_Static_assert(CACHE_BLOCK_SIZE % FC_PAGE_SIZE == 0, "blocks must cover whole pages");
_Static_assert(GC_ROM_CACHE_MAX_FILE <= CACHE_BLOCKS * CACHE_BLOCK_SIZE, "a fetched file must fit in the blocks");
#endif

#if GC_ROM_STATS_INTERVAL != 0
#define REPORT_TICKS secs_to_ticks(GC_ROM_STATS_INTERVAL)
#else
#define REPORT_TICKS (~0ull)
#endif

typedef struct {
    unsigned int base;    /* ROM offset of the data */
    unsigned int size;    /* valid bytes; 0 if the block is empty */
    unsigned int lastUse; /* sUseClock at the last use (LRU) */
    unsigned char* data;
#if GC_ROM_FILE_CACHE
    int pending; /* MEM1->ARAM requests in req[] that may still read data */
    ARQRequest req[FC_BLOCK_PAGES];
#endif
} CacheBlock;

/* A file of the dmadata table, by physical ROM range */
typedef struct {
    unsigned int start;
    unsigned int end;
    unsigned short index; /* dmadata entry */
    unsigned char compressed;
    unsigned char pad;
} RomFile;

/* A resident range */
typedef struct {
    unsigned int start;   /* ROM range served */
    unsigned int end;
    unsigned int romBase; /* start rounded down to 32: the ROM offset stored at `addr` */
    unsigned int addr;    /* ARAM address (MEM1 address with GC_ROM_RESIDENT_ARAM 0) */
    unsigned int lane;    /* bounce buffer and mutex of its reads (GC_ROM_RESIDENT_ARAM 1) */
} ResRange;

/* Where the pieces of one gc_rom_read call came from, and what it cost; merged into the
 * statistics once per call */
#define SRC_RES (1u << 0)   /* resident range */
#define SRC_HIT (1u << 1)   /* block cache, no transfer */
#define SRC_ARAM (1u << 2)  /* block refilled from the file cache */
#define SRC_DISC (1u << 3)  /* block read from the disc */
#define SRC_FETCH (1u << 4) /* whole file read from the disc into the file cache */

typedef struct {
    unsigned int src;
    unsigned int resPieces;
    unsigned int resBytes;
    unsigned int hits;
    unsigned int aramFills;
    unsigned int aramFillBytes;
    unsigned int discFills;
    unsigned int fetches;
    unsigned int fetchBytes;
    unsigned int evictions;
    unsigned int discReads;
    unsigned int discBytes;
    u64 discTicks;
} ReadCtx;

typedef struct {
    unsigned int calls; /* gc_rom_read calls */
    unsigned int resPieces;
    unsigned int hits;
    unsigned int aramFills;
    unsigned int discFills;
    unsigned int fetches;
    unsigned int evictions;
    unsigned int discReads;
    unsigned int slow[3]; /* calls over SLOW_MS_1/2/3 */
    u64 bytes;
    u64 resBytes;
    u64 aramFillBytes;
    u64 fetchBytes;
    u64 discBytes;
    u64 ticks; /* inside gc_rom_read, lock waits included */
    u64 discTicks;
} RomStats;

/* Per reading thread (by OS thread id) */
#define STAT_THREADS 8
typedef struct {
    int used;
    int tid;
    unsigned int calls;
    unsigned int offRes; /* calls that went past the resident ranges */
    u64 bytes;
    u64 ticks;
    u64 maxTicks;
} ThreadStats;

static mutex_t sRomMutex = LWP_MUTEX_NULL;
static FILE* sRomFile;
static unsigned int sRomSize;
static CacheBlock sCache[CACHE_BLOCKS];
static unsigned char* sCacheMem; /* the blocks' storage, contiguous */
static unsigned int sUseClock;

#if GC_ROM_DVD_DIRECT
static int sDvdDirect; /* 1: disc reads go to DVD_ReadPrio at sDvdBase + offset */
static s64 sDvdBase;
static dvdcmdblk sDvdBlock;
#endif

/* The dmadata files, sorted by start (empty until a ROM is open) */
static RomFile* sFiles;
static unsigned int sFileCount;

/* Resident ranges. Written only while the ROM mutex is held (a re-open also takes every lane);
 * read without locks (a range is complete before sResCount counts it). */
static ResRange sRes[RES_MAX];
static volatile int sResCount;

/* Statistics: totals since boot, the totals at the last report, the window's slowest call */
static RomStats sStats;
static RomStats sReported;
static u64 sReportTime;
static u64 sWinMaxTicks;
static unsigned int sWinMaxOffset;
static int sWinMaxTid;
static ThreadStats sThreadStats[STAT_THREADS];
static volatile int sReporterTid = -2; /* the thread that logs the statistics */

#if GC_ROM_RESIDENT_ARAM
static mutex_t sAramMutex[ARAM_LANES] = { LWP_MUTEX_NULL, LWP_MUTEX_NULL }; /* per lane: its bounce buffer */
static lwpq_t sAramQueue = LWP_TQUEUE_NULL; /* threads waiting for a resident read or preload DMA */
static u32 sAramBlockLens[ARAM_BLOCK_SLOTS];
static unsigned int sAramPool;     /* our share of ARAM (taken once): start, 0 if none */
static unsigned int sAramPoolSize; /* bytes */
static unsigned int sAramPoolUsed; /* bytes handed out to resident ranges and the file cache */
static unsigned char* sAramBounce[ARAM_LANES];
static const unsigned int sAramBounceSize[ARAM_LANES] = { ARAM_BOUNCE_SIZE, ARAM_BOUNCE_SIZE_OTHER };
#endif

#if GC_ROM_FILE_CACHE
/* Per file (sFiles index): its pages and its place in the LRU list */
typedef struct {
    unsigned short first; /* first page, FC_NONE if not cached */
    unsigned short prev;  /* LRU neighbours (towards the most / least recently used) */
    unsigned short next;
    unsigned short pages;
} FcFile;

static lwpq_t sFcQueue = LWP_TQUEUE_NULL; /* threads waiting for file cache DMAs */
static unsigned int sFcAram;              /* ARAM address of page 0 */
static unsigned int sFcPages;             /* 0: no file cache */
static unsigned int sFcFreePages;
static unsigned int sFcCursor; /* next-fit allocation start */
static u32* sFcUsed;           /* page bitmap */
static unsigned short* sFcLink; /* per page: the file's next page */
static FcFile* sFc;
static unsigned int sFcTableFiles; /* entries in sFc */
static unsigned int sFcTablePages; /* entries in sFcLink */
static unsigned short sFcHead;     /* most recently used file */
static unsigned short sFcTail;     /* least recently used file */
static unsigned int sFcFiles;      /* files cached now */
static unsigned int sFcBytes;      /* their bytes */
#endif

#if GC_ROM_TRACE
typedef struct {
    int used;
    int tid;
    int file;           /* sFiles index, -1 outside files */
    unsigned int start; /* offset of the first read */
    unsigned int next;  /* end of the last read */
    unsigned int reads;
    unsigned int src;
    unsigned int discBytes;
    u64 first; /* start of the first read */
    u64 last;  /* end of the last read */
    u64 ticks; /* inside gc_rom_read */
    u64 discTicks;
} TraceRun;

#define TRACE_TOP 10
static TraceRun sRuns[STAT_THREADS];
static unsigned int* sResFileReads; /* per sFiles index: resident reads since the last report */
static unsigned int* sResFileBytes;
#endif

/* The libultra shim's thread id of the caller (weak: the stand-alone bridge test has no shim) */
extern int osGetThreadId(void* thread) __attribute__((weak));

static int thread_id(void) {
    return (osGetThreadId != NULL) ? osGetThreadId(NULL) : -1;
}

void gc_ogc_rom_init(void) {
    if (sRomMutex == LWP_MUTEX_NULL && LWP_MutexInit(&sRomMutex, false) != 0) {
        gc_halt("gc_ogc_rom_init: cannot create the ROM mutex");
    }
#if GC_ROM_RESIDENT_ARAM
    for (int lane = 0; lane < ARAM_LANES; lane++) {
        if (sAramMutex[lane] == LWP_MUTEX_NULL && LWP_MutexInit(&sAramMutex[lane], false) != 0) {
            gc_halt("gc_ogc_rom_init: cannot create the ARAM mutex");
        }
    }
    if (sAramQueue == LWP_TQUEUE_NULL && LWP_InitQueue(&sAramQueue) != 0) {
        gc_halt("gc_ogc_rom_init: cannot create the ARAM wait queue");
    }
#endif
#if GC_ROM_FILE_CACHE
    if (sFcQueue == LWP_TQUEUE_NULL && LWP_InitQueue(&sFcQueue) != 0) {
        gc_halt("gc_ogc_rom_init: cannot create the file cache wait queue");
    }
#endif
}

static unsigned int be32(const unsigned char* p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

static unsigned int min_u(unsigned int a, unsigned int b) {
    return (a < b) ? a : b;
}

/* Throughput for the logs, in 64 bits: bytes * 1000 overflows 32 bits from 4.3 MB on */
static unsigned int kb_per_second(u64 bytes, unsigned int ms) {
    return (ms != 0) ? (unsigned int)(bytes * 1000 / 1024 / ms) : 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* Disc                                                                                           */
/* ---------------------------------------------------------------------------------------------- */

static int file_read(unsigned int offset, void* dst, unsigned int size) {
    size_t got;

    if (fseek(sRomFile, offset, SEEK_SET) != 0) {
        gc_log("ROM: seek to %08X failed (errno %d)", offset, errno);
        return -1;
    }
    got = fread(dst, 1, size, sRomFile);
    if (got != size) {
        gc_log("ROM: read of %u bytes at %08X returned %u (errno %d)", size, offset, (unsigned int)got, errno);
        clearerr(sRomFile);
        return -1;
    }
    return 0;
}

/* Read [offset, offset + size) of the ROM from the disc. Caller holds the ROM mutex; the range lies in
 * the ROM. ctx (may be NULL) gets the transfer counted. */
static int disc_read(unsigned int offset, unsigned char* dst, unsigned int size, ReadCtx* ctx) {
    u64 t0 = gettime();
    u64 dt;
    int ret;

#if GC_ROM_DVD_DIRECT
    // DI DMA: 32-byte aligned buffer and length, offset in 4-byte units. Every caller's buffer is a
    // cache block (or the aligned staging) at a 32-byte aligned ROM offset; anything else is fread.
    if (sDvdDirect && ((unsigned int)dst & 31) == 0 && (size & 31) == 0 && (offset & 3) == 0) {
        s32 got = DVD_ReadPrio(&sDvdBlock, dst, size, sDvdBase + offset, 2);

        ret = (got == (s32)size) ? 0 : -1;
        if (ret != 0) {
            gc_log("ROM: DVD read of %u bytes at %08X returned %d (drive status %d); trying the file", size, offset,
                   (int)got, (int)DVD_GetDriveStatus());
            ret = file_read(offset, dst, size);
        }
    } else
#endif
    {
        ret = file_read(offset, dst, size);
    }

    dt = gettime() - t0;
    if (ctx != NULL) {
        ctx->discReads++;
        ctx->discBytes += size;
        ctx->discTicks += dt;
    }
    return ret;
}

#if GC_ROM_DVD_DIRECT
/* ROM on dvd: (an ISO9660 disc in the drive): find its first sector (libiso9660 reports it as st_ino)
 * and check that a direct read there returns the header just read through the file. Caller holds the
 * ROM mutex. */
static void dvd_direct_setup(const char* path, FILE* file, const unsigned char* header) {
    struct stat st;
    s64 base;
    s32 got;

    sDvdDirect = 0;
    if (strncmp(path, "dvd:", 4) != 0) {
        return;
    }
    if (fstat(fileno(file), &st) != 0 || st.st_ino == 0) {
        gc_log("ROM: no sector number for %s; reading through the file system", path);
        return;
    }
    base = (s64)st.st_ino * ISO_SECTOR_SIZE;
    got = DVD_ReadPrio(&sDvdBlock, sCacheMem, 0x40, base, 2);
    if (got != 0x40 || memcmp(sCacheMem, header, 0x40) != 0) {
        gc_log("ROM: direct DVD read at %08X+%llX does not match (%d); reading through the file system",
               (unsigned int)st.st_ino, (unsigned long long)base, (int)got);
        return;
    }
    sDvdBase = base;
    sDvdDirect = 1;
    gc_log("ROM: reading the disc directly (DVD_ReadPrio from disc offset %08llX, sector %u)",
           (unsigned long long)base, (unsigned int)st.st_ino);
}
#endif

/* ---------------------------------------------------------------------------------------------- */
/* Files                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

static int file_compare(const void* a, const void* b) {
    unsigned int sa = ((const RomFile*)a)->start;
    unsigned int sb = ((const RomFile*)b)->start;

    return (sa > sb) - (sa < sb);
}

/* Load the dmadata table into sFiles. Caller holds the ROM mutex; the cache blocks are scratch. */
static void files_load(void) {
    unsigned char* raw = sCacheMem;
    unsigned int count = 0;
    unsigned int i;

    sFileCount = 0;
    if (sFiles == NULL) {
        sFiles = gc_mem_alloc(DMADATA_MAX * sizeof(RomFile), 32);
        if (sFiles == NULL) {
            gc_log("ROM: no memory for the file table");
            return;
        }
    }
    if (disc_read(DMADATA_ROM, raw, DMADATA_SIZE, NULL) != 0) {
        gc_log("ROM: cannot read the dmadata table");
        return;
    }
    for (i = 0; i < DMADATA_MAX; i++) {
        const unsigned char* e = raw + i * 16;
        unsigned int vromStart = be32(e);
        unsigned int vromEnd = be32(e + 4);
        unsigned int romStart = be32(e + 8);
        unsigned int romEnd = be32(e + 12);
        unsigned int end;

        if (vromEnd == 0) {
            break;
        }
        if (romStart == DMA_NO_DATA) {
            continue;
        }
        // romEnd is 0 for a file stored uncompressed
        end = (romEnd != 0) ? romEnd : romStart + (vromEnd - vromStart);
        if (end <= romStart || end > sRomSize) {
            continue;
        }
        sFiles[count].start = romStart;
        sFiles[count].end = end;
        sFiles[count].index = (unsigned short)i;
        sFiles[count].compressed = (romEnd != 0);
        count++;
    }
    qsort(sFiles, count, sizeof(RomFile), file_compare);
    // The lookup needs disjoint ranges; clip an overlap (none in the US ROM)
    for (i = 0; i + 1 < count; i++) {
        if (sFiles[i].end > sFiles[i + 1].start) {
            sFiles[i].end = sFiles[i + 1].start;
        }
    }
    sFileCount = count;
}

/* The sFiles index of the file holding `offset`, or -1. Lock-free: the table only changes in
 * gc_ogc_rom_open. */
static int file_find(unsigned int offset) {
    unsigned int lo = 0;
    unsigned int hi = sFileCount;

    while (lo < hi) {
        unsigned int mid = (lo + hi) / 2;

        if (sFiles[mid].start <= offset) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo == 0 || offset >= sFiles[lo - 1].end) {
        return -1;
    }
    return (int)lo - 1;
}

/* ---------------------------------------------------------------------------------------------- */
/* ARAM                                                                                           */
/* ---------------------------------------------------------------------------------------------- */

#if GC_ROM_RESIDENT_ARAM
/* ARQ completion, in the ARAM interrupt: the ARQ has already marked the request finished */
static void aram_dma_done(ARQRequest* req) {
    LWP_ThreadBroadcast(sAramQueue);
}

/* Queue one resident-read or preload DMA (high priority, not split) */
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

/* Copy [offset, offset + size) of resident range r out of ARAM, through its lane's bounce buffer */
static void aram_read(const ResRange* r, unsigned int offset, unsigned char* dst, unsigned int size) {
    unsigned char* bounce = sAramBounce[r->lane];
    unsigned int bounceSize = sAramBounceSize[r->lane];
    ARQRequest req;

    LWP_MutexLock(sAramMutex[r->lane]);
    while (size != 0) {
        unsigned int spanStart = offset & ~(ARAM_ALIGN - 1);
        unsigned int head = offset - spanStart;
        unsigned int n = min_u(bounceSize - head, size);
        unsigned int spanLen = (head + n + ARAM_ALIGN - 1) & ~(ARAM_ALIGN - 1);

        // The DMA writes MEM1 behind the data cache: drop the bounce buffer's lines first.
        DCInvalidateRange(bounce, spanLen);
        aram_dma_start(&req, ARQ_ARAMTOMRAM, bounce, r->addr + (spanStart - r->romBase), spanLen);
        aram_dma_wait(&req, spanLen);
        memcpy(dst, bounce + head, n);
        offset += n;
        dst += n;
        size -= n;
    }
    LWP_MutexUnlock(sAramMutex[r->lane]);
}

/* The bounce buffer of a lane, allocated at its first use. Caller holds the ROM mutex. */
static int aram_lane_buffer(unsigned int lane) {
    if (sAramBounce[lane] == NULL) {
        sAramBounce[lane] = gc_mem_alloc(sAramBounceSize[lane], ARAM_ALIGN);
        if (sAramBounce[lane] == NULL) {
            gc_log("ROM: no memory for a %u KB ARAM bounce buffer", sAramBounceSize[lane] / 1024);
            return -1;
        }
    }
    return 0;
}

/* Take our share of ARAM, once: everything above the current AR_Alloc top but GC_ROM_ARAM_RESERVE
 * bytes. Resident ranges and the file cache are carved from it in order. */
static int aram_setup(void) {
    const char* how = "was already initialised";
    unsigned int top;
    unsigned int base;
    unsigned int size;

    if (sAramPool != 0) {
        return 0;
    }
    if (!AR_CheckInit()) {
        // AR_Init keeps ARAM 0-0x3FFF for the OS and DSP; AR_Alloc hands out memory from 0x4000
        // and records each length in the array it is given (with NULL it writes through NULL, so
        // whoever initialises ARAM must pass one).
        AR_Init(sAramBlockLens, ARAM_BLOCK_SLOTS);
        how = "initialised here";
    }
    // After AR_Init, which clears the DMA callback that ARQ_Init installs. No-op if already done.
    ARQ_Init();

    // The current top of the AR_Alloc stack
    top = AR_Alloc(0);
    AR_Free(NULL);
    base = (top + ARAM_ALIGN - 1) & ~(ARAM_ALIGN - 1);
    if (base + GC_ROM_ARAM_RESERVE + ARAM_BOUNCE_SIZE > AR_GetSize()) {
        gc_log("ROM: no room in ARAM (top %08X of %u KB)", top, AR_GetSize() / 1024);
        return -1;
    }
    size = (AR_GetSize() - base - GC_ROM_ARAM_RESERVE) & ~(ARAM_ALIGN - 1);
    AR_Alloc(base - top + size);
    sAramPool = base;
    sAramPoolSize = size;
    sAramPoolUsed = 0;
    gc_log("ARAM: %u KB (%u KB internal), %s; ROM data gets %08X-%08X (%u KB, %u KB left for others)",
           AR_GetSize() / 1024, AR_GetInternalSize() / 1024, how, base, base + size, size / 1024,
           GC_ROM_ARAM_RESERVE / 1024);
    return 0;
}

/* Hand out `size` bytes (a multiple of 32) of our ARAM share */
static int aram_take(unsigned int size, unsigned int* addr) {
    if (aram_setup() != 0) {
        return -1;
    }
    if (size > sAramPoolSize - sAramPoolUsed) {
        gc_log("ROM: no room in ARAM for %u KB (%u KB of %u KB used)", size / 1024, sAramPoolUsed / 1024,
               sAramPoolSize / 1024);
        return -1;
    }
    *addr = sAramPool + sAramPoolUsed;
    sAramPoolUsed += size;
    return 0;
}
#endif

/* ---------------------------------------------------------------------------------------------- */
/* Block cache and file cache                                                                     */
/* ---------------------------------------------------------------------------------------------- */

#if GC_ROM_FILE_CACHE
static void fc_dma_done(ARQRequest* req) {
    LWP_ThreadBroadcast(sFcQueue);
}

static void fc_wait(ARQRequest* req, int count) {
    u32 level;
    int i;

    _CPU_ISR_Disable(level);
    for (i = 0; i < count; i++) {
        while (*(volatile u32*)&req[i].state != ARQ_TASK_FINISHED) {
            LWP_ThreadSleep(sFcQueue);
        }
    }
    _CPU_ISR_Restore(level);
}
#endif

/* The block holding `offset`, or NULL */
static CacheBlock* block_find(unsigned int offset) {
    int i;

    for (i = 0; i < CACHE_BLOCKS; i++) {
        if (offset - sCache[i].base < sCache[i].size) {
            sCache[i].lastUse = ++sUseClock;
            return &sCache[i];
        }
    }
    return NULL;
}

/* The least recently used block, emptied and free to overwrite */
static CacheBlock* block_victim(void) {
    CacheBlock* victim = &sCache[0];
    int i;

    for (i = 1; i < CACHE_BLOCKS; i++) {
        if (sCache[i].lastUse < victim->lastUse) {
            victim = &sCache[i];
        }
    }
#if GC_ROM_FILE_CACHE
    if (victim->pending != 0) {
        fc_wait(victim->req, victim->pending);
        victim->pending = 0;
    }
#endif
    victim->size = 0;
    victim->lastUse = ++sUseClock;
    return victim;
}

/* Empty every block. Caller holds the ROM mutex. */
static void cache_reset(void) {
    int i;

    for (i = 0; i < CACHE_BLOCKS; i++) {
#if GC_ROM_FILE_CACHE
        if (sCache[i].pending != 0) {
            fc_wait(sCache[i].req, sCache[i].pending);
            sCache[i].pending = 0;
        }
#endif
        sCache[i].base = 0;
        sCache[i].size = 0;
        sCache[i].lastUse = 0;
    }
}

/* Read the 64 KB-aligned block at `base` from the disc */
static CacheBlock* block_fill_disc(unsigned int base, ReadCtx* ctx) {
    CacheBlock* b = block_victim();
    unsigned int size = min_u(sRomSize - base, CACHE_BLOCK_SIZE);

    if (disc_read(base, b->data, size, ctx) != 0) {
        return NULL;
    }
    b->base = base;
    b->size = size;
    ctx->src |= SRC_DISC;
    ctx->discFills++;
    return b;
}

#if GC_ROM_FILE_CACHE
/* The stored span of a file: its range widened to 32-byte boundaries (ARAM DMA granularity) */
static unsigned int fc_span_start(const RomFile* file) {
    return file->start & ~31u;
}

static unsigned int fc_span_end(const RomFile* file) {
    return (file->end + 31) & ~31u;
}

static int fc_cacheable(int f) {
    const RomFile* file = &sFiles[f];
    unsigned int size = file->end - file->start;
    int i;

    if (size > GC_ROM_CACHE_MAX_FILE || size > sFcPages * FC_PAGE_SIZE / 2) {
        return 0;
    }
    for (i = 0; i < sResCount; i++) {
        if (file->start < sRes[i].end && sRes[i].start < file->end) {
            return 0;
        }
    }
    return 1;
}

static void fc_lru_unlink(unsigned int f) {
    FcFile* e = &sFc[f];

    if (e->prev != FC_NONE) {
        sFc[e->prev].next = e->next;
    } else {
        sFcHead = e->next;
    }
    if (e->next != FC_NONE) {
        sFc[e->next].prev = e->prev;
    } else {
        sFcTail = e->prev;
    }
    e->prev = FC_NONE;
    e->next = FC_NONE;
}

static void fc_lru_push(unsigned int f) {
    FcFile* e = &sFc[f];

    e->prev = FC_NONE;
    e->next = sFcHead;
    if (sFcHead != FC_NONE) {
        sFc[sFcHead].prev = (unsigned short)f;
    } else {
        sFcTail = (unsigned short)f;
    }
    sFcHead = (unsigned short)f;
}

static void fc_touch(unsigned int f) {
    if (sFcHead != f) {
        fc_lru_unlink(f);
        fc_lru_push(f);
    }
}

/* Drop file f from the file cache */
static void fc_evict(unsigned int f, ReadCtx* ctx) {
    FcFile* e = &sFc[f];
    unsigned int p = e->first;

    while (p != FC_NONE) {
        sFcUsed[p >> 5] &= ~(1u << (p & 31));
        p = sFcLink[p];
    }
    sFcFreePages += e->pages;
    sFcFiles--;
    sFcBytes -= sFiles[f].end - sFiles[f].start;
    fc_lru_unlink(f);
    e->first = FC_NONE;
    e->pages = 0;
    if (ctx != NULL) {
        ctx->evictions++;
    }
}

/* One free page, next-fit from the cursor. A free page must exist. */
static unsigned int fc_page_alloc(void) {
    unsigned int words = (sFcPages + 31) / 32;
    unsigned int w = sFcCursor >> 5;
    u32 mask = ~0u << (sFcCursor & 31);
    unsigned int i;

    for (i = 0; i <= words; i++) {
        u32 free = ~sFcUsed[w] & mask;

        if (free != 0) {
            unsigned int p = (w << 5) + (unsigned int)__builtin_ctz(free);

            if (p < sFcPages) {
                sFcUsed[w] |= 1u << (p & 31);
                sFcCursor = (p + 1 < sFcPages) ? p + 1 : 0;
                return p;
            }
        }
        mask = ~0u;
        w = (w + 1 < words) ? w + 1 : 0;
    }
    gc_halt("ROM: file cache page bitmap is inconsistent (%u free pages)", sFcFreePages);
}

/* Give file f `count` pages, evicting the least recently used files as needed */
static void fc_alloc(unsigned int f, unsigned int count, ReadCtx* ctx) {
    FcFile* e = &sFc[f];
    unsigned int prev = FC_NONE;
    unsigned int i;

    while (sFcFreePages < count) {
        if (sFcTail == FC_NONE) {
            gc_halt("ROM: file cache: %u pages wanted, %u free and nothing to evict", count, sFcFreePages);
        }
        fc_evict(sFcTail, ctx);
    }
    for (i = 0; i < count; i++) {
        unsigned int p = fc_page_alloc();

        if (prev == FC_NONE) {
            e->first = (unsigned short)p;
        } else {
            sFcLink[prev] = (unsigned short)p;
        }
        prev = p;
    }
    sFcLink[prev] = FC_NONE;
    sFcFreePages -= count;
    e->pages = (unsigned short)count;
    sFcFiles++;
    sFcBytes += sFiles[f].end - sFiles[f].start;
    fc_lru_push(f);
}

/* Post the transfers between `mem` and file f's pages from page number `page` on, for `len` bytes (a
 * multiple of 32), one request per run of consecutive ARAM pages. Returns the number of requests. */
static int fc_post(unsigned int f, unsigned int page, unsigned char* mem, unsigned int len, u32 dir,
                   ARQRequest* req) {
    unsigned int p = sFc[f].first;
    int count = 0;

    while (page-- != 0) {
        p = sFcLink[p];
    }
    while (len != 0) {
        unsigned int runPage = p;
        unsigned int runLen = 0;
        unsigned int last;

        do {
            runLen += min_u(len - runLen, FC_PAGE_SIZE);
            last = p;
            p = sFcLink[p];
        } while (runLen < len && p == last + 1);
        ARQ_PostRequestAsync(&req[count++], FC_OWNER, dir, ARQ_PRIO_LO, sFcAram + runPage * FC_PAGE_SIZE,
                             MEM_VIRTUAL_TO_PHYSICAL(mem), runLen, fc_dma_done);
        mem += runLen;
        len -= runLen;
    }
    return count;
}

/* Read file f whole from the disc through the blocks (which keep it) and copy it into the file cache.
 * The copies to ARAM run while the next block is read; block_victim waits for them before a block
 * is reused. */
static int fc_fetch(unsigned int f, ReadCtx* ctx) {
    const RomFile* file = &sFiles[f];
    unsigned int start = fc_span_start(file);
    unsigned int end = fc_span_end(file);
    unsigned int pos;

    fc_alloc(f, (end - start + FC_PAGE_SIZE - 1) / FC_PAGE_SIZE, ctx);
    for (pos = start; pos < end; pos += CACHE_BLOCK_SIZE) {
        unsigned int len = min_u(end - pos, CACHE_BLOCK_SIZE);
        CacheBlock* b = block_victim();

        if (disc_read(pos, b->data, len, ctx) != 0) {
            fc_evict(f, NULL);
            return -1;
        }
        b->base = pos;
        b->size = len;
        // The DMA reads MEM1 behind the data cache
        DCFlushRange(b->data, len);
        b->pending = fc_post(f, (pos - start) / FC_PAGE_SIZE, b->data, len, ARQ_MRAMTOARAM, b->req);
    }
    ctx->src |= SRC_FETCH;
    ctx->fetches++;
    ctx->fetchBytes += end - start;
    return 0;
}

/* Refill a block from file f's pages, from the page holding `offset` on */
static CacheBlock* block_fill_aram(unsigned int f, unsigned int offset, ReadCtx* ctx) {
    const RomFile* file = &sFiles[f];
    unsigned int start = fc_span_start(file);
    unsigned int page = (offset - start) / FC_PAGE_SIZE;
    unsigned int base = start + page * FC_PAGE_SIZE;
    unsigned int len = min_u(fc_span_end(file) - base, CACHE_BLOCK_SIZE);
    CacheBlock* b = block_victim();
    int count;

    // The DMA writes MEM1 behind the data cache: drop the block's lines first
    DCInvalidateRange(b->data, len);
    count = fc_post(f, page, b->data, len, ARQ_ARAMTOMRAM, b->req);
    fc_wait(b->req, count);
    b->base = base;
    b->size = len;
    fc_touch(f);
    ctx->src |= SRC_ARAM;
    ctx->aramFills++;
    ctx->aramFillBytes += len;
    return b;
}
#endif

/* Fill a block that holds `offset`. Caller holds the ROM mutex. */
static CacheBlock* block_miss(unsigned int offset, ReadCtx* ctx) {
#if GC_ROM_FILE_CACHE
    int f;

    if (sFcPages != 0 && (f = file_find(offset)) >= 0 && fc_cacheable(f)) {
        if (sFc[f].first == FC_NONE && fc_fetch(f, ctx) == 0) {
            CacheBlock* b = block_find(offset);

            if (b != NULL) {
                return b;
            }
        }
        if (sFc[f].first != FC_NONE) {
            return block_fill_aram(f, offset, ctx);
        }
    }
#endif
    return block_fill_disc(offset & ~(CACHE_BLOCK_SIZE - 1), ctx);
}

/* Copy [offset, offset + size) through the blocks. Caller holds the ROM mutex. */
static int cached_read(unsigned int offset, unsigned char* dst, unsigned int size, ReadCtx* ctx) {
    while (size != 0) {
        CacheBlock* block = block_find(offset);
        unsigned int inBlock;
        unsigned int n;

        if (block != NULL) {
            ctx->src |= SRC_HIT;
            ctx->hits++;
        } else {
            block = block_miss(offset, ctx);
            if (block == NULL) {
                return -1;
            }
        }
        inBlock = offset - block->base;
        n = min_u(block->size - inBlock, size);
        memcpy(dst, block->data + inBlock, n);
        offset += n;
        dst += n;
        size -= n;
    }
    return 0;
}

/* ---------------------------------------------------------------------------------------------- */
/* Statistics and tracing                                                                         */
/* ---------------------------------------------------------------------------------------------- */

static ThreadStats* thread_stats(int tid) {
    ThreadStats* free = NULL;
    int i;

    for (i = 0; i < STAT_THREADS; i++) {
        if (sThreadStats[i].used && sThreadStats[i].tid == tid) {
            return &sThreadStats[i];
        }
        if (!sThreadStats[i].used && free == NULL) {
            free = &sThreadStats[i];
        }
    }
    if (free != NULL) {
        free->used = 1;
        free->tid = tid;
    }
    return free;
}

/* Merge one call's ReadCtx into the totals. Interrupts off: any thread may be in here. */
static void stats_merge(const ReadCtx* c, int tid, unsigned int offset, unsigned int size, u64 ticks) {
    unsigned int ms = (unsigned int)ticks_to_millisecs(ticks);
    ThreadStats* t;
    u32 level;

    _CPU_ISR_Disable(level);
    sStats.calls++;
    sStats.bytes += size;
    sStats.ticks += ticks;
    sStats.resPieces += c->resPieces;
    sStats.resBytes += c->resBytes;
    sStats.hits += c->hits;
    sStats.aramFills += c->aramFills;
    sStats.aramFillBytes += c->aramFillBytes;
    sStats.discFills += c->discFills;
    sStats.fetches += c->fetches;
    sStats.fetchBytes += c->fetchBytes;
    sStats.evictions += c->evictions;
    sStats.discReads += c->discReads;
    sStats.discBytes += c->discBytes;
    sStats.discTicks += c->discTicks;
    sStats.slow[0] += (ms >= SLOW_MS_1);
    sStats.slow[1] += (ms >= SLOW_MS_2);
    sStats.slow[2] += (ms >= SLOW_MS_3);
    if (ticks > sWinMaxTicks) {
        sWinMaxTicks = ticks;
        sWinMaxOffset = offset;
        sWinMaxTid = tid;
    }
    t = thread_stats(tid);
    if (t != NULL) {
        t->calls++;
        t->offRes += (c->src & ~SRC_RES) != 0;
        t->bytes += size;
        t->ticks += ticks;
        if (ticks > t->maxTicks) {
            t->maxTicks = ticks;
        }
    }
    _CPU_ISR_Restore(level);
}

/* Add a preload's disc transfers to the totals (not a gc_rom_read call) */
static void stats_add_disc(const ReadCtx* c) {
    u32 level;

    _CPU_ISR_Disable(level);
    sStats.discReads += c->discReads;
    sStats.discBytes += c->discBytes;
    sStats.discTicks += c->discTicks;
    _CPU_ISR_Restore(level);
}

/* "#<dmadata index>" of the file holding offset, for the logs */
static unsigned int file_label(unsigned int offset) {
    int f = file_find(offset);

    return (f >= 0) ? sFiles[f].index : 9999;
}

#if GC_ROM_TRACE
static void src_text(unsigned int src, char* out) {
    static const char* const names[] = { "res", "hit", "aram", "disc", "fetch" };
    unsigned int i;

    out[0] = '\0';
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (src & (1u << i)) {
            if (out[0] != '\0') {
                strcat(out, "+");
            }
            strcat(out, names[i]);
        }
    }
}

static void trace_log_run(const TraceRun* r) {
    char src[32];

    src_text(r->src, src);
    if (r->file >= 0) {
        const RomFile* file = &sFiles[r->file];

        gc_log("ROM: t%d #%u%s %07X+%X: %u reads %u B in %u us (%u us reading), disc %u KB %u us [%s]", r->tid,
               file->index, file->compressed ? "c" : "u", r->start, r->next - r->start, r->reads,
               r->next - r->start, (unsigned int)ticks_to_microsecs(r->last - r->first),
               (unsigned int)ticks_to_microsecs(r->ticks), r->discBytes / 1024,
               (unsigned int)ticks_to_microsecs(r->discTicks), src);
    } else {
        gc_log("ROM: t%d (no file) %07X+%X: %u reads in %u us, disc %u KB [%s]", r->tid, r->start, r->next - r->start,
               r->reads, (unsigned int)ticks_to_microsecs(r->ticks), r->discBytes / 1024, src);
    }
}

/* Account one call to the caller's run (consecutive reads of one file); log the run it ends */
static void trace_read(int tid, unsigned int offset, unsigned int size, const ReadCtx* c, u64 t0, u64 t1) {
    int f = file_find(offset);
    TraceRun done;
    TraceRun* run = NULL;
    int haveDone = 0;
    u32 level;
    int i;

    _CPU_ISR_Disable(level);
    for (i = 0; i < STAT_THREADS; i++) {
        if (sRuns[i].used && sRuns[i].tid == tid) {
            run = &sRuns[i];
            break;
        }
    }
    if (run == NULL) {
        for (i = 0; i < STAT_THREADS && run == NULL; i++) {
            if (!sRuns[i].used) {
                run = &sRuns[i];
            }
        }
        if (run == NULL) {
            run = &sRuns[0];
        }
    }
    if (run->used && (run->tid != tid || run->file != f || run->next != offset)) {
        done = *run;
        haveDone = 1;
        run->used = 0;
    }
    if (!run->used) {
        memset(run, 0, sizeof(*run));
        run->used = 1;
        run->tid = tid;
        run->file = f;
        run->start = offset;
        run->first = t0;
    }
    run->reads++;
    run->next = offset + size;
    run->src |= c->src;
    run->discBytes += c->discBytes;
    run->discTicks += c->discTicks;
    run->last = t1;
    run->ticks += t1 - t0;
    if (c->src == SRC_RES && f >= 0 && sResFileReads != NULL) {
        sResFileReads[f]++;
        sResFileBytes[f] += size;
    }
    _CPU_ISR_Restore(level);

    if (haveDone && (GC_ROM_TRACE >= 2 || (done.src & ~SRC_RES) != 0)) {
        trace_log_run(&done);
    }
}

/* The most-read resident files since the last report, then clear the counts */
static void trace_report_resident(void) {
    unsigned int top[TRACE_TOP];
    unsigned int reads[TRACE_TOP];
    unsigned int bytes[TRACE_TOP];
    unsigned int count = 0;
    char line[400];
    int len;
    unsigned int f;
    unsigned int i;
    u32 level;

    if (sResFileReads == NULL) {
        return;
    }
    _CPU_ISR_Disable(level);
    for (f = 0; f < sFileCount; f++) {
        unsigned int r = sResFileReads[f];
        unsigned int j;

        if (r == 0) {
            continue;
        }
        if (count < TRACE_TOP) {
            count++;
        } else if (r <= reads[count - 1]) {
            continue;
        }
        // Insert, keeping reads[] in descending order
        for (j = count - 1; j > 0 && reads[j - 1] < r; j--) {
            top[j] = top[j - 1];
            reads[j] = reads[j - 1];
            bytes[j] = bytes[j - 1];
        }
        top[j] = f;
        reads[j] = r;
        bytes[j] = sResFileBytes[f];
    }
    memset(sResFileReads, 0, sFileCount * sizeof(unsigned int));
    memset(sResFileBytes, 0, sFileCount * sizeof(unsigned int));
    _CPU_ISR_Restore(level);

    len = snprintf(line, sizeof(line), "ROM: resident files read most:");
    for (i = 0; i < count && len < (int)sizeof(line); i++) {
        len += snprintf(line + len, sizeof(line) - len, " #%u %ux %uKB;", sFiles[top[i]].index, reads[i],
                        bytes[i] / 1024);
    }
    if (count != 0) {
        gc_log("%s", line);
    }
}
#endif

/* Log the window since the last report. Called without locks, never on the audio thread. */
static void stats_report(u64 now) {
    RomStats d;
    ThreadStats threads[STAT_THREADS];
    u64 winMax;
    unsigned int maxOffset;
    int maxTid;
    unsigned int windowMs;
    unsigned int callUs;
    unsigned int fcFiles = 0;
    unsigned int fcKb = 0;
    unsigned int fcTotalKb = 0;
    char line[400];
    int len;
    int i;
    u32 level;

    _CPU_ISR_Disable(level);
    if (sReportTime == 0 || now - sReportTime < REPORT_TICKS) {
        // The first call only starts the first window (the time base may not start at 0 at boot)
        if (sReportTime == 0) {
            sReported = sStats;
            sWinMaxTicks = 0;
            memset(sThreadStats, 0, sizeof(sThreadStats));
            sReportTime = now;
        }
        _CPU_ISR_Restore(level);
        return;
    }
    windowMs = (unsigned int)ticks_to_millisecs(now - sReportTime);
    d.calls = sStats.calls - sReported.calls;
    d.resPieces = sStats.resPieces - sReported.resPieces;
    d.hits = sStats.hits - sReported.hits;
    d.aramFills = sStats.aramFills - sReported.aramFills;
    d.discFills = sStats.discFills - sReported.discFills;
    d.fetches = sStats.fetches - sReported.fetches;
    d.evictions = sStats.evictions - sReported.evictions;
    d.discReads = sStats.discReads - sReported.discReads;
    for (i = 0; i < 3; i++) {
        d.slow[i] = sStats.slow[i] - sReported.slow[i];
    }
    d.bytes = sStats.bytes - sReported.bytes;
    d.resBytes = sStats.resBytes - sReported.resBytes;
    d.aramFillBytes = sStats.aramFillBytes - sReported.aramFillBytes;
    d.fetchBytes = sStats.fetchBytes - sReported.fetchBytes;
    d.discBytes = sStats.discBytes - sReported.discBytes;
    d.ticks = sStats.ticks - sReported.ticks;
    d.discTicks = sStats.discTicks - sReported.discTicks;
    winMax = sWinMaxTicks;
    maxOffset = sWinMaxOffset;
    maxTid = sWinMaxTid;
    memcpy(threads, sThreadStats, sizeof(threads));
    memset(sThreadStats, 0, sizeof(sThreadStats));
    sReported = sStats;
    sWinMaxTicks = 0;
    sReportTime = now;
    _CPU_ISR_Restore(level);

#if GC_ROM_FILE_CACHE
    fcFiles = sFcFiles;
    fcKb = sFcBytes / 1024;
    fcTotalKb = sFcPages * FC_PAGE_SIZE / 1024;
#endif
    callUs = (unsigned int)ticks_to_microsecs(d.ticks);
    gc_log("ROM: last %u s: %u reads %u KB; resident %u KB; blocks: %u hits, %u from file cache (%u KB), %u from disc; "
           "%u files fetched (%u KB), %u evicted; file cache %u files %u/%u KB",
           windowMs / 1000, d.calls, (unsigned int)(d.bytes / 1024), (unsigned int)(d.resBytes / 1024), d.hits,
           d.aramFills, (unsigned int)(d.aramFillBytes / 1024), d.discFills, d.fetches,
           (unsigned int)(d.fetchBytes / 1024), d.evictions, fcFiles, fcKb, fcTotalKb);
    gc_log("ROM: last %u s: disc %u reads %u KB in %u ms (%u KB/s, %u KB/min); reads %u us avg, slowest %u us "
           "(t%d #%u %07X); over %u/%u/%u ms: %u/%u/%u",
           windowMs / 1000, d.discReads, (unsigned int)(d.discBytes / 1024),
           (unsigned int)ticks_to_millisecs(d.discTicks),
           kb_per_second(d.discBytes, (unsigned int)ticks_to_millisecs(d.discTicks)),
           (windowMs != 0) ? (unsigned int)(d.discBytes * 60 / 1024 * 1000 / windowMs) : 0,
           (d.calls != 0) ? callUs / d.calls : 0, (unsigned int)ticks_to_microsecs(winMax), maxTid,
           file_label(maxOffset), maxOffset, SLOW_MS_1, SLOW_MS_2, SLOW_MS_3, d.slow[0], d.slow[1], d.slow[2]);

    len = snprintf(line, sizeof(line), "ROM: per thread (reads, KB, avg us, max us, past resident):");
    for (i = 0; i < STAT_THREADS && len < (int)sizeof(line); i++) {
        const ThreadStats* t = &threads[i];

        if (t->used) {
            len += snprintf(line + len, sizeof(line) - len, " t%d %u %u %u %u %u;", t->tid, t->calls,
                            (unsigned int)(t->bytes / 1024),
                            (t->calls != 0) ? (unsigned int)(ticks_to_microsecs(t->ticks) / t->calls) : 0,
                            (unsigned int)ticks_to_microsecs(t->maxTicks), t->offRes);
        }
    }
    gc_log("%s", line);
#if GC_ROM_TRACE
    trace_report_resident();
#endif
}

/* ---------------------------------------------------------------------------------------------- */
/* Reads                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

/* The resident range holding `offset`, or NULL; *limit is lowered to the start of the next
 * range above `offset` */
static const ResRange* res_find(unsigned int offset, unsigned int* limit) {
    int count = sResCount;
    int i;

    for (i = 0; i < count; i++) {
        const ResRange* r = &sRes[i];

        if (offset >= r->start && offset < r->end) {
            return r;
        }
        if (r->start > offset && r->start < *limit) {
            *limit = r->start;
        }
    }
    return NULL;
}

int gc_rom_read(unsigned int rom_offset, void* dst, unsigned int size) {
    unsigned char* out = dst;
    unsigned int offset = rom_offset;
    unsigned int left = size;
    ReadCtx ctx;
    u64 t0 = gettime();
    u64 t1;
    int tid;

    if (sRomFile == NULL) {
        gc_log("gc_rom_read(%08X, %u): no ROM is open", rom_offset, size);
        return -1;
    }

    memset(&ctx, 0, sizeof(ctx));
    while (left != 0) {
        unsigned int limit = sRomSize;
        const ResRange* r;
        unsigned int n;

        if (offset >= sRomSize) {
            memset(out, 0, left);
            break;
        }
        r = res_find(offset, &limit);
        if (r != NULL) {
            n = min_u(r->end - offset, left);
#if GC_ROM_RESIDENT_ARAM
            aram_read(r, offset, out, n);
#else
            memcpy(out, (const unsigned char*)r->addr + (offset - r->start), n);
#endif
            ctx.src |= SRC_RES;
            ctx.resPieces++;
            ctx.resBytes += n;
        } else {
            int ret;

            n = min_u(limit - offset, left);
            LWP_MutexLock(sRomMutex);
            ret = cached_read(offset, out, n, &ctx);
            LWP_MutexUnlock(sRomMutex);
            if (ret != 0) {
                return -1;
            }
        }
        offset += n;
        out += n;
        left -= n;
    }

    t1 = gettime();
    tid = thread_id();
    stats_merge(&ctx, tid, rom_offset, size, t1 - t0);
#if GC_ROM_TRACE
    trace_read(tid, rom_offset, size, &ctx, t0, t1);
#endif
    // Reports come from the last thread that read past the resident ranges (DmaMgr, in the game), so
    // never from the audio thread, which only reads the audio data and has a deadline (the log may
    // write to SD).
    if ((ctx.src & ~SRC_RES) != 0) {
        sReporterTid = tid;
    }
    if (GC_ROM_STATS_INTERVAL != 0 && tid == sReporterTid && (sReportTime == 0 || t1 - sReportTime >= REPORT_TICKS)) {
        stats_report(t1);
    }
    return 0;
}

unsigned int gc_rom_size(void) {
    return sRomSize;
}

/* ---------------------------------------------------------------------------------------------- */
/* Open                                                                                           */
/* ---------------------------------------------------------------------------------------------- */

static const char* rom_error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static const char* rom_error(const char* fmt, ...) {
    static char sMessage[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(sMessage, sizeof(sMessage), fmt, args);
    va_end(args);
    return sMessage;
}

/* Forget the resident ranges and the file cache (a new ROM file). Caller holds the ROM mutex. */
static void rom_reset(void) {
#if GC_ROM_RESIDENT_ARAM
    int lane;

    // Every lane, in order: no resident read is in flight while the ranges' ARAM is given back
    for (lane = 0; lane < ARAM_LANES; lane++) {
        LWP_MutexLock(sAramMutex[lane]);
    }
#endif
    sResCount = 0;
#if GC_ROM_RESIDENT_ARAM
    sAramPoolUsed = 0;
    for (lane = ARAM_LANES - 1; lane >= 0; lane--) {
        LWP_MutexUnlock(sAramMutex[lane]);
    }
#endif
    cache_reset(); // also waits for file cache DMAs still reading the blocks
#if GC_ROM_FILE_CACHE
    sFcPages = 0;
#endif
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
    rom_reset();
    if (sRomFile != NULL) {
        fclose(sRomFile);
    }
    sRomFile = file;
    sRomSize = (unsigned int)size;
#if GC_ROM_DVD_DIRECT
    dvd_direct_setup(path, file, header);
#endif
    files_load();
    cache_reset();
    LWP_MutexUnlock(sRomMutex);

#if GC_ROM_TRACE
    if (sResFileReads == NULL) {
        sResFileReads = gc_mem_alloc(DMADATA_MAX * sizeof(unsigned int), 32);
        sResFileBytes = gc_mem_alloc(DMADATA_MAX * sizeof(unsigned int), 32);
        if (sResFileReads == NULL || sResFileBytes == NULL) {
            sResFileReads = NULL;
        } else {
            memset(sResFileReads, 0, DMADATA_MAX * sizeof(unsigned int));
            memset(sResFileBytes, 0, DMADATA_MAX * sizeof(unsigned int));
        }
    }
#endif

    gc_log("ROM: %.20s, %u MiB, code %.4s, CRC %08X %08X: OK; %u files in the dmadata table",
           (const char*)header + 0x20, sRomSize >> 20, (const char*)header + 0x3B, crc1, crc2, sFileCount);
    return NULL;
}

/* ---------------------------------------------------------------------------------------------- */
/* Preload and file cache setup                                                                   */
/* ---------------------------------------------------------------------------------------------- */

#if GC_ROM_RESIDENT_ARAM
#if GC_ROM_ARAM_VERIFY
static unsigned int checksum(unsigned int h, const unsigned char* p, unsigned int size) {
    while (size-- != 0) {
        h = h * 31 + *p++;
    }
    return h;
}

/* Read [r->start, r->end) back through aram_read in odd-sized pieces into an odd address, as callers
 * do, and compare with the checksum of the data that was loaded. Caller holds the ROM mutex
 * (the cache blocks are the destination). */
static int aram_verify(const ResRange* r, unsigned int expected) {
    unsigned char* buf = sCacheMem + 3;
    unsigned int offset = r->start;
    unsigned int h = 0;
    unsigned int pieces = 0;
    u64 t0 = gettime();

    while (offset < r->end) {
        unsigned int n = min_u(1 + (pieces * 7919u) % (ARAM_BOUNCE_SIZE + 5000), r->end - offset);

        aram_read(r, offset, buf, n);
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
    ReadCtx ctx;
    ResRange* r;
    unsigned int addr;
    unsigned int lane;
    unsigned int offset;
    unsigned int pendingLen = 0;
    int cur = 0;
    int ret = 0;
    u64 t0;
    unsigned int ms;
#if GC_ROM_ARAM_VERIFY
    unsigned int sum = 0;
#endif

    gc_log("ROM: loading %08X-%08X (%u KB) into ARAM...", start, end, (end - start) / 1024);
    t0 = gettime();
    memset(&ctx, 0, sizeof(ctx));
    // The cache blocks double as two staging buffers: the disc fills one while the other is
    // DMA'd to ARAM. Holding the ROM mutex keeps cached reads out meanwhile. No lane mutex is
    // needed: the new range's ARAM is not served until sResCount counts it.
    stage[0] = sCacheMem;
    stage[1] = sCacheMem + PRELOAD_CHUNK;
    LWP_MutexLock(sRomMutex);
    cache_reset();
    // The first range (the audio data) gets a lane of its own, the others share the second
    lane = (sResCount == 0) ? 0 : 1;
    if (aram_lane_buffer(lane) != 0 || aram_take(romEnd - romBase, &addr) != 0) {
        LWP_MutexUnlock(sRomMutex);
        return -1;
    }
    for (offset = romBase; offset < romEnd; offset += PRELOAD_CHUNK) {
        unsigned int n = min_u(romEnd - offset, PRELOAD_CHUNK);
        // Past the end of the ROM (only when end is an unaligned end of the file) the staging
        // bytes are stale; they land in ARAM but are never served.
        unsigned int fileBytes = min_u(n, sRomSize - offset);

        if (disc_read(offset, stage[cur], fileBytes, &ctx) != 0) {
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
        aram_dma_start(&req, ARQ_MRAMTOARAM, stage[cur], addr + (offset - romBase), n);
        pendingLen = n;
        cur ^= 1;
    }
    if (pendingLen != 0) {
        aram_dma_wait(&req, pendingLen);
    }
    r = &sRes[sResCount];
    if (ret == 0) {
        r->start = start;
        r->end = end;
        r->romBase = romBase;
        r->addr = addr;
        r->lane = lane;
    } else {
        sAramPoolUsed -= romEnd - romBase;
    }
    if (ret == 0) {
        ms = (unsigned int)ticks_to_millisecs(gettime() - t0);
        gc_log("ROM: resident range %08X-%08X loaded into ARAM at %08X in %u ms (%u KB/s); %u KB of ARAM left", start,
               end, addr, ms, kb_per_second(end - start, ms), (sAramPoolSize - sAramPoolUsed) / 1024);
#if GC_ROM_ARAM_VERIFY
        if (aram_verify(r, sum) != 0) {
            sAramPoolUsed -= romEnd - romBase;
            ret = -1;
        }
#endif
        if (ret == 0) {
            sResCount = sResCount + 1;
        }
    }
    cache_reset();
    LWP_MutexUnlock(sRomMutex);
    stats_add_disc(&ctx);
    return ret;
}
#else
static int preload_mem1(unsigned int start, unsigned int end) {
    unsigned char* buffer;
    ReadCtx ctx;
    ResRange* r;
    unsigned int offset;
    u64 t0;
    unsigned int ms;

    buffer = gc_mem_alloc(end - start, 32);
    if (buffer == NULL) {
        return -1;
    }

    gc_log("ROM: loading %08X-%08X (%u KB) into RAM...", start, end, (end - start) / 1024);
    t0 = gettime();
    memset(&ctx, 0, sizeof(ctx));
    LWP_MutexLock(sRomMutex);
    for (offset = start; offset < end; offset += PRELOAD_CHUNK) {
        if (disc_read(offset, buffer + (offset - start), min_u(end - offset, PRELOAD_CHUNK), &ctx) != 0) {
            LWP_MutexUnlock(sRomMutex);
            return -1;
        }
    }
    r = &sRes[sResCount];
    r->start = start;
    r->end = end;
    r->romBase = start;
    r->addr = (unsigned int)buffer;
    sResCount = sResCount + 1;
    LWP_MutexUnlock(sRomMutex);

    ms = (unsigned int)ticks_to_millisecs(gettime() - t0);
    gc_log("ROM: resident range loaded in %u ms (%u KB/s)", ms, kb_per_second(end - start, ms));
    stats_add_disc(&ctx);
    return 0;
}
#endif

int gc_ogc_rom_preload(unsigned int start, unsigned int end) {
    int i;

    if (sRomFile == NULL || start >= end || end > sRomSize || sResCount == RES_MAX) {
        gc_log("ROM: cannot preload %08X-%08X", start, end);
        return -1;
    }
    for (i = 0; i < sResCount; i++) {
        if (start < sRes[i].end && sRes[i].start < end) {
            gc_log("ROM: %08X-%08X overlaps the resident range %08X-%08X", start, end, sRes[i].start, sRes[i].end);
            return -1;
        }
    }
#if GC_ROM_RESIDENT_ARAM
    return preload_aram(start, end);
#else
    return preload_mem1(start, end);
#endif
}

int gc_ogc_rom_cache_init(void) {
#if GC_ROM_FILE_CACHE
    unsigned int pages;
    unsigned int addr;
    unsigned int i;

    if (sRomFile == NULL || sFileCount == 0) {
        gc_log("ROM: no file cache without a ROM and its file table");
        return -1;
    }
    LWP_MutexLock(sRomMutex);
    if (aram_setup() != 0) {
        LWP_MutexUnlock(sRomMutex);
        return -1;
    }
    pages = min_u((sAramPoolSize - sAramPoolUsed) / FC_PAGE_SIZE, FC_NONE - 1);
    if (pages < FC_MIN_PAGES) {
        LWP_MutexUnlock(sRomMutex);
        gc_log("ROM: no file cache: only %u KB of ARAM left", pages * FC_PAGE_SIZE / 1024);
        return -1;
    }
    if (sFcTablePages < pages || sFcTableFiles < sFileCount) {
        // Allocated once (a later init after a re-open never needs more)
        sFcUsed = gc_mem_alloc(((pages + 31) / 32) * 4, 32);
        sFcLink = gc_mem_alloc(pages * sizeof(unsigned short), 32);
        sFc = gc_mem_alloc(DMADATA_MAX * sizeof(FcFile), 32);
        if (sFcUsed == NULL || sFcLink == NULL || sFc == NULL) {
            sFcTablePages = 0;
            sFcTableFiles = 0;
            LWP_MutexUnlock(sRomMutex);
            gc_log("ROM: no memory for the file cache tables");
            return -1;
        }
        sFcTablePages = pages;
        sFcTableFiles = DMADATA_MAX;
    }
    aram_take(pages * FC_PAGE_SIZE, &addr);

    memset(sFcUsed, 0, ((pages + 31) / 32) * 4);
    for (i = 0; i < sFileCount; i++) {
        sFc[i].first = FC_NONE;
        sFc[i].prev = FC_NONE;
        sFc[i].next = FC_NONE;
        sFc[i].pages = 0;
    }
    sFcHead = FC_NONE;
    sFcTail = FC_NONE;
    sFcFiles = 0;
    sFcBytes = 0;
    sFcCursor = 0;
    sFcFreePages = pages;
    sFcAram = addr;
    sFcPages = pages;
    LWP_MutexUnlock(sRomMutex);

    gc_log("ROM: file cache: %u KB of ARAM at %08X (%u pages of %u bytes), files up to %u KB",
           pages * FC_PAGE_SIZE / 1024, addr, pages, FC_PAGE_SIZE,
           min_u(GC_ROM_CACHE_MAX_FILE, pages * FC_PAGE_SIZE / 2) / 1024);
    return 0;
#else
    return -1;
#endif
}

void gc_ogc_rom_print_stats(void) {
    RomStats s;
    u32 level;

    _CPU_ISR_Disable(level);
    s = sStats;
    _CPU_ISR_Restore(level);
    gc_log("ROM: since boot: %u reads %u KB (%u KB resident, %u block hits, %u from file cache, %u from disc, %u "
           "files fetched); disc %u reads %u KB in %u ms (%u KB/s); over %u/%u/%u ms: %u/%u/%u",
           s.calls, (unsigned int)(s.bytes / 1024), (unsigned int)(s.resBytes / 1024), s.hits, s.aramFills,
           s.discFills, s.fetches, s.discReads, (unsigned int)(s.discBytes / 1024),
           (unsigned int)ticks_to_millisecs(s.discTicks),
           kb_per_second(s.discBytes, (unsigned int)ticks_to_millisecs(s.discTicks)), SLOW_MS_1, SLOW_MS_2,
           SLOW_MS_3, s.slow[0], s.slow[1], s.slow[2]);
}

#if GC_ROM_CHECK
/* ROM self-test (GC_ROM_CHECK=1 builds, make GC_ROM_CHECK=1): read the whole ROM back through gc_rom_read, the path
 * the game's DMA takes (resident ARAM ranges, the ARAM file cache, the disc or disc image), and compare the CRC32 of
 * every 64 KB with a table made from the build machine's ROM (port/gc/tools/gen_rom_crc.py). A mismatch names the
 * chunk and the dmadata files in it; each bad chunk is read a second time to tell a wrong copy (same CRC again) from
 * a read that varies. */
extern const unsigned int gGcRomCheckSize, gGcRomCheckChunk, gGcRomCheckCount, gGcRomCheckCrc[];

static unsigned int crc32_update(unsigned int crc, const unsigned char* p, unsigned int n) {
    static unsigned int table[256];
    unsigned int i;

    if (table[1] == 0) {
        for (i = 0; i < 256; i++) {
            unsigned int c = i;
            int k;

            for (k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            table[i] = c;
        }
    }
    crc = ~crc;
    for (i = 0; i < n; i++) {
        crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/* dmadata indices of the files overlapping [start, end), as text */
static void rom_check_files(unsigned int start, unsigned int end, char* out, unsigned int outSize) {
    unsigned int i, len = 0;

    out[0] = '\0';
    for (i = 0; i < sFileCount && len + 8 < outSize; i++) {
        if (sFiles[i].start < end && sFiles[i].end > start) {
            len += (unsigned int)snprintf(out + len, outSize - len, " %u", sFiles[i].index);
        }
    }
}

void gc_ogc_rom_check(void) {
    unsigned char* buf;
    unsigned int chunk = gGcRomCheckChunk;
    unsigned int i, bad = 0, unstable = 0;
    u64 t0 = gettime();

    if (sRomSize != gGcRomCheckSize) {
        gc_log("ROM check: the ROM is %u bytes, the table was made for %u; skipped", sRomSize, gGcRomCheckSize);
        return;
    }
    buf = gc_mem_alloc(chunk, 32);
    if (buf == NULL) {
        gc_log("ROM check: no memory");
        return;
    }
    gc_log("ROM check: reading %u KB in %u chunks through gc_rom_read...", sRomSize / 1024, gGcRomCheckCount);
    for (i = 0; i < gGcRomCheckCount; i++) {
        unsigned int off = i * chunk;
        unsigned int n = (sRomSize - off < chunk) ? sRomSize - off : chunk;
        unsigned int crc;

        if (gc_rom_read(off, buf, n) != 0) {
            gc_log("ROM check: read error at %08X", off);
            bad++;
            continue;
        }
        crc = crc32_update(0, buf, n);
        if (crc != gGcRomCheckCrc[i]) {
            char files[96];
            unsigned int crc2 = 0xFFFFFFFFu;

            if (gc_rom_read(off, buf, n) == 0) {
                crc2 = crc32_update(0, buf, n);
            }
            unstable += (crc2 != crc);
            if (bad < 40) {
                rom_check_files(off, off + n, files, sizeof(files));
                gc_log("ROM check: BAD %08X-%08X: crc %08X, again %08X, want %08X; files%s", off, off + n, crc, crc2,
                       gGcRomCheckCrc[i], files);
            }
            bad++;
        }
    }
    gc_log("ROM check: %u of %u chunks wrong (%u read differently the second time), %u ms", bad, gGcRomCheckCount,
           unstable, (unsigned int)ticks_to_millisecs(gettime() - t0));
}
#endif
