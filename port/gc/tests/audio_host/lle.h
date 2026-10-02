/**
 * Reference for the audio host test: MM's real audio microcode (aspMain) executed by an RSP
 * interpreter, mupen64plus-rsp-cxd4 (CC0). The interpreter is not part of the repository: the
 * Makefile builds this only when CXD4=<path to its source> is given (see the Makefile).
 */
#ifndef AUDIO_HOST_LLE_H
#define AUDIO_HOST_LLE_H

#include <stdint.h>

#define LLE_RAM_SIZE 0x1000000

/** Set up the interpreter. Returns 0 on success. */
int Lle_Init(void);

/** Zero the vector and scalar register files and the flags. */
void Lle_ResetRegs(void);

/**
 * Run the microcode at IMEM 0 (imem: 4 KiB, big-endian) as osSpTaskLoad/osSpTaskStartGo start it,
 * with the OSTask already placed at dmem[0xFC0]. ram (LLE_RAM_SIZE bytes) and dmem (4 KiB) are
 * big-endian images, updated in place. Returns the number of instructions executed, or -1 if the
 * microcode did not stop.
 */
long Lle_Run(uint8_t* ram, uint8_t* dmem, const uint8_t* imem);

#endif
