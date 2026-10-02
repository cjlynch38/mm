/**
 * Layout checks for structures the game reads directly from ROM data or shares with libultra.
 * The GameCube build reads the original N64 bytes unchanged (both CPUs are big-endian), so these
 * structures must have exactly the sizes and field offsets the IDO-compiled N64 build had.
 */
#include "global.h"
#include "assert.h"
#include "stddef.h"
#include "z64dma.h"
#include "audio/soundfont.h"

#define CHECK_SIZE(type, size) static_assert(sizeof(type) == (size), "sizeof(" #type ") != " #size)
#define CHECK_OFFSET(type, field, offset) \
    static_assert(offsetof(type, field) == (offset), "offsetof(" #type ", " #field ") != " #offset)

// ROM file system
CHECK_SIZE(DmaEntry, 0x10);
CHECK_SIZE(RomFile, 0x8);

// Graphics data embedded in assets
CHECK_SIZE(Gfx, 0x8);
CHECK_SIZE(Vtx, 0x10);
CHECK_SIZE(Mtx, 0x40);

// Scene and room data
CHECK_SIZE(SceneCmd, 0x8);
CHECK_SIZE(RoomList, 0x8);
CHECK_SIZE(ActorEntry, 0x10);
CHECK_SIZE(EnvLightSettings, 0x16);
CHECK_SIZE(CollisionHeader, 0x2C);
CHECK_SIZE(CollisionPoly, 0x10);
CHECK_SIZE(BgCamInfo, 0x8);

// Animation data
CHECK_SIZE(SkeletonHeader, 0x8);
CHECK_SIZE(AnimationHeader, 0x10);

// Audio data (soundfonts are relocated in place from ROM bytes)
CHECK_SIZE(Sample, 0x10);
CHECK_SIZE(TunedSample, 0x8);
CHECK_SIZE(Instrument, 0x20);
CHECK_SIZE(Drum, 0x10);
CHECK_SIZE(SoundEffect, 0x8);

// libultra structures whose fields the game reads directly
CHECK_SIZE(OSMesgQueue, 0x18);
CHECK_OFFSET(OSMesgQueue, validCount, 0x08);
CHECK_OFFSET(OSMesgQueue, first, 0x0C);
CHECK_OFFSET(OSMesgQueue, msgCount, 0x10);
CHECK_SIZE(OSTimer, 0x20);
CHECK_SIZE(OSThread, 0x1B0);
