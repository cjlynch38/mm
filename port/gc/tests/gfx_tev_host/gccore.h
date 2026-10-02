/**
 * Host stand-in for libogc's <gccore.h> used by the gfx_tev host test: the libogc types, the GX constants
 * gfx_tev.c uses (values copied from libogc 3.1.0 ogc/gx.h) and the GX functions it calls, which mock_gx.c
 * records into gMockGx.
 */
#ifndef MOCK_GCCORE_H
#define MOCK_GCCORE_H

#include <stdbool.h>
#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef float f32;

typedef struct _gx_color {
    u8 r, g, b, a;
} GXColor;

#define GX_FALSE 0
#define GX_TRUE 1
#define GX_DISABLE 0
#define GX_ENABLE 1

#define GX_COLOR0 0
#define GX_ALPHA0 2
#define GX_COLOR0A0 4
#define GX_COLORNULL 0xff

#define GX_TEXCOORD0 0x0
#define GX_TEXCOORD1 0x1
#define GX_TEXCOORDNULL 0xff
#define GX_TEXMAP0 0
#define GX_TEXMAP1 1
#define GX_TEXMAP_NULL 0xff

#define GX_NEVER 0
#define GX_LESS 1
#define GX_EQUAL 2
#define GX_LEQUAL 3
#define GX_GREATER 4
#define GX_NEQUAL 5
#define GX_GEQUAL 6
#define GX_ALWAYS 7

#define GX_BM_NONE 0
#define GX_BM_BLEND 1
#define GX_BM_LOGIC 2
#define GX_BM_SUBTRACT 3

#define GX_BL_ZERO 0
#define GX_BL_ONE 1
#define GX_BL_SRCCLR 2
#define GX_BL_INVSRCCLR 3
#define GX_BL_SRCALPHA 4
#define GX_BL_INVSRCALPHA 5
#define GX_BL_DSTALPHA 6
#define GX_BL_INVDSTALPHA 7
#define GX_BL_DSTCLR GX_BL_SRCCLR
#define GX_BL_INVDSTCLR GX_BL_INVSRCCLR

#define GX_LO_CLEAR 0
#define GX_LO_COPY 3

#define GX_CC_CPREV 0
#define GX_CC_APREV 1
#define GX_CC_C0 2
#define GX_CC_A0 3
#define GX_CC_C1 4
#define GX_CC_A1 5
#define GX_CC_C2 6
#define GX_CC_A2 7
#define GX_CC_TEXC 8
#define GX_CC_TEXA 9
#define GX_CC_RASC 10
#define GX_CC_RASA 11
#define GX_CC_ONE 12
#define GX_CC_HALF 13
#define GX_CC_KONST 14
#define GX_CC_ZERO 15

#define GX_CA_APREV 0
#define GX_CA_A0 1
#define GX_CA_A1 2
#define GX_CA_A2 3
#define GX_CA_TEXA 4
#define GX_CA_RASA 5
#define GX_CA_KONST 6
#define GX_CA_ZERO 7

#define GX_MAX_TEVSTAGE 16
#define GX_TEV_ADD 0
#define GX_TEV_SUB 1
#define GX_TB_ZERO 0
#define GX_TB_ADDHALF 1
#define GX_TB_SUBHALF 2
#define GX_CS_SCALE_1 0
#define GX_CS_SCALE_2 1
#define GX_CS_SCALE_4 2
#define GX_CS_DIVIDE_2 3

#define GX_TEVPREV 0
#define GX_TEVREG0 1
#define GX_TEVREG1 2
#define GX_TEVREG2 3

#define GX_AOP_AND 0
#define GX_AOP_OR 1
#define GX_AOP_XOR 2
#define GX_AOP_XNOR 3

#define GX_KCOLOR0 0
#define GX_KCOLOR1 1
#define GX_KCOLOR2 2
#define GX_KCOLOR3 3

#define GX_TEV_KCSEL_1 0x00
#define GX_TEV_KCSEL_7_8 0x01
#define GX_TEV_KCSEL_3_4 0x02
#define GX_TEV_KCSEL_5_8 0x03
#define GX_TEV_KCSEL_1_2 0x04
#define GX_TEV_KCSEL_3_8 0x05
#define GX_TEV_KCSEL_1_4 0x06
#define GX_TEV_KCSEL_1_8 0x07
#define GX_TEV_KCSEL_K0 0x0C
#define GX_TEV_KCSEL_K1 0x0D
#define GX_TEV_KCSEL_K2 0x0E
#define GX_TEV_KCSEL_K3 0x0F
#define GX_TEV_KCSEL_K0_R 0x10
#define GX_TEV_KCSEL_K1_R 0x11
#define GX_TEV_KCSEL_K2_R 0x12
#define GX_TEV_KCSEL_K3_R 0x13
#define GX_TEV_KCSEL_K0_G 0x14
#define GX_TEV_KCSEL_K1_G 0x15
#define GX_TEV_KCSEL_K2_G 0x16
#define GX_TEV_KCSEL_K3_G 0x17
#define GX_TEV_KCSEL_K0_B 0x18
#define GX_TEV_KCSEL_K1_B 0x19
#define GX_TEV_KCSEL_K2_B 0x1A
#define GX_TEV_KCSEL_K3_B 0x1B
#define GX_TEV_KCSEL_K0_A 0x1C
#define GX_TEV_KCSEL_K1_A 0x1D
#define GX_TEV_KCSEL_K2_A 0x1E
#define GX_TEV_KCSEL_K3_A 0x1F

#define GX_TEV_KASEL_1 0x00
#define GX_TEV_KASEL_7_8 0x01
#define GX_TEV_KASEL_3_4 0x02
#define GX_TEV_KASEL_5_8 0x03
#define GX_TEV_KASEL_1_2 0x04
#define GX_TEV_KASEL_3_8 0x05
#define GX_TEV_KASEL_1_4 0x06
#define GX_TEV_KASEL_1_8 0x07
#define GX_TEV_KASEL_K0_R 0x10
#define GX_TEV_KASEL_K1_R 0x11
#define GX_TEV_KASEL_K2_R 0x12
#define GX_TEV_KASEL_K3_R 0x13
#define GX_TEV_KASEL_K0_G 0x14
#define GX_TEV_KASEL_K1_G 0x15
#define GX_TEV_KASEL_K2_G 0x16
#define GX_TEV_KASEL_K3_G 0x17
#define GX_TEV_KASEL_K0_B 0x18
#define GX_TEV_KASEL_K1_B 0x19
#define GX_TEV_KASEL_K2_B 0x1A
#define GX_TEV_KASEL_K3_B 0x1B
#define GX_TEV_KASEL_K0_A 0x1C
#define GX_TEV_KASEL_K1_A 0x1D
#define GX_TEV_KASEL_K2_A 0x1E
#define GX_TEV_KASEL_K3_A 0x1F

void GX_SetNumTevStages(u8 num);
void GX_SetTevOrder(u8 tevstage, u8 texcoord, u32 texmap, u8 color);
void GX_SetTevColorIn(u8 tevstage, u8 a, u8 b, u8 c, u8 d);
void GX_SetTevAlphaIn(u8 tevstage, u8 a, u8 b, u8 c, u8 d);
void GX_SetTevColorOp(u8 tevstage, u8 tevop, u8 tevbias, u8 tevscale, u8 clamp, u8 tevregid);
void GX_SetTevAlphaOp(u8 tevstage, u8 tevop, u8 tevbias, u8 tevscale, u8 clamp, u8 tevregid);
void GX_SetTevKColorSel(u8 tevstage, u8 sel);
void GX_SetTevKAlphaSel(u8 tevstage, u8 sel);
void GX_SetTevColor(u8 tev_regid, GXColor color);
void GX_SetTevKColor(u8 sel, GXColor col);
void GX_SetBlendMode(u8 type, u8 src_fact, u8 dst_fact, u8 op);
void GX_SetAlphaCompare(u8 comp0, u8 ref0, u8 aop, u8 comp1, u8 ref1);
void GX_SetZMode(u8 enable, u8 func, u8 update_enable);
void GX_SetZCompLoc(u8 before_tex);
void GX_SetColorUpdate(u8 enable);
void GX_SetAlphaUpdate(u8 enable);

#endif
