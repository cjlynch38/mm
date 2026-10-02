/**
 * Placeholders for the RSP microcode symbols (the incbins in rsp/) and the audio microcode's DRAM stack
 * (aspMainStack, an incbin of ROM data on N64).
 *
 * On N64 these bracket microcode blobs that the scheduler hands to the RSP. The GameCube build
 * has no RSP: the libultra shim completes tasks without running them, so the game only needs
 * distinct, aligned addresses and sensible End - Start sizes (sys_ucode.c and audio/lib/thread.c
 * compute rspboot and aspMain data sizes from them). Each blob is a zero-filled .bss array with
 * its N64 size, so every End - Start equals the N64 value. Nothing here comes from the ROM.
 */
#include "ultra64.h"
#include "PR/gs2dex.h"
#include "variables.h"

/*
 * Defines <name>Start as a zeroed, 16-byte aligned u64 array of `size` bytes and <name>End as
 * the address just past it. End is an assembler symbol so End - Start is exact regardless of how
 * the compiler and linker lay out data.
 */
#define UCODE_BLOB(name, size)                                                         \
    u64 name##Start[(size) / sizeof(u64)] ALIGNED(16);                                 \
    __asm__(".globl " #name "End\n\t.set " #name "End, " #name "Start + " #size "\n\t" \
            ".size " #name "End, 0")

// Sizes in bytes, from the N64 build (build/n64-us/mm-n64-us.elf)
UCODE_BLOB(rspbootText, 0x160);

UCODE_BLOB(aspMainText, 0x1000);
UCODE_BLOB(aspMainData, 0x2E0);

UCODE_BLOB(gspF3DZEX2_NoN_PosLight_fifoText, 0x1630);
UCODE_BLOB(gspF3DZEX2_NoN_PosLight_fifoData, 0x420);

UCODE_BLOB(gspS2DEX2_fifoText, 0x18C0);
UCODE_BLOB(gspS2DEX2_fifoData, 0x390);

// JPEG decoder ucode (z_jpeg.c declares these itself; the path is unused in MM)
UCODE_BLOB(njpgdspMainText, 0xAF0);
UCODE_BLOB(njpgdspMainData, 0x60);

// DRAM stack for the audio task (OSTask.dram_stack); declared in variables.h
STACK(aspMainStack, 0x400) ALIGNED(16);
