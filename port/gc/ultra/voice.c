/**
 * libultra Voice Recognition Unit API (osVoice*) for the GameCube: there is never a VRU.
 *
 * cont.c never reports CONT_TYPE_VOICE, so padmgr.c never initialises one and the game should not
 * reach these. If it does, every call fails the way a missing device does, without touching the
 * handle or any output buffer.
 */
#include "gc_ultra_internal.h"
#include "PR/os_voice.h"

s32 osVoiceInit(OSMesgQueue* mq, OSVoiceHandle* hd, int channel) {
    return CONT_ERR_NO_CONTROLLER;
}

s32 osVoiceSetWord(OSVoiceHandle* hd, u8* word) {
    return CONT_ERR_NO_CONTROLLER;
}

s32 osVoiceCheckWord(u8* word) {
    return CONT_ERR_NO_CONTROLLER;
}

s32 osVoiceStartReadData(OSVoiceHandle* hd) {
    return CONT_ERR_NO_CONTROLLER;
}

s32 osVoiceStopReadData(OSVoiceHandle* hd) {
    return CONT_ERR_NO_CONTROLLER;
}

s32 osVoiceGetReadData(OSVoiceHandle* hd, OSVoiceData* result) {
    return CONT_ERR_NO_CONTROLLER;
}

s32 osVoiceClearDictionary(OSVoiceHandle* hd, u8 numWords) {
    return CONT_ERR_NO_CONTROLLER;
}

s32 osVoiceMaskDictionary(OSVoiceHandle* hd, u8* maskPattern, int size) {
    return CONT_ERR_NO_CONTROLLER;
}

s32 osVoiceControlGain(OSVoiceHandle* hd, s32 analog, s32 digital) {
    return CONT_ERR_NO_CONTROLLER;
}
