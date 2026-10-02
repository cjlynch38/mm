/**
 * libultra Rumble Pak API (osMotorInit, __osMotorAccess behind osMotorStart/osMotorStop) on the
 * GameCube controllers' built-in rumble motors.
 *
 * cont.c reports a controller pak on every connected port, and padmgr.c only calls these for
 * ports it sees as standard controllers with a pak, so osMotorInit always finds a "rumble pak".
 * On N64 both functions perform SI transfers on the given queue and wait for their own events;
 * nothing is posted to the queue here, which leaves it in the same state.
 * padmgr.c accepts only 0, PFS_ERR_DEVICE, PFS_ERR_CONTRFAIL and PFS_ERR_NOPACK from osMotorInit.
 */
#include "gc_ultra_internal.h"
#include "PR/os_motor.h"

#define GC_MOTOR_BANK 0x80 // MOTOR_ID: the bank osMotorInit leaves selected on a rumble pak

s32 osMotorInit(OSMesgQueue* mq, OSPfs* pfs, s32 channel) {
    pfs->queue = mq;
    pfs->channel = channel;
    pfs->activebank = 0xFF;
    pfs->status = 0;

    if ((channel < 0) || (channel >= MAXCONTROLLERS)) {
        return PFS_ERR_NOPACK;
    }

    pfs->activebank = GC_MOTOR_BANK;
    pfs->status = PFS_MOTOR_INITIALIZED;
    return 0;
}

s32 __osMotorAccess(OSPfs* pfs, s32 flag) {
    if (!(pfs->status & PFS_MOTOR_INITIALIZED)) {
        return PFS_ERR_INVALID;
    }

    gc_pad_rumble(pfs->channel, flag != MOTOR_STOP);
    return 0;
}
