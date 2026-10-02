/**
 * bridge_test: stand-alone test DOL for the libogc implementation of gc_bridge.h (port/gc/ogc),
 * without the game. Covers threads and priorities, the OS lock and condition variable,
 * semaphores with timeouts, interrupt masking, time and video timing, pads, gc_mem_alloc,
 * storage and ROM reads (CRC32s compared with values computed on the host from the US 1.0 ROM),
 * save load/store and logging from several threads, and finally gc_halt.
 * Every result line goes through gc_log: console, USB Gecko or OSReport, and log.txt on SD.
 * The summary line is "bridge_test: RESULT PASS" or "bridge_test: RESULT FAIL".
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/usbgecko.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "gc_ogc.h"

#define MAIN_PRIO 64
#define SAVE_SIZE 0x20000
#define ARRAY_COUNT(arr) (sizeof(arr) / sizeof((arr)[0]))

static int sPassed;
static int sFailed;

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

static unsigned int ms_since(u64 start) {
    return (unsigned int)ticks_to_millisecs(gettime() - start);
}

static void sleep_ms(unsigned int ms) {
    usleep(ms * 1000);
}

static void spin_ms(unsigned int ms) {
    u64 start = gettime();

    while (ms_since(start) < ms) {}
}

/* CRC-32 (IEEE, as zlib.crc32) */
static unsigned int crc32(const void* data, unsigned int size) {
    static unsigned int sTable[256];
    const unsigned char* p = data;
    unsigned int crc = 0xFFFFFFFF;
    unsigned int i;

    if (sTable[1] == 0) {
        for (i = 0; i < 256; i++) {
            unsigned int c = i;
            int k;

            for (k = 0; k < 8; k++) {
                c = (c & 1) ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            }
            sTable[i] = c;
        }
    }
    for (i = 0; i < size; i++) {
        crc = sTable[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

/* ---------------------------------------------------------------------------------------------- */
/* Threads                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

static gc_thread_t sHighHandle;
static volatile int sHighRan;
static volatile int sHighSawHandle;
static volatile int sLowRan;
static char sOrder[16];
static volatile int sOrderLen;
static volatile int sCounter;
static volatile int sCounterStop;
static volatile int sAfterExit;

static void high_entry(void* arg) {
    sHighSawHandle = (gc_thread_self() == sHighHandle);
    sHighRan = 1;
}

static void low_entry(void* arg) {
    sLowRan = 1;
}

static void yield_entry(void* arg) {
    int i;

    for (i = 0; i < 3; i++) {
        sOrder[sOrderLen++] = (char)(int)arg;
        gc_thread_yield();
    }
}

static void counter_entry(void* arg) {
    while (!sCounterStop) {
        sCounter++;
        usleep(1000);
    }
    gc_thread_exit();
    sAfterExit = 1;
}

static void test_threads(void) {
    gc_thread_t self = gc_thread_self();
    gc_thread_t t;
    int lowBefore;
    int c1;
    int c2;
    int c3;

    gc_log("threads:");
    gc_thread_set_prio(self, MAIN_PRIO);

    check(gc_thread_create(&sHighHandle, high_entry, NULL, 0x2000, 100) == 0 && sHighRan,
          "a higher-priority thread runs before gc_thread_create returns");
    check(sHighSawHandle, "the new thread already finds its handle in the caller's variable");

    gc_thread_create(&t, low_entry, NULL, 0x2000, 10);
    lowBefore = sLowRan;
    spin_ms(5);
    check(!lowBefore && !sLowRan, "a lower-priority thread does not run while main is busy");
    sleep_ms(5);
    check(sLowRan, "it runs once main sleeps");

    sLowRan = 0;
    gc_thread_create(&t, low_entry, NULL, 0x2000, 10);
    gc_thread_set_prio(t, 100);
    check(sLowRan, "raising a ready thread above main preempts main at once");

    // Two equal-priority threads created while main is above them, then released together.
    gc_thread_set_prio(self, 90);
    gc_thread_create(&t, yield_entry, (void*)'1', 0x2000, 80);
    gc_thread_create(&t, yield_entry, (void*)'2', 0x2000, 80);
    check(sOrderLen == 0, "threads below main wait");
    gc_thread_set_prio(self, MAIN_PRIO);
    gc_log("  (after lowering main: %d of 6 steps done)", sOrderLen);
    sleep_ms(5);
    sOrder[sOrderLen] = '\0';
    check(strcmp(sOrder, "121212") == 0, "gc_thread_yield alternates equal-priority threads (%s)", sOrder);

    gc_thread_create(&t, counter_entry, NULL, 0x2000, 70);
    sleep_ms(20);
    check(sCounter > 5, "counter thread runs (%d)", sCounter);
    gc_thread_suspend(t);
    gc_thread_suspend(t);
    c1 = sCounter;
    sleep_ms(20);
    c2 = sCounter;
    check(c1 == c2, "suspended thread does not run (%d -> %d)", c1, c2);
    gc_thread_resume(t);
    sleep_ms(20);
    c3 = sCounter;
    check(c3 > c2, "one resume undoes two suspends (%d -> %d)", c2, c3);
    sCounterStop = 1;
    sleep_ms(10);
    c1 = sCounter;
    sleep_ms(10);
    check(c1 == sCounter && !sAfterExit, "the thread ended with gc_thread_exit");
    gc_log("  %u bridge threads so far", gc_ogc_thread_count());
}

/* ---------------------------------------------------------------------------------------------- */
/* OS lock and condition variable                                                                 */
/* ---------------------------------------------------------------------------------------------- */

static volatile int sItems;
static volatile int sConsumed;
static volatile int sDone;
static volatile int sConsumersExited;

static void consumer_entry(void* arg) {
    for (;;) {
        gc_os_lock();
        while (sItems == 0 && !sDone) {
            gc_os_wait();
        }
        if (sItems > 0) {
            sItems--;
            sConsumed++;
            gc_os_unlock();
            continue;
        }
        sConsumersExited++;
        gc_os_unlock();
        return;
    }
}

static void test_os_lock(void) {
    static const int sPrios[] = { 70, 75, 50 };
    gc_thread_t t;
    unsigned int i;
    int n;

    gc_log("OS lock:");
    for (i = 0; i < ARRAY_COUNT(sPrios); i++) {
        gc_thread_create(&t, consumer_entry, NULL, 0x2000, sPrios[i]);
    }
    for (n = 0; n < 100; n++) {
        gc_os_lock();
        sItems++;
        gc_os_broadcast();
        gc_os_unlock();
        if (n % 10 == 0) {
            sleep_ms(1);
        }
    }
    sleep_ms(10);
    gc_os_lock();
    sDone = 1;
    gc_os_broadcast();
    gc_os_unlock();
    sleep_ms(10);
    check(sConsumed == 100 && sItems == 0, "3 consumers took 100 items exactly once (%d, %d left)", sConsumed,
          sItems);
    check(sConsumersExited == 3, "all consumers woke up for shutdown (%d)", sConsumersExited);
}

/* ---------------------------------------------------------------------------------------------- */
/* Semaphores                                                                                     */
/* ---------------------------------------------------------------------------------------------- */

static gc_sem_t sPostSem;

static void poster_entry(void* arg) {
    sleep_ms(20);
    gc_sem_post(sPostSem);
}

static void test_semaphores(void) {
    gc_sem_t s;
    gc_sem_t s2;
    gc_thread_t t;
    u64 t0;
    unsigned int ms;
    int r;

    gc_log("semaphores:");
    check(gc_sem_create(&s, 0) == 0 && gc_sem_create(&s2, 2) == 0, "create");

    t0 = gettime();
    r = gc_sem_wait_ticks(s, GC_TB_HZ / 20);
    ms = ms_since(t0);
    check(r == 1 && ms >= 50 && ms <= 70, "empty: 50 ms wait times out (r=%d after %u ms)", r, ms);

    gc_sem_post(s);
    t0 = gettime();
    r = gc_sem_wait_ticks(s, GC_TB_HZ);
    ms = ms_since(t0);
    check(r == 0 && ms <= 2, "posted: wait returns at once (r=%d after %u ms)", r, ms);

    gc_sem_wait(s2);
    gc_sem_wait(s2);
    r = gc_sem_wait_ticks(s2, GC_TB_HZ / 100);
    check(r == 1, "initial count 2: two waits succeed, the third times out");

    check(gc_sem_wait_ticks(s, 0) == 1, "zero timeout on an empty semaphore");

    sPostSem = s;
    gc_thread_create(&t, poster_entry, NULL, 0x2000, 70);
    t0 = gettime();
    r = gc_sem_wait_ticks(s, GC_TB_HZ);
    ms = ms_since(t0);
    check(r == 0 && ms >= 15 && ms <= 40, "post from another thread after 20 ms wakes the waiter (r=%d after %u ms)",
          r, ms);

    gc_thread_create(&t, poster_entry, NULL, 0x2000, 70);
    t0 = gettime();
    gc_sem_wait(s);
    ms = ms_since(t0);
    check(ms >= 15 && ms <= 40, "blocking wait woken by a post (%u ms)", ms);
}

/* ---------------------------------------------------------------------------------------------- */
/* Interrupts, time, video                                                                        */
/* ---------------------------------------------------------------------------------------------- */

static void test_irq_time_video(void) {
    unsigned int l1;
    unsigned int l2;
    u64 t0;
    u64 prev;
    u64 dt;
    int monotonic = 1;
    int i;
    int hz;
    unsigned int measured;

    gc_log("interrupts:");
    check((PPCMfmsr() & MSR_EE) != 0, "enabled at the start");
    l1 = gc_irq_disable();
    check((PPCMfmsr() & MSR_EE) == 0, "gc_irq_disable clears MSR[EE]");
    l2 = gc_irq_disable();
    t0 = gettime();
    while (gettime() - t0 < GC_TB_HZ / 10000) {}
    check(l2 == 0, "nested disable reports 'was disabled'; the timebase still runs");
    gc_irq_restore(l2);
    check((PPCMfmsr() & MSR_EE) == 0, "inner restore keeps interrupts off");
    gc_irq_restore(l1);
    check((PPCMfmsr() & MSR_EE) != 0, "outer restore turns them back on");

    gc_log("time:");
    prev = gc_time_ticks();
    for (i = 0; i < 10000; i++) {
        u64 now = gc_time_ticks();

        if (now < prev) {
            monotonic = 0;
        }
        prev = now;
    }
    check(monotonic, "gc_time_ticks is monotonic");
    check(TB_TIMER_CLOCK == 40500, "TB_TIMER_CLOCK is 40500 kHz");

    gc_log("video:");
    hz = gc_video_refresh_hz();
    gc_video_wait_vsync();
    t0 = gc_time_ticks();
    for (i = 0; i < 60; i++) {
        gc_video_wait_vsync();
    }
    dt = gc_time_ticks() - t0;
    measured = (unsigned int)(60ull * GC_TB_HZ * 100 / dt);
    check(hz == 50 || hz == 60, "refresh rate %d Hz", hz);
    check(measured > (unsigned int)hz * 100 - 150 && measured < (unsigned int)hz * 100 + 150,
          "60 retraces took %u ms: %u.%02u Hz", (unsigned int)ticks_to_millisecs(dt), measured / 100, measured % 100);
    gc_video_set_black(1);
    for (i = 0; i < 10; i++) {
        gc_video_wait_vsync();
    }
    gc_video_set_black(0);
    check(1, "gc_video_set_black on and off");
}

/* ---------------------------------------------------------------------------------------------- */
/* Pads, memory                                                                                   */
/* ---------------------------------------------------------------------------------------------- */

static void test_pads(void) {
    gc_pad_t pads[4];
    int i;
    int valid = 1;

    gc_log("pads:");
    for (i = 0; i < 3; i++) {
        gc_pad_read(pads);
        gc_video_wait_vsync();
    }
    for (i = 0; i < 4; i++) {
        if (pads[i].connected != 0 && pads[i].connected != 1) {
            valid = 0;
        }
    }
    check(valid, "connected: %d %d %d %d", pads[0].connected, pads[1].connected, pads[2].connected,
          pads[3].connected);
    gc_log("  pad 1: buttons %04X stick %d,%d C %d,%d L %u R %u", pads[0].buttons, pads[0].stickX, pads[0].stickY,
           pads[0].substickX, pads[0].substickY, pads[0].triggerL, pads[0].triggerR);
    if (pads[0].connected) {
        gc_pad_rumble(0, 1);
        sleep_ms(150);
        gc_pad_rumble(0, 0);
        check(1, "rumble on/off on port 1");
    }
}

static void test_memory(void) {
    unsigned int hi = (unsigned int)SYS_GetArena1Hi();
    unsigned char* a;
    unsigned char* b;
    void* m;

    gc_log("memory:");
    a = gc_mem_alloc(1000, 0);
    check(a != NULL && ((unsigned int)a & 31) == 0 && (unsigned int)a + 1024 <= hi,
          "1000 bytes: %08X, 32-byte aligned, below the old arena top", (unsigned int)a);
    b = gc_mem_alloc(5000, 4096);
    check(b != NULL && ((unsigned int)b & 4095) == 0 && b + 5000 <= a, "5000 bytes aligned 4096: %08X",
          (unsigned int)b);
    check((unsigned char*)SYS_GetArena1Hi() == b, "arena hi follows the allocations");
    check(gc_mem_alloc(64, 3) == NULL, "alignment 3 is refused");
    check(gc_mem_alloc(64 << 20, 32) == NULL, "64 MB is refused");
    m = malloc(4096);
    check(m != NULL && (unsigned char*)m + 4096 <= b, "malloc stays below the shim buffers (%08X)", (unsigned int)m);
    free(m);
}

/* ---------------------------------------------------------------------------------------------- */
/* Storage and ROM                                                                                */
/* ---------------------------------------------------------------------------------------------- */

/* Expected CRC32s, computed on the host with zlib.crc32 from the US 1.0 ROM (zero-filled past
 * the end, as gc_rom_read does). */
static const struct {
    unsigned int offset;
    unsigned int size;
    unsigned int crc;
    const char* name;
} sRomRanges[] = {
    { 0x0000000, 0x001000, 0x77CEA4D4, "header and boot code" },
    { 0x001A500, 0x006100, 0x0419F6C1, "dmadata" },
    { 0x0020000, 0x001000, 0xDFAA9D65, "crosses resident start" },
    { 0x0100000, 0x010000, 0xC958E2F6, "inside resident range" },
    { 0x05E0000, 0x001000, 0x49AE5954, "crosses resident end" },
    { 0x1000000, 0x100000, 0xD86548C9, "1 MB outside resident range" },
    { 0x0C0FFEE, 0x00014D, 0xBBA87880, "unaligned small read" },
    { 0x1FFFF00, 0x000100, 0x29058C73, "last 256 bytes" },
    { 0x1FFFF80, 0x000100, 0x9E83C213, "crosses end of ROM" },
    { 0x2000000, 0x000040, 0x758D6336, "past end of ROM" },
};

/* Ranges read piecewise by the concurrent readers */
static const struct {
    unsigned int offset;
    unsigned int size;
    unsigned int crc;
} sConcurrentRanges[] = {
    { 0x1800000, 0x40000, 0x620DA248 },
    { 0x0A00000, 0x40000, 0xB9FBC5DB },
    { 0x0100000, 0x10000, 0xC958E2F6 },
};

static unsigned char* sRomBuf;
static gc_sem_t sReaderDone;
static volatile int sReaderOk[3];

static void check_rom_ranges(const char* when) {
    unsigned int i;

    for (i = 0; i < ARRAY_COUNT(sRomRanges); i++) {
        // Odd destination offset for the unaligned case, to exercise the copy paths
        unsigned char* dst = sRomBuf + ((sRomRanges[i].offset & 1) ? 3 : 0);
        u64 t0 = gettime();
        int r = gc_rom_read(sRomRanges[i].offset, dst, sRomRanges[i].size);
        unsigned int crc = crc32(dst, sRomRanges[i].size);

        check(r == 0 && crc == sRomRanges[i].crc, "%s: %07X+%X crc %08X (want %08X, %u ms)", when,
              sRomRanges[i].offset, sRomRanges[i].size, crc, sRomRanges[i].crc, ms_since(t0));
    }
}

static void reader_entry(void* arg) {
    int idx = (int)arg;
    unsigned int offset = sConcurrentRanges[idx].offset;
    unsigned int size = sConcurrentRanges[idx].size;
    unsigned char* buf = malloc(size);
    int ok = (buf != NULL);
    int pass;

    for (pass = 0; pass < 2 && ok; pass++) {
        unsigned int done = 0;

        memset(buf, 0xAA, size);
        while (done < size && ok) {
            // Odd piece sizes so pieces straddle cache blocks
            unsigned int n = 4000 + idx * 1000;

            if (n > size - done) {
                n = size - done;
            }
            ok = gc_rom_read(offset + done, buf + done, n) == 0;
            done += n;
            gc_thread_yield();
        }
        ok = ok && crc32(buf, size) == sConcurrentRanges[idx].crc;
    }
    free(buf);
    sReaderOk[idx] = ok;
    gc_sem_post(sReaderDone);
}

static unsigned int be32_at(const unsigned char* p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | p[3];
}

/* Every file of the ROM's dmadata table read through gc_rom_read in 1 KB pieces (as Yaz0 does), twice
 * in different orders, and compared with fread from a second handle on the ROM file. That is more
 * data than the file cache holds, so files are fetched, evicted, refilled from ARAM and fetched
 * again. Files are read up to 1 MB. */
static void check_files(const char* path) {
    FILE* file = fopen(path, "rb");
    unsigned char* table = malloc(0x6200);
    unsigned char* expect = malloc(0x100000);
    unsigned char* got = sRomBuf + 3;
    unsigned int entries = 0;
    unsigned int files = 0;
    unsigned int bad = 0;
    unsigned long long bytes = 0;
    u64 t0 = gettime();
    int pass;

    if (file == NULL || table == NULL || expect == NULL || fseek(file, 0x1A500, SEEK_SET) != 0 ||
        fread(table, 1, 0x6200, file) != 0x6200) {
        check(0, "file cache: cannot read the dmadata table through a second handle on %s", path);
        goto done;
    }
    while (entries < 0x6200 / 16 && be32_at(table + entries * 16 + 4) != 0) {
        entries++;
    }
    for (pass = 0; pass < 2; pass++) {
        unsigned int k;

        for (k = 0; k < entries; k++) {
            // Pass 0 in ROM order, pass 1 in a scattered order (997 is prime, so this is a permutation)
            unsigned int i = (pass == 0) ? k : (k * 997u + 11u) % entries;
            const unsigned char* e = table + i * 16;
            unsigned int vromStart = be32_at(e);
            unsigned int vromEnd = be32_at(e + 4);
            unsigned int romStart = be32_at(e + 8);
            unsigned int romEnd = be32_at(e + 12);
            unsigned int size = (romEnd != 0) ? romEnd - romStart : vromEnd - vromStart;
            unsigned int done = 0;

            if (romStart == 0xFFFFFFFF || size == 0) {
                continue;
            }
            if (size > 0x100000) {
                size = 0x100000;
            }
            while (done < size) {
                unsigned int n = (size - done < 0x400) ? size - done : 0x400;

                if (gc_rom_read(romStart + done, got + done, n) != 0) {
                    break;
                }
                done += n;
            }
            if (done != size || fseek(file, romStart, SEEK_SET) != 0 || fread(expect, 1, size, file) != size ||
                memcmp(got, expect, size) != 0) {
                if (bad++ < 5) {
                    gc_log("  file cache: entry %u (%08X+%X) differs, pass %d", i, romStart, size, pass);
                }
            }
            files++;
            bytes += size;
        }
    }
    check(bad == 0, "file cache: %u files (%llu KB) read twice through gc_rom_read match fread (%u differ, %u ms)",
          files, bytes / 1024, bad, ms_since(t0));
    gc_ogc_rom_print_stats();
done:
    if (file != NULL) {
        fclose(file);
    }
    free(table);
    free(expect);
}

static void test_rom(void) {
    char path[64];
    const char* root;
    const char* error;
    gc_thread_t t;
    int i;

    gc_log("storage:");
    root = gc_ogc_storage_mount();
    check(root != NULL, "mounted %s", root ? root : "nothing");
    if (root == NULL) {
        return;
    }
    if (gc_ogc_sd_mounted()) {
        gc_ogc_log_open_file(GC_LOG_PATH);
    }

    gc_log("ROM:");
    snprintf(path, sizeof(path), "%s%s", root, GC_ROM_FILE);
    error = gc_ogc_rom_open(path);
    check(error == NULL, "open and validate %s%s%s", path, error ? ": " : "", error ? error : "");
    if (error != NULL) {
        return;
    }
    check(gc_rom_size() == GC_ROM_SIZE, "size %u", gc_rom_size());
    sRomBuf = malloc(0x100000 + 64);
    if (sRomBuf == NULL) {
        check(0, "no memory for the test buffer");
        return;
    }

    check_rom_ranges("file");
    check(gc_ogc_rom_preload(GC_ROM_RESIDENT_START, GC_ROM_RESIDENT_END) == 0, "preload the resident range");
    check_rom_ranges("resident");
    check(gc_ogc_rom_preload(GC_ROM_HOT_START, GC_ROM_HOT_END) == 0, "preload the hot range");
    check(gc_ogc_rom_preload(GC_ROM_HOT_START + 0x100, GC_ROM_HOT_START + 0x200) != 0,
          "an overlapping preload is refused");
    check_rom_ranges("hot range");
    check(gc_ogc_rom_cache_init() == 0, "create the ARAM file cache");
    check_rom_ranges("file cache");
    check_rom_ranges("file cache, again");
    check_files(path);

    snprintf(path, sizeof(path), "%s%s", root, GC_DIR "/no_such_rom.z64");
    error = gc_ogc_rom_open(path);
    check(error != NULL, "a missing ROM is reported: %s", error ? error : "(no error)");
    check(gc_rom_read(0, sRomBuf, 0x1000) == 0 && crc32(sRomBuf, 0x1000) == sRomRanges[0].crc,
          "the open ROM stays usable after a failed open");

    // Equal priorities and a yield after every piece, so the readers interleave on the cache.
    gc_sem_create(&sReaderDone, 0);
    for (i = 0; i < 3; i++) {
        gc_thread_create(&t, reader_entry, (void*)i, 0x4000, 70);
    }
    for (i = 0; i < 3; i++) {
        check(gc_sem_wait_ticks(sReaderDone, 10 * GC_TB_HZ) == 0, "concurrent reader finished");
    }
    check(sReaderOk[0] && sReaderOk[1] && sReaderOk[2], "3 concurrent readers got the right data (%d %d %d)",
          sReaderOk[0], sReaderOk[1], sReaderOk[2]);
    gc_ogc_rom_print_stats();
    free(sRomBuf);
}

/* ---------------------------------------------------------------------------------------------- */
/* Saves                                                                                          */
/* ---------------------------------------------------------------------------------------------- */

static long size_of(const char* path) {
    struct stat st;

    return (stat(path, &st) == 0) ? (long)st.st_size : -1;
}

static void fill(unsigned char* buf, unsigned int seed) {
    unsigned int i;

    for (i = 0; i < SAVE_SIZE; i++) {
        buf[i] = (unsigned char)(i * 7 + seed + (i >> 9));
    }
}

static void test_saves(void) {
    static const char sPath[] = GC_SD_DIR "/bridge_test.fla";
    unsigned char* buf = malloc(SAVE_SIZE);
    unsigned char* expect = malloc(SAVE_SIZE);
    FILE* f;
    int r;
    unsigned int i;
    int erased = 1;

    gc_log("saves:");
    if (buf == NULL || expect == NULL) {
        check(0, "no memory");
        return;
    }
    if (!gc_ogc_sd_mounted()) {
        check(gc_save_store(buf, SAVE_SIZE) != 0, "without an SD card a store fails cleanly");
        check(gc_save_load(buf, SAVE_SIZE) != 0, "and a load reports no save");
        free(buf);
        free(expect);
        return;
    }

    gc_ogc_save_set_path(sPath);
    remove(sPath);
    remove(GC_SD_DIR "/bridge_test.fla.bak");
    remove(GC_SD_DIR "/bridge_test.fla.tmp");

    r = gc_save_load(buf, SAVE_SIZE);
    check(r == 1, "load without a file returns 1 (%d)", r);

    fill(expect, 1);
    check(gc_save_store(expect, SAVE_SIZE) == 0, "store A");
    memset(buf, 0, SAVE_SIZE);
    check(gc_save_load(buf, SAVE_SIZE) == 0 && memcmp(buf, expect, SAVE_SIZE) == 0, "load returns A");

    fill(expect, 2);
    check(gc_save_store(expect, SAVE_SIZE) == 0, "store B");
    check(size_of(GC_SD_DIR "/bridge_test.fla.bak") == SAVE_SIZE, "A kept as .bak");
    check(size_of(GC_SD_DIR "/bridge_test.fla.tmp") < 0, "no .tmp left behind");
    memset(buf, 0, SAVE_SIZE);
    check(gc_save_load(buf, SAVE_SIZE) == 0 && memcmp(buf, expect, SAVE_SIZE) == 0, "load returns B");

    remove(sPath);
    fill(expect, 1);
    memset(buf, 0, SAVE_SIZE);
    check(gc_save_load(buf, SAVE_SIZE) == 0 && memcmp(buf, expect, SAVE_SIZE) == 0,
          "with the save gone, load recovers A from .bak");

    f = fopen(sPath, "wb");
    if (f != NULL) {
        fwrite(expect, 1, 100, f);
        fclose(f);
    }
    memset(buf, 0, SAVE_SIZE);
    r = gc_save_load(buf, SAVE_SIZE);
    for (i = 100; i < SAVE_SIZE; i++) {
        if (buf[i] != 0xFF) {
            erased = 0;
        }
    }
    check(r == 0 && memcmp(buf, expect, 100) == 0 && erased, "a short file loads, the rest reads as erased flash");

    remove(sPath);
    remove(GC_SD_DIR "/bridge_test.fla.bak");
    gc_ogc_save_set_path(GC_SAVE_PATH);
    free(buf);
    free(expect);
}

/* ---------------------------------------------------------------------------------------------- */
/* Logging                                                                                        */
/* ---------------------------------------------------------------------------------------------- */

static gc_sem_t sLoggerDone;

static void logger_entry(void* arg) {
    int id = (int)arg;
    int i;

    for (i = 0; i < 10; i++) {
        gc_log("  logger %d line %d", id, i);
        if (id == 2) {
            // Wakes up from a timer interrupt while a lower-priority logger holds the log lock
            usleep(700);
        }
    }
    gc_sem_post(sLoggerDone);
}

static void test_logging(void) {
    long before = gc_ogc_sd_mounted() ? size_of(GC_LOG_PATH) : -1;
    gc_thread_t t;
    int i;
    int ok = 1;

    gc_log("logging:");
    gc_sem_create(&sLoggerDone, 0);
    for (i = 0; i < 3; i++) {
        gc_thread_create(&t, logger_entry, (void*)i, 0x4000, (i == 2) ? 80 : 60);
    }
    for (i = 0; i < 3; i++) {
        ok = ok && gc_sem_wait_ticks(sLoggerDone, 10 * GC_TB_HZ) == 0;
    }
    check(ok, "3 threads logged 10 lines each");
    if (gc_ogc_sd_mounted()) {
        long after = size_of(GC_LOG_PATH);

        check(after > before && before > 0, "log.txt grows and is synced (%ld -> %ld bytes)", before, after);
    }
}

/* ---------------------------------------------------------------------------------------------- */

static volatile int sTicker;
static volatile int sLateThreadRan;

static void ticker_entry(void* arg) {
    for (;;) {
        sTicker++;
        usleep(1000);
    }
}

static void late_entry(void* arg) {
    sLateThreadRan = 1;
}

static void halt_report(const char* line) {
    fputs(line, stdout);
    fflush(stdout);
    if (gc_ogc_log_gecko()) {
        usb_sendbuffer_safe_ex(1, line, strlen(line), 10000);
    }
}

/* Checks after gc_halt that the ticker (a bridge thread) has stopped and that a bridge thread
 * created after the halt (by a thread gc_halt does not stop) never runs its entry. It is a raw LWP
 * thread, so gc_halt leaves it running, and it reports without gc_log, whose lock gc_halt keeps.
 * (That the halt unblanks the screen is checked on the screenshot.) */
static void* halt_watcher_entry(void* arg) {
    char line[160];
    gc_thread_t late;
    int created;
    int t1;
    int t2;

    while (!gc_ogc_halted()) {
        usleep(5000);
    }
    usleep(50000);
    t1 = sTicker;
    usleep(100000);
    t2 = sTicker;
    snprintf(line, sizeof(line), "bridge_test: after gc_halt the ticker is %s (%d -> %d)\n",
             (t1 == t2) ? "stopped: pass" : "still running: FAIL", t1, t2);
    halt_report(line);

    // Higher priority than this watcher: its entry would run before gc_thread_create returns.
    created = gc_thread_create(&late, late_entry, NULL, 0x2000, 110) == 0;
    usleep(20000);
    snprintf(line, sizeof(line), "bridge_test: a thread created after gc_halt %s (created %d)\n",
             (created && !sLateThreadRan) ? "did not run: pass" : "ran: FAIL", created);
    halt_report(line);
    return NULL;
}

static void reset_callback(void) {
    gc_log("bridge_test: reset callback called");
}

int main(void) {
    gc_thread_t t;
    lwp_t watcher;

    gc_ogc_video_init();
    gc_ogc_init();
    gc_set_reset_callback(reset_callback);
    gc_log("bridge_test: libogc side of gc_bridge.h, built " __DATE__ " " __TIME__);
    gc_log("USB Gecko in slot B: %s", gc_ogc_log_gecko() ? "yes" : "no");

    test_threads();
    test_os_lock();
    test_semaphores();
    test_irq_time_video();
    test_pads();
    test_rom();
    test_memory();
    test_saves();
    test_logging();
    gc_ogc_print_memory_map();

    gc_log("bridge_test: %d passed, %d failed", sPassed, sFailed);
    gc_log("bridge_test: RESULT %s", sFailed == 0 ? "PASS" : "FAIL");

    // Last, gc_halt: it must stop the ticker thread and show the console even though the game
    // blanked the screen (the screenshot of this run must show the HALT text).
    gc_thread_create(&t, ticker_entry, NULL, 0x2000, 50);
    LWP_CreateThread(&watcher, halt_watcher_entry, NULL, NULL, 0, 100);
    sleep_ms(10);
    gc_video_set_black(1);
    gc_halt("bridge_test finished (this halt is part of the test; ticker at %d)", sTicker);
}
