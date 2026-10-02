/**
 * Majora's Mask's audio microcode (aspMain, "nead" ABI) on the CPU.
 *
 * The RSP runs one audio task per audio update: a list of 64-bit commands (Acmd, include/PR/abi.h)
 * built by AudioSynth_Update (src/audio/lib/synthesis.c). This file executes such a list against a
 * model of the RSP's 4 KiB data memory (DMEM), with the same results as the microcode itself.
 *
 * Every command is implemented from MM's own microcode (extracted/n64-us/incbin/aspMainText,
 * loaded at IMEM 0x000 by osSpTaskLoad, disassembled with rabbitizer), not from the HLE emulators,
 * which differ from it in several places. What that means in practice:
 *   - Commands decode their fields like the microcode (for example LOADBUFF's size is bits 16-23 of
 *     w0 times 16, ENVMIXER's buffers are 8-bit fields times 16).
 *   - They work on whole vectors, so counts round up: CLEARBUFF and S8DEC to 16 bytes, MIXER and
 *     ADPCM to 32, ADDMIXER and the opcode 0 multiply to 64, ENVMIXER to 16 samples, INTERL and
 *     RESAMPLE to 8 samples, DMEMMOVE to 2 bytes. Most loops are do-while loops, so a count of 0
 *     still does one pass (DMEMMOVE of 0 bytes moves 2).
 *   - Every product is rounded and saturated as the vector unit does it (vmulf/vmacf/vmudm/vmadh
 *     semantics), including where the HLE emulators simplify: RESAMPLE rounds each of its 4
 *     products and saturates the partial sums, MIXER scales the destination by 0x7FFF/0x8000,
 *     FILTER averages the old and new coefficients with rounding.
 *   - DMEM scratch areas are used as the microcode uses them (the codebook at 0x330, the state at
 *     0xFB0, RESAMPLE's address tables at 0xFD0, ...), DMAs ignore the low 3 address bits and
 *     transfer multiples of 8 bytes, and RAM state blocks are written whole (32 bytes).
 *   - The order of loads and stores inside each loop is kept where buffers can overlap, and the
 *     vector unit state that commands leave behind is modelled where a later command depends on it:
 *     the ENVSETUP volumes and ramps, FILTER's count, and v31, whose sign bits ADDMIXER adds to its
 *     first 8 samples (a microcode quirk).
 * Unaligned DMEM operands of the 16-byte vector loads/stores (lqv/sqv) are handled byte-exactly,
 * except that the bytes such a load leaves untouched in a vector register come from this model's
 * own copy of that register rather than from the RSP's register file. MM only uses 16-byte aligned
 * buffers there.
 *
 * Opcode table of MM's aspMain (DMEM 0x10, from aspMainData):
 *    0  (A_SPNOOP in abi.h) multiplies a DMEM buffer by 32 samples of another one; MM's UnkCmd19
 *    1  ADPCM        2  CLEARBUFF     3  no-op         4  ADDMIXER      5  RESAMPLE
 *    6  RESAMPLE_ZOH 7  FILTER        8  SETBUFF       9  DUPLICATE    10  DMEMMOVE
 *   11  LOADADPCM   12  MIXER        13  INTERLEAVE   14  HILOGAIN     15  SETLOOP
 *   16  copy blocks (not in abi.h)   17  INTERL       18  ENVSETUP1    19  ENVMIXER
 *   20  LOADBUFF    21  SAVEBUFF     22  ENVSETUP2    23  S8DEC
 * Higher opcodes jump through whatever follows the table; they are skipped and counted as errors.
 *
 * The host test (port/gc/tests/audio_host) checks all of this against the real microcode run on an
 * RSP interpreter: DMEM and RAM match bit for bit on random instances of every command and on
 * MM-like synthesis lists.
 *
 * Byte order: DMEM and RAM hold big-endian data, as on N64. The GameCube is big-endian too, so the
 * BE16 conversions vanish there; they only swap in the x86 host tests.
 *
 * Performance: the hot commands (ADPCM, RESAMPLE, ENVMIXER, MIXER, ADDMIXER, FILTER, INTERL,
 * INTERLEAVE, DMEMMOVE) have a straight loop over the samples, used when their buffers do not
 * overlap (always, for MM's lists), and keep the exact vector-by-vector version for the rest. The
 * straight loops take exact shortcuts: ADPCM frames are unrolled, ENVMIXER skips the wet half for
 * notes without reverb and all of it for silent ones, RESAMPLE drops the intermediate saturations
 * the microcode's own table can never reach. Paired-single maths does not fit: a 16 x 16-bit product
 * needs 31 bits, more than a single-precision mantissa, so results would not be exact.
 */
#include "aud_ucode.h"
#include "PR/abi.h"
#include "stdbool.h"

#define DMEM_MASK 0xFFF

// DMEM layout of the microcode
#define DMEM_CONST 0x000         // 8 constants {0, 1, 2, -1, 0x20, 0x800, 0x7FFF, 0x4000}
#define DMEM_RESAMPLE_ONES 0x070 // {1 x 8}
#define DMEM_RESAMPLE_LUT 0x0E0  // 64 rows of 4 interpolation coefficients
#define DMEM_RESAMPLE_LUT_SIZE 0x200
#define DMEM_RESAMPLE_LUT_MAGIC 0x0C39 // first coefficient: RESAMPLE reloads the table if it differs
#define DMEM_BUF_IN 0x2E0              // SETBUFF
#define DMEM_BUF_OUT 0x2E2
#define DMEM_BUF_COUNT 0x2E4
#define DMEM_LOOP_ADDR 0x2E8  // SETLOOP
#define DMEM_DRAM_STACK 0x2EC // OSTask dram_stack
#define DMEM_ALIST 0x2F0      // command list buffer
#define DMEM_ALIST_SIZE 0x40
#define DMEM_ADPCM_BOOK 0x330 // LOADADPCM
#define DMEM_STATE 0xFB0      // RESAMPLE and FILTER state (32 bytes)
#define DMEM_RESAMPLE_FRAC 0xFB8
#define DMEM_RESAMPLE_ADDRS 0xFD0 // 8 sample addresses, then 8 LUT row addresses
#define DMEM_FILTER_TAPS 0xFD0    // zeros, coefficients, zeros
#define DMEM_FILTER_COEFS 0xFE0

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define BE16(x) ((u16)__builtin_bswap16((u16)(x)))
#else
#define BE16(x) ((u16)(x))
#endif

#define ALIGN16_COUNT(n) (((n) + 15) & ~15)

// The host tests can turn the straight-loop fast paths off, to check them against the exact versions
#ifdef AUD_HOST_TEST
int gAudForceExact;
#define FAST_PATH_OK (!gAudForceExact)
#else
#define FAST_PATH_OK true
#endif

/*
 * Vector unit and scalar register state that commands leave for later commands
 */
typedef struct {
    /* 0x00 */ u16 env[8];      // $v1 after ENVSETUP1/2: volume L, L + ramp, R, R + ramp, reverb, reverb + ramp
    /* 0x10 */ u32 envRampL;    // $21
    /* 0x14 */ u32 envRampR;    // $22
    /* 0x18 */ u32 envRampRev;  // $11
    /* 0x1C */ u32 filterCount; // $15 after FILTER with flags >= 2
    /* 0x20 */ s16 v31[8];      // $v31 (ADDMIXER doubles it and adds the carries to its first vector)
} AudRegs;

static u8 sDmem[AUD_DMEM_SIZE] __attribute__((aligned(32)));
static AudRegs sRegs;
static u32 sOpCount[AUD_NUM_OPS];
static u32 sErrorCount;
static unsigned long long (*sProfileClock)(void);
static unsigned long long sOpTicks[AUD_NUM_OPS];
// A copy of the resample table in the microcode's data; sLutSafe points to it when it allows
// RESAMPLE's shortcut (Aud_CheckLut), else NULL
static u8 sLutCopy[DMEM_RESAMPLE_LUT_SIZE];
static const u8* sLutSource;
static const u8* sLutSafe;

/* ------------------------------------------------------------------------------------------------ */
/* DMEM access                                                                                      */
/* ------------------------------------------------------------------------------------------------ */

/* Scalar unit loads and stores: any address, wrapping at the end of DMEM */

static inline u32 Dmem_Lbu(u32 addr) {
    return sDmem[addr & DMEM_MASK];
}

static inline u32 Dmem_Lhu(u32 addr) {
    return (Dmem_Lbu(addr) << 8) | Dmem_Lbu(addr + 1);
}

static inline s32 Dmem_Lh(u32 addr) {
    return (s16)Dmem_Lhu(addr);
}

static inline u32 Dmem_Lw(u32 addr) {
    return (Dmem_Lhu(addr) << 16) | Dmem_Lhu(addr + 2);
}

static inline void Dmem_Sb(u32 addr, u32 val) {
    sDmem[addr & DMEM_MASK] = val;
}

static inline void Dmem_Sh(u32 addr, u32 val) {
    Dmem_Sb(addr, val >> 8);
    Dmem_Sb(addr + 1, val);
}

static inline void Dmem_Sw(u32 addr, u32 val) {
    Dmem_Sh(addr, val >> 16);
    Dmem_Sh(addr + 2, val);
}

/* 16-bit lanes at even addresses below DMEM_SIZE (the fast paths) */

static inline s32 Dmem_Get16(u32 addr) {
    return (s16)BE16(*(u16*)&sDmem[addr]);
}

static inline void Dmem_Set16(u32 addr, s32 val) {
    *(u16*)&sDmem[addr] = BE16(val);
}

static inline s16* Dmem_Ptr16(u32 addr) {
    return (s16*)&sDmem[addr];
}

static inline s32 Ptr_Get16(const s16* p) {
    return (s16)BE16(*p);
}

static inline void Ptr_Set16(s16* p, s32 val) {
    *p = BE16(val);
}

/* Vector register bytes (lane i / 2, high byte first) */

static inline u32 Vec_GetByte(const s16* v, u32 i) {
    u32 lane = (u16)v[(i >> 1) & 7];

    return (i & 1) ? (lane & 0xFF) : (lane >> 8);
}

static inline void Vec_SetByte(s16* v, u32 i, u32 byte) {
    u32 lane = (u16)v[(i >> 1) & 7];

    lane = (i & 1) ? ((lane & 0xFF00) | (byte & 0xFF)) : ((lane & 0x00FF) | ((byte & 0xFF) << 8));
    v[(i >> 1) & 7] = lane;
}

/** lqv vt[0]: bytes from addr to the end of its 16-byte line, into register bytes 0.. */
static void Vec_Lqv(s16* v, u32 addr) {
    u32 i;
    u32 n;

    addr &= DMEM_MASK;
    if ((addr & 0xF) == 0) {
        for (i = 0; i < 8; i++) {
            v[i] = Dmem_Get16(addr + 2 * i);
        }
    } else {
        n = 16 - (addr & 0xF);
        for (i = 0; i < n; i++) {
            Vec_SetByte(v, i, sDmem[addr + i]);
        }
    }
}

/** sqv vt[0]: register bytes 0.. to addr, up to the end of its 16-byte line */
static void Vec_Sqv(const s16* v, u32 addr) {
    u32 i;
    u32 n;

    addr &= DMEM_MASK;
    if ((addr & 0xF) == 0) {
        for (i = 0; i < 8; i++) {
            Dmem_Set16(addr + 2 * i, v[i]);
        }
    } else {
        n = 16 - (addr & 0xF);
        for (i = 0; i < n; i++) {
            sDmem[addr + i] = Vec_GetByte(v, i);
        }
    }
}

/** lsv/llv/ldv vt[e]: `n` bytes (2, 4 or 8) from any address into register bytes e.. */
static void Vec_LoadBytes(s16* v, u32 e, u32 addr, u32 n) {
    u32 i;

    addr &= DMEM_MASK;
    if (((addr & 1) == 0) && ((e & 1) == 0) && (addr + n <= AUD_DMEM_SIZE)) {
        for (i = 0; i < n; i += 2) {
            v[((e + i) >> 1) & 7] = Dmem_Get16(addr + i);
        }
    } else {
        for (i = 0; i < n; i++) {
            Vec_SetByte(v, e + i, sDmem[(addr + i) & DMEM_MASK]);
        }
    }
}

/** ssv/slv/sdv vt[e]: register bytes e.. (`n` of them) to any address */
static void Vec_StoreBytes(const s16* v, u32 e, u32 addr, u32 n) {
    u32 i;

    addr &= DMEM_MASK;
    if (((addr & 1) == 0) && ((e & 1) == 0) && (addr + n <= AUD_DMEM_SIZE)) {
        for (i = 0; i < n; i += 2) {
            Dmem_Set16(addr + i, v[((e + i) >> 1) & 7]);
        }
    } else {
        for (i = 0; i < n; i++) {
            sDmem[(addr + i) & DMEM_MASK] = Vec_GetByte(v, e + i);
        }
    }
}

/** lpv vt[0]: 8 bytes from any address, each into the high byte of a lane */
static void Vec_Lpv(s16* v, u32 addr) {
    u32 i;

    for (i = 0; i < 8; i++) {
        v[i] = (s16)(Dmem_Lbu(addr + i) << 8);
    }
}

/** Is [addr, addr + size) inside DMEM without wrapping, and `align`-aligned? */
static inline s32 Dmem_Fits(u32 addr, u32 size, u32 align) {
    return ((addr & (align - 1)) == 0) && (addr < AUD_DMEM_SIZE) && (size <= AUD_DMEM_SIZE - addr);
}

/** Do [a, a + aSize) and [b, b + bSize) (no wrapping) overlap? */
static inline s32 Dmem_Overlap(u32 a, u32 aSize, u32 b, u32 bSize) {
    return (a < b + bSize) && (b < a + aSize);
}

/* ------------------------------------------------------------------------------------------------ */
/* Arithmetic                                                                                       */
/* ------------------------------------------------------------------------------------------------ */

/** Saturate to s16 (one compare: out of range iff x + 0x8000 is not in 0..0xFFFF) */
static inline s32 Sat16(s32 x) {
    if ((u32)x + 0x8000 > 0xFFFF) {
        x = (x >> 31) ^ 0x7FFF;
    }
    return x;
}

/** vmulf: (a * b * 2 + 0x8000) >> 16, saturated (only -0x8000 * -0x8000 saturates) */
static inline s32 Vmulf(s32 a, s32 b) {
    s32 r = (a * b + 0x4000) >> 15;

    return (r > 0x7FFF) ? 0x7FFF : r;
}

/* ------------------------------------------------------------------------------------------------ */
/* DMA                                                                                              */
/* ------------------------------------------------------------------------------------------------ */

/**
 * SP DMA between DMEM and RAM, as the microcode programs it: `lenReg` is the value written to
 * SP_RD_LEN/SP_WR_LEN (bytes - 1). The RSP ignores the low 3 bits of both addresses and transfers
 * whole 8-byte units; DMEM addresses wrap at 4 KiB. The microcode writes size - 1 even for a size of
 * 0, which asks for 256 lines of 4 KiB with a skip: such requests are skipped (MM never makes them),
 * and so are transfers outside GameCube RAM (see AUD_RAM_PTR).
 */
static void Aud_Dma(u32 dmemAddr, u32 dramAddr, u32 lenReg, s32 write) {
    u32 len;
    u32 first;
    u8* ram;

    if (lenReg > 0xFFF) {
        sErrorCount++;
        return;
    }

    len = (lenReg | 7) + 1;
    dmemAddr &= 0xFF8;
    ram = AUD_RAM_PTR(dramAddr & ~7, len);
    if (ram == NULL) {
        sErrorCount++;
        return;
    }
    first = AUD_DMEM_SIZE - dmemAddr;
    if (first > len) {
        first = len;
    }

    if (write) {
        __builtin_memcpy(ram, &sDmem[dmemAddr], first);
        if (len > first) {
            __builtin_memcpy(ram + first, &sDmem[0], len - first);
        }
    } else {
        __builtin_memcpy(&sDmem[dmemAddr], ram, first);
        if (len > first) {
            __builtin_memcpy(&sDmem[0], ram + first, len - first);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Simple commands                                                                                  */
/* ------------------------------------------------------------------------------------------------ */

/** CLEARBUFF: zero `count` bytes rounded up to 16 (nothing if 0), at any address */
static void Aud_ClearBuff(u32 w0, u32 w1) {
    u32 dmem = w0 & 0xFFFF;
    u32 size = ALIGN16_COUNT(w1 & 0xFFFF);
    u32 i;

    dmem &= DMEM_MASK;
    if (size <= AUD_DMEM_SIZE - dmem) {
        __builtin_memset(&sDmem[dmem], 0, size);
    } else {
        for (i = 0; i < size; i++) {
            sDmem[(dmem + i) & DMEM_MASK] = 0;
        }
    }
}

/** SETBUFF: in, out and count for ADPCM, S8DEC, RESAMPLE and RESAMPLE_ZOH, kept in DMEM */
static void Aud_SetBuff(u32 w0, u32 w1) {
    Dmem_Sh(DMEM_BUF_IN, w0);
    Dmem_Sh(DMEM_BUF_OUT, w1 >> 16);
    Dmem_Sh(DMEM_BUF_COUNT, w1);
}

/** SETLOOP: RAM address of the loop start state for ADPCM/S8DEC with A_LOOP */
static void Aud_SetLoop(u32 w0, u32 w1) {
    Dmem_Sw(DMEM_LOOP_ADDR, w1 & AUD_ADDR_MASK);
}

/** LOADBUFF / SAVEBUFF: size is bits 16-23 of w0 times 16 */
static void Aud_LoadBuff(u32 w0, u32 w1) {
    Aud_Dma(w0 & 0xFFFF, w1 & AUD_ADDR_MASK, ((w0 >> 12) & 0xFF0) - 1, false);
}

static void Aud_SaveBuff(u32 w0, u32 w1) {
    Aud_Dma(w0 & 0xFFFF, w1 & AUD_ADDR_MASK, ((w0 >> 12) & 0xFF0) - 1, true);
}

/** LOADADPCM: the codebook (count bytes) to DMEM 0x330 */
static void Aud_LoadAdpcm(u32 w0, u32 w1) {
    Aud_Dma(DMEM_ADPCM_BOOK, w1 & AUD_ADDR_MASK, (w0 & 0xFFFF) - 1, false);
}

/**
 * DMEMMOVE: 16-byte blocks (each read whole, then written) while 16 or more bytes remain, then
 * halfwords, so odd counts round up to even. Any addresses. A count below 16 branches straight into
 * the halfword loop, past its check for nothing left, so it always moves at least 2 bytes, even for
 * a count of 0.
 */
static void Aud_DmemMove(u32 w0, u32 w1) {
    s32 count = w1 & 0xFFFF;
    u32 src = (w0 & 0xFFFF) & DMEM_MASK;
    u32 dst = (w1 >> 16) & DMEM_MASK;
    u32 size = (count < 2) ? 2 : ((count + 1) & ~1);
    s16 v1[8];
    s16 v2[8];

    // A forward copy with dst <= src, or between disjoint buffers, is a memmove
    if (FAST_PATH_OK && (src + size <= AUD_DMEM_SIZE) && (dst + size <= AUD_DMEM_SIZE) &&
        ((dst <= src) || (dst >= src + size))) {
        __builtin_memmove(&sDmem[dst], &sDmem[src], size);
        return;
    }

    if (count >= 16) {
        do {
            Vec_LoadBytes(v1, 0, src, 8);
            Vec_LoadBytes(v2, 0, src + 8, 8);
            count -= 16;
            src += 16;
            Vec_StoreBytes(v1, 0, dst, 8);
            Vec_StoreBytes(v2, 0, dst + 8, 8);
            dst += 16;
        } while (count >= 16);
        if (count == 0) {
            return;
        }
    }
    do {
        Vec_LoadBytes(v1, 0, src, 2);
        count -= 2;
        src += 2;
        Vec_StoreBytes(v1, 0, dst, 2);
        dst += 2;
    } while (count > 0);
}

/** INTERLEAVE: count (bytes per channel) rounded up to 8, 4 samples of each side per step */
static void Aud_Interleave(u32 w0, u32 w1) {
    s32 count = (w0 >> 12) & 0xFF0;
    u32 out = w0 & 0xFFFF;
    u32 left = w1 >> 16;
    u32 right = w1 & 0xFFFF;
    u32 size = (count > 8) ? ((count + 7) & ~7) : 8; // bytes per side
    s16 l[8];
    s16 r[8];
    s16 lr[8];
    u32 i;

    if (FAST_PATH_OK && Dmem_Fits(out, 2 * size, 2) && Dmem_Fits(left, size, 2) && Dmem_Fits(right, size, 2) &&
        !Dmem_Overlap(out, 2 * size, left, size) && !Dmem_Overlap(out, 2 * size, right, size)) {
        s16* po = Dmem_Ptr16(out);
        const s16* pl = Dmem_Ptr16(left);
        const s16* pr = Dmem_Ptr16(right);

        for (i = 0; i < size / 2; i++) {
            po[2 * i] = pl[i];
            po[2 * i + 1] = pr[i];
        }
        return;
    }

    do {
        Vec_LoadBytes(l, 0, left, 8);
        Vec_LoadBytes(r, 0, right, 8);
        count -= 8;
        for (i = 0; i < 4; i++) {
            lr[2 * i] = l[i];
            lr[2 * i + 1] = r[i];
        }
        Vec_StoreBytes(lr, 0, out, 8);
        Vec_StoreBytes(lr, 8, out + 8, 8);
        out += 16;
        left += 8;
        right += 8;
    } while (count > 0);
}

/** INTERL: every other sample, count (output samples) rounded up to 8 */
static void Aud_Interl(u32 w0, u32 w1) {
    s32 count = w0 & 0xFFFF;
    u32 out = w1 & 0xFFFF;
    u32 in = w1 >> 16;
    u32 n = (count > 8) ? ((count + 7) & ~7) : 8; // output samples
    s16 v[8];
    u32 i;

    // Forward, one sample at a time, is the same as the vector loop when out is at or below in (in place,
    // as MM halves its reverb) or the buffers are apart
    if (FAST_PATH_OK && Dmem_Fits(in, 4 * n, 2) && Dmem_Fits(out, 2 * n, 2) &&
        ((out <= in) || !Dmem_Overlap(out, 2 * n, in, 4 * n))) {
        s16* po = Dmem_Ptr16(out);
        const s16* pi = Dmem_Ptr16(in);

        for (i = 0; i < n; i++) {
            po[i] = pi[2 * i];
        }
        return;
    }

    do {
        for (i = 0; i < 8; i++) {
            Vec_LoadBytes(v, 2 * i, in + 4 * i, 2);
        }
        in += 32;
        count -= 8;
        Vec_StoreBytes(v, 0, out, 8);
        Vec_StoreBytes(v, 8, out + 8, 8);
        out += 16;
    } while (count > 0);
}

/** DUPLICATE: 128 bytes from src, stored `count` times (at least once) from dst on */
static void Aud_Duplicate(u32 w0, u32 w1) {
    s32 count = (w0 >> 16) & 0xFF;
    u32 src = w0 & 0xFFFF;
    u32 dst = w1 >> 16;
    s16 v[8][8];
    u32 i;

    __builtin_memset(v, 0, sizeof(v));
    for (i = 0; i < 8; i++) {
        Vec_Lqv(v[i], src + 16 * i);
    }
    do {
        count--;
        for (i = 0; i < 8; i++) {
            Vec_Sqv(v[i], dst + 16 * i);
        }
        dst += 0x80;
    } while (count > 0);
}

/**
 * Opcode 16 (not in MM's abi.h): `count` (at least 1) blocks of `size` bytes rounded up to 32, copied
 * as one run from src to dst, 32 bytes at a time.
 */
static void Aud_CopyBlocks(u32 w0, u32 w1) {
    s32 count = (w0 >> 16) & 0xFF;
    u32 src = w0 & 0xFFFF;
    u32 dst = w1 >> 16;
    s32 size;
    s16 v1[8];
    s16 v2[8];

    __builtin_memset(v1, 0, sizeof(v1));
    __builtin_memset(v2, 0, sizeof(v2));
    do {
        count--;
        size = w1 & 0xFFFF;
        do {
            Vec_Lqv(v1, src);
            Vec_Lqv(v2, src + 16);
            size -= 32;
            src += 32;
            Vec_Sqv(v1, dst);
            Vec_Sqv(v2, dst + 16);
            dst += 32;
        } while (size > 0);
    } while (count > 0);
}

/** HILOGAIN: samples times a UQ4.4 gain (bits 16-23 of w0), in place; count rounded up to 32 bytes */
static void Aud_HiLoGain(u32 w0, u32 w1) {
    s32 count = w0 & 0xFFFF;
    u32 dmem = w1 >> 16;
    s32 gain = (w0 >> 16) & 0xFF;
    u32 size = (count > 32) ? ((count + 31) & ~31) : 32;
    s16 v[16];
    s16* p;
    u32 i;

    if (FAST_PATH_OK && Dmem_Fits(dmem, size, 16)) {
        // y = (x * frac << 12 + (x * int) << 16) >> 16 = (x * gain) >> 4, saturated
        p = Dmem_Ptr16(dmem);
        do {
            for (i = 0; i < 16; i++) {
                Ptr_Set16(&p[i], Sat16((Ptr_Get16(&p[i]) * gain) >> 4));
            }
            p += 16;
            count -= 32;
        } while (count > 0);
        return;
    }

    __builtin_memset(v, 0, sizeof(v));
    do {
        Vec_Lqv(&v[0], dmem);
        Vec_Lqv(&v[8], dmem + 16);
        for (i = 0; i < 16; i++) {
            v[i] = Sat16((v[i] * gain) >> 4);
        }
        Vec_Sqv(&v[0], dmem);
        Vec_Sqv(&v[8], dmem + 16);
        count -= 32;
        dmem += 32;
    } while (count > 0);
}

/**
 * Opcode 0, which MM's abi.h calls A_SPNOOP (AudioSynth_UnkCmd19 sends it for samples with
 * bookOffset 3): multiplies a DMEM buffer, in 64-byte blocks, by the 32 samples at
 * (w1 & 0xFFFF) + bits 16-23 of w0, with the product saturated to 16 bits (vmudh).
 */
static void Aud_MultTable(u32 w0, u32 w1) {
    s32 count = w0 & 0xFFFF;
    u32 dmem = w1 >> 16;
    u32 table = (w1 & 0xFFFF) + ((w0 >> 16) & 0xFF);
    s16 t[32];
    s16 v[32];
    u32 i;

    for (i = 0; i < 4; i++) {
        Vec_LoadBytes(&t[8 * i], 0, table + 0x10 * i, 8);
        Vec_LoadBytes(&t[8 * i], 8, table + 0x10 * i + 8, 8);
    }
    __builtin_memset(v, 0, sizeof(v));
    do {
        for (i = 0; i < 4; i++) {
            Vec_Lqv(&v[8 * i], dmem + 16 * i);
        }
        for (i = 0; i < 32; i++) {
            v[i] = Sat16(v[i] * t[i]);
        }
        for (i = 0; i < 4; i++) {
            Vec_Sqv(&v[8 * i], dmem + 16 * i);
        }
        count -= 0x40;
        dmem += 0x40;
    } while (count > 0);
}

/* ------------------------------------------------------------------------------------------------ */
/* ADPCM and 8-bit decoding                                                                         */
/* ------------------------------------------------------------------------------------------------ */

/**
 * 8 samples of one half of an ADPCM frame: o[i] = (b1[i] * l1 + b2[i] * l2 + sum over j < i of
 * b2[i - 1 - j] * x[j] + x[i] << 11) >> 11, saturated, accumulated in 32 bits (wrapping like the
 * vector accumulator's 32 bits that the microcode keeps). l1, l2 are the two samples before the half.
 */
static inline __attribute__((always_inline)) void Aud_AdpcmHalf(s32* o, const s32* b1, const s32* b2, const s32* x,
                                                                s32 l1, s32 l2) {
    u32 acc;
    s32 i;
    s32 j;

#pragma GCC unroll 8
    for (i = 0; i < 8; i++) {
        acc = (u32)(b1[i] * l1) + (u32)(b2[i] * l2) + ((u32)x[i] << 11);
#pragma GCC unroll 8
        for (j = 0; j < i; j++) {
            acc += (u32)(b2[i - 1 - j] * x[j]);
        }
        o[i] = Sat16((s32)acc >> 11);
    }
}

/**
 * ADPCM: decodes frames of 16 samples (9 bytes, or 5 with A_ADPCM_SHORT) from in to out. The output
 * starts with the 16 samples of the state (zeros with A_INIT, the SETLOOP state with A_LOOP), and the
 * last 16 samples written are saved to the state address (w1). count (bytes of output after the state)
 * rounds up to 32; 0 decodes nothing.
 *
 * Each sample is the predictor's dot product with the two samples before the frame and the frame's
 * earlier samples (the second half uses the first half's samples 6 and 7, after rounding), plus the
 * residual << 11, all accumulated in 32 bits and then shifted right by 11 and saturated. The microcode
 * reads each frame's header, data and codebook entry before storing the previous frame.
 */
static void Aud_Adpcm(u32 w0, u32 w1) {
    u32 flags = (w0 >> 16) & 0xFF;
    u32 in = Dmem_Lhu(DMEM_BUF_IN);
    u32 out = Dmem_Lhu(DMEM_BUF_OUT);
    s32 count = Dmem_Lhu(DMEM_BUF_COUNT);
    u32 stateAddr = w1 & AUD_ADDR_MASK;
    s32 shortMode = flags & A_ADPCM_SHORT;
    u32 frameSize = shortMode ? 5 : 9;
    s32 shiftBase = shortMode ? 14 : 12;
    s16 zero[8];
    s16 prev[8];
    s16 data[8];
    s16 nextData[8];
    s16 book[16];
    s16 nextBook[16];
    s16 x[16];
    s16 o[16];
    u32 header;
    u32 nextHeader;
    u32 entry;
    s32 shift;
    u32 nibbles;
    u32 acc;
    u32 nframes;
    u32 inSize;
    s32 i;
    s32 j;

    Vec_Lqv(sRegs.v31, DMEM_CONST);
    __builtin_memset(zero, 0, sizeof(zero));
    __builtin_memset(prev, 0, sizeof(prev));
    __builtin_memset(data, 0, sizeof(data));
    __builtin_memset(nextData, 0, sizeof(nextData));

    Vec_Sqv(zero, out);
    Vec_Sqv(zero, out + 16);
    if (!(flags & A_INIT)) {
        Aud_Dma(out, (flags & A_LOOP) ? Dmem_Lw(DMEM_LOOP_ADDR) : stateAddr, 0x1F, false);
    }
    Vec_Lqv(prev, out + 16);
    out += 32;

    nframes = (count + 31) >> 5;
    inSize = (nframes - 1) * frameSize + 9;
    if (FAST_PATH_OK && (count != 0) && ((out & 1) == 0) && (out + nframes * 32 <= AUD_DMEM_SIZE) &&
        (in + inSize <= AUD_DMEM_SIZE) && !Dmem_Overlap(out, nframes * 32, in, inSize) &&
        !Dmem_Overlap(out, nframes * 32, DMEM_ADPCM_BOOK, 16 * 32)) {
        // The output overlaps neither the input nor the codebook: straight through, frame by frame
        const u8* src = &sDmem[in];
        s16* dst = Dmem_Ptr16(out);
        const s16* bk;
        s32 b1[8];
        s32 b2[8];
        s32 r[16];
        s32 res[16];
        s32 l1 = prev[6];
        s32 l2 = prev[7];
        u32 d;

        do {
            header = src[0];
            bk = Dmem_Ptr16(DMEM_ADPCM_BOOK + (header & 0xF) * 32);
            for (i = 0; i < 8; i++) {
                b1[i] = Ptr_Get16(&bk[i]);
                b2[i] = Ptr_Get16(&bk[8 + i]);
            }
            // Residual << 12 (<< 14) as s16, then >> (12 - scale) (>> (14 - scale)) if positive
            shift = shiftBase - (s32)(header >> 4);
            shift = 16 + ((shift > 0) ? shift : 0);
            if (shortMode) {
                for (i = 0; i < 4; i++) {
                    d = src[1 + i];
                    r[4 * i + 0] = (s32)((d & 0xC0) << 24) >> shift;
                    r[4 * i + 1] = (s32)((d & 0x30) << 26) >> shift;
                    r[4 * i + 2] = (s32)((d & 0x0C) << 28) >> shift;
                    r[4 * i + 3] = (s32)(d << 30) >> shift;
                }
            } else {
                for (i = 0; i < 8; i++) {
                    d = src[1 + i];
                    r[2 * i + 0] = (s32)((d & 0xF0) << 24) >> shift;
                    r[2 * i + 1] = (s32)(d << 28) >> shift;
                }
            }
            Aud_AdpcmHalf(&res[0], b1, b2, &r[0], l1, l2);
            Aud_AdpcmHalf(&res[8], b1, b2, &r[8], res[6], res[7]);
            for (i = 0; i < 16; i++) {
                Ptr_Set16(&dst[i], res[i]);
            }
            l1 = res[14];
            l2 = res[15];
            src += frameSize;
            dst += 16;
            count -= 32;
        } while (count > 0);
        out += nframes * 32;
    } else if (count != 0) {
        header = Dmem_Lbu(in);
        Vec_LoadBytes(data, 0, in + 1, 8);
        entry = DMEM_ADPCM_BOOK + (header & 0xF) * 32;
        Vec_Lqv(&book[0], entry);
        Vec_Lqv(&book[8], entry + 16);

        do {
            in += frameSize;

            // Residuals, moved to the top bits and scaled by 2^scale (vmudm with 0x8000 >> (k - 1))
            shift = shiftBase - (s32)(header >> 4);
            for (i = 0; i < 16; i++) {
                if (shortMode) {
                    nibbles = (u16)data[i >> 3];
                    x[i] = (s16)(((nibbles >> (14 - 2 * (i & 7))) & 3) << 14);
                } else {
                    nibbles = (u16)data[i >> 2];
                    x[i] = (s16)(((nibbles >> (12 - 4 * (i & 3))) & 0xF) << 12);
                }
                if (shift > 0) {
                    x[i] >>= shift;
                }
            }

            nextHeader = Dmem_Lbu(in);
            Vec_LoadBytes(nextData, 0, in + 1, 8);

            for (i = 0; i < 8; i++) {
                acc = (u32)(book[i] * prev[6]) + (u32)(book[8 + i] * prev[7]) + ((u32)x[i] << 11);
                for (j = 0; j < i; j++) {
                    acc += (u32)(book[8 + i - 1 - j] * x[j]);
                }
                o[i] = Sat16((s32)acc >> 11);
            }
            for (i = 0; i < 8; i++) {
                acc = (u32)(book[i] * o[6]) + (u32)(book[8 + i] * o[7]) + ((u32)x[8 + i] << 11);
                for (j = 0; j < i; j++) {
                    acc += (u32)(book[8 + i - 1 - j] * x[8 + j]);
                }
                o[8 + i] = Sat16((s32)acc >> 11);
            }

            entry = DMEM_ADPCM_BOOK + (nextHeader & 0xF) * 32;
            Vec_Lqv(&nextBook[0], entry);
            Vec_Lqv(&nextBook[8], entry + 16);

            count -= 32;
            Vec_StoreBytes(o, 0, out, 8);
            Vec_StoreBytes(o, 8, out + 8, 8);
            Vec_StoreBytes(&o[8], 0, out + 16, 8);
            Vec_StoreBytes(&o[8], 8, out + 24, 8);
            out += 32;

            for (i = 0; i < 8; i++) {
                prev[i] = o[8 + i];
                data[i] = nextData[i];
            }
            for (i = 0; i < 16; i++) {
                book[i] = nextBook[i];
            }
            header = nextHeader;
        } while (count > 0);
    }

    Aud_Dma(out - 32, stateAddr, 0x1F, true);
}

/**
 * S8DEC: 8-bit samples (16 per 16 bytes of input) to 16-bit (byte << 8), with the same state handling
 * as ADPCM. count rounds up to 32 bytes of output; 0 decodes nothing.
 */
static void Aud_S8Dec(u32 w0, u32 w1) {
    u32 flags = (w0 >> 16) & 0xFF;
    u32 in = Dmem_Lhu(DMEM_BUF_IN);
    u32 out = Dmem_Lhu(DMEM_BUF_OUT);
    s32 count = Dmem_Lhu(DMEM_BUF_COUNT);
    u32 stateAddr = w1 & AUD_ADDR_MASK;
    s16 zero[8];
    s16 lo[8];
    s16 hi[8];
    s16 nextLo[8];
    s16 nextHi[8];

    __builtin_memset(zero, 0, sizeof(zero));
    Vec_Sqv(zero, out);
    Vec_Sqv(zero, out + 16);
    if (!(flags & A_INIT)) {
        Aud_Dma(out, (flags & A_LOOP) ? Dmem_Lw(DMEM_LOOP_ADDR) : stateAddr, 0x1F, false);
    }
    out += 32;

    if (count != 0) {
        Vec_Lpv(lo, in);
        Vec_Lpv(hi, in + 8);
        while (true) {
            in += 16;
            count -= 32;
            Vec_Lpv(nextLo, in);
            Vec_Sqv(lo, out);
            Vec_Lpv(nextHi, in + 8);
            Vec_Sqv(hi, out + 16);
            if (count <= 0) {
                break;
            }
            out += 32;
            __builtin_memcpy(lo, nextLo, sizeof(lo));
            __builtin_memcpy(hi, nextHi, sizeof(hi));
        }
        out += 32;
    }

    Aud_Dma(out - 32, stateAddr, 0x1F, true);
}

/* ------------------------------------------------------------------------------------------------ */
/* Resampling                                                                                       */
/* ------------------------------------------------------------------------------------------------ */

/** One RESAMPLE output: the 4 samples at sampleAddr times LUT row lutAddr, each product rounded */
static inline s32 Aud_ResampleTap(u32 sampleAddr, u32 lutAddr) {
    s16 s[4];
    s16 c[4];

    Vec_LoadBytes(s, 0, sampleAddr, 8);
    Vec_LoadBytes(c, 0, lutAddr, 8);
    return Sat16(Sat16(Vmulf(s[0], c[0]) + Vmulf(s[1], c[1])) + Sat16(Vmulf(s[2], c[2]) + Vmulf(s[3], c[3])));
}

/**
 * RESAMPLE's straight loop over `n` outputs from position pos (16.16 samples from in), returning the
 * position after them. With safeLut, the table is one whose products cannot saturate (no -0x8000) and
 * whose rows have |l0| + |l1| and |l2| + |l3| at most 0x7FFF, so the partial sums cannot either (each
 * rounded product is at most |l| in size): only the final sum needs saturating.
 */
static inline __attribute__((always_inline)) u32 Aud_ResampleLoop(s16* po, s32 in, u32 pos, u32 step, s32 n,
                                                                  const s32 safeLut) {
    const s16* lut = Dmem_Ptr16(DMEM_RESAMPLE_LUT);
    const s16* s;
    const s16* c;
    s32 k;

    for (k = 0; k < n; k++) {
        s = Dmem_Ptr16(in + 2 * (pos >> 16));
        c = &lut[(pos >> 10 & 0x3F) * 4];
        if (safeLut) {
            Ptr_Set16(&po[k], Sat16(((Ptr_Get16(&s[0]) * Ptr_Get16(&c[0]) + 0x4000) >> 15) +
                                    ((Ptr_Get16(&s[1]) * Ptr_Get16(&c[1]) + 0x4000) >> 15) +
                                    ((Ptr_Get16(&s[2]) * Ptr_Get16(&c[2]) + 0x4000) >> 15) +
                                    ((Ptr_Get16(&s[3]) * Ptr_Get16(&c[3]) + 0x4000) >> 15)));
        } else {
            Ptr_Set16(
                &po[k],
                Sat16(Sat16(Vmulf(Ptr_Get16(&s[0]), Ptr_Get16(&c[0])) + Vmulf(Ptr_Get16(&s[1]), Ptr_Get16(&c[1]))) +
                      Sat16(Vmulf(Ptr_Get16(&s[2]), Ptr_Get16(&c[2])) + Vmulf(Ptr_Get16(&s[3]), Ptr_Get16(&c[3])))));
        }
        pos += step;
    }
    return pos;
}

/**
 * Is the table at `lut` (DMEM_RESAMPLE_LUT_SIZE big-endian bytes) safe for Aud_ResampleLoop's
 * shortcut: no -0x8000, and |l0| + |l1|, |l2| + |l3| at most 0x7FFF in every row?
 */
static s32 Aud_CheckLut(const u8* lut) {
    s32 row;
    s32 i;
    s32 c[4];

    for (row = 0; row < 64; row++) {
        for (i = 0; i < 4; i++) {
            c[i] = (s16)((lut[row * 8 + 2 * i] << 8) | lut[row * 8 + 2 * i + 1]);
            if (c[i] == -0x8000) {
                return false;
            }
            c[i] = (c[i] < 0) ? -c[i] : c[i];
        }
        if ((c[0] + c[1] > 0x7FFF) || (c[2] + c[3] > 0x7FFF)) {
            return false;
        }
    }
    return true;
}

/** Position of output lane i of a batch: acc = base + mul * pitch (48-bit vector accumulator) */
static inline void Aud_ResamplePos(s64 acc, s16* intPart, u16* fracPart) {
    s64 hi = acc >> 16;

    // vmadm: int = clamp(acc >> 16); vmadn: frac = acc & 0xFFFF, or 0 / 0xFFFF if acc >> 16 clamps
    if (hi > 0x7FFF) {
        *intPart = 0x7FFF;
        *fracPart = 0xFFFF;
    } else if (hi < -0x8000) {
        *intPart = -0x8000;
        *fracPart = 0;
    } else {
        *intPart = hi;
        *fracPart = acc & 0xFFFF;
    }
}

/**
 * RESAMPLE: 4-tap interpolation with the 64-phase table at DMEM 0xE0. The state (w1, 32 bytes) holds
 * the 4 input samples before the current position and the position's fraction. They are stored in
 * front of the input (in - 8; A_INIT starts from zeros), then output k is the dot product of the 4
 * samples at in - 8 + 2 * int(pos) with the table row of the top 6 fraction bits, pos = frac + k * 2 *
 * pitch (16.16). Outputs come 8 at a time; count (bytes) rounds up to 16, at least 8 outputs.
 * Flags 2 and 4 (unused by MM) store the state samples at half or double spacing instead.
 */
static void Aud_Resample(u32 w0, u32 w1) {
    s32 in;
    s32 out;
    s32 count;
    u32 flags = (w0 >> 16) & 0xFF;
    u32 pitch = w0 & 0xFFFF;
    u32 stateAddr = w1 & AUD_ADDR_MASK;
    s16 state[4];
    s16 zero[4];
    u32 frac;
    s32 nb;
    u32 outBytes;
    u32 endPos;
    u32 pos;
    u32 step;
    s16* po;
    s16 ints[8];
    u16 fracs[8];
    s16 res[8];
    s64 base;
    s32 i;

    if (Dmem_Lhu(DMEM_RESAMPLE_LUT) != DMEM_RESAMPLE_LUT_MAGIC) {
        Aud_Dma(DMEM_RESAMPLE_LUT, Dmem_Lw(DMEM_DRAM_STACK), DMEM_RESAMPLE_LUT_SIZE - 1, false);
    }
    in = Dmem_Lh(DMEM_BUF_IN);
    out = Dmem_Lh(DMEM_BUF_OUT);
    count = Dmem_Lh(DMEM_BUF_COUNT);
    Vec_Lqv(sRegs.v31, DMEM_RESAMPLE_ONES);

    if (flags & A_INIT) {
        Dmem_Sh(DMEM_RESAMPLE_FRAC, 0);
        __builtin_memset(zero, 0, sizeof(zero));
        Vec_StoreBytes(zero, 0, DMEM_STATE, 8);
    } else {
        Aud_Dma(DMEM_STATE, stateAddr, 0x1F, false);
    }
    Vec_LoadBytes(state, 0, DMEM_STATE, 8);
    if (flags & 2) {
        in -= 4;
        Vec_StoreBytes(state, 0, in, 2);
        Vec_StoreBytes(state, 4, in + 2, 2);
    } else if (flags & 4) {
        in -= 16;
        for (i = 0; i < 8; i++) {
            Vec_StoreBytes(state, 2 * (i >> 1), in + 2 * i, 2);
        }
    } else {
        in -= 8;
        Vec_StoreBytes(state, 0, in, 8);
    }
    frac = Dmem_Lhu(DMEM_RESAMPLE_FRAC);

    nb = (count > 16) ? ((count + 15) >> 4) : 1;
    outBytes = nb * 16;
    step = pitch * 2;
    // Position just past the last output, and the furthest sample read (the end state reads 4 there)
    endPos = frac + step * (u32)(nb * 8 + 7);

    if (FAST_PATH_OK && ((endPos >> 16) <= 0x7FFF) && (in >= 0) && ((in & 1) == 0) && Dmem_Fits(out, outBytes, 16) &&
        ((u32)in + 2 * (endPos >> 16) + 8 <= AUD_DMEM_SIZE) &&
        !Dmem_Overlap(out, outBytes, in, 2 * (endPos >> 16) + 8) &&
        !Dmem_Overlap(out, outBytes, DMEM_RESAMPLE_LUT, DMEM_RESAMPLE_LUT_SIZE) &&
        !Dmem_Overlap(out, outBytes, DMEM_STATE, AUD_DMEM_SIZE - DMEM_STATE) &&
        !Dmem_Overlap(in, 2 * (endPos >> 16) + 8, DMEM_RESAMPLE_ADDRS, 0x20)) {
        // No position saturates, the output overlaps nothing it reads, and the input does not reach the
        // address tables the vector loop rewrites between batches: one straight loop. With the
        // microcode's own table in DMEM, only the final sum can saturate (see Aud_CheckLut).
        po = Dmem_Ptr16(out);
        pos = frac;
        if ((sLutSafe != NULL) &&
            (__builtin_memcmp(&sDmem[DMEM_RESAMPLE_LUT], sLutSafe, DMEM_RESAMPLE_LUT_SIZE) == 0)) {
            pos = Aud_ResampleLoop(po, in, pos, step, nb * 8, true);
        } else {
            pos = Aud_ResampleLoop(po, in, pos, step, nb * 8, false);
        }
        // The positions of the batch after the last one: the pipelined loop leaves their address tables
        for (i = 0; i < 8; i++) {
            ints[i] = (pos + step * i) >> 16;
            fracs[i] = (pos + step * i) & 0xFFFF;
            Dmem_Sh(DMEM_RESAMPLE_ADDRS + 2 * i, in + 2 * (u16)ints[i]);
            Dmem_Sh(DMEM_RESAMPLE_ADDRS + 0x10 + 2 * i, DMEM_RESAMPLE_LUT + (fracs[i] >> 10) * 8);
        }
    } else {
        // Batch by batch, as the vector loop: positions of 8 outputs from the previous batch's last
        base = frac;
        for (i = 0; i < 8; i++) {
            Aud_ResamplePos(base + (s64)(2 * i) * pitch, &ints[i], &fracs[i]);
        }
        do {
            for (i = 0; i < 8; i++) {
                res[i] = Aud_ResampleTap((u16)(in + 2 * (u16)ints[i]), DMEM_RESAMPLE_LUT + (fracs[i] >> 10) * 8);
            }
            base = ((s64)ints[7] << 16) + fracs[7];
            for (i = 0; i < 8; i++) {
                Aud_ResamplePos(base + (s64)(2 * (i + 1)) * pitch, &ints[i], &fracs[i]);
                // The address tables (sample, LUT row) the loop keeps in DMEM, before the batch is stored
                Dmem_Sh(DMEM_RESAMPLE_ADDRS + 2 * i, in + 2 * (u16)ints[i]);
                Dmem_Sh(DMEM_RESAMPLE_ADDRS + 0x10 + 2 * i, DMEM_RESAMPLE_LUT + (fracs[i] >> 10) * 8);
            }
            count -= 16;
            Vec_Sqv(res, out);
            out += 16;
        } while (count > 0);
    }

    // State: the fraction and the 4 samples at the position after the last output
    Dmem_Sh(DMEM_RESAMPLE_FRAC, fracs[0]);
    Vec_LoadBytes(state, 0, (u16)(in + 2 * (u16)ints[0]), 8);
    Vec_StoreBytes(state, 0, DMEM_STATE, 8);
    Aud_Dma(DMEM_STATE, stateAddr, 0x1F, true);
}

/**
 * RESAMPLE_ZOH: nearest-lower-sample resampling. The position is a 16.16 DMEM byte address starting at
 * (in << 16) | (w1 & 0xFFFF) and advancing by pitch * 4; each output is the sample at the even address
 * below it. count (bytes) rounds up to 8.
 */
static void Aud_ResampleZoh(u32 w0, u32 w1) {
    s32 in = Dmem_Lh(DMEM_BUF_IN);
    u32 out = Dmem_Lh(DMEM_BUF_OUT);
    s32 count = Dmem_Lh(DMEM_BUF_COUNT);
    u32 step = (w0 & 0xFFFF) << 2;
    u32 pos = (w1 & 0xFFFF) | ((u32)in << 16);
    s16 v[4];
    u32 i;

    do {
        for (i = 0; i < 4; i++) {
            Vec_LoadBytes(v, 2 * i, (pos >> 16) & 0xFFFE, 2);
            pos += step;
        }
        count -= 8;
        Vec_StoreBytes(v, 0, out, 8);
        out += 8;
    } while (count > 0);
}

/* ------------------------------------------------------------------------------------------------ */
/* Mixing                                                                                           */
/* ------------------------------------------------------------------------------------------------ */

/** MIXER lane: (out * v31[6] * 2 + 0x8000 + in * gain * 2) >> 16 in the 48-bit accumulator, saturated */
static inline s32 Aud_Mix1(s32 o, s32 i, s32 scale, s32 gain) {
    s64 acc = (s64)o * scale * 2 + 0x8000 + (s64)i * gain * 2;

    return Sat16((s32)(acc >> 16));
}

/** MIXER: out += in * gain (s16 fraction); the destination is scaled by 0x7FFF/0x8000. count rounds up to 32 bytes */
static void Aud_Mixer(u32 w0, u32 w1) {
    s32 count = (w0 >> 12) & 0xFF0;
    u32 out = w1 & 0xFFFF;
    u32 in = w1 >> 16;
    s32 gain = (s16)w0;
    u32 size = (count > 32) ? ((count + 31) & ~31) : 32;
    s32 scale;
    s16* po;
    s16* pi;
    s16 o0[8];
    s16 o1[8];
    s16 i0[8];
    s16 i1[8];
    u32 k;

    Vec_Lqv(sRegs.v31, DMEM_CONST);
    scale = sRegs.v31[6];

    if (FAST_PATH_OK && (scale == 0x7FFF) && Dmem_Fits(out, size, 16) && Dmem_Fits(in, size, 16) &&
        ((in == out) || !Dmem_Overlap(out, size, in, size))) {
        // o * 0x7FFF + i * gain + 0x4000 stays within 32 bits
        po = Dmem_Ptr16(out);
        pi = Dmem_Ptr16(in);
        for (k = 0; k < size / 2; k++) {
            Ptr_Set16(&po[k], Sat16((Ptr_Get16(&po[k]) * 0x7FFF + Ptr_Get16(&pi[k]) * gain + 0x4000) >> 15));
        }
        return;
    }

    // The vector loop's order: the next input vector is read before the result is stored, the next
    // output vector after the first half is stored
    __builtin_memset(o0, 0, sizeof(o0));
    __builtin_memset(o1, 0, sizeof(o1));
    __builtin_memset(i0, 0, sizeof(i0));
    __builtin_memset(i1, 0, sizeof(i1));
    Vec_Lqv(o0, out);
    Vec_Lqv(i0, in);
    Vec_Lqv(o1, out + 16);
    Vec_Lqv(i1, in + 16);
    do {
        for (k = 0; k < 8; k++) {
            o0[k] = Aud_Mix1(o0[k], i0[k], scale, gain);
            o1[k] = Aud_Mix1(o1[k], i1[k], scale, gain);
        }
        count -= 32;
        in += 32;
        Vec_Lqv(i0, in);
        Vec_Sqv(o0, out);
        Vec_Lqv(o0, out + 32);
        Vec_Sqv(o1, out + 16);
        Vec_Lqv(i1, in + 16);
        out += 32;
        Vec_Lqv(o1, out + 16);
    } while (count > 0);
}

/**
 * ADDMIXER: out += in, saturated, in 64-byte steps (count rounds up to 64). The microcode starts with
 * vaddc v31, v31, v31, meant to clear the carry flags; instead it sets them from the sign bits of
 * whatever v31 holds (the constants of the last ADPCM, MIXER, RESAMPLE or FILTER), and the first vadd
 * adds them to the first 8 samples.
 */
static void Aud_AddMixer(u32 w0, u32 w1) {
    s32 count = (w0 >> 12) & 0xFF0;
    u32 out = w1 & 0xFFFF;
    u32 in = w1 >> 16;
    u32 size = (count > 64) ? ((count + 63) & ~63) : 64;
    s32 carry[8];
    s16 o[32];
    s16 v[32];
    s16* po;
    s16* pi;
    u32 k;

    for (k = 0; k < 8; k++) {
        carry[k] = (u16)sRegs.v31[k] >> 15;
        sRegs.v31[k] = (u16)(sRegs.v31[k] << 1);
    }

    if (FAST_PATH_OK && Dmem_Fits(out, size, 16) && Dmem_Fits(in, size, 16) &&
        ((in == out) || !Dmem_Overlap(out, size, in, size))) {
        po = Dmem_Ptr16(out);
        pi = Dmem_Ptr16(in);
        for (k = 0; k < 8; k++) {
            Ptr_Set16(&po[k], Sat16(Ptr_Get16(&po[k]) + Ptr_Get16(&pi[k]) + carry[k]));
        }
        for (; k < size / 2; k++) {
            Ptr_Set16(&po[k], Sat16(Ptr_Get16(&po[k]) + Ptr_Get16(&pi[k])));
        }
        return;
    }

    __builtin_memset(o, 0, sizeof(o));
    __builtin_memset(v, 0, sizeof(v));
    do {
        for (k = 0; k < 4; k++) {
            Vec_Lqv(&o[8 * k], out + 16 * k);
            Vec_Lqv(&v[8 * k], in + 16 * k);
        }
        in += 64;
        for (k = 0; k < 32; k++) {
            o[k] = Sat16(o[k] + v[k] + ((k < 8) ? carry[k] : 0));
        }
        for (k = 0; k < 8; k++) {
            carry[k] = 0;
        }
        count -= 64;
        for (k = 0; k < 4; k++) {
            Vec_Sqv(&o[8 * k], out + 16 * k);
        }
        out += 64;
    } while (count > 0);
}

/** ENVSETUP1: reverb volume (bits 16-23 of w0, << 8) and its ramp; left and right ramps for ENVSETUP2 */
static void Aud_EnvSetup1(u32 w0, u32 w1) {
    u32 rev = (w0 >> 8) & 0xFF00;

    __builtin_memset(sRegs.env, 0, sizeof(sRegs.env));
    sRegs.envRampRev = w0 & 0xFFFF;
    sRegs.env[4] = rev;
    sRegs.env[5] = rev + sRegs.envRampRev;
    sRegs.envRampL = w1 >> 16;
    sRegs.envRampR = w1 & 0xFFFF;
}

/** ENVSETUP2: left and right volumes */
static void Aud_EnvSetup2(u32 w0, u32 w1) {
    sRegs.env[0] = w1 >> 16;
    sRegs.env[1] = (w1 >> 16) + sRegs.envRampL;
    sRegs.env[2] = w1 & 0xFFFF;
    sRegs.env[3] = (w1 & 0xFFFF) + sRegs.envRampR;
}

/**
 * ENVMIXER's straight loop, for buffers that do not overlap. The caller specializes it: without XOR
 * flags (useXor false) dry samples need no truncation to 16 bits, and with a reverb volume of 0
 * throughout and no wet XOR (useWet false) every wet sample is 0, so the wet buffers stay as they are.
 */
static inline __attribute__((always_inline)) void
Aud_EnvMixerLoop(const s16* pin, s16* pdl, s16* pdr, s16* pwl, s16* pwr, u16* vol, u16 rampL, u16 rampR, u16 rampRev,
                 s32 count, s32 xorDryL, s32 xorDryR, s32 xorWetL, s32 xorWetR, const s32 useXor, const s32 useWet) {
    s32 volL;
    s32 volR;
    s32 rev;
    s32 s;
    s32 dryL;
    s32 dryR;
    s32 wetL;
    s32 wetR;
    s32 half;
    s32 k;

    do {
        for (half = 0; half < 2; half++) {
            volL = vol[0 + half];
            volR = vol[2 + half];
            rev = vol[4 + half];
            for (k = 0; k < 8; k++) {
                s = Ptr_Get16(&pin[k]);
                dryL = (s * volL) >> 16;
                dryR = (s * volR) >> 16;
                if (useXor) {
                    dryL = (s16)(dryL ^ xorDryL);
                    dryR = (s16)(dryR ^ xorDryR);
                }
                Ptr_Set16(&pdl[k], Sat16(Ptr_Get16(&pdl[k]) + dryL));
                Ptr_Set16(&pdr[k], Sat16(Ptr_Get16(&pdr[k]) + dryR));
                if (useWet) {
                    wetL = (dryL * rev) >> 16;
                    wetR = (dryR * rev) >> 16;
                    if (useXor) {
                        wetL = (s16)(wetL ^ xorWetL);
                        wetR = (s16)(wetR ^ xorWetR);
                    }
                    Ptr_Set16(&pwl[k], Sat16(Ptr_Get16(&pwl[k]) + wetL));
                    Ptr_Set16(&pwr[k], Sat16(Ptr_Get16(&pwr[k]) + wetR));
                }
            }
            pin += 8;
            pdl += 8;
            pdr += 8;
            pwl += 8;
            pwr += 8;
        }
        vol[0] += rampL;
        vol[1] += rampL;
        vol[2] += rampR;
        vol[3] += rampR;
        vol[4] += rampRev;
        vol[5] += rampRev;
        count -= 16;
    } while (count > 0);
}

/**
 * ENVMIXER: splits a mono buffer into dry and wet (reverb) left/right, added (saturated) to four
 * buffers. Per sample: dry = (in * vol) >> 16 (vol unsigned), XORed with -1 for the "strong" flags
 * (bits 1 and 0); wet = (dry * reverb) >> 16, XORed with 0xFFFC / 0xFFFE (bits 3 and 2); bit 4 swaps
 * the wet sides. Samples 0-7 of each 16 use the volumes, 8-15 the volumes plus one ramp step; both
 * advance by two steps (mod 2^16) per 16 samples. count (samples, bits 8-15) rounds up to 16.
 */
static void Aud_EnvMixer(u32 w0, u32 w1) {
    u32 in = (w0 >> 12) & 0xFF0;
    s32 count = (w0 >> 8) & 0xFF;
    u32 dl = (w1 >> 20) & 0xFF0;
    u32 dr = (w1 >> 12) & 0xFF0;
    u32 wl = (w1 >> 4) & 0xFF0;
    u32 wr = (w1 << 4) & 0xFF0;
    s32 swap = w0 & 0x10;
    s32 xorDryL = -(s32)((w0 >> 1) & 1);
    s32 xorDryR = -(s32)(w0 & 1);
    s32 xorWetL = -(s32)((w0 & 8) >> 1);
    s32 xorWetR = -(s32)((w0 & 4) >> 1);
    u32 size = ((count > 16) ? ((count + 15) & ~15) : 16) * 2;
    u16 rampL;
    u16 rampR;
    u16 rampRev;
    u16 vol[6];
    s16 x[16];
    s16 vdl[16];
    s16 vdr[16];
    s16 vwl[16];
    s16 vwr[16];
    s32 dryL;
    s32 dryR;
    s32 wetL;
    s32 wetR;
    s32 half;
    s32 k;

    sRegs.envRampL *= 2;
    sRegs.envRampR *= 2;
    sRegs.envRampRev *= 2;
    rampL = sRegs.envRampL;
    rampR = sRegs.envRampR;
    rampRev = sRegs.envRampRev;
    for (k = 0; k < 6; k++) {
        vol[k] = sRegs.env[k];
    }

    if (FAST_PATH_OK && Dmem_Fits(in, size, 16) && Dmem_Fits(dl, size, 16) && Dmem_Fits(dr, size, 16) &&
        Dmem_Fits(wl, size, 16) && Dmem_Fits(wr, size, 16) && !Dmem_Overlap(in, size, dl, size) &&
        !Dmem_Overlap(in, size, dr, size) && !Dmem_Overlap(in, size, wl, size) && !Dmem_Overlap(in, size, wr, size) &&
        !Dmem_Overlap(dl, size, dr, size) && !Dmem_Overlap(dl, size, wl, size) && !Dmem_Overlap(dl, size, wr, size) &&
        !Dmem_Overlap(dr, size, wl, size) && !Dmem_Overlap(dr, size, wr, size) && !Dmem_Overlap(wl, size, wr, size)) {
        const s16* pin = Dmem_Ptr16(in);
        s16* pdl = Dmem_Ptr16(dl);
        s16* pdr = Dmem_Ptr16(dr);
        // With the wet sides swapped, the left wet goes to wr and the right one to wl
        s16* pwl = Dmem_Ptr16(swap ? wr : wl);
        s16* pwr = Dmem_Ptr16(swap ? wl : wr);

        if ((xorDryL | xorDryR | xorWetL | xorWetR) != 0) {
            Aud_EnvMixerLoop(pin, pdl, pdr, pwl, pwr, vol, rampL, rampR, rampRev, count, xorDryL, xorDryR, xorWetL,
                             xorWetR, true, true);
        } else if ((vol[0] | vol[1] | vol[2] | vol[3] | rampL | rampR) == 0) {
            // Silent: every dry sample is 0, so every wet one too. Only the reverb volumes ramp.
            k = (count > 16) ? ((count + 15) >> 4) : 1;
            vol[4] += rampRev * k;
            vol[5] += rampRev * k;
        } else if ((vol[4] | vol[5] | rampRev) == 0) {
            Aud_EnvMixerLoop(pin, pdl, pdr, pwl, pwr, vol, rampL, rampR, rampRev, count, 0, 0, 0, 0, false, false);
        } else {
            Aud_EnvMixerLoop(pin, pdl, pdr, pwl, pwr, vol, rampL, rampR, rampRev, count, 0, 0, 0, 0, false, true);
        }
    } else {
        // The vector loop's order: all four destinations of 16 samples are read before any is written
        __builtin_memset(x, 0, sizeof(x));
        Vec_Lqv(&x[0], in);
        do {
            Vec_Lqv(&x[8], in + 16);
            in += 32;
            for (half = 0; half < 2; half++) {
                Vec_Lqv(&vdl[8 * half], dl + 16 * half);
                Vec_Lqv(&vdr[8 * half], dr + 16 * half);
            }
            for (half = 0; half < 2; half++) {
                Vec_Lqv(&vwl[8 * half], wl + 16 * half);
                Vec_Lqv(&vwr[8 * half], wr + 16 * half);
            }
            for (k = 0; k < 16; k++) {
                half = k >> 3;
                dryL = (s16)(((x[k] * (s32)vol[0 + half]) >> 16) ^ xorDryL);
                dryR = (s16)(((x[k] * (s32)vol[2 + half]) >> 16) ^ xorDryR);
                wetL = (s16)(((dryL * (s32)vol[4 + half]) >> 16) ^ xorWetL);
                wetR = (s16)(((dryR * (s32)vol[4 + half]) >> 16) ^ xorWetR);
                vdl[k] = Sat16(vdl[k] + dryL);
                vdr[k] = Sat16(vdr[k] + dryR);
                vwl[k] = Sat16(vwl[k] + (swap ? wetR : wetL));
                vwr[k] = Sat16(vwr[k] + (swap ? wetL : wetR));
            }
            Vec_Sqv(&vdl[0], dl);
            Vec_Sqv(&vdr[0], dr);
            Vec_Sqv(&vdl[8], dl + 16);
            Vec_Sqv(&vdr[8], dr + 16);
            Vec_Sqv(&vwl[0], wl);
            Vec_Sqv(&vwr[0], wr);
            Vec_Lqv(&x[0], in);
            Vec_Sqv(&vwl[8], wl + 16);
            Vec_Sqv(&vwr[8], wr + 16);
            dl += 32;
            dr += 32;
            wl += 32;
            wr += 32;
            vol[0] += rampL;
            vol[1] += rampL;
            vol[2] += rampR;
            vol[3] += rampR;
            vol[4] += rampRev;
            vol[5] += rampRev;
            count -= 16;
        } while (count > 0);
    }

    for (k = 0; k < 6; k++) {
        sRegs.env[k] = vol[k];
    }
}

/* ------------------------------------------------------------------------------------------------ */
/* Filter                                                                                           */
/* ------------------------------------------------------------------------------------------------ */

/**
 * FILTER with flags >= 2 sets the count (w0 & 0xFFFF bytes) and loads 8 coefficients (16 bytes at w1)
 * to DMEM 0xFE0, between zeros at 0xFD0 and 0xFF0. FILTER with flags 0 or 1 (A_INIT) then filters the
 * buffer at w0 & 0xFFFF in place, 8 samples per step (count rounds up to 16 bytes), with the state at
 * w1 (32 bytes: the 8 previous input samples, the previous coefficients; zeros with A_INIT).
 *
 * The coefficients used are the rounded average of the new and the previous ones, (new + old + 1) >> 1
 * (so they are halved after A_INIT), and are saved as the next "previous" ones. Output i of a step is
 * (0x8000 + 2 * sum_j z[16 + i - j] * w[j]) >> 16, saturated, over the 16 samples w = the 8 previous
 * inputs then the 8 current ones (j = 1..15), where z is the 24 samples at DMEM 0xFD0: an 8-tap FIR
 * while 0xFD0-0xFDF and 0xFF0-0xFFF hold the zeros the load left there.
 */
static void Aud_Filter(u32 w0, u32 w1) {
    u32 flags = (w0 >> 16) & 0xFF;
    u32 addr = w1 & AUD_ADDR_MASK;
    u32 dmem;
    s32 count;
    s16 zero[8];
    s16 c[8];
    s16 cOld[8];
    s16 z[24];
    s16 w[16];
    s64 acc;
    s32 acc32;
    s32 l1;
    s32 firOnly;
    u32 size;
    s32 i;
    s32 j;

    __builtin_memset(zero, 0, sizeof(zero));
    Vec_Sqv(zero, DMEM_STATE);
    Vec_Sqv(zero, DMEM_STATE + 16);

    if (flags > 1) {
        sRegs.filterCount = w0 & 0xFFFF;
        Vec_Sqv(zero, DMEM_FILTER_TAPS);
        Vec_Sqv(zero, DMEM_FILTER_TAPS + 0x20);
        Aud_Dma(DMEM_FILTER_COEFS, addr, 0xF, false);
        return;
    }
    if (flags == 0) {
        Aud_Dma(DMEM_STATE, addr, 0x1F, false);
    }

    Vec_Lqv(c, DMEM_FILTER_COEFS);
    Vec_Lqv(cOld, DMEM_STATE + 16);
    for (i = 0; i < 8; i++) {
        c[i] = Sat16((0x8000 + (c[i] + cOld[i]) * 0x8000) >> 16);
    }
    Vec_Sqv(c, DMEM_FILTER_COEFS);
    Vec_Sqv(c, DMEM_STATE + 16);

    l1 = 0;
    firOnly = true;
    for (i = 0; i < 24; i++) {
        z[i] = Dmem_Get16(DMEM_FILTER_TAPS + 2 * i);
        l1 += (z[i] < 0) ? -z[i] : z[i];
        if (((i < 8) || (i >= 16)) && (z[i] != 0)) {
            firOnly = false;
        }
    }
    for (i = 0; i < 8; i++) {
        sRegs.v31[i] = z[1 + i];
    }

    dmem = w0 & 0xFFFF;
    count = sRegs.filterCount;
    size = (count > 16) ? ((count + 15) & ~15) : 16;
    Vec_Lqv(&w[0], DMEM_STATE);
    __builtin_memset(&w[8], 0, sizeof(s16) * 8);

    if (FAST_PATH_OK && firOnly && (l1 < 0x7FFF) && Dmem_Fits(dmem, size, 16)) {
        // The 8-tap FIR in 32 bits (|2 * sum| < 2^31 - 0x8000), in place
        s16* p = Dmem_Ptr16(dmem);
        s32 cf[8];
        s32 win[16];

        for (i = 0; i < 8; i++) {
            cf[i] = z[8 + i];
            win[i] = w[i];
        }
        do {
            for (i = 0; i < 8; i++) {
                win[8 + i] = Ptr_Get16(&p[i]);
            }
#pragma GCC unroll 8
            for (i = 0; i < 8; i++) {
                acc32 = 0;
#pragma GCC unroll 8
                for (j = 0; j < 8; j++) {
                    acc32 += cf[j] * win[8 + i - j];
                }
                Ptr_Set16(&p[i], Sat16((0x8000 + 2 * acc32) >> 16));
            }
            for (i = 0; i < 8; i++) {
                win[i] = win[8 + i];
            }
            p += 8;
            count -= 16;
        } while (count > 0);
        for (i = 0; i < 8; i++) {
            w[i] = win[i];
        }
    } else {
        do {
            Vec_Lqv(&w[8], dmem);
            for (i = 0; i < 8; i++) {
                acc = 0;
                for (j = 1; j < 16; j++) {
                    acc += z[16 + i - j] * w[j];
                }
                c[i] = Sat16((s32)((0x8000 + 2 * acc) >> 16));
            }
            count -= 16;
            Vec_Sqv(c, dmem);
            dmem += 16;
            for (i = 0; i < 8; i++) {
                w[i] = w[8 + i];
            }
        } while (count > 0);
    }
    // The loop counts $15 down: another FILTER without a new count filters a single step
    sRegs.filterCount = count;

    Vec_Sqv(&w[0], DMEM_STATE);
    Aud_Dma(DMEM_STATE, addr, 0x1F, true);
}

/* ------------------------------------------------------------------------------------------------ */
/* Task                                                                                             */
/* ------------------------------------------------------------------------------------------------ */

static void Aud_Dispatch(u32 op, u32 w0, u32 w1) {
    switch (op) {
        case 0:
            Aud_MultTable(w0, w1);
            break;
        case A_ADPCM:
            Aud_Adpcm(w0, w1);
            break;
        case A_CLEARBUFF:
            Aud_ClearBuff(w0, w1);
            break;
        case A_UNK3:
            break;
        case A_ADDMIXER:
            Aud_AddMixer(w0, w1);
            break;
        case A_RESAMPLE:
            Aud_Resample(w0, w1);
            break;
        case A_RESAMPLE_ZOH:
            Aud_ResampleZoh(w0, w1);
            break;
        case A_FILTER:
            Aud_Filter(w0, w1);
            break;
        case A_SETBUFF:
            Aud_SetBuff(w0, w1);
            break;
        case A_DUPLICATE:
            Aud_Duplicate(w0, w1);
            break;
        case A_DMEMMOVE:
            Aud_DmemMove(w0, w1);
            break;
        case A_LOADADPCM:
            Aud_LoadAdpcm(w0, w1);
            break;
        case A_MIXER:
            Aud_Mixer(w0, w1);
            break;
        case A_INTERLEAVE:
            Aud_Interleave(w0, w1);
            break;
        case A_HILOGAIN:
            Aud_HiLoGain(w0, w1);
            break;
        case A_SETLOOP:
            Aud_SetLoop(w0, w1);
            break;
        case 16:
            Aud_CopyBlocks(w0, w1);
            break;
        case A_INTERL:
            Aud_Interl(w0, w1);
            break;
        case A_ENVSETUP1:
            Aud_EnvSetup1(w0, w1);
            break;
        case A_ENVMIXER:
            Aud_EnvMixer(w0, w1);
            break;
        case A_LOADBUFF:
            Aud_LoadBuff(w0, w1);
            break;
        case A_SAVEBUFF:
            Aud_SaveBuff(w0, w1);
            break;
        case A_ENVSETUP2:
            Aud_EnvSetup2(w0, w1);
            break;
        case A_S8DEC:
            Aud_S8Dec(w0, w1);
            break;
        default:
            sErrorCount++;
            break;
    }
}

void AudUcode_RunTask(const AudUcodeTask* task) {
    u32 alistAddr = task->alistAddr;
    s32 remaining = task->alistSize;
    s32 chunk;
    s32 pos;
    u32 w0;
    u32 w1;
    u32 op;
    unsigned long long start;

    // Start-up: the microcode's data to DMEM 0, the OSTask (osSpTaskLoad) at 0xFC0
    __builtin_memcpy(&sDmem[0], task->ucodeData, AUD_UCODE_DATA_SIZE);
    if ((sLutSource == NULL) ||
        (__builtin_memcmp(sLutCopy, &task->ucodeData[DMEM_RESAMPLE_LUT], DMEM_RESAMPLE_LUT_SIZE) != 0)) {
        __builtin_memcpy(sLutCopy, &task->ucodeData[DMEM_RESAMPLE_LUT], DMEM_RESAMPLE_LUT_SIZE);
        sLutSource = sLutCopy;
        sLutSafe = Aud_CheckLut(sLutCopy) ? sLutCopy : NULL;
    }
    if (task->header != NULL) {
        __builtin_memcpy(&sDmem[AUD_TASK_HEADER_ADDR], task->header, AUD_TASK_HEADER_SIZE);
    }
    Dmem_Sw(DMEM_DRAM_STACK, task->dramStack);

    // Commands are fetched 8 at a time into DMEM 0x2F0 and executed from there
    while (remaining > 0) {
        chunk = (remaining > DMEM_ALIST_SIZE) ? DMEM_ALIST_SIZE : remaining;
        Aud_Dma(DMEM_ALIST, alistAddr, chunk - 1, false);
        for (pos = 0; pos < chunk; pos += 8) {
            w0 = Dmem_Lw(DMEM_ALIST + pos);
            w1 = Dmem_Lw(DMEM_ALIST + pos + 4);
            alistAddr += 8;
            remaining -= 8;
            op = (w0 >> 24) & 0x7F;
            if (op < AUD_NUM_OPS) {
                sOpCount[op]++;
            }
            if (sProfileClock == NULL) {
                Aud_Dispatch(op, w0, w1);
            } else {
                start = sProfileClock();
                Aud_Dispatch(op, w0, w1);
                sOpTicks[op & (AUD_NUM_OPS - 1)] += sProfileClock() - start;
            }
        }
    }
}

u8* AudUcode_GetDmem(void) {
    return sDmem;
}

void AudUcode_Reset(void) {
    __builtin_memset(sDmem, 0, sizeof(sDmem));
    __builtin_memset(&sRegs, 0, sizeof(sRegs));
    AudUcode_ResetStats();
}

void AudUcode_ResetStats(void) {
    __builtin_memset(sOpCount, 0, sizeof(sOpCount));
    __builtin_memset(sOpTicks, 0, sizeof(sOpTicks));
    sErrorCount = 0;
}

u32 AudUcode_GetOpCount(u32 op) {
    return (op < AUD_NUM_OPS) ? sOpCount[op] : 0;
}

u32 AudUcode_GetErrorCount(void) {
    return sErrorCount;
}

void AudUcode_SetProfileClock(unsigned long long (*clock)(void)) {
    sProfileClock = clock;
}

unsigned long long AudUcode_GetOpTicks(u32 op) {
    return (op < AUD_NUM_OPS) ? sOpTicks[op] : 0;
}
