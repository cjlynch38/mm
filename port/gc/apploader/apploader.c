/*
 * mmgcport apploader: a small open-source GameCube apploader.
 *
 * SPDX-License-Identifier: CC0-1.0
 * Written from scratch for the Majora's Mask GameCube port and dedicated to the public domain
 * (Creative Commons CC0 1.0: https://creativecommons.org/publicdomain/zero/1.0/). It contains no
 * Nintendo code; it implements the apploader interface as documented by YAGCD ("Yet Another
 * GameCube Documentation", sections on the disc layout and the apploader).
 *
 * The boot program (the IPL's BS2, Dolphin's emulated BS2, or Swiss with "BS2 boot") copies the
 * apploader image from disc offset 0x2460 to 0x81200000 and calls the entry point named in the
 * apploader header (disc offset 0x2450):
 *
 *     apploader_entry(&init, &main, &close);
 *     init(report);                                   report: an OSReport-like printf
 *     while (main(&dst, &size, &offset))              each call asks for one disc read...
 *         DVD-read `size` bytes at disc `offset` into `dst`;   ...which the boot program does
 *     entry = close();                                then it jumps to entry
 *
 * Every request satisfies the DI DMA rules: dst 32-byte aligned, size a multiple of 32, offset a
 * multiple of 4. What this apploader requests, in order:
 *   1. boot.bin (disc header, 0x440 bytes at 0): DOL and FST locations;
 *   2. the main DOL's header, then every text and data section straight to its load address.
 *      Section pieces that break the DMA rules (unaligned address, offset or length) go through a
 *      bounce buffer and are copied into place on the next call;
 *   3. bi2.bin (0x2000 bytes at 0x440) and the FST, to the top of MEM1 as retail apploaders and
 *      Swiss do, recorded in low memory (0x80000034 arena hi, 0x80000038 FST, 0x8000003C FST max
 *      size, 0x800000F4 bi2).
 * close() then writes the data cache back, invalidates the instruction cache over the loaded code
 * and, if the DOL starts with libogc's argv area ("_arg" at entry + 4), passes argv[0] = "dvd:/"
 * the way Swiss and other loaders pass arguments. The port's storage code reads it to learn that
 * it was booted from the disc in the DVD drive.
 *
 * The DOL's BSS is not cleared here: the program's own startup code does that (libogc's crt0 and
 * the Nintendo SDK's both do).
 *
 * The apploader runs in the boot program's context: it must not touch memory above 0x81300000
 * (the IPL lives there) and has no C runtime, so its .bss is cleared by apploader_entry itself.
 */

typedef unsigned char u8;
typedef unsigned int u32;

typedef void (*ReportFunc)(const char* fmt, ...);
typedef void (*InitFunc)(ReportFunc report);
typedef int (*MainFunc)(void** dst, u32* size, u32* offset);
typedef void* (*CloseFunc)(void);

#define DISC_MAGIC 0xC2339F3Du
#define DISC_HEADER_SIZE 0x440u /* boot.bin */
#define BI2_OFFSET 0x440u
#define BI2_SIZE 0x2000u

#define DOL_TEXT_SECTIONS 7
#define DOL_SECTIONS 18

#define MEM1_START 0x80000000u
#define MEM1_END 0x81800000u
/* The apploader's own window (0x81200000-0x81300000); the IPL itself runs above it */
#define APPLOADER_BASE 0x81200000u
#define APPLOADER_LIMIT 0x81300000u
/* DOL sections may not load below the start of the OS globals area */
#define DOL_LOWEST 0x80003000u

#define BOUNCE_SIZE 0x8000u
#define DMA_ALIGN 32u

/* libogc's crt0 reserves an argv block after the branch at the entry point */
#define ARGV_MAGIC 0x5F617267u /* '_arg' */
#define ARGV0 "dvd:/"

#define LOWMEM(offset) (*(volatile u32*)(MEM1_START + (offset)))

typedef struct {
    u32 offset[DOL_SECTIONS]; /* file offsets: 7 text sections, then 11 data sections */
    u32 address[DOL_SECTIONS];
    u32 size[DOL_SECTIONS];
    u32 bssAddress;
    u32 bssSize;
    u32 entry;
    u32 pad[7];
} DolHeader;

typedef struct {
    u32 magic;
    char* commandLine;
    u32 length;
    u32 argc;
    char** argv;
    char** endArgv;
} ArgvBlock;

enum {
    STATE_START,
    STATE_DISC_HEADER,
    STATE_DOL_HEADER,
    STATE_SECTIONS,
    STATE_BI2,
    STATE_FST,
    STATE_DONE,
};

extern u8 __bss_start[];
extern u8 __bss_end[];

/* The DMA destinations own whole cache lines (aligned, padded to 32), so invalidating them never
 * drops another variable's writes. */
static u8 sDiscHeader[DISC_HEADER_SIZE] __attribute__((aligned(32)));
static DolHeader sDol __attribute__((aligned(32)));
static u8 sBounce[BOUNCE_SIZE] __attribute__((aligned(32)));

static ReportFunc sReport;
static int sState;
static u32 sDolOffset;
static u32 sFstOffset;
static u32 sFstSize;
static u32 sFstAddress;
static u32 sBi2Address;
static u32 sArgAddress;
static int sSection;
static u32 sSectionDone;
static u32 sLoadedBytes;

/* Bounce copy still to do: sCopyLength bytes from sBounce + sCopySkip to sCopyDst */
static u8* sCopyDst;
static u32 sCopySkip;
static u32 sCopyLength;
static u32 sCopySpan;

/* GCC may emit calls to these even in freestanding code */
void* memcpy(void* dst, const void* src, unsigned int n);
void* memset(void* dst, int c, unsigned int n);

void* memcpy(void* dst, const void* src, unsigned int n) {
    u8* d = dst;
    const u8* s = src;

    while (n-- != 0) {
        *d++ = *s++;
    }
    return dst;
}

void* memset(void* dst, int c, unsigned int n) {
    u8* d = dst;

    while (n-- != 0) {
        *d++ = (u8)c;
    }
    return dst;
}

static u32 be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/* Drop the cached lines over [p, p + n) (DMA destinations, before the DMA and before reading) */
static void dc_invalidate(const void* p, u32 n) {
    u32 a = (u32)p & ~(DMA_ALIGN - 1);
    u32 end = (u32)p + n;

    for (; a < end; a += DMA_ALIGN) {
        __asm__ volatile("dcbi 0,%0" : : "r"(a) : "memory");
    }
    __asm__ volatile("sync" : : : "memory");
}

/* Write the cached lines over [p, p + n) back to memory */
static void dc_flush(const void* p, u32 n) {
    u32 a = (u32)p & ~(DMA_ALIGN - 1);
    u32 end = (u32)p + n;

    for (; a < end; a += DMA_ALIGN) {
        __asm__ volatile("dcbf 0,%0" : : "r"(a) : "memory");
    }
    __asm__ volatile("sync" : : : "memory");
}

/* Drop stale instructions over [p, p + n) (after dc_flush) */
static void ic_invalidate(const void* p, u32 n) {
    u32 a = (u32)p & ~(DMA_ALIGN - 1);
    u32 end = (u32)p + n;

    for (; a < end; a += DMA_ALIGN) {
        __asm__ volatile("icbi 0,%0" : : "r"(a) : "memory");
    }
    __asm__ volatile("sync; isync" : : : "memory");
}

static void report(const char* message) {
    if (sReport != 0) {
        sReport("%s", message);
    }
}

/* A broken disc: say why and stop here, since returning would jump into garbage */
static void fail(const char* message) __attribute__((noreturn));
static void fail(const char* message) {
    report("mmgcport apploader: cannot boot this disc:\n");
    report(message);
    for (;;) {
        __asm__ volatile("" : : : "memory");
    }
}

static int request(void** dst, u32* size, u32* offset, void* to, u32 length, u32 from) {
    dc_invalidate(to, length);
    *dst = to;
    *size = length;
    *offset = from;
    return 1;
}

static int section_is_text(int i) {
    return i < DOL_TEXT_SECTIONS;
}

static void check_dol(void) {
    int i;
    int entryFound = 0;

    for (i = 0; i < DOL_SECTIONS; i++) {
        u32 a = sDol.address[i];
        u32 n = sDol.size[i];

        if (n == 0) {
            continue;
        }
        if (a < DOL_LOWEST || a >= APPLOADER_BASE || n > APPLOADER_BASE - a) {
            fail("a DOL section lies outside 0x80003000-0x81200000\n");
        }
        if (section_is_text(i) && sDol.entry >= a && sDol.entry < a + n) {
            entryFound = 1;
        }
    }
    if (!entryFound) {
        fail("the DOL's entry point is not in a text section\n");
    }
}

/* The next read of the DOL's sections, or 0 once all are loaded */
static int section_request(void** dst, u32* size, u32* offset) {
    while (sSection < DOL_SECTIONS) {
        u32 total = sDol.size[sSection];
        u32 address;
        u32 from;
        u32 left;
        u32 aligned;
        u32 n;

        if (sSectionDone >= total) {
            sSection++;
            sSectionDone = 0;
            continue;
        }
        address = sDol.address[sSection] + sSectionDone;
        from = sDolOffset + sDol.offset[sSection] + sSectionDone;
        left = total - sSectionDone;

        if ((address & (DMA_ALIGN - 1)) == 0 && (from & 3) == 0 && left >= DMA_ALIGN) {
            // Straight to the load address, whole cache lines only; the tail goes through the bounce buffer
            n = left & ~(DMA_ALIGN - 1);
            sSectionDone += n;
            sLoadedBytes += n;
            return request(dst, size, offset, (void*)address, n, from);
        }

        // Through the bounce buffer: read the 32-byte aligned span around the piece
        aligned = from & ~(DMA_ALIGN - 1);
        n = BOUNCE_SIZE - (from - aligned);
        if (n > left) {
            n = left;
        }
        // An unaligned head: only up to the next aligned address, if direct reads can follow
        if ((address & (DMA_ALIGN - 1)) != 0) {
            u32 head = DMA_ALIGN - (address & (DMA_ALIGN - 1));

            if (head < n && ((from + head) & 3) == 0) {
                n = head;
            }
        }
        sCopyDst = (u8*)address;
        sCopySkip = from - aligned;
        sCopyLength = n;
        sCopySpan = (sCopySkip + n + DMA_ALIGN - 1) & ~(DMA_ALIGN - 1);
        sSectionDone += n;
        sLoadedBytes += n;
        return request(dst, size, offset, sBounce, sCopySpan, aligned);
    }
    return 0;
}

static void finish_bounce_copy(void) {
    if (sCopyLength == 0) {
        return;
    }
    dc_invalidate(sBounce, sCopySpan);
    memcpy(sCopyDst, sBounce + sCopySkip, sCopyLength);
    sCopyLength = 0;
}

static void apploader_init(ReportFunc reportFunc) {
    sReport = reportFunc;
    report("mmgcport apploader (CC0), loading the main DOL\n");
}

static int apploader_main(void** dst, u32* size, u32* offset) {
    u32 fstMax;

    finish_bounce_copy();

    switch (sState) {
        case STATE_START:
            sState = STATE_DISC_HEADER;
            return request(dst, size, offset, sDiscHeader, DISC_HEADER_SIZE, 0);

        case STATE_DISC_HEADER:
            dc_invalidate(sDiscHeader, DISC_HEADER_SIZE);
            if (be32(sDiscHeader + 0x1C) != DISC_MAGIC) {
                fail("no GameCube disc magic in the disc header\n");
            }
            sDolOffset = be32(sDiscHeader + 0x420);
            sFstOffset = be32(sDiscHeader + 0x424);
            sFstSize = be32(sDiscHeader + 0x428);
            fstMax = be32(sDiscHeader + 0x42C);
            if (sDolOffset == 0 || (sDolOffset & 3) != 0) {
                fail("the disc header has no (or a misaligned) main DOL offset\n");
            }
            if (sFstSize != 0 && (sFstOffset & 3) != 0) {
                fail("the disc header has a misaligned FST offset\n");
            }
            if (fstMax < sFstSize) {
                fstMax = sFstSize;
            }
            if (fstMax > 0x100000) {
                fail("the FST is larger than 1 MiB\n");
            }
            // Top of MEM1, as retail apploaders do: FST, then bi2.bin under it, then argv under that
            sFstAddress = MEM1_END - ((fstMax + DMA_ALIGN - 1) & ~(DMA_ALIGN - 1));
            sBi2Address = sFstAddress - BI2_SIZE;
            sArgAddress = sBi2Address - DMA_ALIGN;
            LOWMEM(0x38) = sFstAddress;
            LOWMEM(0x3C) = fstMax;
            sState = STATE_DOL_HEADER;
            return request(dst, size, offset, &sDol, sizeof(sDol), sDolOffset);

        case STATE_DOL_HEADER:
            dc_invalidate(&sDol, sizeof(sDol));
            check_dol();
            sSection = 0;
            sSectionDone = 0;
            sState = STATE_SECTIONS;
            // fall through
        case STATE_SECTIONS:
            if (section_request(dst, size, offset)) {
                return 1;
            }
            sState = STATE_BI2;
            return request(dst, size, offset, (void*)sBi2Address, BI2_SIZE, BI2_OFFSET);

        case STATE_BI2:
            LOWMEM(0xF4) = sBi2Address;
            sState = STATE_FST;
            if (sFstSize != 0) {
                return request(dst, size, offset, (void*)sFstAddress, (sFstSize + DMA_ALIGN - 1) & ~(DMA_ALIGN - 1),
                               sFstOffset);
            }
            // fall through
        case STATE_FST:
        default:
            sState = STATE_DONE;
            return 0;
    }
}

/* argv[0] = "dvd:/" in libogc's argv block, if the program has one */
static void pass_argv(void) {
    static const char argv0[] = ARGV0;
    volatile u32* entry = (volatile u32*)sDol.entry;
    ArgvBlock* block = (ArgvBlock*)(sDol.entry + 8);
    char* line = (char*)sArgAddress;

    if (entry[1] != ARGV_MAGIC) {
        return;
    }
    memcpy(line, argv0, sizeof(argv0));
    block->magic = ARGV_MAGIC;
    block->commandLine = line;
    block->length = sizeof(argv0);
    block->argc = 0;
    block->argv = 0;
    block->endArgv = 0;
    dc_flush(line, sizeof(argv0));
    dc_flush(block, sizeof(*block));
}

static void* apploader_close(void) {
    int i;

    finish_bounce_copy();
    // Retail apploaders set arena hi under what they placed at the top of MEM1
    LOWMEM(0x34) = sArgAddress;
    pass_argv();

    // Bounce copies sit in the data cache: write them back, then make the instruction cache see
    // the new code (DMA bypasses both caches).
    for (i = 0; i < DOL_SECTIONS; i++) {
        if (sDol.size[i] != 0) {
            dc_flush((void*)sDol.address[i], sDol.size[i]);
            if (section_is_text(i)) {
                ic_invalidate((void*)sDol.address[i], sDol.size[i]);
            }
        }
    }
    dc_flush((const void*)MEM1_START, 0x100);

    if (sReport != 0) {
        sReport("mmgcport apploader: %u bytes of DOL loaded, FST at %08x, entry %08x\n", sLoadedBytes, sFstAddress,
                sDol.entry);
    }
    return (void*)sDol.entry;
}

/* The entry point named in the apploader header; first in the image (see apploader.ld) */
void apploader_entry(InitFunc* init, MainFunc* main, CloseFunc* close)
    __attribute__((section(".text.entry"), used));
void apploader_entry(InitFunc* init, MainFunc* main, CloseFunc* close) {
    u8* p;

    // The boot program copies only the image, not .bss
    for (p = __bss_start; p < __bss_end; p++) {
        *p = 0;
    }
    dc_flush(__bss_start, (u32)(__bss_end - __bss_start));

    *init = apploader_init;
    *main = apploader_main;
    *close = apploader_close;
}
