/**
 * Host test of the software audio microcode (port/gc/audio/aud_ucode.c), built for x86 by the Makefile
 * next to this file.
 *
 *   1. Unit tests per command: expected values worked out here from the microcode's semantics (field
 *      layout, rounding, saturation, count rounding, state handling), independently of the
 *      implementation, including hand-computed vectors.
 *   2. Fast paths against the exact vector-by-vector versions (gAudForceExact) on random inputs.
 *   3. With the reference interpreter (CXD4=<mupen64plus-rsp-cxd4 source>): random instances of every
 *      command and random MM-like synthesis lists, run both here and as MM's real microcode
 *      (extracted/n64-us/incbin/aspMainText) on cxd4. All of DMEM and RAM must match bit for bit.
 *   4. aud_test --replay <capture.bin>...: tasks captured from the game (AUD_CAPTURE builds of
 *      port/gc/audio/aud_task.c, converted by audcap.py), fast paths against exact versions and, with
 *      the reference, against the real microcode.
 *
 * Usage: aud_test [seed] [iterations], or aud_test --replay <capture.bin>... Exit status 0 if every
 * test passed.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aud_ucode.h"
#ifdef AUD_LLE
#include "lle.h"
#endif

#define _SHIFTL(v, s, w) ((unsigned int)(((unsigned int)(v) & ((0x01 << (w)) - 1)) << (s)))
#include "PR/abi.h"

// ENVMIXER destinations as synthesis.c packs them (AUDIO_MK_CMD of the addresses >> 4)
#define AUDIO_SHIFT(dl, dr, wl, wr) \
    (_SHIFTL((dl) >> 4, 24, 8) | _SHIFTL((dr) >> 4, 16, 8) | _SHIFTL((wl) >> 4, 8, 8) | _SHIFTL((wr) >> 4, 0, 8))

#ifndef AUD_EXTRACTED_DIR
#define AUD_EXTRACTED_DIR "extracted/n64-us"
#endif

extern int gAudForceExact;

/* ------------------------------------------------------------------------------------------------ */
/* Fake RDRAM and test data                                                                         */
/* ------------------------------------------------------------------------------------------------ */

#define RAM_SIZE 0x1000000
#define RAM_TEXT 0x100000
#define RAM_DATA 0x101000
#define RAM_STACK 0x102000
#define RAM_ALIST 0x110000
#define RAM_POOL 0x200000 // random data: samples, codebooks, states
#define RAM_POOL_SIZE 0x100000

// MM's DMEM layout (src/audio/lib/synthesis.c)
#define DMEM_TEMP 0x3B0
#define DMEM_TEMP2 0x3C0
#define DMEM_SURROUND_TEMP 0x4B0
#define DMEM_UNCOMPRESSED_NOTE 0x570
#define DMEM_HAAS_TEMP 0x5B0
#define DMEM_COMB_TEMP 0x750
#define DMEM_COMPRESSED_ADPCM_DATA 0x930
#define DMEM_LEFT_CH 0x930
#define DMEM_RIGHT_CH 0xAD0
#define DMEM_WET_TEMP 0x3D0
#define DMEM_WET_SCRATCH 0x710
#define DMEM_WET_LEFT_CH 0xC70
#define DMEM_WET_RIGHT_CH 0xE10
#define DMEM_1CH_SIZE (13 * 16 * 2)
#define DMEM_2CH_SIZE (2 * DMEM_1CH_SIZE)

#define DMEM_RAND_LO 0x340

u8* gAudHostRam;

static u8 sAspText[0x1000];
static u8 sAspData[AUD_UCODE_DATA_SIZE];
static u8 sAspStack[AUD_DRAM_STACK_SIZE];
static int sHaveLle;
static int sFailures;
static int sChecks;
static const char* sTestName = "";

static u64 sRng = 0x9E3779B97F4A7C15ull;

static u32 Rand(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 7;
    sRng ^= sRng << 17;
    return (u32)(sRng >> 16);
}

static u32 RandRange(u32 lo, u32 hi) { // inclusive
    return lo + Rand() % (hi - lo + 1);
}

static s16 RandSample(void) {
    switch (Rand() % 4) {
        case 0:
            return (s16)Rand();
        case 1:
            return (s16)((s16)Rand() >> 4);
        case 2:
            return (Rand() & 1) ? 0x7FFF : -0x8000;
        default:
            return (s16)((s16)Rand() >> 8);
    }
}

static void Fail(const char* fmt, ...) {
    va_list args;

    sFailures++;
    fprintf(stderr, "FAIL [%s]: ", sTestName);
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}

#define CHECK_EQ(actual, expected, ...)                                         \
    do {                                                                        \
        long long a_ = (long long)(actual);                                     \
        long long e_ = (long long)(expected);                                   \
        sChecks++;                                                              \
        if (a_ != e_) {                                                         \
            Fail("%s:%d: %lld != %lld (expected)", __FILE__, __LINE__, a_, e_); \
            fprintf(stderr, "    ");                                            \
            fprintf(stderr, __VA_ARGS__);                                       \
            fputc('\n', stderr);                                                \
        }                                                                       \
    } while (0)

static int LoadFile(const char* name, void* buf, size_t size) {
    char path[512];
    FILE* f;
    size_t n;

    snprintf(path, sizeof(path), "%s/incbin/%s", AUD_EXTRACTED_DIR, name);
    f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s (run the N64 build's extraction first)\n", path);
        return -1;
    }
    n = fread(buf, 1, size, f);
    fclose(f);
    if (n != size) {
        fprintf(stderr, "%s: %zu bytes, expected %zu\n", path, n, size);
        return -1;
    }
    return 0;
}

/* Big-endian accessors for the RAM and DMEM images */

static void Wr16(u8* p, u32 v) {
    p[0] = v >> 8;
    p[1] = v;
}

static void Wr32(u8* p, u32 v) {
    Wr16(p, v >> 16);
    Wr16(p + 2, v);
}

static s32 Rd16(const u8* p) {
    return (s16)((p[0] << 8) | p[1]);
}

static s32 Sat(s64 x) {
    return (x > 32767) ? 32767 : (x < -32768) ? -32768 : (s32)x;
}

/* ------------------------------------------------------------------------------------------------ */
/* Command lists and task execution                                                                 */
/* ------------------------------------------------------------------------------------------------ */

#define MAX_CMDS 4096

static Acmd sCmds[MAX_CMDS];
static int sNumCmds;

static Acmd* NewCmd(void) {
    if (sNumCmds >= MAX_CMDS) {
        fprintf(stderr, "command list overflow\n");
        exit(2);
    }
    return &sCmds[sNumCmds++];
}

static void Cmd(u32 w0, u32 w1) {
    Acmd* a = NewCmd();

    a->words.w0 = w0;
    a->words.w1 = w1;
}

/* The encodings synthesis.c uses where it does not use the abi.h macros */
static void CmdDmemMove(u32 in, u32 out, u32 size) {
    Cmd(_SHIFTL(A_DMEMMOVE, 24, 8) | _SHIFTL(in, 0, 24), _SHIFTL(out, 16, 16) | _SHIFTL(size, 0, 16));
}

static void CmdInterl(u32 in, u32 out, u32 n) {
    Cmd(_SHIFTL(A_INTERL, 24, 8) | _SHIFTL(n, 0, 16), _SHIFTL(in, 16, 16) | _SHIFTL(out, 0, 16));
}

static void CmdEnvSetup2(u32 l, u32 r) {
    Cmd(_SHIFTL(A_ENVSETUP2, 24, 8), _SHIFTL(l, 16, 16) | _SHIFTL(r, 0, 16));
}

static void CmdHiLoGain(u32 gain, u32 in, u32 out, u32 size) {
    Cmd(_SHIFTL(A_HILOGAIN, 24, 8) | _SHIFTL(gain, 16, 8) | _SHIFTL(size, 0, 16),
        _SHIFTL(in, 16, 16) | _SHIFTL(out, 0, 16));
}

static void CmdUnk19(u32 dmem1, u32 dmem2, u32 size, u32 arg4) {
    Cmd(_SHIFTL(A_SPNOOP, 24, 8) | _SHIFTL(arg4, 16, 8) | _SHIFTL(size, 0, 16),
        _SHIFTL(dmem1, 16, 16) | _SHIFTL(dmem2, 0, 16));
}

#define A(macro, ...)           \
    do {                        \
        Acmd* a_ = NewCmd();    \
        macro(a_, __VA_ARGS__); \
    } while (0)

/** The OSTask at DMEM 0xFC0, as osSpTaskLoad leaves it for the list in sCmds */
static void WriteTaskHeader(u8* dmem) {
    u8* h = &dmem[AUD_TASK_HEADER_ADDR];

    memset(h, 0, AUD_TASK_HEADER_SIZE);
    Wr32(h + 0x00, 2); // M_AUDTASK
    Wr32(h + 0x08, RAM_TEXT);
    Wr32(h + 0x0C, 0x1000);
    Wr32(h + 0x10, RAM_TEXT);
    Wr32(h + 0x14, 0x1000);
    Wr32(h + 0x18, RAM_DATA);
    Wr32(h + 0x1C, AUD_UCODE_DATA_SIZE - 1);
    Wr32(h + 0x20, RAM_STACK);
    Wr32(h + 0x30, RAM_ALIST);
    Wr32(h + 0x34, sNumCmds * 8);
}

static void WriteList(u8* ram) {
    int i;

    for (i = 0; i < sNumCmds; i++) {
        Wr32(&ram[RAM_ALIST + 8 * i], sCmds[i].words.w0);
        Wr32(&ram[RAM_ALIST + 8 * i + 4], sCmds[i].words.w1);
    }
    memcpy(&ram[RAM_TEXT], sAspText, sizeof(sAspText));
    memcpy(&ram[RAM_DATA], sAspData, sizeof(sAspData));
    memcpy(&ram[RAM_STACK], sAspStack, sizeof(sAspStack));
}

/** Run sCmds on this implementation: ram/dmem are the images before, updated in place */
static void RunMine(u8* ram, u8* dmem) {
    AudUcodeTask task;

    WriteList(ram);
    WriteTaskHeader(dmem);
    memcpy(AudUcode_GetDmem(), dmem, AUD_DMEM_SIZE);
    gAudHostRam = ram;
    task.ucodeData = &ram[RAM_DATA];
    task.dramStack = RAM_STACK;
    task.alistAddr = RAM_ALIST;
    task.alistSize = sNumCmds * 8;
    task.header = &dmem[AUD_TASK_HEADER_ADDR];
    AudUcode_RunTask(&task);
    memcpy(dmem, AudUcode_GetDmem(), AUD_DMEM_SIZE);
}

static u8* sRamA;
static u8* sRamB;
static u8* sRamInit;
static u8 sDmemA[AUD_DMEM_SIZE];
static u8 sDmemB[AUD_DMEM_SIZE];
static u8 sDmemInit[AUD_DMEM_SIZE];

/** Random DMEM and RAM pool contents for the next task */
static void RandomizeMemory(void) {
    u32 i;

    for (i = 0; i < AUD_DMEM_SIZE; i += 2) {
        Wr16(&sDmemInit[i], RandSample());
    }
    for (i = RAM_POOL; i < RAM_POOL + RAM_POOL_SIZE; i += 2) {
        Wr16(&sRamInit[i], RandSample());
    }
}

static int Compare(const char* what, const u8* a, const u8* b, u32 size, u32 base) {
    u32 i;
    u32 n = 0;

    if (memcmp(a, b, size) == 0) {
        return 0;
    }
    for (i = 0; i < size; i++) {
        if (a[i] != b[i]) {
            if (n < 8) {
                fprintf(stderr, "    %s 0x%06X: %02X, expected %02X\n", what, base + i, a[i], b[i]);
            }
            n++;
        }
    }
    fprintf(stderr, "    %s: %u bytes differ\n", what, n);
    return 1;
}

/**
 * Debugging aid (AUD_TEST_VERBOSE=3 with the reference): run sCmds one command per task and report the
 * commands that write into DMEM below 0x340, where the real microcode keeps its command buffer
 */
static __attribute__((unused)) void FindLowDmemWrites(void) {
    static u8 before[AUD_DMEM_SIZE];
    AudUcodeTask task;
    Acmd saved = sCmds[0];
    int n = sNumCmds;
    int k;
    u32 i;

    memcpy(sRamA, sRamInit, RAM_SIZE);
    AudUcode_Reset();
    memcpy(AudUcode_GetDmem(), sDmemInit, AUD_DMEM_SIZE);
    gAudHostRam = sRamA;
    for (k = 0; k < n; k++) {
        Wr32(&sRamA[RAM_ALIST], sCmds[k].words.w0);
        Wr32(&sRamA[RAM_ALIST + 4], sCmds[k].words.w1);
        memcpy(&sRamA[RAM_DATA], sAspData, sizeof(sAspData));
        memcpy(before, AudUcode_GetDmem(), AUD_DMEM_SIZE);
        task.ucodeData = sAspData;
        task.dramStack = RAM_STACK;
        task.alistAddr = RAM_ALIST;
        task.alistSize = 8;
        task.header = NULL;
        AudUcode_RunTask(&task);
        for (i = 0x2F8; i < 0x340; i++) {
            if (AudUcode_GetDmem()[i] != before[i]) {
                fprintf(stderr, "    command %d (%08X %08X) writes DMEM 0x%X\n", k, sCmds[k].words.w0,
                        sCmds[k].words.w1, i);
                break;
            }
        }
    }
    sCmds[0] = saved;
}

static void PrintList(void) {
    int i;

    for (i = 0; i < sNumCmds && i < ((getenv("AUD_TEST_VERBOSE") != NULL) ? 4096 : 64); i++) {
        fprintf(stderr, "    cmd %2d: %08X %08X\n", i, sCmds[i].words.w0, sCmds[i].words.w1);
    }
}

/**
 * Run sCmds from the same initial state twice: fast paths allowed (A) and forced off (B). Then, with the
 * reference interpreter, run it as the real microcode (B) and compare with A. Returns 0 on a match.
 */
static int RunAndCompare(const char* name) {
    int bad = 0;

    sChecks++;
    memcpy(sRamA, sRamInit, RAM_SIZE);
    memcpy(sDmemA, sDmemInit, AUD_DMEM_SIZE);
    AudUcode_Reset();
    gAudForceExact = 0;
    RunMine(sRamA, sDmemA);
    if (AudUcode_GetErrorCount() != 0) {
        fprintf(stderr, "    %u unknown commands or zero-size DMAs\n", (unsigned)AudUcode_GetErrorCount());
    }

    memcpy(sRamB, sRamInit, RAM_SIZE);
    memcpy(sDmemB, sDmemInit, AUD_DMEM_SIZE);
    AudUcode_Reset();
    gAudForceExact = 1;
    RunMine(sRamB, sDmemB);
    gAudForceExact = 0;

    if (Compare("DMEM fast/exact", sDmemA, sDmemB, AUD_DMEM_SIZE, 0) ||
        Compare("RAM fast/exact", sRamA, sRamB, RAM_SIZE, 0)) {
        bad = 1;
    }

#ifdef AUD_LLE
    if (sHaveLle) {
        memcpy(sRamB, sRamInit, RAM_SIZE);
        memcpy(sDmemB, sDmemInit, AUD_DMEM_SIZE);
        WriteList(sRamB);
        WriteTaskHeader(sDmemB);
        Lle_ResetRegs();
        if (getenv("AUD_TEST_VERBOSE") != NULL) {
            fprintf(stderr, "%s\n", name);
            if (atoi(getenv("AUD_TEST_VERBOSE")) > 1) {
                PrintList();
            }
            if (atoi(getenv("AUD_TEST_VERBOSE")) > 2) {
                FindLowDmemWrites();
            }
        }
        if (Lle_Run(sRamB, sDmemB, sAspText) != 0) {
            fprintf(stderr, "    the microcode did not end with BREAK\n");
            bad = 1;
        }
        if (Compare("DMEM vs microcode", sDmemA, sDmemB, AUD_DMEM_SIZE, 0) ||
            Compare("RAM vs microcode", sRamA, sRamB, RAM_SIZE, 0)) {
            bad = 1;
        }
    }
#endif

    if (bad) {
        Fail("%s", name);
        PrintList();
#ifdef AUD_LLE
        if (sHaveLle && (getenv("AUD_TEST_BISECT") != NULL)) {
            // The shortest prefix of the list that already differs
            int n = sNumCmds;
            int k;

            unsetenv("AUD_TEST_BISECT");
            for (k = 1; k <= n; k++) {
                sNumCmds = k;
                if (RunAndCompare("prefix")) {
                    fprintf(stderr, "    first difference after command %d\n", k - 1);
                    break;
                }
            }
            sNumCmds = n;
            setenv("AUD_TEST_BISECT", "1", 1);
        }
#endif
    }
    return bad;
}

/** Run sCmds on this implementation only, from sDmemInit/sRamInit, into sDmemA/sRamA */
static void RunUnit(void) {
    memcpy(sRamA, sRamInit, RAM_SIZE);
    memcpy(sDmemA, sDmemInit, AUD_DMEM_SIZE);
    AudUcode_Reset();
    RunMine(sRamA, sDmemA);
}

static void Begin(const char* name) {
    sTestName = name;
    sNumCmds = 0;
    memset(sDmemInit, 0, sizeof(sDmemInit));
    memset(&sRamInit[RAM_POOL], 0, RAM_POOL_SIZE);
}

/* ------------------------------------------------------------------------------------------------ */
/* Unit tests: expected values from the microcode's semantics, worked out independently              */
/* ------------------------------------------------------------------------------------------------ */

static void Test_ClearBuff(void) {
    int i;

    Begin("CLEARBUFF");
    memset(sDmemInit, 0x55, sizeof(sDmemInit));
    A(aClearBuffer, 0x401, 1);  // rounds up to 16 bytes, any address
    A(aClearBuffer, 0x501, 0);  // nothing
    A(aClearBuffer, 0x600, 17); // 32 bytes
    RunUnit();
    for (i = 0; i < 18; i++) {
        CHECK_EQ(sDmemA[0x400 + i], (i >= 1 && i < 17) ? 0 : 0x55, "0x%X", 0x400 + i);
        CHECK_EQ(sDmemA[0x500 + i], 0x55, "0x%X", 0x500 + i);
    }
    for (i = 0; i < 34; i++) {
        CHECK_EQ(sDmemA[0x600 + i], (i < 32) ? 0 : 0x55, "0x%X", 0x600 + i);
    }
}

static void Test_DmemMove(void) {
    int i;

    Begin("DMEMMOVE");
    for (i = 0; i < 0x100; i++) {
        sDmemInit[0x400 + i] = i;
    }
    CmdDmemMove(0x400, 0x800, 5);  // odd: rounds up to 6
    CmdDmemMove(0x420, 0x880, 0);  // 0: below 16 the halfword loop runs once before any check
    CmdDmemMove(0x400, 0x402, 20); // overlapping forward copy: 16-byte block, then halfwords
    RunUnit();
    CHECK_EQ(sDmemA[0x880], 0x20, "count 0 moves a halfword");
    CHECK_EQ(sDmemA[0x881], 0x21, "count 0 moves a halfword");
    CHECK_EQ(sDmemA[0x882], 0, "count 0 moves only a halfword");
    for (i = 0; i < 8; i++) {
        CHECK_EQ(sDmemA[0x800 + i], (i < 6) ? i : 0, "dst 0x%X", 0x800 + i);
    }
    // Block 0x400-0x40F is read whole and written to 0x402-0x411; then halfwords 0x410.. -> 0x412..,
    // reading what the block wrote (0x410 now holds 0x0E)
    for (i = 0; i < 16; i++) {
        CHECK_EQ(sDmemA[0x402 + i], i, "block 0x%X", 0x402 + i);
    }
    CHECK_EQ(sDmemA[0x412], 0x0E, "tail 0x412");
    CHECK_EQ(sDmemA[0x413], 0x0F, "tail 0x413");
    CHECK_EQ(sDmemA[0x414], 0x0E, "tail 0x414"); // 0x412 copied after it was written
    CHECK_EQ(sDmemA[0x415], 0x0F, "tail 0x415");
    CHECK_EQ(sDmemA[0x416], 0x16, "past the end 0x416");
}

static void Test_Mixer(void) {
    static const s16 outs[] = { 100, -100, 32767, -32768, 1000, -1, 0, 12345 };
    static const s16 ins[] = { 1000, 1000, 32767, -32768, -2000, -1, 32767, -12345 };
    static const s16 gains[] = { 0x4000, 0x4000, 0x7FFF, 0x7FFF, -0x8000, 0x7FFF, 1, 0x2000 };
    s32 expected;
    int i;
    int n = 0;

    Begin("MIXER");
    for (i = 0; i < 8; i++) {
        Wr16(&sDmemInit[0x400 + 64 * i + 0], outs[i]);
        Wr16(&sDmemInit[0x800 + 64 * i + 0], ins[i]);
        A(aMix, 1, (u16)gains[i], 0x800 + 64 * i, 0x400 + 64 * i); // count 16 bytes: rounds up to 32
    }
    RunUnit();
    for (i = 0; i < 8; i++) {
        // vmulf out * 0x7FFF, then vmacf in * gain: (2 * o * 0x7FFF + 0x8000 + 2 * i * g) >> 16, saturated
        expected = Sat(((s64)2 * outs[i] * 0x7FFF + 0x8000 + (s64)2 * ins[i] * gains[i]) >> 16);
        CHECK_EQ(Rd16(&sDmemA[0x400 + 64 * i]), expected, "case %d", i);
        n++;
    }
    // Hand-worked: 100 * 0x7FFF + 1000 * 0x4000 + 0x4000 = 19677084, >> 15 = 600
    CHECK_EQ(Rd16(&sDmemA[0x400]), 600, "hand-worked");
    // -100 * 0x7FFF + 1000 * 0x4000 + 0x4000 = 13116484, >> 15 = 400
    CHECK_EQ(Rd16(&sDmemA[0x440]), 400, "hand-worked negative");
    // The whole 32 bytes are processed: sample 15 of each pair of vectors is 0 + 0
    CHECK_EQ(Rd16(&sDmemA[0x400 + 30]), 0, "rounded count");
    (void)n;
}

static void Test_AddMixer(void) {
    int i;

    Begin("ADDMIXER");
    for (i = 0; i < 32; i++) {
        Wr16(&sDmemInit[0x400 + 2 * i], 1000 * i - 16000);
        Wr16(&sDmemInit[0x800 + 2 * i], (i == 5) ? 32767 : 7);
    }
    // MIXER first (count 32, gain 0, on a scratch buffer) leaves v31 = the constants at DMEM 0, whose
    // lane 3 is 0xFFFF: ADDMIXER's vaddc v31, v31, v31 turns that into a carry into sample 3
    A(aMix, 2, 0, 0xC00, 0xC00);
    A(aAddMixer, 16, 0x800, 0x400, 0x7FFF); // 16 bytes: rounds up to 64
    RunUnit();
    for (i = 0; i < 32; i++) {
        CHECK_EQ(Rd16(&sDmemA[0x400 + 2 * i]), Sat(1000 * i - 16000 + ((i == 5) ? 32767 : 7) + (i == 3)), "sample %d",
                 i);
    }
}

static void Test_HiLoGain(void) {
    static const s16 x[] = { 1000, -1000, 32767, -32768, 7, -7, 3, -3 };
    static const u8 gains[] = { 0x10, 0x18, 0x20, 0xFF, 0x11, 0x11, 0x08, 0x08 };
    int i;

    Begin("HILOGAIN");
    for (i = 0; i < 8; i++) {
        Wr16(&sDmemInit[0x400 + 32 * i], x[i]);
        CmdHiLoGain(gains[i], 0x400 + 32 * i, 0, 2);
    }
    RunUnit();
    for (i = 0; i < 8; i++) {
        // UQ4.4 (unsigned: 0xFF is 15.9375): floor(x * gain / 16), saturated
        CHECK_EQ(Rd16(&sDmemA[0x400 + 32 * i]), Sat(((s64)x[i] * gains[i]) >> 4), "case %d", i);
    }
    CHECK_EQ(Rd16(&sDmemA[0x400 + 32 * 1]), -1500, "hand-worked 1.5x");
    CHECK_EQ(Rd16(&sDmemA[0x400 + 32 * 5]), -8, "hand-worked floor(-7 * 17 / 16)");
}

static void Test_Interleave(void) {
    int i;

    Begin("INTERLEAVE/INTERL");
    for (i = 0; i < 16; i++) {
        Wr16(&sDmemInit[0x400 + 2 * i], 0x1000 + i);
        Wr16(&sDmemInit[0x500 + 2 * i], 0x2000 + i);
        Wr16(&sDmemInit[0x600 + 2 * i], 0x3000 + i);
    }
    A(aInterleave, 0x800, 0x400, 0x500, 16); // 8 samples per side
    CmdInterl(0x600, 0x900, 3);              // rounds up to 8 output samples
    RunUnit();
    for (i = 0; i < 8; i++) {
        CHECK_EQ(Rd16(&sDmemA[0x800 + 4 * i]), 0x1000 + i, "L %d", i);
        CHECK_EQ(Rd16(&sDmemA[0x802 + 4 * i]), 0x2000 + i, "R %d", i);
        CHECK_EQ(Rd16(&sDmemA[0x900 + 2 * i]), 0x3000 + 2 * i, "interl %d", i);
    }
    CHECK_EQ(Rd16(&sDmemA[0x820]), 0, "interleave stops after 8 per side");
}

static void Test_Duplicate(void) {
    int i;

    Begin("DUPLICATE");
    for (i = 0; i < 0x80; i++) {
        sDmemInit[0x400 + i] = i;
    }
    A(aDuplicate, 2, 0x400, 0x480);
    A(aDuplicate, 0, 0x400, 0x600); // 0 still stores once
    RunUnit();
    for (i = 0; i < 0x100; i++) {
        CHECK_EQ(sDmemA[0x480 + i], i & 0x7F, "copy 0x%X", 0x480 + i);
    }
    CHECK_EQ(sDmemA[0x600 + 0x7F], 0x7F, "count 0");
    CHECK_EQ(sDmemA[0x580], 0, "past the copies");
}

static void Test_EnvMixer(void) {
    static const s16 x[4] = { 1000, -1000, 32767, -32768 };
    s32 volL;
    s32 volR;
    s32 rev;
    s32 dry;
    s32 wet;
    int i;
    int k;

    Begin("ENVMIXER");
    for (i = 0; i < 32; i++) {
        Wr16(&sDmemInit[0x400 + 2 * i], x[i & 3]);
    }
    // Reverb 0x40 (<< 8 = 0x4000), its ramp +0x100 per 8 samples; left 0x8000 ramp +0x10, right 0x2000 ramp -0x20
    A(aEnvSetup1, 0x40, 0x100, 0x10, (u16)-0x20);
    CmdEnvSetup2(0x8000, 0x2000);
    // 20 samples: rounds up to 32. Bit 1 (XOR the left dry with -1) set: x1 = 0, x2 = 1
    A(aEnvMixer, 0x400, 20, 0, 0, 0, 1, 0, AUDIO_SHIFT(0x800, 0x900, 0xA00, 0xB00), _SHIFTL(A_ENVMIXER, 24, 8));
    RunUnit();
    for (k = 0; k < 32; k++) {
        // Volumes for samples 8n..8n+7 are the start plus n ramp steps (16-bit)
        volL = (u16)(0x8000 + 0x10 * (k / 8));
        volR = (u16)(0x2000 - 0x20 * (k / 8));
        rev = (u16)(0x4000 + 0x100 * (k / 8));
        dry = (s16)((((s32)x[k & 3] * volL) >> 16) ^ -1);
        wet = (dry * rev) >> 16;
        CHECK_EQ(Rd16(&sDmemA[0x800 + 2 * k]), dry, "dry L %d", k);
        CHECK_EQ(Rd16(&sDmemA[0xA00 + 2 * k]), wet, "wet L %d", k);
        dry = ((s32)x[k & 3] * volR) >> 16;
        wet = (dry * rev) >> 16;
        CHECK_EQ(Rd16(&sDmemA[0x900 + 2 * k]), dry, "dry R %d", k);
        CHECK_EQ(Rd16(&sDmemA[0xB00 + 2 * k]), wet, "wet R %d", k);
    }
    // Hand-worked: 1000 * 0x8000 >> 16 = 500, XOR -1 = -501; wet -501 * 0x4000 >> 16 = -126 (floor)
    CHECK_EQ(Rd16(&sDmemA[0x800]), -501, "hand-worked dry");
    CHECK_EQ(Rd16(&sDmemA[0xA00]), -126, "hand-worked wet");
}

/** ADPCM with a codebook whose only nonzero entries are book1[0] = 2048 (1.0) and book2[0] = 0x800 */
static void Test_Adpcm(void) {
    static const u8 frame[9] = { 0x00, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0 };
    s32 expected[16];
    s32 nib;
    int i;

    Begin("ADPCM");
    // Codebook entry 0 at RAM_POOL: book1 (8), book2 (8); entry 1: zeros
    memset(&sRamInit[RAM_POOL], 0, 64);
    Wr16(&sRamInit[RAM_POOL + 0], 2048);
    Wr16(&sRamInit[RAM_POOL + 16], 0x800);
    // State: previous frame ends with 300, 400
    for (i = 0; i < 16; i++) {
        Wr16(&sRamInit[RAM_POOL + 0x100 + 2 * i], (i == 14) ? 300 : (i == 15) ? 400 : 0);
    }
    memcpy(&sDmemInit[0x800], frame, sizeof(frame)); // header 0x00: predictor 0, scale 0
    A(aLoadADPCM, 64, RAM_POOL);
    A(aSetBuffer, 0, 0x800, 0x400, 32);
    A(aADPCMdec, 0, RAM_POOL + 0x100);
    RunUnit();

    // Residuals: scale 0, so (nibble << 12) >> 12, the signed nibble. Sample 0: book1[0] * prev[14] +
    // book2[0] * prev[15] + r0 << 11 = 2048 * 300 + 2048 * 400 + 2048 * r0, >> 11 = 700 + r0. Sample i > 0:
    // r_i plus book2[0] * r_{i-1} >> 11 (rdot), i.e. r_i + r_{i-1}; book2[0] also has lane 0 only.
    for (i = 0; i < 16; i++) {
        nib = (frame[1 + i / 2] >> ((i & 1) ? 0 : 4)) & 0xF;
        expected[i] = (nib >= 8) ? nib - 16 : nib;
    }
    {
        s32 r[16];
        for (i = 0; i < 16; i++) {
            r[i] = expected[i];
        }
        expected[0] = 700 + r[0];
        for (i = 1; i < 8; i++) {
            expected[i] = r[i] + r[i - 1];
        }
        // Second half: book1[0] * out[6] + book2[0] * out[7] for lane 0
        expected[8] = expected[6] + expected[7] + r[8];
        for (i = 9; i < 16; i++) {
            expected[i] = r[i] + r[i - 1];
        }
    }
    for (i = 0; i < 16; i++) {
        CHECK_EQ(Rd16(&sDmemA[0x400 + 2 * i]), (i == 14) ? 300 : (i == 15) ? 400 : 0, "state %d", i);
        CHECK_EQ(Rd16(&sDmemA[0x420 + 2 * i]), expected[i], "sample %d", i);
        // The last frame is saved as the new state
        CHECK_EQ(Rd16(&sRamA[RAM_POOL + 0x100 + 2 * i]), expected[i], "saved state %d", i);
    }
    // Hand-worked: nibbles 1, 2 -> 701, 3
    CHECK_EQ(Rd16(&sDmemA[0x420]), 701, "hand-worked 0");
    CHECK_EQ(Rd16(&sDmemA[0x422]), 3, "hand-worked 1");
}

/** ADPCM scale and 2-bit mode, with an all-zero codebook: outputs are the scaled residuals */
static void Test_AdpcmScale(void) {
    int i;
    int scale;
    s32 nib;
    s32 r;

    for (scale = 0; scale < 16; scale++) {
        Begin("ADPCM scale");
        memset(&sRamInit[RAM_POOL], 0, 256);
        sDmemInit[0x800] = scale << 4;
        for (i = 0; i < 8; i++) {
            sDmemInit[0x801 + i] = 0x9F - 0x13 * i;
        }
        sDmemInit[0x880] = (scale << 4) | 1; // 2-bit frame, predictor 1
        for (i = 0; i < 4; i++) {
            sDmemInit[0x881 + i] = 0x1B * (i + 1);
        }
        A(aLoadADPCM, 64, RAM_POOL);
        A(aSetBuffer, 0, 0x800, 0x400, 32);
        A(aADPCMdec, A_INIT, RAM_POOL + 0x100);
        A(aSetBuffer, 0, 0x880, 0x600, 32);
        A(aADPCMdec, A_INIT | A_ADPCM_SHORT, RAM_POOL + 0x140);
        RunUnit();
        for (i = 0; i < 16; i++) {
            nib = (sDmemInit[0x801 + i / 2] >> ((i & 1) ? 0 : 4)) & 0xF;
            r = (s16)(nib << 12);
            r = (scale < 12) ? (r >> (12 - scale)) : r;
            CHECK_EQ(Rd16(&sDmemA[0x420 + 2 * i]), r, "4-bit scale %d sample %d", scale, i);
            nib = (sDmemInit[0x881 + i / 4] >> (6 - 2 * (i & 3))) & 3;
            r = (s16)(nib << 14);
            r = (scale < 14) ? (r >> (14 - scale)) : r;
            CHECK_EQ(Rd16(&sDmemA[0x620 + 2 * i]), r, "2-bit scale %d sample %d", scale, i);
        }
    }
}

/** RESAMPLE: positions, table rows, per-product rounding and the saved state */
static void Test_Resample(void) {
    static const u16 pitches[] = { 0x8000, 0x4000, 0xC000, 0x1234 };
    s32 in[64];
    s32 s;
    u32 pos;
    u32 idx;
    const u8* row;
    int p;
    int k;
    int j;

    for (p = 0; p < 4; p++) {
        Begin("RESAMPLE");
        for (k = 0; k < 64; k++) {
            in[k] = RandSample();
            Wr16(&sDmemInit[0x800 + 2 * k], in[k]);
        }
        A(aSetBuffer, 0, 0x800, 0x400, 24); // 12 outputs: rounds up to 16
        A(aResample, A_INIT, pitches[p], RAM_POOL);
        RunUnit();
        pos = 0;
        for (k = 0; k < 16; k++) {
            // The 4 taps start at in - 4 samples (the zeroed state), at the integer position
            idx = pos >> 16;
            row = &sAspData[0xE0 + ((pos & 0xFFFF) >> 10) * 8];
            s = 0;
            {
                s32 prod[4];
                for (j = 0; j < 4; j++) {
                    s32 sample = ((s32)idx + j - 4 < 0) ? 0 : in[idx + j - 4];
                    s64 r = ((s64)sample * Rd16(&row[2 * j]) * 2 + 0x8000) >> 16;
                    prod[j] = Sat(r);
                }
                s = Sat(Sat(prod[0] + prod[1]) + Sat(prod[2] + prod[3]));
            }
            CHECK_EQ(Rd16(&sDmemA[0x400 + 2 * k]), s, "pitch 0x%X output %d", pitches[p], k);
            pos += 2 * pitches[p];
        }
        // State: the 4 samples before the next position, and its fraction
        idx = pos >> 16;
        for (j = 0; j < 4; j++) {
            CHECK_EQ(Rd16(&sRamA[RAM_POOL + 2 * j]), ((s32)idx + j - 4 < 0) ? 0 : in[idx + j - 4], "state %d", j);
        }
        CHECK_EQ((u16)Rd16(&sRamA[RAM_POOL + 8]), pos & 0xFFFF, "state fraction");
    }
}

/** FILTER: 8-tap FIR with the averaged coefficients; A_INIT halves them */
static void Test_Filter(void) {
    static const s16 coefs[8] = { 0x2000, -0x1000, 0x0800, 0x0400, -0x0200, 0x0100, 0x0080, 0x4000 };
    s32 c[8];
    s32 x[24];
    s64 acc;
    int i;
    int k;

    Begin("FILTER");
    for (i = 0; i < 8; i++) {
        Wr16(&sRamInit[RAM_POOL + 2 * i], coefs[i]);
        // Rounded average with the zero "previous" coefficients of A_INIT: (c + 0 + 1) >> 1
        c[i] = (coefs[i] + 1) >> 1;
    }
    for (i = 0; i < 8; i++) {
        x[i] = 0; // A_INIT history
    }
    for (i = 0; i < 16; i++) {
        x[8 + i] = RandSample();
        Wr16(&sDmemInit[0x400 + 2 * i], x[8 + i]);
    }
    A(aFilter, 2, 32, RAM_POOL);
    A(aFilter, A_INIT, 0x400, RAM_POOL + 0x100);
    RunUnit();
    for (k = 0; k < 16; k++) {
        acc = 0x8000;
        for (i = 0; i < 8; i++) {
            acc += (s64)2 * c[i] * x[8 + k - i];
        }
        CHECK_EQ(Rd16(&sDmemA[0x400 + 2 * k]), Sat(acc >> 16), "output %d", k);
    }
    // State: the last 8 inputs, then the coefficients used
    for (i = 0; i < 8; i++) {
        CHECK_EQ(Rd16(&sRamA[RAM_POOL + 0x100 + 2 * i]), x[16 + i], "state input %d", i);
        CHECK_EQ(Rd16(&sRamA[RAM_POOL + 0x110 + 2 * i]), c[i], "state coefficient %d", i);
    }
}

static void Test_ResampleZoh(void) {
    int k;

    Begin("RESAMPLE_ZOH");
    for (k = 0; k < 32; k++) {
        Wr16(&sDmemInit[0x800 + 2 * k], 100 * k);
    }
    A(aSetBuffer, 0, 0x800, 0x400, 10); // 5 samples: rounds up to 8
    A(aResampleZoh, 0x6000, 0);         // 0.75 samples per output
    RunUnit();
    for (k = 0; k < 8; k++) {
        // Byte position (0x800 << 16) + k * 0x6000 * 4, rounded down to an even address
        CHECK_EQ(Rd16(&sDmemA[0x400 + 2 * k]), 100 * ((k * 0x6000 * 4) >> 17), "output %d", k);
    }
}

static void Test_S8Dec(void) {
    int k;

    Begin("S8DEC");
    for (k = 0; k < 32; k++) {
        sDmemInit[0x800 + k] = (u8)(k * 9 - 100);
    }
    A(aSetBuffer, 0, 0x800, 0x400, 40); // 2 frames
    A(aS8Dec, A_INIT, RAM_POOL);
    RunUnit();
    for (k = 0; k < 16; k++) {
        CHECK_EQ(Rd16(&sDmemA[0x400 + 2 * k]), 0, "initial state %d", k);
    }
    for (k = 0; k < 32; k++) {
        CHECK_EQ(Rd16(&sDmemA[0x420 + 2 * k]), (s16)((u8)(k * 9 - 100) << 8), "sample %d", k);
    }
    for (k = 0; k < 16; k++) {
        CHECK_EQ(Rd16(&sRamA[RAM_POOL + 2 * k]), (s16)((u8)((16 + k) * 9 - 100) << 8), "state %d", k);
    }
}

static void Test_MultTable(void) {
    int k;

    Begin("opcode 0 (UnkCmd19)");
    for (k = 0; k < 64; k++) {
        Wr16(&sDmemInit[0x400 + 2 * k], (k - 20) * 37);
    }
    // As synthesis.c sends it for bookOffset 3: the buffer times its own first 32 samples
    CmdUnk19(0x400, 0x400, 70, 0); // rounds up to 128 bytes
    RunUnit();
    for (k = 0; k < 64; k++) {
        CHECK_EQ(Rd16(&sDmemA[0x400 + 2 * k]), Sat((s64)((k - 20) * 37) * (((k & 31) - 20) * 37)), "sample %d", k);
    }
}

static void Test_LoadSave(void) {
    int i;

    Begin("LOADBUFF/SAVEBUFF");
    for (i = 0; i < 64; i++) {
        sRamInit[RAM_POOL + i] = i;
    }
    // The RSP DMA ignores the low 3 bits of both addresses
    A(aLoadBuffer, RAM_POOL + 5, 0x40C, 32);
    A(aSaveBuffer, 0x40C, RAM_POOL + 0x103, 16);
    A(aLoadBuffer, RAM_POOL, 0x500, 0); // size 0: skipped (counted as an error)
    RunUnit();
    for (i = 0; i < 32; i++) {
        CHECK_EQ(sDmemA[0x408 + i], i, "load 0x%X", 0x408 + i);
    }
    for (i = 0; i < 16; i++) {
        CHECK_EQ(sRamA[RAM_POOL + 0x100 + i], i, "save %d", i);
    }
    CHECK_EQ(AudUcode_GetErrorCount(), 1, "zero-size DMA");
}

/* ------------------------------------------------------------------------------------------------ */
/* Random lists (fast vs exact, and against the microcode with CXD4)                                */
/* ------------------------------------------------------------------------------------------------ */

/**
 * A random DMEM buffer address for `size` bytes, between the codebook (0x330) and the state scratch
 * (0xFB0). Never below: a command that wrote into the command buffer (0x2F0-0x32F) would change the
 * commands still to come, and the real microcode would jump into garbage.
 */
static u32 RandDmem(u32 align, u32 size) {
    return RandRange(DMEM_RAND_LO, 0xFB0 - size) & ~(align - 1);
}

/** base + delta, kept at or above DMEM_RAND_LO */
static u32 NearDmem(u32 base, s32 delta, u32 align) {
    s32 addr = (s32)base + delta;

    return ((addr < DMEM_RAND_LO) ? (u32)DMEM_RAND_LO : (u32)addr) & ~(align - 1);
}

static u32 RandRam(u32 size) {
    return RAM_POOL + (RandRange(0, RAM_POOL_SIZE - size - 64) & ~1);
}

static void RandFilterCoefs(u32 addr);

/** One random command of type `op`, with whatever setup it needs before it */
static void RandomCommand(u32 op) {
    u32 in;
    u32 out;
    u32 count;
    u32 flags;

    switch (op) {
        case 0:
            count = RandRange(0, 0x100);
            in = RandDmem(16, count + 64);
            CmdUnk19(in, RandDmem(1, 64), count, Rand() & 0xFF);
            break;
        case A_ADPCM:
            count = RandRange(0, 9) * 32 - (Rand() & 1) * RandRange(0, 31);
            if ((s32)count < 0) {
                count = 0;
            }
            A(aLoadADPCM, RandRange(1, 16) * 16, RandRam(256) & ~7);
            if (Rand() & 1) {
                A(aSetLoop, RandRam(32) & ~7);
            }
            in = RandDmem(1, count + 64);
            // 16-byte aligned out (as in MM): cxd4 skips sqv stores that start in the second half of a
            // 16-byte line, which the RSP does
            out = RandDmem(16, count + 64);
            A(aSetBuffer, 0, in, out, count);
            A(aADPCMdec, Rand() & 7, RandRam(32) & ~7);
            break;
        case A_CLEARBUFF:
            A(aClearBuffer, RandDmem(1, 0x200), RandRange(0, 0x1F0));
            break;
        case A_UNK3:
            Cmd(_SHIFTL(A_UNK3, 24, 8) | (Rand() & 0xFFFFFF), Rand());
            break;
        case A_ADDMIXER:
            // Define v31 the ways synthesis.c does before a haas ADDMIXER
            if (Rand() & 1) {
                A(aMix, 2, Rand(), RandDmem(16, 32), RandDmem(16, 32));
            } else {
                A(aSetBuffer, 0, RandDmem(2, 0x200), RandDmem(16, 0x100), 16);
                A(aResample, A_INIT, 0x8000, RandRam(32) & ~7);
            }
            count = RandRange(0, 0x1C0);
            A(aAddMixer, count, RandDmem(16, count + 64), RandDmem(16, count + 64), 0x7FFF);
            break;
        case A_RESAMPLE:
            count = RandRange(0, 0x1A0);
            flags = (Rand() & 3) ? (Rand() & 1) : ((Rand() & 1) ? 2 : 4);
            if ((Rand() & 7) == 0) {
                A(aClearBuffer, 0xE0, 16); // the microcode then reloads its table from dram_stack
            } else if ((Rand() & 3) == 0) {
                // Random rows, but the first coefficient kept: no reload, and saturating products/sums
                A(aLoadBuffer, RandRam(0x40) & ~7, 0xE8 + 8 * RandRange(0, 40), 16 * RandRange(1, 4));
            }
            in = RandRange(0x400, 0x900) & ~((Rand() & 1) ? 1 : 0);
            out = (Rand() & 1) ? RandDmem(16, count + 32) : NearDmem(in, 2 * RandRange(0, 8) - 0x10, 16);
            A(aSetBuffer, 0, in, out, count);
            A(aResample, flags, Rand() & 0xFFFF, RandRam(32) & ~7);
            break;
        case A_RESAMPLE_ZOH:
            count = RandRange(0, 0x1A0);
            in = RandDmem(2, 0x400);
            out = RandDmem(1, count + 16);
            A(aSetBuffer, 0, in, out, count);
            A(aResampleZoh, Rand() & 0xFFFF, Rand() & 0xFFFF);
            break;
        case A_FILTER:
            count = RandRange(0, 0x1A0);
            in = RandRam(16) & ~7;
            RandFilterCoefs(in);
            A(aFilter, 2, count, in);
            A(aFilter, Rand() & 1, RandDmem(16, count + 32), RandRam(32) & ~7);
            if (Rand() & 1) {
                // Again without a new count: one step
                A(aFilter, 0, RandDmem(16, 32), RandRam(32) & ~7);
            }
            break;
        case A_DUPLICATE:
            count = RandRange(0, 6);
            in = RandDmem(16, 0x80);
            A(aDuplicate, count, in, RandDmem(16, 0x80 * (count + 1)));
            break;
        case A_DMEMMOVE:
            count = RandRange(0, 0x1A0);
            // Even addresses: the halfword tail uses lsv, which cxd4 does not model at odd addresses
            // (MM only moves s16 buffers)
            in = RandDmem(2, count + 2);
            out = (Rand() & 1) ? RandDmem(2, count + 2) : NearDmem(in, 2 * RandRange(0, 20) - 20, 2);
            CmdDmemMove(in, out, count);
            break;
        case A_MIXER:
            count = RandRange(0, 0x40);
            in = RandDmem(16, count * 16 + 32);
            out = (Rand() & 1) ? RandDmem(16, count * 16 + 32) : NearDmem(in, 16 * RandRange(0, 6) - 48, 16);
            A(aMix, count, Rand(), in, out);
            break;
        case A_INTERLEAVE:
            count = RandRange(0, 0x1A0);
            A(aInterleave, RandDmem(1, 2 * count + 16), RandDmem(1, count + 8), RandDmem(1, count + 8), count);
            break;
        case A_HILOGAIN:
            count = RandRange(0, 0x1C0);
            CmdHiLoGain(Rand() & 0xFF, RandDmem(16, count + 32), 0, count);
            break;
        case 16:
            count = RandRange(0, 3);
            in = RandDmem(16, 0x200);
            Cmd(_SHIFTL(16, 24, 8) | _SHIFTL(count, 16, 8) | in,
                _SHIFTL(RandDmem(16, 0x200), 16, 16) | RandRange(0, 0x60));
            break;
        case A_INTERL:
            count = RandRange(0, 0xD0);
            // Even addresses: lsv/slv (see DMEMMOVE)
            in = RandDmem(2, 4 * count + 32);
            out = (Rand() & 1) ? in : RandDmem(2, 2 * count + 16);
            CmdInterl(in, out, count);
            break;
        case A_ENVMIXER:
            // A third of them without XOR flags, then without reverb or silent (the specialized loops)
            flags = (Rand() % 3 == 0) ? (Rand() & 0x10) : (Rand() & 0x1F);
            if (!(flags & 0xF) && (Rand() & 1)) {
                if (Rand() & 1) {
                    A(aEnvSetup1, 0, 0, Rand() & 0xFFFF, Rand() & 0xFFFF);
                    CmdEnvSetup2(Rand() & 0xFFFF, Rand() & 0xFFFF);
                } else {
                    A(aEnvSetup1, Rand() & 0xFF, (Rand() & 1) ? (Rand() & 0xFFFF) : 0, 0, 0);
                    CmdEnvSetup2(0, 0);
                }
            } else {
                A(aEnvSetup1, Rand() & 0xFF, Rand() & 0xFFFF, Rand() & 0xFFFF, Rand() & 0xFFFF);
                CmdEnvSetup2(Rand() & 0xFFFF, Rand() & 0xFFFF);
            }
            count = RandRange(0, 0xFF);
            in = RandDmem(16, 2 * count + 32);
            Cmd(_SHIFTL(A_ENVMIXER, 24, 8) | _SHIFTL(in >> 4, 16, 8) | _SHIFTL(count, 8, 8) | flags,
                AUDIO_SHIFT(RandDmem(16, 0x220), RandDmem(16, 0x220), RandDmem(16, 0x220), RandDmem(16, 0x220)));
            if (Rand() & 1) {
                // Again without a new setup: the ramps continue (doubled again)
                Cmd(_SHIFTL(A_ENVMIXER, 24, 8) | _SHIFTL(in >> 4, 16, 8) | _SHIFTL(count, 8, 8) | flags,
                    AUDIO_SHIFT(RandDmem(16, 0x220), RandDmem(16, 0x220), RandDmem(16, 0x220), RandDmem(16, 0x220)));
            }
            break;
        case A_LOADBUFF:
            count = RandRange(1, 0x40) * 16;
            A(aLoadBuffer, RandRam(count + 8), RandDmem(1, count + 8), count);
            break;
        case A_SAVEBUFF:
            count = RandRange(1, 0x40) * 16;
            A(aSaveBuffer, RandDmem(1, count + 8), RandRam(count + 8), count);
            break;
        case A_S8DEC:
            count = RandRange(0, 8) * 32 - (Rand() & 1) * RandRange(0, 31);
            if ((s32)count < 0) {
                count = 0;
            }
            if (Rand() & 1) {
                A(aSetLoop, RandRam(32) & ~7);
            }
            A(aSetBuffer, 0, RandDmem(1, count + 32), RandDmem(16, count + 64), count);
            A(aS8Dec, Rand() & 3, RandRam(32) & ~7);
            break;
        default:
            break;
    }
}

static const u32 sRandomOps[] = {
    0,        A_ADPCM,     A_CLEARBUFF, A_UNK3,     A_ADDMIXER,   A_RESAMPLE, A_RESAMPLE_ZOH,
    A_FILTER, A_DUPLICATE, A_DMEMMOVE,  A_MIXER,    A_INTERLEAVE, A_HILOGAIN, 16,
    A_INTERL, A_ENVMIXER,  A_LOADBUFF,  A_SAVEBUFF, A_S8DEC,
};

#define NUM_RANDOM_OPS (sizeof(sRandomOps) / sizeof(sRandomOps[0]))

/** Saturation corners of opcode 0's vmudh, with the table at every alignment */
static void Test_MultTableCorners(void) {
    static const s16 vals[] = { -0x8000, 0x7FFF, -0x7FFF, 0x4000, -0x4000, 1, -1, 0x100 };
    char name[64];
    u32 align;
    int k;

    for (align = 0; align < 16; align++) {
        snprintf(name, sizeof(name), "opcode 0 corners, table at +%u", align);
        Begin(name);
        RandomizeMemory();
        for (k = 0; k < 32; k++) {
            Wr16(&sDmemInit[0x800 + 2 * k], vals[k & 7]);
            Wr16(&sDmemInit[0x900 + align + 2 * k], vals[(k / 8 + k) & 7]);
        }
        CmdUnk19(0x800, 0x900 + align, 64, 0);
        RunAndCompare(name);
    }
}

/**
 * RESAMPLE with its input running into DMEM 0xFD0-0xFEF, the sample and table row addresses that the
 * vector loop rewrites before each batch: later batches read those addresses, not the samples that were
 * there (MM's inputs never get there; this keeps the straight loop from being used for it)
 */
static void Test_ResampleNearAddrTables(void) {
    char name[64];
    u32 in;
    u32 pitch;

    for (in = 0xF60; in <= 0xFB0; in += 8) {
        for (pitch = 0x4000; pitch <= 0x10000; pitch += 0x2000) {
            snprintf(name, sizeof(name), "RESAMPLE input at 0x%X, pitch 0x%X", in, pitch);
            Begin(name);
            RandomizeMemory();
            A(aSetBuffer, 0, in, 0x800, 0x40);
            A(aResample, A_INIT, pitch & 0xFFFF, RandRam(32) & ~7);
            RunAndCompare(name);
        }
    }
}

/** Rows of MM's low- and high-pass tables (src/audio/lib/data.c): symmetric FIRs */
static const s16 sMmFilters[][8] = {
    { 0, 0, 0, 32767, 0, 0, 0, 0 },
    { 3854, 4188, 4398, 4469, 4398, 4188, 3854, 3416 },
    { -2252, -693, 7121, 11962, 7121, -693, -2252, 668 },
    { 841, -853, 863, 26829, 863, -853, 841, -820 },
    { -289, -291, -289, 30736, -289, -291, -289, -290 },
    { -772, -3, -6985, 17240, -6985, -3, -772, -3 },
};

/**
 * 8 filter coefficients at RAM addr: random ones, MM's (symmetric), or random symmetric ones whose
 * absolute values add up to about 0xFFFF (the limit of the 32-bit loop)
 */
static void RandFilterCoefs(u32 addr) {
    s32 c[8];
    s32 sum;
    int i;

    switch (Rand() % 4) {
        case 0:
            break; // the random contents
        case 1:
            for (i = 0; i < 8; i++) {
                Wr16(&sRamInit[addr + 2 * i], sMmFilters[Rand() % 6][i]);
            }
            break;
        default:
            sum = 0;
            for (i = 0; i < 4; i++) {
                c[i] = (s32)RandRange(0, 0x3000) - 0x1800;
                sum += (c[i] < 0) ? -c[i] : c[i];
            }
            c[7] = (s32)RandRange(0, 0x2000) - 0x1000;
            sum = 2 * sum - ((c[3] < 0) ? -c[3] : c[3]) + ((c[7] < 0) ? -c[7] : c[7]);
            // Scale the centre tap to bring the total near 0xFFFF, sometimes just over
            c[3] += ((c[3] < 0) ? -1 : 1) * (0xFFFF - sum + (s32)RandRange(0, 2) - 1);
            if ((c[3] < -0x8000) || (c[3] > 0x7FFF)) {
                c[3] = 0x7FFF;
            }
            for (i = 0; i < 8; i++) {
                Wr16(&sRamInit[addr + 2 * i], (i < 4) ? c[i] : (i < 7) ? c[6 - i] : c[7]);
            }
            break;
    }
}

/**
 * FILTER at the bounds of the 32-bit loop: coefficients whose absolute values add up to 0xFFFF and
 * 0x10000, symmetric or not, on full-scale samples with the signs of the coefficients. Only -0x8000
 * has a magnitude of 0x8000, so the largest sum, 0x8000 times the total (2^31 at 0x10000, which
 * overflows the 32-bit loop's sum + 0x4000), takes negative coefficients on -0x8000 samples: the
 * "extreme" case, all coefficients negative. The mixed-sign cases stay up to about 50000 below it.
 */
static void Test_FilterBounds(void) {
    static const s32 base[8] = { 4000, -6000, 8000, 0, 8000, -6000, 4000, -4000 };
    static const char* const sSignNames[] = { "positive", "negative", "extreme" };
    char name[64];
    s32 c[8];
    int total;
    int sym;
    int sign;
    int i;

    for (total = 0xFFFE; total <= 0x10000; total++) {
        for (sym = 0; sym < 2; sym++) {
            for (sign = 0; sign < 3; sign++) {
                snprintf(name, sizeof(name), "FILTER sum 0x%X %s %s", total, sym ? "symmetric" : "asymmetric",
                         sSignNames[sign]);
                Begin(name);
                RandomizeMemory();
                for (i = 0; i < 8; i++) {
                    c[i] = (sign == 2) ? -abs(base[i]) : base[i];
                }
                if (!sym) {
                    c[0] += 1;
                    c[6] -= 1;
                }
                c[3] = total - 40000; // |c| of the others: 40000
                if (sign == 2) {
                    c[3] = -c[3];
                }
                for (i = 0; i < 8; i++) {
                    Wr16(&sRamInit[RAM_POOL + 2 * i], c[i]);
                    // State: 8 previous inputs, then old coefficients equal to the new ones (no halving)
                    Wr16(&sRamInit[RAM_POOL + 0x100 + 16 + 2 * i], c[i]);
                }
                // Samples: input n meets coefficient j at output n + j; full scale with the sign of the
                // coefficient that the middle outputs see (all -0x8000 in the extreme case)
                for (i = 0; i < 64; i++) {
                    s32 cj = c[(64 - i) % 8];
                    s32 s = ((sign == 2) || ((cj < 0) != (sign != 0))) ? -0x8000 : 0x7FFF;

                    Wr16(&sDmemInit[0x800 + 2 * i], s);
                }
                for (i = 0; i < 8; i++) {
                    Wr16(&sRamInit[RAM_POOL + 0x100 + 2 * i], -0x8000);
                }
                A(aFilter, 2, 128, RAM_POOL);
                A(aFilter, 0, 0x800, RAM_POOL + 0x100);
                RunAndCompare(name);
            }
        }
    }
}

/**
 * ADPCM and RESAMPLE writing just below their input, as MM does for some notes: around the distances
 * where the output starts to catch up with input that the vector loop has not read yet
 */
static void Test_OutputBelowInput(void) {
    static const u32 sFrames[] = { 1, 2, 5, 9, 23 };
    static const u16 sPitches[] = { 0x8000, 0x8FAC, 0x7F00, 0x6000, 0x7FF0, 0x4000 };
    char name[96];
    u32 f;
    u32 shortMode;
    s32 d;
    s32 dMin;
    u32 p;
    u32 frameSize;
    u32 count;

    for (f = 0; f < sizeof(sFrames) / sizeof(sFrames[0]); f++) {
        for (shortMode = 0; shortMode < 2; shortMode++) {
            frameSize = shortMode ? 5 : 9;
            // The straight loop's limit: in - out >= 32 * frames - frameSize * (frames - 1). Closer than
            // that by up to about 20 bytes the result happens to be the same; then it differs.
            dMin = 32 * sFrames[f] - frameSize * (sFrames[f] - 1);
            for (d = dMin - 50; d <= dMin + 10; d += 3) {
                snprintf(name, sizeof(name), "ADPCM %u frames%s, input %d bytes above the output", sFrames[f],
                         shortMode ? " (2-bit)" : "", d);
                Begin(name);
                RandomizeMemory();
                A(aLoadADPCM, 16 * 16, RandRam(256) & ~7);
                A(aSetBuffer, 0, 0x600 + d, 0x600 - 32, sFrames[f] * 32);
                A(aADPCMdec, (shortMode ? A_ADPCM_SHORT : 0) | (Rand() & 1), RandRam(32) & ~7);
                RunAndCompare(name);
            }
        }
    }
    for (p = 0; p < sizeof(sPitches) / sizeof(sPitches[0]); p++) {
        for (count = 0x160; count <= 0x180; count += 0x20) {
            for (d = 0; d <= 0x30; d += 2) {
                snprintf(name, sizeof(name), "RESAMPLE pitch 0x%X, %u bytes, input %d bytes above the output",
                         sPitches[p], count, d);
                Begin(name);
                RandomizeMemory();
                // The input (after the 4 state samples, 8 bytes) starts d bytes above out
                A(aSetBuffer, 0, 0x600 + 8 + d, 0x600, count);
                A(aResample, Rand() & 1, sPitches[p], RandRam(32) & ~7);
                RunAndCompare(name);
            }
        }
    }
}

static void Test_RandomCommands(int iterations) {
    char name[64];
    u32 i;
    int it;

    for (i = 0; i < NUM_RANDOM_OPS; i++) {
        for (it = 0; it < iterations; it++) {
            snprintf(name, sizeof(name), "random op %u #%d", sRandomOps[i], it);
            Begin(name);
            RandomizeMemory();
            RandomCommand(sRandomOps[i]);
            if (RunAndCompare(name)) {
                break;
            }
        }
    }

    // Mixed lists: several commands of any kind in one task
    for (it = 0; it < iterations; it++) {
        snprintf(name, sizeof(name), "random list #%d", it);
        Begin(name);
        RandomizeMemory();
        for (i = 0; i < 12; i++) {
            RandomCommand(sRandomOps[Rand() % NUM_RANDOM_OPS]);
        }
        if (RunAndCompare(name)) {
            break;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* MM-like synthesis lists (the command patterns of AudioSynth_ProcessSample and friends)            */
/* ------------------------------------------------------------------------------------------------ */

/** One note, as AudioSynth_ProcessSample builds it (ADPCM source, the common branches) */
static void SynthNote(u32 numSamplesPerUpdate, int first) {
    u32 pitch = RandRange(0x1000, 0xFFFF);
    u32 numSamplesToLoad = (pitch * numSamplesPerUpdate * 2) >> 16;
    u32 numFrames = (numSamplesToLoad + 15) / 16 + 1;
    u32 chunk = ((numFrames * 9) + 16 + 15) & ~15;
    u32 dmemData = DMEM_COMPRESSED_ADPCM_DATA - chunk;
    u32 pad = Rand() & 0xF;
    u32 flags = first ? A_INIT : A_CONTINUE;
    u32 size = numSamplesPerUpdate * 2;
    u32 state = RandRam(0x2E0) & ~15;
    u32 haas = Rand() % 3;
    u32 dests;

    A(aLoadADPCM, 16 * 2 * RandRange(1, 4), RandRam(256) & ~15);
    A(aLoadBuffer, RandRam(chunk + 16) & ~15, dmemData, chunk);
    if (Rand() & 1) {
        A(aSetLoop, RandRam(32) & ~15);
        flags = A_LOOP;
    }
    A(aSetBuffer, 0, dmemData + pad, DMEM_UNCOMPRESSED_NOTE, numFrames * 32);
    A(aADPCMdec, flags | ((Rand() % 8 == 0) ? A_ADPCM_SHORT : 0), state + 0x000);
    A(aSetBuffer, 0, DMEM_UNCOMPRESSED_NOTE + 32, DMEM_TEMP, size);
    A(aResample, first ? A_INIT : 0, pitch, state + 0x020);
    if (Rand() & 1) {
        CmdHiLoGain(RandRange(0x10, 0x7F), DMEM_TEMP, 0, size + 32);
    }
    if (Rand() & 1) {
        u32 coefs = RandRam(16) & ~15;

        RandFilterCoefs(coefs);
        A(aFilter, 2, size, coefs);
        A(aFilter, first ? A_INIT : 0, DMEM_TEMP, state + 0x040);
    }
    if ((Rand() & 3) == 0) {
        u32 combSize = RandRange(1, 8) * 16;
        u32 combDmem = DMEM_COMB_TEMP - combSize;

        CmdDmemMove(DMEM_TEMP, DMEM_COMB_TEMP, size);
        if (first) {
            A(aClearBuffer, combDmem, combSize);
        } else {
            A(aLoadBuffer, state + 0xE0, combDmem, combSize);
        }
        A(aSaveBuffer, DMEM_TEMP + size - combSize, state + 0xE0, combSize);
        A(aMix, size >> 4, Rand() & 0xFFFF, DMEM_COMB_TEMP, combDmem);
        CmdDmemMove(combDmem, DMEM_TEMP, size);
    }
    if ((Rand() & 7) == 0) {
        CmdUnk19(DMEM_TEMP, DMEM_TEMP, size, 0);
    }

    // ProcessEnvelope
    dests = AUDIO_SHIFT(DMEM_LEFT_CH, DMEM_RIGHT_CH, DMEM_WET_LEFT_CH, DMEM_WET_RIGHT_CH);
    if (haas != 0) {
        A(aClearBuffer, DMEM_HAAS_TEMP, DMEM_1CH_SIZE);
        dests = (haas == 1) ? AUDIO_SHIFT(DMEM_HAAS_TEMP, DMEM_RIGHT_CH, DMEM_WET_LEFT_CH, DMEM_WET_RIGHT_CH)
                            : AUDIO_SHIFT(DMEM_LEFT_CH, DMEM_HAAS_TEMP, DMEM_WET_LEFT_CH, DMEM_WET_RIGHT_CH);
    }
    // Reverb volume (0 for many notes) with a ramp only when it changes; the strong/swap flags are rare
    {
        u32 rev = (Rand() & 1) ? 0 : RandRange(0, 0x7F) * 2;
        u32 rampRev = (Rand() % 4 == 0) ? (Rand() & 0xFFFF) : 0;

        A(aEnvSetup1, rev, rampRev, Rand() & 0xFFFF, Rand() & 0xFFFF);
    }
    CmdEnvSetup2(Rand() & 0xFFFF, Rand() & 0xFFFF);
    Cmd(_SHIFTL(A_ENVMIXER, 24, 8) | _SHIFTL(DMEM_TEMP >> 4, 16, 8) | _SHIFTL(numSamplesPerUpdate, 8, 8) |
            ((Rand() % 8 == 0) ? (Rand() & 0x1F) : 0),
        dests);

    // ApplyHaasEffect
    if (haas != 0) {
        u32 delay = RandRange(0, 15) * 2;
        u32 prevDelay = first ? 0 : RandRange(0, 15) * 2;

        if (!first && (delay != prevDelay)) {
            u32 n = size + delay - prevDelay;
            A(aSetBuffer, 0, DMEM_HAAS_TEMP, DMEM_TEMP, n);
            A(aResampleZoh, (((size << 15) / 2) - 1) / ((n - 2) / 2), 0);
        } else {
            CmdDmemMove(DMEM_HAAS_TEMP, DMEM_TEMP, size);
        }
        if (prevDelay != 0) {
            A(aLoadBuffer, state + 0xA0, DMEM_HAAS_TEMP, (prevDelay + 15) & ~15);
            CmdDmemMove(DMEM_TEMP, DMEM_HAAS_TEMP + prevDelay, size + delay - prevDelay);
        } else {
            CmdDmemMove(DMEM_TEMP, DMEM_HAAS_TEMP, size + delay);
        }
        if (delay != 0) {
            A(aSaveBuffer, DMEM_HAAS_TEMP + size, state + 0xA0, (delay + 15) & ~15);
        }
        A(aAddMixer, ((size + 63) & ~63), DMEM_HAAS_TEMP, (haas == 1) ? DMEM_LEFT_CH : DMEM_RIGHT_CH, 0x7FFF);
    }
}

/** One update of AudioSynth_ProcessSamples with one reverb (default load/save) and some notes */
static void SynthUpdate(u32 numSamplesPerUpdate, u32 aiBuf, int first) {
    u32 size = numSamplesPerUpdate * 2;
    u32 ring = RAM_POOL + 0xC0000;
    u32 pos = RandRange(0, 0x1000) * 2;
    u32 notes = RandRange(1, 10);
    u32 i;

    A(aClearBuffer, DMEM_LEFT_CH, DMEM_2CH_SIZE);
    // Reverb: load, mix into the dry channels, decay, leak, notes, filter, save
    A(aLoadBuffer, ring + pos, DMEM_WET_LEFT_CH, size);
    A(aLoadBuffer, ring + 0x10000 + pos, DMEM_WET_RIGHT_CH, size);
    A(aMix, DMEM_2CH_SIZE >> 4, 0x7FFF, DMEM_WET_LEFT_CH, DMEM_LEFT_CH);
    A(aMix, DMEM_2CH_SIZE >> 4, RandRange(0x4000, 0x7000) + 0x8000, DMEM_WET_LEFT_CH, DMEM_WET_LEFT_CH);
    if (Rand() & 1) {
        CmdDmemMove(DMEM_WET_LEFT_CH, DMEM_WET_SCRATCH, DMEM_1CH_SIZE);
        A(aMix, DMEM_1CH_SIZE >> 4, Rand() & 0x7FFF, DMEM_WET_RIGHT_CH, DMEM_WET_LEFT_CH);
        A(aMix, DMEM_1CH_SIZE >> 4, Rand() & 0x7FFF, DMEM_WET_SCRATCH, DMEM_WET_RIGHT_CH);
    }
    for (i = 0; i < notes; i++) {
        SynthNote(numSamplesPerUpdate, first);
    }
    if (Rand() & 1) {
        u32 coefs = RandRam(16) & ~15;

        RandFilterCoefs(coefs);
        A(aFilter, 2, size, coefs);
        A(aFilter, first ? A_INIT : 0, DMEM_WET_LEFT_CH, RandRam(32) & ~15);
    }
    if (Rand() & 1) {
        // Downsampled reverb save: halve in place, then save
        CmdInterl(DMEM_WET_LEFT_CH, DMEM_WET_LEFT_CH, 13 * 16);
        CmdInterl(DMEM_WET_RIGHT_CH, DMEM_WET_RIGHT_CH, 13 * 16);
    }
    A(aSaveBuffer, DMEM_WET_LEFT_CH, ring + pos, size);
    A(aSaveBuffer, DMEM_WET_RIGHT_CH, ring + 0x10000 + pos, size);
    A(aInterleave, DMEM_TEMP, DMEM_LEFT_CH, DMEM_RIGHT_CH, size);
    A(aSaveBuffer, DMEM_TEMP, aiBuf, 2 * size);
}

static void Test_SynthLists(int iterations) {
    char name[64];
    int it;
    u32 n;

    for (it = 0; it < iterations; it++) {
        snprintf(name, sizeof(name), "MM-like list #%d", it);
        Begin(name);
        RandomizeMemory();
        // numSamplesPerUpdate: multiples of 8 around 0xB8 (updatesPerFrame 3 at 32 kHz)
        n = RandRange(0x15, 0x1A) * 8;
        SynthUpdate(n, RAM_POOL + 0xF0000, it & 1);
        SynthUpdate(n, RAM_POOL + 0xF0000 + 4 * n, 0);
        if (RunAndCompare(name)) {
            break;
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Captured game tasks (AUD_CAPTURE builds, audcap.py)                                              */
/* ------------------------------------------------------------------------------------------------ */

static u32 Be32(const u8* p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/**
 * Replays every task of a capture file from its own DMEM and RAM: fast paths, exact versions and (with
 * the reference) the real microcode must give the same DMEM and RAM. Game addresses are used modulo
 * 16 MiB, as the RSP does; a capture whose pieces of RAM collide there is reported and skipped.
 */
static void Test_Replay(const char* path) {
    static u8 owner[RAM_SIZE / 8]; // which 8-byte units of RAM a piece of this task has written
    u8 header[AUD_TASK_HEADER_SIZE];
    char name[96];
    FILE* f = fopen(path, "rb");
    u8* file;
    long size;
    const u8* p;
    const u8* udata;
    const u8* t;
    u32 ntasks;
    u32 k;
    u32 n;
    u32 i;
    u32 addr;
    u32 len;
    u32 off;
    int clash;

    if ((f == NULL) || (fseek(f, 0, SEEK_END) != 0) || ((size = ftell(f)) < 12) || (fseek(f, 0, SEEK_SET) != 0)) {
        fprintf(stderr, "cannot read %s\n", path);
        sFailures++;
        if (f != NULL) {
            fclose(f);
        }
        return;
    }
    file = malloc(size);
    if ((file == NULL) || (fread(file, 1, size, f) != (size_t)size) || (memcmp(file, "AUDCAP1", 8) != 0)) {
        fprintf(stderr, "%s is not a capture file (audcap.py)\n", path);
        sFailures++;
        fclose(f);
        free(file);
        return;
    }
    fclose(f);

    ntasks = Be32(file + 8);
    udata = file + 12;
    p = udata + AUD_UCODE_DATA_SIZE;
    for (k = 0; k < ntasks; k++) {
        snprintf(name, sizeof(name), "captured task %u", (unsigned)k);
        sTestName = name;
        t = p;
        p += 12 + AUD_DMEM_SIZE + AUD_TASK_HEADER_SIZE;
        n = Be32(p);
        p += 4;

        // RAM: the pieces the list reads (the aspMainStack copy among them), then the microcode's data where
        // the OSTask points (the GameCube build's OSTask points to zero-filled stand-ins for both)
        memset(owner, 0, sizeof(owner));
        clash = 0;
        for (i = 0; i <= n; i++) {
            if (i < n) {
                addr = Be32(p) & 0xFFFFFF;
                len = Be32(p + 4);
            } else {
                // Not a piece: placed by hand below
                addr = Be32(t + 12 + AUD_DMEM_SIZE + 0x18) & 0xFFFFF8;
                len = AUD_UCODE_DATA_SIZE;
                for (off = 0; off < len; off += 8) {
                    if (owner[((addr + off) & 0xFFFFFF) >> 3] != 0) {
                        clash = 1;
                    }
                }
                if (addr + len <= RAM_SIZE) {
                    memcpy(&sRamInit[addr], udata, len);
                }
                break;
            }
            for (off = 0; off < len; off += 8) {
                u32 unit = ((addr + off) & 0xFFFFFF) >> 3;

                if ((owner[unit] != 0) && (memcmp(&sRamInit[unit * 8], p + 8 + off, 8) != 0)) {
                    clash = 1;
                }
                owner[unit] = 1;
            }
            if (addr + len <= RAM_SIZE) {
                memcpy(&sRamInit[addr], p + 8, len);
            } else {
                clash = 1;
            }
            p += 8 + len;
        }
        if (clash) {
            fprintf(stderr, "%s: pieces of RAM collide modulo 16 MiB, skipped\n", name);
            continue;
        }
        // The list itself must be there (captures before the fix of lists over 4 KiB lack it)
        for (off = 0; off < Be32(t + 4); off += 8) {
            if (owner[((Be32(t) + off) & 0xFFFFFF) >> 3] == 0) {
                clash = 1;
            }
        }
        if (clash) {
            Fail("%s: the capture lacks the command list", name);
            continue;
        }
        memcpy(sDmemInit, t + 12, AUD_DMEM_SIZE);

        // As RunAndCompare, with the captured list and OSTask, whose dram_stack points to the real
        // aspMainStack (the game's points to a zero-filled stand-in; DMEM keeps the OSTask, so the
        // microcode and this implementation must see the same one)
        memcpy(header, t + 12 + AUD_DMEM_SIZE, AUD_TASK_HEADER_SIZE);
        Wr32(&header[0x20], Be32(t + 8));
        sChecks++;
        memcpy(sRamA, sRamInit, RAM_SIZE);
        memcpy(sRamB, sRamInit, RAM_SIZE);
        for (i = 0; i < 2; i++) {
            AudUcodeTask task;
            u8* ram = (i == 0) ? sRamA : sRamB;
            u8* dmem = (i == 0) ? sDmemA : sDmemB;

            AudUcode_Reset();
            memcpy(AudUcode_GetDmem(), sDmemInit, AUD_DMEM_SIZE);
            gAudHostRam = ram;
            gAudForceExact = i;
            task.ucodeData = udata;
            task.dramStack = Be32(t + 8);
            task.alistAddr = Be32(t);
            task.alistSize = Be32(t + 4);
            task.header = header;
            AudUcode_RunTask(&task);
            gAudForceExact = 0;
            memcpy(dmem, AudUcode_GetDmem(), AUD_DMEM_SIZE);
            if ((i == 0) && (AudUcode_GetErrorCount() != 0)) {
                fprintf(stderr, "%s: %u unknown commands or zero-size DMAs\n", name,
                        (unsigned)AudUcode_GetErrorCount());
            }
        }
        if (Compare("DMEM fast/exact", sDmemA, sDmemB, AUD_DMEM_SIZE, 0) ||
            Compare("RAM fast/exact", sRamA, sRamB, RAM_SIZE, 0)) {
            Fail("%s", name);
            continue;
        }
#ifdef AUD_LLE
        if (sHaveLle) {
            memcpy(sRamB, sRamInit, RAM_SIZE);
            memcpy(sDmemB, sDmemInit, AUD_DMEM_SIZE);
            memcpy(&sDmemB[AUD_TASK_HEADER_ADDR], header, AUD_TASK_HEADER_SIZE);
            Lle_ResetRegs();
            if (Lle_Run(sRamB, sDmemB, sAspText) < 0) {
                Fail("%s: the microcode did not end with BREAK", name);
                continue;
            }
            if (Compare("DMEM vs microcode", sDmemA, sDmemB, AUD_DMEM_SIZE, 0) ||
                Compare("RAM vs microcode", sRamA, sRamB, RAM_SIZE, 0)) {
                Fail("%s", name);
            }
        }
#endif
    }
    printf("replay %s: %u tasks\n", path, (unsigned)ntasks);
    free(file);
}

/* ------------------------------------------------------------------------------------------------ */

int main(int argc, char** argv) {
    int iterations = 200;

    if ((argc > 2) && (strcmp(argv[1], "--replay") == 0)) {
        if (LoadFile("aspMainText", sAspText, sizeof(sAspText)) ||
            LoadFile("aspMainData", sAspData, sizeof(sAspData)) ||
            LoadFile("aspMainStack", sAspStack, sizeof(sAspStack))) {
            return 2;
        }
        sRamA = calloc(1, RAM_SIZE + 0x10000);
        sRamB = calloc(1, RAM_SIZE + 0x10000);
        sRamInit = calloc(1, RAM_SIZE + 0x10000);
        if ((sRamA == NULL) || (sRamB == NULL) || (sRamInit == NULL)) {
            return 2;
        }
#ifdef AUD_LLE
        sHaveLle = (Lle_Init() == 0);
#endif
        printf("audio host test, replay: %s\n",
               sHaveLle ? "with the microcode on cxd4" : "without the reference interpreter");
        for (int a = 2; a < argc; a++) {
            Test_Replay(argv[a]);
        }
        printf("total: %d checks, %d failures\n", sChecks, sFailures);
        return (sFailures == 0) ? 0 : 1;
    }
    if (argc > 1) {
        sRng ^= strtoull(argv[1], NULL, 0) * 0x2545F4914F6CDD1Dull;
    }
    if (argc > 2) {
        iterations = atoi(argv[2]);
    }
    if (LoadFile("aspMainText", sAspText, sizeof(sAspText)) || LoadFile("aspMainData", sAspData, sizeof(sAspData)) ||
        LoadFile("aspMainStack", sAspStack, sizeof(sAspStack))) {
        return 2;
    }
    sRamA = calloc(1, RAM_SIZE + 0x10000);
    sRamB = calloc(1, RAM_SIZE + 0x10000);
    sRamInit = calloc(1, RAM_SIZE + 0x10000);
    if ((sRamA == NULL) || (sRamB == NULL) || (sRamInit == NULL)) {
        return 2;
    }

#ifdef AUD_LLE
    sHaveLle = (Lle_Init() == 0);
#endif
    printf("audio host test: %s\n", sHaveLle ? "with the microcode on cxd4" : "without the reference interpreter");

    Test_ClearBuff();
    Test_DmemMove();
    Test_Mixer();
    Test_AddMixer();
    Test_HiLoGain();
    Test_Interleave();
    Test_Duplicate();
    Test_EnvMixer();
    Test_Adpcm();
    Test_AdpcmScale();
    Test_Resample();
    Test_Filter();
    Test_ResampleZoh();
    Test_S8Dec();
    Test_MultTable();
    Test_LoadSave();
    printf("unit tests: %d checks, %d failures\n", sChecks, sFailures);

    Test_MultTableCorners();
    Test_ResampleNearAddrTables();
    Test_FilterBounds();
    Test_OutputBelowInput();
    Test_RandomCommands(iterations);
    Test_SynthLists(iterations / 4 + 1);
    printf("total: %d checks, %d failures\n", sChecks, sFailures);
    return (sFailures == 0) ? 0 : 1;
}
