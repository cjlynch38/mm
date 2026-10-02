/**
 * gc_log and gc_halt.
 *
 * Every log line goes to:
 *  - the text console on the XFB (stdout), which is the M2 display;
 *  - a USB Gecko in memory card slot B, if one answers at boot. Dolphin emulates one as a TCP
 *    server, and port/gc/tools/run_dolphin.ps1 records that stream: in Dolphin it is the log that
 *    leaves the emulator. Sends give up after a bounded number of retries, so a Gecko that
 *    nobody reads cannot hang the game;
 *  - otherwise Dolphin's OSReport UART (SYS_Report: EXI channel 0 device 1), shown in Dolphin's
 *    log as OSREPORT lines; on real hardware the write is ignored;
 *  - sd:/mmgcport/log.txt, truncated at boot and fsync'd after every line so the file survives a
 *    crash or hang. Lines logged before the SD card is mounted are kept in RAM and written out
 *    when the file is opened.
 * The Gecko and the file get a timestamp (seconds since boot) in front of each line.
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
#define LOG_EARLY_SIZE 0x4000

/* EXI channel 1 = memory card slot B */
#define GECKO_CHANNEL 1
#define GECKO_RETRIES 10000

static mutex_t sLogMutex = LWP_MUTEX_NULL;
static FILE* sLogFile;
static int sGecko;
static u64 sLogEpoch;
static char sLine[LOG_LINE_MAX];
static char sEarly[LOG_EARLY_SIZE];
static unsigned int sEarlyLen;
static int sEarlyDropped;
static volatile int sHalted;

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

static void log_sync_file(void) {
    fflush(sLogFile);
    fsync(fileno(sLogFile));
}

/* Caller holds the log lock. `line` ends with a newline. */
static void log_emit(const char* line) {
    char stamp[24];
    u64 ms;
    size_t stampLen;
    size_t len = strlen(line);

    fputs(line, stdout);
    fflush(stdout);

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

    if (sLogFile != NULL) {
        fputs(stamp, sLogFile);
        fputs(line, sLogFile);
        log_sync_file();
    } else if (sEarlyLen + stampLen + len < sizeof(sEarly)) {
        memcpy(sEarly + sEarlyLen, stamp, stampLen);
        memcpy(sEarly + sEarlyLen + stampLen, line, len);
        sEarlyLen += stampLen + len;
    } else {
        sEarlyDropped++;
    }
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
    fwrite(sEarly, 1, sEarlyLen, sLogFile);
    if (sEarlyDropped != 0) {
        fprintf(sLogFile, "(%d early log lines did not fit in the boot buffer)\n", sEarlyDropped);
    }
    sEarlyLen = 0;
    sEarlyDropped = 0;
    log_sync_file();
    log_unlock();

    gc_log("log: writing to %s", path);
}

int gc_ogc_halted(void) {
    return sHalted;
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
    // After the stop, so the VI service thread cannot blank the screen again.
    gc_ogc_video_show_console();
    park_forever();
}
