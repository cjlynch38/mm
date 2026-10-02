/**
 * Host-side test of the libultra device files in port/gc/ultra (vi, sp, pi, cache, cont, motor,
 * ai, flash, voice). The real files are built with the decomp's headers against mocks of
 * gc_bridge.h (mock_bridge.c) and of the libultra core (mock_ultra.c), and driven the way the
 * game's callers drive them (sched.c, irqmgr.c, padmgr.c, z_std_dma.c, sys_flashrom.c).
 *
 *   make -C port/gc/tests/ultra_devices run
 */
#include "gc_ultra_internal.h"
#include "PR/os_internal_flash.h"
#include "PR/os_motor.h"
#include "PR/os_voice.h"
#include "macros.h"
#include "stdbool.h"
#include "mock.h"

void __gcFlashFlush(void);

#define CHECK(cond) ((cond) ? (void)0 : mock_fail("%s:%d: CHECK(%s)", __FILE__, __LINE__, #cond))
#define CHECK_EQ(a, b)                                                                                     \
    do {                                                                                                   \
        long checkA_ = (long)(a);                                                                          \
        long checkB_ = (long)(b);                                                                          \
        if (checkA_ != checkB_) {                                                                          \
            mock_fail("%s:%d: %s == %s: 0x%lX != 0x%lX", __FILE__, __LINE__, #a, #b, checkA_, checkB_); \
        }                                                                                                  \
    } while (0)

static void Fill(void* dst, u8 value, u32 size) {
    u32 i;

    for (i = 0; i < size; i++) {
        ((u8*)dst)[i] = value;
    }
}

static s32 Equal(const void* a, const void* b, u32 size) {
    u32 i;

    for (i = 0; i < size; i++) {
        if (((const u8*)a)[i] != ((const u8*)b)[i]) {
            return false;
        }
    }
    return true;
}

static s32 AllBytes(const void* a, u8 value, u32 size) {
    u32 i;

    for (i = 0; i < size; i++) {
        if (((const u8*)a)[i] != value) {
            return false;
        }
    }
    return true;
}

/* ---------------------------------------------------------------------------------------------- */

static void TestCache(void) {
    u8 buf[64];

    gc_log("== cache");
    Fill(buf, 0x42, sizeof(buf));
    osInvalDCache(buf, sizeof(buf));
    osInvalICache(buf, sizeof(buf));
    osWritebackDCache(buf, sizeof(buf));
    osWritebackDCacheAll();
    CHECK(AllBytes(buf, 0x42, sizeof(buf)));
}

static void TestAi(void) {
    u8 buf[0x400];

    gc_log("== ai");
    CHECK_EQ(osAiSetFrequency(32000), 32028); // the AI's real rate for 32 kHz, as the N64 returns 32006
    CHECK_EQ(osAiGetLength(), 0);
    CHECK_EQ(osAiSetNextBuffer(buf, sizeof(buf)), 0);
}

static void TestVoice(void) {
    OSVoiceHandle hd;
    OSVoiceData data;
    OSMesgQueue mq;
    u8 word[4] = { 0 };
    u8 mask[1] = { 0 };

    gc_log("== voice");
    Fill(&hd, 0x5A, sizeof(hd));
    Fill(&data, 0x5A, sizeof(data));
    CHECK_EQ(osVoiceInit(&mq, &hd, 0), CONT_ERR_NO_CONTROLLER);
    CHECK_EQ(osVoiceSetWord(&hd, word), CONT_ERR_NO_CONTROLLER);
    CHECK_EQ(osVoiceCheckWord(word), CONT_ERR_NO_CONTROLLER);
    CHECK_EQ(osVoiceStartReadData(&hd), CONT_ERR_NO_CONTROLLER);
    CHECK_EQ(osVoiceStopReadData(&hd), CONT_ERR_NO_CONTROLLER);
    CHECK_EQ(osVoiceGetReadData(&hd, &data), CONT_ERR_NO_CONTROLLER);
    CHECK_EQ(osVoiceClearDictionary(&hd, 1), CONT_ERR_NO_CONTROLLER);
    CHECK_EQ(osVoiceMaskDictionary(&hd, mask, 1), CONT_ERR_NO_CONTROLLER);
    CHECK_EQ(osVoiceControlGain(&hd, 0, 0), CONT_ERR_NO_CONTROLLER);
    CHECK(AllBytes(&hd, 0x5A, sizeof(hd)));
    CHECK(AllBytes(&data, 0x5A, sizeof(data)));
}

static void TestMotor(void) {
    OSPfs pfs;
    OSPfs uninit;
    OSMesgQueue mq;
    OSMesg buf[1];
    int calls;

    gc_log("== motor");
    osCreateMesgQueue(&mq, buf, 1);
    Fill(&pfs, 0, sizeof(pfs));
    CHECK_EQ(osMotorInit(&mq, &pfs, 1), 0);
    CHECK(pfs.status & PFS_MOTOR_INITIALIZED);
    CHECK_EQ(pfs.channel, 1);
    CHECK(pfs.queue == &mq);
    CHECK_EQ(osMotorStart(&pfs), 0);
    CHECK_EQ(mock_rumble(1), 1);
    CHECK_EQ(osMotorStop(&pfs), 0);
    CHECK_EQ(mock_rumble(1), 0);
    CHECK_EQ(MQ_GET_COUNT(&mq), 0); // nothing left on the SI queue

    Fill(&uninit, 0, sizeof(uninit));
    calls = mock_rumble_calls();
    CHECK_EQ(__osMotorAccess(&uninit, MOTOR_START), PFS_ERR_INVALID);
    CHECK_EQ(mock_rumble_calls(), calls);

    // Bad channel: a code padmgr.c accepts
    CHECK_EQ(osMotorInit(&mq, &pfs, MAXCONTROLLERS), PFS_ERR_NOPACK);
}

/* ---------------------------------------------------------------------------------------------- */

static gc_pad_t Pad(u16 buttons, s8 stickX, s8 stickY, s8 substickX, s8 substickY, u8 triggerL, u8 triggerR) {
    gc_pad_t pad;

    pad.buttons = buttons;
    pad.stickX = stickX;
    pad.stickY = stickY;
    pad.substickX = substickX;
    pad.substickY = substickY;
    pad.triggerL = triggerL;
    pad.triggerR = triggerR;
    pad.connected = 1;
    return pad;
}

static void ContSentinel(OSContPad* pad) {
    pad->button = 0x1234;
    pad->stick_x = 5;
    pad->stick_y = 6;
    pad->errno = 0x77;
}

/** One PadMgr_HandleRetrace read cycle: start, take the SI event, decode. */
static void ContReadCycle(OSMesgQueue* siQueue, OSContPad* data) {
    int si = mock_event_posts(OS_EVENT_SI);

    CHECK_EQ(osContStartReadData(siQueue), 0);
    CHECK_EQ(mock_event_posts(OS_EVENT_SI), si + 1);
    CHECK_EQ(osRecvMesg(siQueue, NULL, OS_MESG_NOBLOCK), 0);
    CHECK_EQ(MQ_GET_COUNT(siQueue), 0);
    osContGetReadData(data);
}

static void TestCont(void) {
    gc_pad_t pads[MAXCONTROLLERS];
    OSContStatus status[MAXCONTROLLERS];
    OSContPad data[MAXCONTROLLERS];
    OSMesgQueue siQueue;
    OSMesg siBuf[1];
    u8 pattern;
    int si0;
    int i;

    gc_log("== cont");
    // main.c: the SI event goes to the serial event queue padmgr.c hands to osCont*
    osCreateMesgQueue(&siQueue, siBuf, 1);
    osSetEventMesg(OS_EVENT_SI, &siQueue, NULL);
    si0 = mock_event_posts(OS_EVENT_SI);

    // osContInit (PadSetup_Init): port 0 always present, others as plugged in
    Fill(pads, 0, sizeof(pads));
    pads[1].connected = 1;
    pads[3].connected = 1;
    mock_set_pads(pads);
    Fill(status, 0xAB, sizeof(status));
    pattern = 0xFF;
    CHECK_EQ(osContInit(&siQueue, &pattern, status), 0);
    CHECK_EQ(pattern, 0x0B);
    for (i = 0; i < MAXCONTROLLERS; i++) {
        if (i == 2) {
            CHECK_EQ(status[i].errno, CONT_NO_RESPONSE_ERROR);
            CHECK_EQ(status[i].type, 0xABAB); // untouched, as libultra
            CHECK_EQ(status[i].status, 0xAB);
        } else {
            CHECK_EQ(status[i].errno, 0);
            CHECK_EQ(status[i].type, CONT_TYPE_NORMAL);
            CHECK_EQ(status[i].status, CONT_CARD_ON);
        }
    }
    CHECK_EQ(mock_event_posts(OS_EVENT_SI), si0); // osContInit consumes its own SI events
    CHECK_EQ(MQ_GET_COUNT(&siQueue), 0);
    pattern = 0xFF;
    CHECK_EQ(osContInit(&siQueue, &pattern, status), 0); // only the first call does anything
    CHECK_EQ(pattern, 0xFF);
    CHECK_EQ(osContSetCh(MAXCONTROLLERS), 0);

    // Digital buttons and stick clamping
    pads[1] = Pad(GC_PAD_A | GC_PAD_B | GC_PAD_START | GC_PAD_Z | GC_PAD_UP | GC_PAD_LEFT, 100, -100, 0, 0, 0, 0);
    pads[3] = Pad(GC_PAD_L | GC_PAD_R | GC_PAD_DOWN | GC_PAD_RIGHT | GC_PAD_X | GC_PAD_Y, 127, -128, 0, 0, 0, 0);
    mock_set_pads(pads);
    for (i = 0; i < MAXCONTROLLERS; i++) {
        ContSentinel(&data[i]);
    }
    ContReadCycle(&siQueue, data);
    CHECK_EQ(data[0].errno, 0); // port 0 with no pad: a neutral controller
    CHECK_EQ(data[0].button, 0);
    CHECK_EQ(data[0].stick_x, 0);
    CHECK_EQ(data[0].stick_y, 0);
    CHECK_EQ(data[1].errno, 0);
    CHECK_EQ(data[1].button, A_BUTTON | B_BUTTON | START_BUTTON | L_TRIG | U_JPAD | L_JPAD);
    CHECK_EQ(data[1].stick_x, 80);
    CHECK_EQ(data[1].stick_y, -80);
    CHECK_EQ(data[2].errno, CONT_NO_RESPONSE_ERROR);
    CHECK_EQ(data[2].button, 0x1234); // untouched, as libultra
    CHECK_EQ(data[2].stick_x, 5);
    CHECK_EQ(data[3].errno, 0);
    CHECK_EQ(data[3].button, Z_TRIG | R_TRIG | D_JPAD | R_JPAD | R_CBUTTONS | L_CBUTTONS);
    CHECK_EQ(data[3].stick_x, 80);
    CHECK_EQ(data[3].stick_y, -80);

    // Analog thresholds and the 0.8 stick scale
    pads[0] = Pad(GC_PAD_A, 40, 40, 40, -40, 0x60, 0x60);
    pads[1] = Pad(0, 50, -50, 41, -41, 0x61, 0x61);
    pads[3] = Pad(0, 7, -7, -41, 41, 0, 0xFF);
    mock_set_pads(pads);
    ContReadCycle(&siQueue, data);
    CHECK_EQ(data[0].button, A_BUTTON);
    CHECK_EQ(data[0].stick_x, 32);
    CHECK_EQ(data[0].stick_y, 32);
    CHECK_EQ(data[1].button, R_CBUTTONS | D_CBUTTONS | Z_TRIG | R_TRIG);
    CHECK_EQ(data[1].stick_x, 40);
    CHECK_EQ(data[1].stick_y, -40);
    CHECK_EQ(data[3].button, L_CBUTTONS | U_CBUTTONS | R_TRIG);
    CHECK_EQ(data[3].stick_x, 5);
    CHECK_EQ(data[3].stick_y, -5);

    // padmgr.c crashes on any errno other than 0, 0x4 and 0x8
    for (i = 0; i < MAXCONTROLLERS; i++) {
        CHECK((data[i].errno == 0) || (data[i].errno == 4) || (data[i].errno == 8));
    }

    // osContSetCh limits how many ports are decoded
    CHECK_EQ(osContSetCh(2), 0);
    ContSentinel(&data[2]);
    ContSentinel(&data[3]);
    ContReadCycle(&siQueue, data);
    CHECK_EQ(data[2].errno, 0x77);
    CHECK_EQ(data[3].button, 0x1234);
    CHECK_EQ(osContSetCh(7), 0); // clamped to MAXCONTROLLERS
    ContSentinel(&data[3]);
    ContReadCycle(&siQueue, data);
    CHECK_EQ(data[3].errno, 0);

    // Status query: port 1 unplugged, port 2 plugged in, port 0 still present without a pad
    pads[0].connected = 0;
    pads[1].connected = 0;
    pads[2] = Pad(0, 0, 0, 0, 0, 0, 0);
    mock_set_pads(pads);
    Fill(status, 0xAB, sizeof(status));
    CHECK_EQ(osContStartQuery(&siQueue), 0);
    CHECK_EQ(osRecvMesg(&siQueue, NULL, OS_MESG_NOBLOCK), 0);
    osContGetQuery(status);
    CHECK_EQ(status[0].errno, 0);
    CHECK_EQ(status[0].type, CONT_TYPE_NORMAL);
    CHECK_EQ(status[1].errno, CONT_NO_RESPONSE_ERROR);
    CHECK_EQ(status[2].errno, 0);
    CHECK_EQ(status[2].type & CONT_TYPE_MASK, CONT_TYPE_NORMAL);
    CHECK_EQ(status[3].errno, 0);
    ContReadCycle(&siQueue, data);
    CHECK_EQ(data[1].errno, CONT_NO_RESPONSE_ERROR);
    CHECK_EQ(data[0].errno, 0);
    CHECK_EQ(data[0].button, 0);

    osSetEventMesg(OS_EVENT_SI, NULL, NULL);
}

/* ---------------------------------------------------------------------------------------------- */

static void TestSp(void) {
    OSTask gfx;
    OSTask aud;
    OSTask jpeg;
    OSMesgQueue interruptQueue;
    OSMesg interruptBuf[8];
    OSMesg msg;
    int sp0 = mock_event_posts(OS_EVENT_SP);
    int dp0 = mock_event_posts(OS_EVENT_DP);

    gc_log("== sp");
    Fill(&gfx, 0, sizeof(gfx));
    Fill(&aud, 0, sizeof(aud));
    Fill(&jpeg, 0, sizeof(jpeg));
    gfx.t.type = M_GFXTASK;
    aud.t.type = M_AUDTASK;
    jpeg.t.type = M_NJPEGTASK;

    // sched.c registers both events on its interrupt queue
    osCreateMesgQueue(&interruptQueue, interruptBuf, ARRAY_COUNT(interruptBuf));
    osSetEventMesg(OS_EVENT_SP, &interruptQueue, (OSMesg)667);
    osSetEventMesg(OS_EVENT_DP, &interruptQueue, (OSMesg)668);

    // A graphics task: exactly one RSP-done and one RDP-done
    osSpTaskLoad(&gfx);
    osSpTaskStartGo(&gfx);
    CHECK_EQ(mock_event_posts(OS_EVENT_SP), sp0 + 1);
    CHECK_EQ(mock_event_posts(OS_EVENT_DP), dp0 + 1);
    CHECK_EQ(MQ_GET_COUNT(&interruptQueue), 2);
    osRecvMesg(&interruptQueue, &msg, OS_MESG_NOBLOCK);
    CHECK(msg == (OSMesg)667);
    osRecvMesg(&interruptQueue, &msg, OS_MESG_NOBLOCK);
    CHECK(msg == (OSMesg)668);

    // RSP-only tasks: one RSP-done each
    osSpTaskLoad(&aud);
    osSpTaskStartGo(&aud);
    osSpTaskLoad(&jpeg);
    osSpTaskStartGo(&jpeg);
    CHECK_EQ(mock_event_posts(OS_EVENT_SP), sp0 + 3);
    CHECK_EQ(mock_event_posts(OS_EVENT_DP), dp0 + 1);
    CHECK_EQ(MQ_GET_COUNT(&interruptQueue), 2);

    // Yield requests are never honoured: the task completes instead
    osSpTaskYield();
    gfx.t.flags = OS_TASK_DP_WAIT;
    CHECK_EQ(osSpTaskYielded(&gfx), 0);
    CHECK_EQ(gfx.t.flags, OS_TASK_DP_WAIT);

    // Status registers: RSP halted, RDP idle; osAfterPreNMI succeeds
    CHECK(__osSpGetStatus() & SP_STATUS_HALT);
    CHECK_EQ(osDpGetStatus() & (DPC_STATUS_CMD_BUSY | DPC_STATUS_PIPE_BUSY | DPC_STATUS_TMEM_BUSY | DPC_STATUS_DMA_BUSY),
             0);
    __osSpSetStatus(SP_SET_HALT | SP_SET_TASKDONE | SP_CLR_INTR_BREAK);
    osDpSetStatus(DPC_SET_FREEZE | DPC_SET_FLUSH);
    CHECK_EQ(osAfterPreNMI(), 0);

    osSetEventMesg(OS_EVENT_SP, NULL, NULL);
    osSetEventMesg(OS_EVENT_DP, NULL, NULL);
}

/* ---------------------------------------------------------------------------------------------- */

typedef struct {
    OSPiHandle* handle;
    OSIoMesg* mb;
} PiWriteArgs;

static void PiWriteDma(void* arg) {
    PiWriteArgs* args = arg;

    osEPiStartDma(args->handle, args->mb, OS_WRITE);
}

static void TestPi(void) {
    static u8 buf[0x100];
    OSPiHandle* handle;
    OSIoMesg mb;
    OSMesgQueue retQueue;
    OSMesg retBuf[2];
    OSMesgQueue cmdQueue;
    OSMesg cmdBuf[50];
    OSMesg msg;
    PiWriteArgs writeArgs;
    int reads0 = mock_rom_reads();
    u32 i;

    gc_log("== pi");
    handle = osCartRomInit();
    CHECK(handle == osCartRomInit());
    CHECK_EQ(handle->type, DEVICE_TYPE_CART);
    CHECK_EQ(handle->baseAddress, 0xB0000000);
    CHECK_EQ(handle->domain, PI_DOMAIN1);

    osCreateMesgQueue(&retQueue, retBuf, ARRAY_COUNT(retBuf));
    Fill(&mb, 0, sizeof(mb));
    mb.hdr.pri = OS_MESG_PRI_NORMAL;
    mb.hdr.retQueue = &retQueue;
    mb.devAddr = 0x1A500;
    mb.dramAddr = buf;
    mb.size = sizeof(buf);

    // libultra refuses DMAs before the PI manager exists
    CHECK_EQ(osEPiStartDma(handle, &mb, OS_READ), -1);
    CHECK_EQ(mock_rom_reads(), reads0);
    CHECK_EQ(MQ_GET_COUNT(&retQueue), 0);

    Fill(&cmdQueue, 0xEE, sizeof(cmdQueue));
    osCreatePiManager(OS_PRIORITY_PIMGR, &cmdQueue, cmdBuf, ARRAY_COUNT(cmdBuf));
    CHECK_EQ(cmdQueue.validCount, 0);
    CHECK_EQ(cmdQueue.msgCount, 50);

    // DmaMgr_DmaRomToRam: synchronous read at the ROM offset, then one message to retQueue
    CHECK_EQ(osEPiStartDma(handle, &mb, OS_READ), 0);
    CHECK_EQ(mock_rom_reads(), reads0 + 1);
    CHECK_EQ(mock_rom_last_offset(), 0x1A500);
    CHECK_EQ(mock_rom_last_size(), sizeof(buf));
    CHECK(mock_rom_last_dst() == buf);
    for (i = 0; i < sizeof(buf); i++) {
        if (buf[i] != mock_rom_byte(0x1A500 + i)) {
            mock_fail("pi: byte %lu differs", (unsigned long)i);
            break;
        }
    }
    CHECK_EQ(MQ_GET_COUNT(&retQueue), 1);
    osRecvMesg(&retQueue, &msg, OS_MESG_NOBLOCK);
    CHECK(msg == (OSMesg)&mb);
    CHECK_EQ(mb.hdr.type, OS_MESG_TYPE_EDMAREAD);
    CHECK(mb.piHandle == handle);

    // AudioLoad_Dma: writes cmdType into the shared handle and uses high priority
    handle->transferInfo.cmdType = 2;
    mb.hdr.pri = OS_MESG_PRI_HIGH;
    mb.devAddr = 0x97F70 + 0x20;
    CHECK_EQ(osEPiStartDma(handle, &mb, OS_READ), 0);
    CHECK_EQ(mock_rom_last_offset(), 0x97F70 + 0x20);
    CHECK_EQ(MQ_GET_COUNT(&retQueue), 1);
    osRecvMesg(&retQueue, NULL, OS_MESG_NOBLOCK);

    // A KSEG1 destination reaches the same RAM through KSEG0
    mock_rom_set_write(false);
    mb.dramAddr = (void*)0xA0123400;
    CHECK_EQ(osEPiStartDma(handle, &mb, OS_READ), 0);
    CHECK(mock_rom_last_dst() == (void*)0x80123400);
    osRecvMesg(&retQueue, NULL, OS_MESG_NOBLOCK);
    mock_rom_set_write(true);

    // No return queue: nothing is sent
    mb.dramAddr = buf;
    mb.hdr.retQueue = NULL;
    CHECK_EQ(osEPiStartDma(handle, &mb, OS_READ), 0);
    CHECK_EQ(MQ_GET_COUNT(&retQueue), 0);

    // Writes to the cartridge are not implemented
    writeArgs.handle = handle;
    writeArgs.mb = &mb;
    CHECK(mock_expect_halt(PiWriteDma, &writeArgs));
}

/* ---------------------------------------------------------------------------------------------- */

static int sVsyncs;

static void Vsync(void) {
    mock_vsync();
    sVsyncs++;
}

static void Drain(OSMesgQueue* mq) {
    while (osRecvMesg(mq, NULL, OS_MESG_NOBLOCK) == 0) {}
}

static void TestVi(void) {
    static u16 fbA[16];
    static u16 fbB[16];
    OSMesgQueue irqQueue;
    OSMesg irqBuf[1];
    OSMesgQueue slowQueue;
    OSMesg slowBuf[8];
    OSMesg msg;
    int threads0 = mock_threads_created();
    int blackCalls;
    int vi0 = mock_event_posts(OS_EVENT_VI);
    int i;

    gc_log("== vi");
    osCreateMesgQueue(&irqQueue, irqBuf, ARRAY_COUNT(irqBuf));

    // gc_ultra_boot() starts the service thread; a second call does nothing
    __gcViInit();
    __gcViInit();
    CHECK_EQ(mock_threads_created(), threads0 + 1);
    CHECK_EQ(mock_thread_prio(threads0), GC_PRIO_SERVICE_VI);

    // Before osCreateViManager retraces only raise OS_EVENT_VI
    osViSwapBuffer(fbA);
    osViSetEvent(&irqQueue, (OSMesg)666, 1);
    Vsync();
    CHECK_EQ(mock_event_posts(OS_EVENT_VI), vi0 + sVsyncs);
    CHECK_EQ(MQ_GET_COUNT(&irqQueue), 0);
    CHECK(osViGetCurrentFramebuffer() == (void*)K0BASE);
    CHECK(osViGetNextFramebuffer() == fbA);
    CHECK_EQ(mock_black_calls(), 0);

    // Idle_ThreadEntry: osCreateViManager does not start a second thread
    osCreateViManager(OS_PRIORITY_VIMGR);
    osCreateViManager(OS_PRIORITY_VIMGR);
    CHECK_EQ(mock_threads_created(), threads0 + 1);

    // The next context becomes current at the retrace; IrqMgr gets its retrace message
    Vsync();
    CHECK(osViGetCurrentFramebuffer() == fbA);
    CHECK(osViGetNextFramebuffer() == fbA);
    CHECK_EQ(MQ_GET_COUNT(&irqQueue), 1);
    osRecvMesg(&irqQueue, &msg, OS_MESG_NOBLOCK);
    CHECK(msg == (OSMesg)666);
    // The N64 VI starts blanked
    CHECK_EQ(mock_black_calls(), 1);
    CHECK_EQ(mock_black_last(), 1);

    // Sched: a swapped buffer only becomes current after the following retrace
    osViSwapBuffer(fbB);
    CHECK(osViGetCurrentFramebuffer() == fbA);
    CHECK(osViGetNextFramebuffer() == fbB);
    Vsync();
    CHECK(osViGetCurrentFramebuffer() == fbB);
    CHECK(osViGetNextFramebuffer() == fbB);
    Drain(&irqQueue);

    // A full client queue drops the message; the VI thread does not block
    Vsync();
    Vsync();
    CHECK_EQ(MQ_GET_COUNT(&irqQueue), 1);
    Drain(&irqQueue);

    // osViBlack takes effect at the retrace; gc_video_set_black only on changes
    blackCalls = mock_black_calls();
    osViBlack(false);
    CHECK_EQ(mock_black_calls(), blackCalls);
    Vsync();
    CHECK_EQ(mock_black_calls(), blackCalls + 1);
    CHECK_EQ(mock_black_last(), 0);
    Vsync();
    CHECK_EQ(mock_black_calls(), blackCalls + 1);
    osViBlack(true);
    Vsync();
    CHECK_EQ(mock_black_last(), 1);
    // ViConfig_UpdateVi: osViSetMode clears the black state, as in libultra
    osViSetMode(&osViModeNtscLan1);
    osViSetSpecialFeatures(OS_VI_DITHER_FILTER_ON | OS_VI_GAMMA_OFF);
    osViSetXScale(1.0f);
    osViSetYScale(1.0f);
    osViExtendVStart(0);
    Vsync();
    CHECK_EQ(mock_black_last(), 0);
    CHECK(osViGetCurrentFramebuffer() == fbB);
    Drain(&irqQueue);

    // retraceCount 3: a message at the first retrace after the change, then every third one
    osCreateMesgQueue(&slowQueue, slowBuf, ARRAY_COUNT(slowBuf));
    osViSetEvent(&slowQueue, (OSMesg)777, 3);
    for (i = 0; i < 7; i++) {
        Vsync();
    }
    CHECK_EQ(MQ_GET_COUNT(&slowQueue), 3);
    CHECK_EQ(MQ_GET_COUNT(&irqQueue), 0);
    CHECK_EQ(mock_event_posts(OS_EVENT_VI), vi0 + sVsyncs);

    // Leave the VI posting to a queue that outlives this function
    osViSetEvent(NULL, NULL, 1);
    Vsync();
}

/* ---------------------------------------------------------------------------------------------- */

#define FLASH_PAGES (FLASH_SIZE / FLASH_BLOCK_SIZE)

static const char* ScenarioName(void) {
    if (mock_save_error()) {
        return "unreadable save";
    }
    return mock_has_initial_save() ? "existing save" : "no save";
}

static u8 sFlashExpect[FLASH_SIZE];
static u8 sFlashRead[FLASH_SIZE];
static u8 sFlashData[FLASH_BLOCK_SIZE * 0x80];

/** SysFlashrom_Read: one osFlashReadArray, then exactly one message on the queue. */
static void FlashRead(OSMesgQueue* mq, u32 pageNum, void* dst, u32 pageCount) {
    OSIoMesg mb;
    OSMesg msg = NULL;

    CHECK_EQ(osFlashReadArray(&mb, OS_MESG_PRI_NORMAL, pageNum, dst, pageCount, mq), 0);
    CHECK_EQ(MQ_GET_COUNT(mq), 1);
    osRecvMesg(mq, &msg, OS_MESG_NOBLOCK);
    CHECK(msg == (OSMesg)&mb);
}

/** SysFlashrom_ExecWrite for whole pages. */
static void FlashWrite(OSMesgQueue* mq, u32 pageNum, const u8* src, u32 pageCount) {
    OSIoMesg mb;
    OSMesg msg = NULL;
    u32 i;

    for (i = 0; i < pageCount; i++) {
        CHECK_EQ(osFlashWriteBuffer(&mb, OS_MESG_PRI_NORMAL, (void*)(src + i * FLASH_BLOCK_SIZE), mq), 0);
        CHECK_EQ(MQ_GET_COUNT(mq), 1);
        osRecvMesg(mq, &msg, OS_MESG_NOBLOCK);
        CHECK(msg == (OSMesg)&mb);
        CHECK_EQ(osFlashWriteArray(pageNum + i), FLASH_STATUS_WRITE_OK);
        CHECK_EQ(MQ_GET_COUNT(mq), 0);
    }
}

/** Wait until gc_save_store has been called `attempts` times in total; returns the ms waited. */
static unsigned int WaitForStoreAttempts(int attempts, unsigned int timeoutMs) {
    unsigned int start = mock_now_ms();

    while ((mock_now_ms() - start < timeoutMs) && (mock_save_store_attempts() < attempts)) {
        mock_sleep_ms(10);
    }
    return mock_now_ms() - start;
}

static void TestFlash(void) {
    OSMesgQueue mq;
    OSMesg mqBuf[1];
    OSPiHandle* handle;
    u32 flashType;
    u32 flashVendor;
    u8 status;
    int threads0 = mock_threads_created();
    int stores0;
    int attempts0;
    unsigned int changedAt;
    unsigned int waited;
    u32 i;

    gc_log("== flash (%s)", ScenarioName());
    osCreateMesgQueue(&mq, mqBuf, ARRAY_COUNT(mqBuf)); // sFlashromMesgQueue has one slot

    // SysFlashrom_InitFlash
    handle = osFlashInit();
    CHECK(handle != NULL);
    CHECK(osFlashInit() == handle);
    CHECK_EQ(handle->type, DEVICE_TYPE_FLASH);
    CHECK_EQ(mock_save_loads(), 1);
    CHECK_EQ(mock_threads_created(), threads0 + 1);
    CHECK_EQ(mock_thread_prio(threads0), GC_PRIO_GAME_MIN);
    CHECK(mock_thread_stack(threads0) >= 0x4000);
    osFlashReadId(&flashType, &flashVendor);
    CHECK_EQ(flashType, 0x11118001);
    CHECK((flashVendor == FLASH_VERSION_MX_PROTO_A) || (flashVendor == FLASH_VERSION_MX_A) ||
          (flashVendor == FLASH_VERSION_MX_C) || (flashVendor == FLASH_VERSION_MEI) ||
          (flashVendor == FLASH_VERSION_MX_B_AND_D));
    osFlashReadStatus(&status);
    CHECK_EQ(status, 0);

    // Initial contents: the save from SD, or an erased chip
    for (i = 0; i < FLASH_SIZE; i++) {
        sFlashExpect[i] = mock_has_initial_save() ? mock_initial_save_byte(i) : 0xFF;
    }
    FlashRead(&mq, 0, sFlashRead, FLASH_PAGES);
    CHECK(Equal(sFlashRead, sFlashExpect, FLASH_SIZE));

    // SysFlashrom_AttemptWrite: erase the sector, program 0x80 pages, read back
    for (i = 0; i < sizeof(sFlashData); i++) {
        sFlashData[i] = (u8)(i * 31 + 7);
    }
    CHECK_EQ(osFlashSectorErase(0x80), FLASH_STATUS_ERASE_OK);
    FlashWrite(&mq, 0x80, sFlashData, 0x80);
    osFlashReadStatus(&status);
    CHECK_EQ(status, 0); // the driver ends every operation with osFlashClearStatus
    FlashRead(&mq, 0x80, sFlashRead, 0x80);
    CHECK(Equal(sFlashRead, sFlashData, sizeof(sFlashData)));
    for (i = 0; i < sizeof(sFlashData); i++) {
        sFlashExpect[0x80 * FLASH_BLOCK_SIZE + i] = sFlashData[i];
    }
    // The neighbouring sectors are untouched
    FlashRead(&mq, 0x7F, sFlashRead, 1);
    CHECK(Equal(sFlashRead, &sFlashExpect[0x7F * FLASH_BLOCK_SIZE], FLASH_BLOCK_SIZE));
    FlashRead(&mq, 0x100, sFlashRead, 1);
    CHECK(Equal(sFlashRead, &sFlashExpect[0x100 * FLASH_BLOCK_SIZE], FLASH_BLOCK_SIZE));

    // Programming without an erase can only clear bits
    Fill(sFlashRead, 0x0F, FLASH_BLOCK_SIZE);
    FlashWrite(&mq, 0x81, sFlashRead, 1);
    FlashRead(&mq, 0x81, sFlashRead, 1);
    for (i = 0; i < FLASH_BLOCK_SIZE; i++) {
        sFlashExpect[0x81 * FLASH_BLOCK_SIZE + i] &= 0x0F;
    }
    CHECK(Equal(sFlashRead, &sFlashExpect[0x81 * FLASH_BLOCK_SIZE], FLASH_BLOCK_SIZE));

    // Out-of-range operations fail and change nothing
    CHECK_EQ(osFlashWriteArray(FLASH_PAGES), FLASH_STATUS_WRITE_ERROR);
    CHECK_EQ(osFlashSectorErase(FLASH_PAGES), FLASH_STATUS_ERASE_ERROR);
    // A read past the end is padded with 0xFF and still completes once
    FlashRead(&mq, FLASH_PAGES - 1, sFlashRead, 2);
    CHECK(Equal(sFlashRead, &sFlashExpect[(FLASH_PAGES - 1) * FLASH_BLOCK_SIZE], FLASH_BLOCK_SIZE));
    CHECK(AllBytes(sFlashRead + FLASH_BLOCK_SIZE, 0xFF, FLASH_BLOCK_SIZE));

    // An erase from a page inside a sector erases the whole 16 KiB sector (pages 0x80-0xFF)
    stores0 = mock_save_stores();
    CHECK_EQ(stores0, 0);
    CHECK_EQ(osFlashSectorErase(0x85), FLASH_STATUS_ERASE_OK);
    changedAt = mock_now_ms();
    for (i = 0x80 * FLASH_BLOCK_SIZE; i < 0x100 * FLASH_BLOCK_SIZE; i++) {
        sFlashExpect[i] = 0xFF;
    }
    FlashRead(&mq, 0, sFlashRead, FLASH_PAGES);
    CHECK(Equal(sFlashRead, sFlashExpect, FLASH_SIZE));

    if (mock_save_error()) {
        // The save on SD exists but could not be read: it is never overwritten
        mock_sleep_ms(1500);
        __gcFlashFlush();
        CHECK_EQ(mock_save_stores(), 0);
    } else {
        // Persistence: nothing within 1 s of the last change, then exactly one store
        mock_sleep_ms(400);
        CHECK_EQ(mock_save_stores(), stores0);
        while ((mock_now_ms() - changedAt < 4000) && (mock_save_stores() == stores0)) {
            mock_sleep_ms(10);
        }
        waited = mock_now_ms() - changedAt;
        gc_log("flash: persisted %u ms after the last change", waited);
        CHECK_EQ(mock_save_stores(), stores0 + 1);
        // The upper bound leaves room for a loaded machine (ASan, parallel builds)
        CHECK((waited >= 990) && (waited <= 3000));
        CHECK(Equal(mock_saved_image(), sFlashExpect, FLASH_SIZE));
        mock_sleep_ms(1500);
        CHECK_EQ(mock_save_stores(), stores0 + 1);

        // osFlashAllErase, then the reset-path flush stores synchronously, once
        CHECK_EQ(osFlashAllErase(), FLASH_STATUS_ERASE_OK);
        __gcFlashFlush();
        CHECK_EQ(mock_save_stores(), stores0 + 2);
        CHECK(AllBytes(mock_saved_image(), 0xFF, FLASH_SIZE));
        __gcFlashFlush();
        mock_sleep_ms(1500); // the writer wakes for the erase but finds nothing left to store
        CHECK_EQ(mock_save_stores(), stores0 + 2);

        // A failed SD write is retried a second later, not only after the game's next save
        attempts0 = mock_save_store_attempts();
        mock_save_fail_next(1);
        FlashWrite(&mq, 0x300, sFlashData, 1);
        waited = WaitForStoreAttempts(attempts0 + 2, 6000);
        gc_log("flash: stored after one failed write, %u ms after the change", waited);
        CHECK_EQ(mock_save_store_attempts(), attempts0 + 2);
        CHECK_EQ(mock_save_stores(), stores0 + 3);
        CHECK((waited >= 1980) && (waited <= 4500));
        CHECK(Equal(mock_saved_image() + 0x300 * FLASH_BLOCK_SIZE, sFlashData, FLASH_BLOCK_SIZE));

        // ... but not forever: after the first attempt and 3 retries it waits for the next change
        mock_save_fail_next(100);
        FlashWrite(&mq, 0x301, sFlashData + FLASH_BLOCK_SIZE, 1);
        WaitForStoreAttempts(attempts0 + 6, 9000);
        mock_sleep_ms(1500);
        CHECK_EQ(mock_save_store_attempts(), attempts0 + 6);
        CHECK_EQ(mock_save_stores(), stores0 + 3);
        mock_save_fail_next(0);
        FlashWrite(&mq, 0x302, sFlashData + 2 * FLASH_BLOCK_SIZE, 1);
        WaitForStoreAttempts(attempts0 + 7, 4000);
        CHECK_EQ(mock_save_stores(), stores0 + 4);
        CHECK(Equal(mock_saved_image() + 0x300 * FLASH_BLOCK_SIZE, sFlashData, 3 * FLASH_BLOCK_SIZE));
    }

    osFlashClearStatus();
    osFlashReadStatus(&status);
    CHECK_EQ(status, 0);
}

int main(void) {
    int failures;

    TestCache();
    TestAi();
    TestVoice();
    TestMotor();
    TestCont();
    TestSp();
    TestPi();
    TestVi();
    TestFlash();

    failures = mock_failures();
    if (failures == 0) {
        gc_log("ALL TESTS PASSED (%s)", ScenarioName());
    } else {
        gc_log("%d CHECKS FAILED", failures);
    }
    return failures != 0;
}
