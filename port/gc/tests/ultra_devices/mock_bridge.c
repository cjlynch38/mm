/**
 * Host (Linux) implementation of the gc_bridge.h functions used by the libultra device files,
 * built with the host's headers: pthreads for threads, POSIX semaphores, malloc for memory, and
 * scripted controllers, ROM and save data. Also checks the OS-lock rules of the contract.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mock.h"

#define MOCK_FLASH_SIZE 0x20000
#define MOCK_MAX_SEMS 32
#define MOCK_MAX_THREADS 16

static pthread_mutex_t sStateMutex = PTHREAD_MUTEX_INITIALIZER;
static int sFailures;
static int sLogs;

void mock_fail(const char* fmt, ...) {
    va_list args;

    va_start(args, fmt);
    fputs("FAIL: ", stdout);
    vprintf(fmt, args);
    fputs("\n", stdout);
    va_end(args);
    __atomic_add_fetch(&sFailures, 1, __ATOMIC_SEQ_CST);
}

int mock_failures(void) {
    return __atomic_load_n(&sFailures, __ATOMIC_SEQ_CST);
}

int mock_logs(void) {
    return __atomic_load_n(&sLogs, __ATOMIC_SEQ_CST);
}

void gc_log(const char* fmt, ...) {
    va_list args;

    va_start(args, fmt);
    fputs("  [gc_log] ", stdout);
    vprintf(fmt, args);
    fputs("\n", stdout);
    va_end(args);
    __atomic_add_fetch(&sLogs, 1, __ATOMIC_SEQ_CST);
}

static __thread jmp_buf* tHaltJump;

void gc_halt(const char* fmt, ...) {
    va_list args;

    va_start(args, fmt);
    fputs("  [gc_halt] ", stdout);
    vprintf(fmt, args);
    fputs("\n", stdout);
    va_end(args);
    if (tHaltJump != NULL) {
        longjmp(*tHaltJump, 1);
    }
    mock_fail("unexpected gc_halt");
    fflush(stdout);
    abort();
}

int mock_expect_halt(void (*fn)(void* arg), void* arg) {
    jmp_buf jump;
    int halted = 0;

    if (setjmp(jump) == 0) {
        tHaltJump = &jump;
        fn(arg);
    } else {
        halted = 1;
    }
    tHaltJump = NULL;
    return halted;
}

void mock_sleep_ms(unsigned int ms) {
    usleep(ms * 1000u);
}

unsigned int mock_now_ms(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ---- OS lock --------------------------------------------------------------------------------- */

static pthread_mutex_t sOsMutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t sOsOwner;
static int sOsOwned;

void gc_os_lock(void) {
    if (mock_os_lock_held()) {
        mock_fail("gc_os_lock: recursive lock (the OS lock is not recursive)");
        return;
    }
    pthread_mutex_lock(&sOsMutex);
    sOsOwner = pthread_self();
    __atomic_store_n(&sOsOwned, 1, __ATOMIC_SEQ_CST);
}

void gc_os_unlock(void) {
    if (!mock_os_lock_held()) {
        mock_fail("gc_os_unlock: lock not held by this thread");
        return;
    }
    __atomic_store_n(&sOsOwned, 0, __ATOMIC_SEQ_CST);
    pthread_mutex_unlock(&sOsMutex);
}

int mock_os_lock_held(void) {
    return __atomic_load_n(&sOsOwned, __ATOMIC_SEQ_CST) && pthread_equal(sOsOwner, pthread_self());
}

/* ---- Threads --------------------------------------------------------------------------------- */

typedef struct {
    void (*entry)(void*);
    void* arg;
} Trampoline;

static int sThreadCount;
static int sThreadPrio[MOCK_MAX_THREADS];
static unsigned int sThreadStack[MOCK_MAX_THREADS];

static void* ThreadTrampoline(void* p) {
    Trampoline t = *(Trampoline*)p;

    free(p);
    t.entry(t.arg);
    return NULL;
}

int gc_thread_create(gc_thread_t* out, void (*entry)(void* arg), void* arg, unsigned int stack_size, int prio) {
    pthread_t thread;
    Trampoline* t = malloc(sizeof(Trampoline));
    int index;

    pthread_mutex_lock(&sStateMutex);
    index = sThreadCount++;
    if (index < MOCK_MAX_THREADS) {
        sThreadPrio[index] = prio;
        sThreadStack[index] = stack_size;
    }
    pthread_mutex_unlock(&sStateMutex);

    t->entry = entry;
    t->arg = arg;
    if (pthread_create(&thread, NULL, ThreadTrampoline, t) != 0) {
        return -1;
    }
    pthread_detach(thread);
    *out = (gc_thread_t)index;
    return 0;
}

int mock_threads_created(void) {
    return sThreadCount;
}

int mock_thread_prio(int index) {
    return sThreadPrio[index];
}

unsigned int mock_thread_stack(int index) {
    return sThreadStack[index];
}

/* ---- Semaphores ------------------------------------------------------------------------------ */

static sem_t sSems[MOCK_MAX_SEMS];
static int sSemCount;

int gc_sem_create(gc_sem_t* out, unsigned int initial_count) {
    int index;

    pthread_mutex_lock(&sStateMutex);
    index = sSemCount++;
    pthread_mutex_unlock(&sStateMutex);
    if (index >= MOCK_MAX_SEMS) {
        return -1;
    }
    sem_init(&sSems[index], 0, initial_count);
    *out = (gc_sem_t)index;
    return 0;
}

void gc_sem_post(gc_sem_t sem) {
    sem_post(&sSems[sem]);
}

void gc_sem_wait(gc_sem_t sem) {
    while (sem_wait(&sSems[sem]) != 0 && errno == EINTR) {}
}

int gc_sem_wait_ticks(gc_sem_t sem, unsigned long long ticks) {
    struct timespec ts;
    unsigned long long ns = ticks * 1000000000ull / GC_TB_HZ;

    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ns / 1000000000ull;
    ts.tv_nsec += ns % 1000000000ull;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    while (sem_timedwait(&sSems[sem], &ts) != 0) {
        if (errno == ETIMEDOUT) {
            return 1;
        }
    }
    return 0;
}

/* ---- Memory ---------------------------------------------------------------------------------- */

void* gc_mem_alloc(unsigned int size, unsigned int align) {
    void* p = NULL;

    if (posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, size) != 0) {
        return NULL;
    }
    memset(p, 0xCD, size); /* never zero: catches code that assumes cleared memory */
    return p;
}

/* ---- Video ----------------------------------------------------------------------------------- */

static sem_t sVsyncGo;
static sem_t sVsyncDone;
static __thread int tVsyncWaited;
static int sBlackCalls;
static int sBlackLast = -1;

__attribute__((constructor)) static void MockInit(void) {
    sem_init(&sVsyncGo, 0, 0);
    sem_init(&sVsyncDone, 0, 0);
}

void gc_video_wait_vsync(void) {
    if (mock_os_lock_held()) {
        mock_fail("gc_video_wait_vsync called with the OS lock held");
    }
    if (tVsyncWaited) {
        sem_post(&sVsyncDone);
    }
    tVsyncWaited = 1;
    while (sem_wait(&sVsyncGo) != 0 && errno == EINTR) {}
}

void mock_vsync(void) {
    struct timespec ts;

    sem_post(&sVsyncGo);
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 2;
    while (sem_timedwait(&sVsyncDone, &ts) != 0) {
        if (errno == ETIMEDOUT) {
            mock_fail("VI service thread did not return to gc_video_wait_vsync (blocked?)");
            return;
        }
    }
}

void gc_video_set_black(int black) {
    if (mock_os_lock_held()) {
        mock_fail("gc_video_set_black called with the OS lock held");
    }
    sBlackCalls++;
    sBlackLast = black;
}

int mock_black_calls(void) {
    return sBlackCalls;
}

int mock_black_last(void) {
    return sBlackLast;
}

/* ---- Controllers ----------------------------------------------------------------------------- */

static gc_pad_t sPads[4];
static int sPadReads;
static int sRumble[4] = { -1, -1, -1, -1 };
static int sRumbleCalls;

void mock_set_pads(const gc_pad_t pads[4]) {
    memcpy(sPads, pads, sizeof(sPads));
}

void gc_pad_read(gc_pad_t pads[4]) {
    if (mock_os_lock_held()) {
        mock_fail("gc_pad_read called with the OS lock held");
    }
    memcpy(pads, sPads, sizeof(sPads));
    sPadReads++;
}

int mock_pad_reads(void) {
    return sPadReads;
}

void gc_pad_rumble(int chan, int on) {
    if ((chan < 0) || (chan > 3)) {
        mock_fail("gc_pad_rumble: bad channel %d", chan);
        return;
    }
    sRumble[chan] = on;
    sRumbleCalls++;
}

int mock_rumble(int chan) {
    return sRumble[chan];
}

int mock_rumble_calls(void) {
    return sRumbleCalls;
}

/* ---- ROM ------------------------------------------------------------------------------------- */

static int sRomWrite = 1;
static int sRomReads;
static unsigned int sRomLastOffset;
static unsigned int sRomLastSize;
static void* sRomLastDst;

unsigned char mock_rom_byte(unsigned int offset) {
    return (unsigned char)((offset * 7u) ^ (offset >> 8) ^ 0x5Au);
}

int gc_rom_read(unsigned int rom_offset, void* dst, unsigned int size) {
    unsigned int i;

    if (mock_os_lock_held()) {
        mock_fail("gc_rom_read called with the OS lock held");
    }
    sRomReads++;
    sRomLastOffset = rom_offset;
    sRomLastSize = size;
    sRomLastDst = dst;
    if (sRomWrite) {
        for (i = 0; i < size; i++) {
            ((unsigned char*)dst)[i] = mock_rom_byte(rom_offset + i);
        }
    }
    return 0;
}

unsigned int gc_rom_size(void) {
    return 0x2000000;
}

void mock_rom_set_write(int enable) {
    sRomWrite = enable;
}

int mock_rom_reads(void) {
    return sRomReads;
}

unsigned int mock_rom_last_offset(void) {
    return sRomLastOffset;
}

unsigned int mock_rom_last_size(void) {
    return sRomLastSize;
}

void* mock_rom_last_dst(void) {
    return sRomLastDst;
}

/* ---- Save data ------------------------------------------------------------------------------- */

static int sSaveLoads;
static int sSaveStores;
static int sSaveStoreAttempts;
static int sSaveFailNext;
static unsigned char sSaved[MOCK_FLASH_SIZE];

int mock_has_initial_save(void) {
    const char* env = getenv("MOCK_SAVE");

    return (env != NULL) && (env[0] == '1');
}

int mock_save_error(void) {
    const char* env = getenv("MOCK_SAVE");

    return (env != NULL) && (env[0] == 'e');
}

unsigned char mock_initial_save_byte(unsigned int offset) {
    return (unsigned char)(offset * 13u + 1u);
}

int gc_save_load(void* dst, unsigned int size) {
    unsigned int i;

    if (mock_os_lock_held()) {
        mock_fail("gc_save_load called with the OS lock held");
    }
    sSaveLoads++;
    if (mock_save_error()) {
        memset(dst, 0x00, size / 2); /* a failed read may leave junk behind */
        return -1;
    }
    if (!mock_has_initial_save()) {
        return 1;
    }
    for (i = 0; i < size; i++) {
        ((unsigned char*)dst)[i] = mock_initial_save_byte(i);
    }
    return 0;
}

int gc_save_store(const void* src, unsigned int size) {
    if (mock_os_lock_held()) {
        mock_fail("gc_save_store called with the OS lock held");
    }
    if (size != MOCK_FLASH_SIZE) {
        mock_fail("gc_save_store: size 0x%X", size);
        return -1;
    }
    __atomic_add_fetch(&sSaveStoreAttempts, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&sSaveFailNext, __ATOMIC_SEQ_CST) > 0) {
        __atomic_sub_fetch(&sSaveFailNext, 1, __ATOMIC_SEQ_CST);
        return -1;
    }
    memcpy(sSaved, src, size);
    __atomic_add_fetch(&sSaveStores, 1, __ATOMIC_SEQ_CST);
    return 0;
}

int mock_save_loads(void) {
    return sSaveLoads;
}

int mock_save_stores(void) {
    return __atomic_load_n(&sSaveStores, __ATOMIC_SEQ_CST);
}

int mock_save_store_attempts(void) {
    return __atomic_load_n(&sSaveStoreAttempts, __ATOMIC_SEQ_CST);
}

void mock_save_fail_next(int count) {
    __atomic_store_n(&sSaveFailNext, count, __ATOMIC_SEQ_CST);
}

const unsigned char* mock_saved_image(void) {
    return sSaved;
}
