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

void Gc_AudRunTask(OSTask* task) {
    AudUcodeTask t;
#if AUD_PROFILE
    u64 start;
    u64 ticks;

    AudUcode_SetProfileClock(gc_time_ticks);
    start = gc_time_ticks();
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
