/**
 * Benchmark of the software audio microcode (port/gc/audio/aud_ucode.c) on tasks captured from the game
 * (AUD_CAPTURE builds, see port/gc/audio/aud_task.c, converted by audcap.py):
 *
 *   make -C port/gc/tests/audio_host bench CAPTURE=<capture.bin> [BENCH_ARGS=<repeats>]
 *
 * Every task runs `repeats` times (default 20), each time from its own captured DMEM and RAM. Prints the
 * average time per task, then per opcode (count and time per task, measured in a second pass with the
 * microcode's profiling clock), and a checksum of every task's DMEM and captured RAM afterwards, which
 * must not change when aud_ucode.c is optimized.
 *
 * The same file builds for the GameCube (GEKKO) with the capture linked in as gBenchCapture[] and the
 * results sent to a USB Gecko (Dolphin), to measure the microcode with the GameCube's code generation.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aud_ucode.h"

#ifdef GEKKO
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/usbgecko.h>
#include <stdarg.h>

extern const u8 gBenchCapture[];
extern const u8 gBenchCaptureEnd[];

#define CLOCK_HZ 40500000.0

static unsigned long long Bench_Clock(void) {
    return gettime();
}

static void Bench_Print(const char* fmt, ...) {
    char buf[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (usb_isgeckoalive(1)) {
        usb_sendbuffer_safe(1, buf, strlen(buf));
    }
    printf("%s", buf);
}
#else
#include <time.h>

#define CLOCK_HZ 1e9

static unsigned long long Bench_Clock(void) {
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

#define Bench_Print printf
#endif

#define RAM_SIZE 0x1000000

u8* gAudHostRam;

static u32 Be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

typedef struct {
    const u8* desc; // list address, size, dram_stack, DMEM, OSTask
    const u8* ram;  // pieces: address, size, bytes
    u32 nram;
} BenchTask;

#define MAX_TASKS 1024

static BenchTask sTasks[MAX_TASKS];
static u32 sNumTasks;
static const u8* sUdata;

static int Bench_Parse(const u8* file, u32 size) {
    const u8* p;
    u32 i;
    u32 k;

    if ((size < 12 + AUD_UCODE_DATA_SIZE) || (memcmp(file, "AUDCAP1", 8) != 0)) {
        return -1;
    }
    sNumTasks = Be32(file + 8);
    if (sNumTasks > MAX_TASKS) {
        sNumTasks = MAX_TASKS;
    }
    sUdata = file + 12;
    p = sUdata + AUD_UCODE_DATA_SIZE;
    for (k = 0; k < sNumTasks; k++) {
        sTasks[k].desc = p;
        p += 12 + AUD_DMEM_SIZE + AUD_TASK_HEADER_SIZE;
        sTasks[k].nram = Be32(p);
        p += 4;
        sTasks[k].ram = p;
        for (i = 0; i < sTasks[k].nram; i++) {
            p += 8 + Be32(p + 4);
        }
        if (p > file + size) {
            return -1;
        }
    }
    return 0;
}

/** Captured DMEM and RAM of task k */
static void Bench_Setup(const BenchTask* t) {
    const u8* p = t->ram;
    u32 i;
    u32 len;

    for (i = 0; i < t->nram; i++) {
        len = Be32(p + 4);
        memcpy(&gAudHostRam[Be32(p) & 0xFFFFFF], p + 8, len);
        p += 8 + len;
    }
    memcpy(AudUcode_GetDmem(), t->desc + 12, AUD_DMEM_SIZE);
}

static void Bench_Run(const BenchTask* t) {
    AudUcodeTask task;

    task.ucodeData = sUdata;
    task.dramStack = Be32(t->desc + 8);
    task.alistAddr = Be32(t->desc);
    task.alistSize = Be32(t->desc + 4);
    task.header = t->desc + 12 + AUD_DMEM_SIZE;
    AudUcode_RunTask(&task);
}

/** FNV-1a over DMEM and the task's captured pieces of RAM */
static u32 Bench_Hash(u32 h, const BenchTask* t) {
    const u8* dmem = AudUcode_GetDmem();
    const u8* p = t->ram;
    u32 i;
    u32 j;
    u32 len;
    const u8* r;

    for (i = 0; i < AUD_DMEM_SIZE; i++) {
        h = (h ^ dmem[i]) * 16777619u;
    }
    for (i = 0; i < t->nram; i++) {
        len = Be32(p + 4);
        r = &gAudHostRam[Be32(p) & 0xFFFFFF];
        for (j = 0; j < len; j++) {
            h = (h ^ r[j]) * 16777619u;
        }
        p += 8 + len;
    }
    return h;
}

/**
 * The microcode's saturation (inline assembly on the GameCube) against its definition, on every value
 * up to 2^18 in size and on both ends of the s32 range
 */
static int Bench_CheckSat16(void) {
    s32 x;
    s32 expected;
    u32 bad = 0;
    u32 i;

    for (i = 0; i < 0x80000 + 0x200; i++) {
        x = (i < 0x80000) ? (s32)i - 0x40000 : (s32)(0x7FFFFF00u + (i - 0x80000));
        expected = (x > 0x7FFF) ? 0x7FFF : (x < -0x8000) ? -0x8000 : x;
        if (AudUcode_TestSat16(x) != expected) {
            if (bad++ < 4) {
                Bench_Print("Sat16(%d) = %d, expected %d\n", (int)x, (int)AudUcode_TestSat16(x), (int)expected);
            }
        }
    }
    Bench_Print("Sat16 check: %s\n", (bad == 0) ? "ok" : "FAILED");
    return bad != 0;
}

static int Bench_Main(const u8* file, u32 size, int repeats) {
    unsigned long long total = 0;
    unsigned long long start;
    u32 hash = 2166136261u;
    u32 cmds = 0;
    u32 k;
    u32 op;
    int r;

    if (Bench_CheckSat16() != 0) {
        return 1;
    }
    if (Bench_Parse(file, size) != 0) {
        Bench_Print("not a capture file (audcap.py)\n");
        return 2;
    }
    gAudHostRam = calloc(1, RAM_SIZE + 0x10000);
    if (gAudHostRam == NULL) {
        Bench_Print("no memory for the RAM image\n");
        return 2;
    }

    // Pass 1: the outputs' checksum and the time per task, without the profiling clock
    AudUcode_Reset();
    for (k = 0; k < sNumTasks; k++) {
        cmds += Be32(sTasks[k].desc + 4) / 8;
        for (r = 0; r < repeats; r++) {
            Bench_Setup(&sTasks[k]);
            start = Bench_Clock();
            Bench_Run(&sTasks[k]);
            total += Bench_Clock() - start;
        }
        hash = Bench_Hash(hash, &sTasks[k]);
    }
    Bench_Print("%u tasks x %d, %u commands per task: %.1f us per task, checksum %08X, %u errors\n",
                (unsigned)sNumTasks, repeats, (unsigned)(cmds / sNumTasks),
                total * 1e6 / CLOCK_HZ / ((double)sNumTasks * repeats), (unsigned)hash,
                (unsigned)AudUcode_GetErrorCount());

    // Pass 2: per opcode
    AudUcode_ResetStats();
    AudUcode_SetProfileClock(Bench_Clock);
    for (k = 0; k < sNumTasks; k++) {
        for (r = 0; r < repeats; r++) {
            Bench_Setup(&sTasks[k]);
            Bench_Run(&sTasks[k]);
        }
    }
    AudUcode_SetProfileClock(NULL);
    for (op = 0; op < AUD_NUM_OPS; op++) {
        if (AudUcode_GetOpCount(op) != 0) {
            Bench_Print("  op %2u: %6.1f per task, %8.2f us per task\n", (unsigned)op,
                        AudUcode_GetOpCount(op) / ((double)sNumTasks * repeats),
                        AudUcode_GetOpTicks(op) * 1e6 / CLOCK_HZ / ((double)sNumTasks * repeats));
        }
    }
    return 0;
}

#ifdef GEKKO
int main(void) {
    int ret;

    VIDEO_Init();
    Bench_Print("aud_bench: GameCube\n");
    ret = Bench_Main(gBenchCapture, gBenchCaptureEnd - gBenchCapture, BENCH_REPEATS);
    Bench_Print("aud_bench: done (%d)\n", ret);
    for (;;) {
        VIDEO_WaitVSync();
    }
}
#else
int main(int argc, char** argv) {
    FILE* f;
    u8* file;
    long size;

    if (argc < 2) {
        fprintf(stderr, "usage: aud_bench <capture.bin> [repeats]\n");
        return 2;
    }
    f = fopen(argv[1], "rb");
    if ((f == NULL) || (fseek(f, 0, SEEK_END) != 0) || ((size = ftell(f)) <= 0) || (fseek(f, 0, SEEK_SET) != 0)) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    file = malloc(size);
    if ((file == NULL) || (fread(file, 1, size, f) != (size_t)size)) {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 2;
    }
    fclose(f);
    return Bench_Main(file, size, (argc > 2) ? atoi(argv[2]) : 20);
}
#endif
