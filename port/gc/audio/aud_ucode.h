/**
 * Private interface of the software audio microcode (port/gc/audio).
 *
 * aud_ucode.c is a CPU implementation of Majora's Mask's RSP audio microcode (aspMain, the "nead"
 * ABI): it executes the Acmd lists that AudioSynth_Update (src/audio/lib/synthesis.c) builds against a
 * model of the RSP's 4 KiB DMEM. aud_task.c adapts it to the OSTask the game hands to the scheduler
 * (Gc_AudRunTask, gc_audio.h). The host tests (port/gc/tests/audio_host) build aud_ucode.c with
 * AUD_HOST_TEST, which maps RAM addresses into a fake 16 MiB RDRAM, and compare it with the real
 * microcode run on an RSP interpreter.
 *
 * Only the decomp's basic types are used here, so that the host tests can build the core with a
 * stand-in PR/ultratypes.h.
 */
#ifndef AUD_UCODE_H
#define AUD_UCODE_H

#include "PR/ultratypes.h"

#define AUD_DMEM_SIZE 0x1000

// Size of aspMainData, which the microcode copies to DMEM 0 when it starts (OSTask ucode_data_size + 1)
#define AUD_UCODE_DATA_SIZE 0x2E0
// Size of aspMainStack (OSTask dram_stack): the microcode reloads its resample table from there
#define AUD_DRAM_STACK_SIZE 0x400
// The OSTask that osSpTaskLoad copies to the end of DMEM
#define AUD_TASK_HEADER_ADDR 0xFC0
#define AUD_TASK_HEADER_SIZE 0x40

/*
 * RAM addresses, as the commands carry them (KSEG0 pointers, or physical addresses), to CPU pointers.
 * The RSP only uses the low 24 bits of an address. GameCube RAM is 24 MiB (0x80000000-0x817FFFFF),
 * so the GameCube build keeps whole addresses: KSEG0 (0x8xxxxxxx) and the uncached mirror
 * (0xCxxxxxxx) are used as they are, physical (0x0xxxxxxx) and N64 KSEG1 (0xAxxxxxxx) addresses
 * are mapped to KSEG0.
 * AUD_RAM_PTR(addr, len) is NULL for a transfer of `len` bytes that does not lie in MEM1 above the
 * OS's low memory (exception vectors and globals), which only a bad pointer can produce: the DMA is
 * then skipped and counted as an error, where the CPU would fault or overwrite the exception vectors
 * (and the RSP would wrap the address into RDRAM).
 */
#ifdef AUD_HOST_TEST
extern u8* gAudHostRam; // fake RDRAM: 16 MiB of big-endian data (port/gc/tests/audio_host)
#define AUD_ADDR_MASK 0xFFFFFF
#define AUD_RAM_PTR(addr, len) (gAudHostRam + ((addr) & 0xFFFFFF))
#else
#define AUD_ADDR_MASK 0xFFFFFFFF
#define AUD_MEM1_SIZE 0x01800000
#define AUD_MEM1_LOW 0x3100 // 0x80000000-0x800030FF: exception vectors and OS globals
static inline u8* AudUcode_RamPtr(u32 addr, u32 len) {
    u32 seg = addr >> 28;
    u32 offset = addr & 0x0FFFFFFF;

    if (((seg != 0x0) && (seg != 0x8) && (seg != 0xA) && (seg != 0xC)) || (offset < AUD_MEM1_LOW) ||
        (len > AUD_MEM1_SIZE) || (offset > AUD_MEM1_SIZE - len)) {
        return NULL;
    }
    return (u8*)(((seg == 0xC) ? 0xC0000000 : 0x80000000) | offset);
}
#define AUD_RAM_PTR(addr, len) AudUcode_RamPtr(addr, len)
#endif

typedef struct AudUcodeTask {
    /* 0x00 */ const u8* ucodeData; // aspMainData, AUD_UCODE_DATA_SIZE bytes (copied to DMEM 0)
    /* 0x04 */ u32 dramStack;       // RAM address of aspMainStack (OSTask dram_stack)
    /* 0x08 */ u32 alistAddr;       // RAM address of the command list (OSTask data_ptr)
    /* 0x0C */ u32 alistSize;       // bytes (OSTask data_size)
    /* 0x10 */ const u8* header;    // AUD_TASK_HEADER_SIZE bytes for DMEM 0xFC0 (the OSTask), or NULL
} AudUcodeTask;

/** Execute one audio task: the microcode's start-up, then every command of the list. */
void AudUcode_RunTask(const AudUcodeTask* task);

/** The DMEM model (big-endian bytes, as on the RSP). It persists across tasks, like the RSP's DMEM. */
u8* AudUcode_GetDmem(void);

/** Clear DMEM, the vector/scalar register state that commands leave for later commands, and the
 *  statistics. */
void AudUcode_Reset(void);

/** Clear the statistics only. */
void AudUcode_ResetStats(void);

/** Number of commands executed per opcode since the statistics were cleared. */
u32 AudUcode_GetOpCount(u32 op);

/** Number of commands with an opcode the microcode has no handler for, and of zero-size DMAs and DMAs
 *  outside RAM, since the statistics were cleared. They are skipped. */
u32 AudUcode_GetErrorCount(void);

/*
 * Optional profiling (AUD_PROFILE builds): time spent per opcode, in clock ticks from the given
 * clock (gc_time_ticks on GameCube).
 */
#define AUD_NUM_OPS 0x20
void AudUcode_SetProfileClock(unsigned long long (*clock)(void));
unsigned long long AudUcode_GetOpTicks(u32 op);

#endif
