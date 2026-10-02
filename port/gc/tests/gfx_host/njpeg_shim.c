/**
 * Glue for test_njpeg.c: builds z_jpeg.c's task (OSTask, JpegTaskData) with the game's own headers, which the
 * test itself (host C library headers) cannot include, and runs Gc_NJpegRunTask (port/gc/game/njpeg_cpu.c).
 */
#include "ultra64.h"
#include "z64jpeg.h"
#include "gc_game.h"

void NJpegTest_Run(void* address, u32 mbCount, u32 mode, void* qY, void* qU, void* qV);

void NJpegTest_Run(void* address, u32 mbCount, u32 mode, void* qY, void* qU, void* qV) {
    JpegTaskData data;
    OSTask task;

    bzero(&data, sizeof(data));
    bzero(&task, sizeof(task));
    data.address = address;
    data.mbCount = mbCount;
    data.mode = mode;
    data.qTableYPtr = qY;
    data.qTableUPtr = qU;
    data.qTableVPtr = qV;
    task.t.type = M_NJPEGTASK;
    task.t.data_ptr = (u64*)&data;
    task.t.data_size = sizeof(data);
    Gc_NJpegRunTask(&task);
}
