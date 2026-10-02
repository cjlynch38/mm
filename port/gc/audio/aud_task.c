/**
 * Gc_AudRunTask (gc_audio.h): the game's audio RSP tasks, run on the CPU by the software microcode
 * (aud_ucode.c). The audio manager thread calls it for each of its tasks (Gc_AudioMgrRunTask in
 * port/gc/ultra/ai.c, from the TARGET_GC code in src/code/audio_thread_manager.c); osSpTaskStartGo
 * (port/gc/ultra/sp.c) calls it for any M_AUDTASK that reaches the scheduler, which the game does not
 * send. Only one thread may run it at a time.
 *
 * AudioThread_Update (src/audio/lib/thread.c) fills the OSTask: data_ptr and data_size are the
 * command list (gAudioCtx.abiCmdBufs, which AudioHeap_WritebackDCache leaves cached under TARGET_GC),
 * ucode_data is aspMainData and dram_stack is aspMainStack. In the GameCube build those two symbols
 * are zero-filled placeholders (port/gc/game/ucode_dummies.c), but the microcode depends on their
 * contents: its constants, opcode table and resample table. This file embeds both from the files the
 * build extracts from the user's ROM (extracted/n64-us/incbin), so the repository holds none of it.
 *
 * AUD_PROFILE builds log, every AUD_PROFILE_TASKS tasks, the average time per task and how it splits
 * over the opcodes. Other builds log the first AUD_ERROR_REPORTS lists with skipped commands (unknown
 * opcodes, zero-size DMAs, DMAs outside RAM), which only a broken command list has.
 *
 * AUD_CAPTURE builds (-DAUD_CAPTURE=1, optionally -DAUD_CAPTURE_AT=<seconds>,... and
 * -DAUD_CAPTURE_TASKS=<n>) log AUD_CAPTURE_TASKS consecutive tasks from each of the given times on
 * (seconds after the first audio task, which comes about 1.6 s after boot), as "audcap:" lines: the
 * DMEM, the OSTask and every piece of RAM the list will read, before the task runs. Logging takes
 * 0.4-1 s of game time per task in Dolphin (5-7 s of real time), which delays the rest of the run.
 * port/gc/tests/audio_host/audcap.py turns such a log into a file that the host test replays against
 * the real microcode and that the benchmark runs (see the Makefile there). The lines carry game data
 * from the ROM: keep the log and the file to yourself.
 */
#include "ultra64.h"
#include "gc_audio.h"
#include "gc_bridge.h"
#include "aud_ucode.h"

#ifndef AUD_PROFILE
#define AUD_PROFILE 0
#endif
#define AUD_PROFILE_TASKS 600
#define AUD_ERROR_REPORTS 8

#ifndef AUD_CAPTURE
#define AUD_CAPTURE 0
#endif
#ifndef AUD_CAPTURE_AT
#define AUD_CAPTURE_AT 600
#endif
#ifndef AUD_CAPTURE_TASKS
#define AUD_CAPTURE_TASKS 30
#endif

extern const u8 gAudAspMainData[AUD_UCODE_DATA_SIZE];
extern const u8 gAudAspMainStack[AUD_DRAM_STACK_SIZE];

// The N64 build's rsp/aspMain.s and aspMainStack incbins (sizes checked against the N64 values)
__asm__(".section .rodata\n"
        ".balign 32\n"
        ".global gAudAspMainData\n"
        "gAudAspMainData:\n"
        ".incbin \"extracted/n64-us/incbin/aspMainData\"\n"
        ".if (. - gAudAspMainData) != 0x2E0\n"
        ".error \"aspMainData is not 0x2E0 bytes\"\n"
        ".endif\n"
        ".balign 32\n"
        ".global gAudAspMainStack\n"
        "gAudAspMainStack:\n"
        ".incbin \"extracted/n64-us/incbin/aspMainStack\"\n"
        ".if (. - gAudAspMainStack) != 0x400\n"
        ".error \"aspMainStack is not 0x400 bytes\"\n"
        ".endif\n"
        ".previous\n");

#if AUD_PROFILE
static u32 sAudProfTasks;
static u32 sAudProfCmds;
static u64 sAudProfTicks;
static u64 sAudProfMaxTicks;

static void Aud_ProfileLog(void) {
    u32 op;
    u32 us;

    gc_log("aud: %u tasks, %u us avg, %u us max, %u commands avg", (unsigned)sAudProfTasks,
           (unsigned)((sAudProfTicks * 1000000) / GC_TB_HZ / sAudProfTasks),
           (unsigned)((sAudProfMaxTicks * 1000000) / GC_TB_HZ), (unsigned)(sAudProfCmds / sAudProfTasks));
    for (op = 0; op < AUD_NUM_OPS; op++) {
        if (AudUcode_GetOpCount(op) != 0) {
            us = (u32)((AudUcode_GetOpTicks(op) * 1000000) / GC_TB_HZ / sAudProfTasks);
            gc_log("aud:   op %2u: %5u per task, %4u us per task", (unsigned)op,
                   (unsigned)(AudUcode_GetOpCount(op) / sAudProfTasks), (unsigned)us);
        }
    }
    if (AudUcode_GetErrorCount() != 0) {
        gc_log("aud: %u unknown commands, zero-size DMAs or DMAs outside RAM skipped",
               (unsigned)AudUcode_GetErrorCount());
    }
    sAudProfTasks = 0;
    sAudProfCmds = 0;
    sAudProfTicks = 0;
    sAudProfMaxTicks = 0;
}
#else
static u32 sAudErrorsSeen;
static u32 sAudErrorReports;
#endif

#if AUD_CAPTURE
static const u32 sAudCapAt[] = { AUD_CAPTURE_AT };
static u32 sAudCapNext;  // next entry of sAudCapAt
static u32 sAudCapLeft;  // tasks still to capture from the current time on
static u32 sAudCapCount; // tasks captured so far
static u64 sAudCapEpoch; // time of the first task: AUD_CAPTURE_AT counts from there

/** "audcap: <tag> <address> <hex>" lines of up to 128 bytes */
static void Aud_CapHex(const char* tag, u32 addr, const u8* data, u32 len) {
    static const char sHex[] = "0123456789ABCDEF";
    char buf[2 * 128 + 1];
    u32 n;
    u32 i;

    while (len != 0) {
        n = (len > 128) ? 128 : len;
        for (i = 0; i < n; i++) {
            buf[2 * i] = sHex[data[i] >> 4];
            buf[2 * i + 1] = sHex[data[i] & 0xF];
        }
        buf[2 * n] = '\0';
        gc_log("audcap: %s %08X %s", tag, (unsigned)addr, buf);
        addr += n;
        data += n;
        len -= n;
    }
}

/** A RAM read of a command, as the microcode's DMA does it (lenReg = bytes - 1; 8-byte units) */
static void Aud_CapRam(u32 addr, u32 lenReg) {
    u32 len;
    const u8* p;

    if (lenReg > 0xFFF) {
        return;
    }
    len = (lenReg | 7) + 1;
    p = AUD_RAM_PTR(addr & ~7, len);
    if (p != NULL) {
        Aud_CapHex("ram", addr & ~7, p, len);
    }
}

/** Log the task before it runs: DMEM, the OSTask, the list and the RAM its commands read */
static void Aud_Capture(OSTask* task) {
    const u8* dmem = AudUcode_GetDmem();
    const u8* list = (const u8*)task->t.data_ptr;
    u32 loop = (dmem[0x2E8] << 24) | (dmem[0x2E9] << 16) | (dmem[0x2EA] << 8) | dmem[0x2EB];
    u32 pos;
    u32 w0;
    u32 w1;
    u32 flags;

    if (sAudCapEpoch == 0) {
        sAudCapEpoch = gc_time_ticks();
    }
    if (sAudCapLeft == 0) {
        if ((sAudCapNext >= sizeof(sAudCapAt) / sizeof(sAudCapAt[0])) ||
            (gc_time_ticks() - sAudCapEpoch < (u64)sAudCapAt[sAudCapNext] * GC_TB_HZ)) {
            return;
        }
        sAudCapNext++;
        sAudCapLeft = AUD_CAPTURE_TASKS;
    }
    sAudCapLeft--;

    gc_log("audcap: task %u %08X %u %08X", (unsigned)sAudCapCount, (unsigned)task->t.data_ptr,
           (unsigned)task->t.data_size, (unsigned)gAudAspMainStack);
    if (sAudCapCount++ == 0) {
        Aud_CapHex("udata", 0, gAudAspMainData, AUD_UCODE_DATA_SIZE);
    }
    Aud_CapHex("dmem", 0, dmem, AUD_DMEM_SIZE);
    Aud_CapHex("hdr", 0, (const u8*)task, AUD_TASK_HEADER_SIZE);
    Aud_CapHex("ram", (u32)gAudAspMainStack, gAudAspMainStack, AUD_DRAM_STACK_SIZE);
    // The list (read 64 bytes at a time, so it can be longer than one DMA)
    Aud_CapHex("ram", (u32)list & ~7, (const u8*)((u32)list & ~7), (((u32)list & 7) + task->t.data_size + 7) & ~7);
    for (pos = 0; pos < task->t.data_size; pos += 8) {
        w0 = (list[pos] << 24) | (list[pos + 1] << 16) | (list[pos + 2] << 8) | list[pos + 3];
        w1 = (list[pos + 4] << 24) | (list[pos + 5] << 16) | (list[pos + 6] << 8) | list[pos + 7];
        flags = (w0 >> 16) & 0xFF;
        switch ((w0 >> 24) & 0x7F) {
            case A_LOADBUFF:
                Aud_CapRam(w1, ((w0 >> 12) & 0xFF0) - 1);
                break;
            case A_LOADADPCM:
                Aud_CapRam(w1, (w0 & 0xFFFF) - 1);
                break;
            case A_SETLOOP:
                loop = w1;
                break;
            case A_ADPCM:
            case A_S8DEC:
                if (!(flags & A_INIT)) {
                    Aud_CapRam((flags & A_LOOP) ? loop : w1, 0x1F);
                }
                break;
            case A_RESAMPLE:
                if (!(flags & A_INIT)) {
                    Aud_CapRam(w1, 0x1F);
                }
                break;
            case A_FILTER:
                if (flags > 1) {
                    Aud_CapRam(w1, 0xF);
                } else if (flags == 0) {
                    Aud_CapRam(w1, 0x1F);
                }
                break;
            default:
                break;
        }
    }
    gc_log("audcap: end");
}
#endif

void Gc_AudRunTask(OSTask* task) {
    AudUcodeTask t;
#if AUD_PROFILE
    u64 start;
    u64 ticks;

    AudUcode_SetProfileClock(gc_time_ticks);
    start = gc_time_ticks();
#endif
#if AUD_CAPTURE
    Aud_Capture(task);
#endif

    t.ucodeData = gAudAspMainData;
    t.dramStack = (u32)gAudAspMainStack;
    t.alistAddr = (u32)task->t.data_ptr;
    t.alistSize = task->t.data_size;
    t.header = (const u8*)task;
    AudUcode_RunTask(&t);

#if AUD_PROFILE
    ticks = gc_time_ticks() - start;
    sAudProfTicks += ticks;
    if (ticks > sAudProfMaxTicks) {
        sAudProfMaxTicks = ticks;
    }
    sAudProfCmds += task->t.data_size / 8;
    if (++sAudProfTasks >= AUD_PROFILE_TASKS) {
        Aud_ProfileLog();
        AudUcode_ResetStats();
    }
#else
    if ((AudUcode_GetErrorCount() != sAudErrorsSeen) && (sAudErrorReports < AUD_ERROR_REPORTS)) {
        gc_log("aud: command list %p (%u bytes): %u unknown commands, zero-size DMAs or DMAs outside RAM skipped",
               (void*)task->t.data_ptr, (unsigned)task->t.data_size,
               (unsigned)(AudUcode_GetErrorCount() - sAudErrorsSeen));
        sAudErrorReports++;
    }
    sAudErrorsSeen = AudUcode_GetErrorCount();
#endif
}
