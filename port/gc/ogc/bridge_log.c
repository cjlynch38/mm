/**
 * gc_log and gc_halt.
 *
 * Every log line goes to:
 *  - the text console (stdout) while it is on screen: during boot, before the GX renderer shows its first frame,
 *    and after gc_halt. Once the renderer owns the display the console is not written: it draws into an uncached
 *    XFB and scrolls by moving all of it, which costs about 20 ms per line on the console (a frame is 50 ms);
 *  - a USB Gecko in memory card slot B, if one answers at boot. Dolphin emulates one as a TCP
 *    server, and port/gc/tools/run_dolphin.ps1 records that stream: in Dolphin it is the log that
 *    leaves the emulator. Sends give up after a bounded number of retries, so a Gecko that
 *    nobody reads cannot hang the game;
 *  - otherwise Dolphin's OSReport UART (SYS_Report: EXI channel 0 device 1), shown in Dolphin's
 *    log as OSREPORT lines; on real hardware the write is ignored;
 *  - a RAM ring buffer, which the log writer thread copies to sd:/mmgcport/log.txt (truncated at boot).
 * The writer runs below every game thread, so the SD card is written while the game waits for its next frame and
 * never stalls it: it writes at most every LOG_WRITE_MS (sooner when LOG_WRITE_BYTES are waiting) and syncs the
 * file after each write, so the file survives a hang or a power cycle up to the last few seconds. gc_halt and
 * gc_ogc_log_flush write out the rest at once. Lines logged before the SD card is mounted wait in the ring. If the
 * ring fills faster than the writer empties it, the file gets a note of how much was lost.
 * The Gecko, the ring and the file get a timestamp (seconds since boot) in front of each line.
 * One mutex serialises all of it, so lines from different threads never interleave.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/machine/processor.h>
#include <ogc/usbgecko.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "gc_ogc.h"

#define LOG_LINE_MAX 512
#define LOG_STAMP_MAX 24

/* Ring buffer of timestamped lines (a power of two) and how the writer drains it */
#define LOG_RING_SIZE 0x10000
#define LOG_WRITE_MS 2000
#define LOG_WRITE_BYTES 0x2000
#define LOG_POLL_MS 250
#define LOG_CHUNK 0x1000
#define LOG_WRITER_STACK 0x8000
/* Lines of the ring shown on the console when the game halts */
#define LOG_HALT_LINES 22

/* EXI channel 1 = memory card slot B */
#define GECKO_CHANNEL 1
#define GECKO_RETRIES 10000

static mutex_t sLogMutex = LWP_MUTEX_NULL;
static FILE* sLogFile;
static int sGecko;
static u64 sLogEpoch;
static char sLine[LOG_LINE_MAX];
static volatile int sHalted;

static char sRing[LOG_RING_SIZE];
static u32 sHead;                /* bytes appended since boot (the ring holds the last LOG_RING_SIZE of them) */
static u32 sFileTail;            /* bytes of those handed to the file */
static u32 sLost;                /* bytes overwritten before the file got them, not yet reported in the file */
static volatile int sWriterBusy; /* the writer took bytes out of the ring and is still writing them */
static lwp_t sWriter = LWP_THREAD_NULL;
static u8 sWriterStack[LOG_WRITER_STACK] __attribute__((aligned(32)));
static char sChunk[LOG_CHUNK];

void gc_ogc_log_init(void) {
    if (sLogEpoch == 0) {
        sLogEpoch = gettime();
        sGecko = usb_isgeckoalive(GECKO_CHANNEL);
    }
    if (sLogMutex == LWP_MUTEX_NULL && LWP_MutexInit(&sLogMutex, false) != 0) {
        sLogMutex = LWP_MUTEX_NULL;
        printf("gc_log: cannot create the log mutex\n");
    }
}

int gc_ogc_log_gecko(void) {
    return sGecko;
}

static void log_lock(void) {
    if (sLogMutex != LWP_MUTEX_NULL) {
        LWP_MutexLock(sLogMutex);
    }
}

static void log_unlock(void) {
    if (sLogMutex != LWP_MUTEX_NULL) {
        LWP_MutexUnlock(sLogMutex);
    }
}

/* Caller holds the log lock */
static void ring_put(const char* text, size_t len) {
    while (len > 0) {
        u32 at = sHead & (LOG_RING_SIZE - 1);
        size_t n = LOG_RING_SIZE - at;

        if (n > len) {
            n = len;
        }
        memcpy(sRing + at, text, n);
        sHead += n;
        text += n;
        len -= n;
    }
    if (sHead - sFileTail > LOG_RING_SIZE) {
        sLost += sHead - LOG_RING_SIZE - sFileTail;
        sFileTail = sHead - LOG_RING_SIZE;
    }
}

/* Copy up to `max` bytes the file has not had yet out of the ring. Caller holds the log lock. */
static u32 ring_take(char* dst, u32 max) {
    u32 n = sHead - sFileTail;
    u32 done = 0;

    if (n > max) {
        n = max;
    }
    while (done < n) {
        u32 at = (sFileTail + done) & (LOG_RING_SIZE - 1);
        u32 run = LOG_RING_SIZE - at;

        if (run > n - done) {
            run = n - done;
        }
        memcpy(dst + done, sRing + at, run);
        done += run;
    }
    sFileTail += n;
    return n;
}

static void file_write(const char* data, u32 len, u32 lost) {
    if (lost != 0) {
        fprintf(sLogFile, "(log: %u bytes of log lines were lost: the SD card was not written fast enough)\n",
                (unsigned int)lost);
    }
    fwrite(data, 1, len, sLogFile);
    fflush(sLogFile);
    fsync(fileno(sLogFile));
}

/* Write everything the ring holds for the file. Caller holds the log lock, and the writer is not writing. */
static void file_drain_locked(void) {
    u32 n;

    while ((n = ring_take(sChunk, sizeof(sChunk))) != 0) {
        u32 lost = sLost;

        sLost = 0;
        file_write(sChunk, n, lost);
    }
}

/* Wait until the writer has finished the write it is in. Caller holds the log lock, so it cannot start another. */
static void writer_wait_idle(void) {
    while (sWriterBusy) {
        usleep(1000);
    }
}

/* Lowest priority: runs only while every game thread waits, so SD card writes never delay a frame. Takes the
 * waiting lines out of the ring under the lock, then writes them without it. */
static void* log_writer(void* arg) {
    u64 last = gettime();

    for (;;) {
        u32 pending;
        u32 n;
        u32 lost;

        usleep(LOG_POLL_MS * 1000);
        if (sHalted) {
            // gc_halt holds the log lock from now on and writes the rest itself
            LWP_SuspendThread(LWP_GetSelf());
            continue;
        }
        log_lock();
        pending = sHead - sFileTail;
        if (pending == 0 || (pending < LOG_WRITE_BYTES && ticks_to_millisecs(gettime() - last) < LOG_WRITE_MS)) {
            log_unlock();
            continue;
        }
        sWriterBusy = 1;
        n = ring_take(sChunk, sizeof(sChunk));
        lost = sLost;
        sLost = 0;
        log_unlock();

        file_write(sChunk, n, lost);
        sWriterBusy = 0;
        if (n == pending) {
            last = gettime();
        }
    }
    return NULL;
}

/* Caller holds the log lock. `line` ends with a newline. */
static void log_emit(const char* line) {
    char stamp[LOG_STAMP_MAX];
    u64 ms;
    size_t stampLen;
    size_t len = strlen(line);

    if (gc_ogc_video_console_visible()) {
        fputs(line, stdout);
        fflush(stdout);
    }

    if (sLogEpoch == 0) {
        sLogEpoch = gettime();
    }
    ms = ticks_to_millisecs(gettime() - sLogEpoch);
    snprintf(stamp, sizeof(stamp), "[%5u.%03u] ", (unsigned int)(ms / 1000), (unsigned int)(ms % 1000));
    stampLen = strlen(stamp);

    if (sGecko) {
        usb_sendbuffer_safe_ex(GECKO_CHANNEL, stamp, stampLen, GECKO_RETRIES);
        usb_sendbuffer_safe_ex(GECKO_CHANNEL, line, len, GECKO_RETRIES);
    } else {
        SYS_Report("%s", line);
    }

    ring_put(stamp, stampLen);
    ring_put(line, len);
}

/* Format into buf (size LOG_LINE_MAX) and make sure the text ends with exactly one newline. */
static void log_format(char* buf, const char* prefix, const char* fmt, va_list args) {
    size_t prefixLen = strlen(prefix);
    size_t len;

    memcpy(buf, prefix, prefixLen);
    buf[prefixLen] = '\0';
    vsnprintf(buf + prefixLen, LOG_LINE_MAX - 1 - prefixLen, fmt, args);
    len = strlen(buf);
    if (len == 0 || buf[len - 1] != '\n') {
        buf[len] = '\n';
        buf[len + 1] = '\0';
    }
}

void gc_log(const char* fmt, ...) {
    va_list args;

    log_lock();
    va_start(args, fmt);
    log_format(sLine, "", fmt, args);
    va_end(args);
    log_emit(sLine);
    log_unlock();
}

void gc_ogc_log_open_file(const char* path) {
    FILE* file;

    mkdir(GC_SD_DIR, 0777); // usually exists already; the error is not interesting
    file = fopen(path, "w");
    if (file == NULL) {
        gc_log("log: cannot create %s; logging to the screen only", path);
        return;
    }

    log_lock();
    sLogFile = file;
    log_unlock();
    if (LWP_CreateThread(&sWriter, log_writer, NULL, sWriterStack, sizeof(sWriterStack), GC_PRIO_IDLE) != 0) {
        // Without the writer, write what the boot logged and give up on the file
        sWriter = LWP_THREAD_NULL;
        log_lock();
        file_drain_locked();
        sLogFile = NULL;
        log_unlock();
        fclose(file);
        gc_log("log: cannot start the log writer; logging to the screen only");
        return;
    }
    gc_log("log: writing to %s", path);
}

void gc_ogc_log_flush(void) {
    if (sLogFile == NULL || sHalted) {
        return;
    }
    log_lock();
    writer_wait_idle();
    file_drain_locked();
    log_unlock();
}

int gc_ogc_halted(void) {
    return sHalted;
}

/* Print the last LOG_HALT_LINES lines of the ring (the halt message among them) on the console. Caller holds the
 * log lock. */
static void log_show_tail(void) {
    u32 start = (sHead > LOG_RING_SIZE) ? sHead - LOG_RING_SIZE : 0;
    u32 at = sHead;
    int lines = 0;

    // Back to the start of the LOG_HALT_LINES-th line from the end (the ring ends with a newline)
    while (at > start) {
        if (at != sHead && sRing[(at - 1) & (LOG_RING_SIZE - 1)] == '\n' && ++lines == LOG_HALT_LINES) {
            break;
        }
        at--;
    }
    printf("\n");
    for (; at != sHead; at++) {
        putchar(sRing[at & (LOG_RING_SIZE - 1)]);
    }
    fflush(stdout);
}

static void park_forever(void) __attribute__((noreturn));
static void park_forever(void) {
    for (;;) {
        LWP_SuspendThread(LWP_GetSelf());
    }
}

void gc_halt(const char* fmt, ...) {
    char msg[LOG_LINE_MAX];
    va_list args;
    unsigned int level;
    int first;

    _CPU_ISR_Disable(level);
    first = !sHalted;
    sHalted = 1;
    _CPU_ISR_Restore(level);

    if (!first) {
        // Another thread is already halting the game; just stop here.
        park_forever();
    }

    va_start(args, fmt);
    log_format(msg, "HALT: ", fmt, args);
    va_end(args);

    // The log lock is never released: other threads that try to log from now on block, and
    // nothing can scroll the halt message off the screen.
    log_lock();
    log_emit(msg);
    log_emit("The game is stopped. Press RESET to restart.\n");
    gc_ogc_stop_threads();
    // After the stop, so the VI service thread cannot blank the screen again. The console was not written while the
    // renderer had the display: show the last lines on it.
    gc_ogc_video_show_console();
    log_show_tail();
    // The writer may be in the middle of a write: it finishes it (the game threads are stopped, so it gets the CPU)
    // and then stops, and the rest is written from here.
    if (sLogFile != NULL) {
        writer_wait_idle();
        file_drain_locked();
    }
    park_forever();
}
