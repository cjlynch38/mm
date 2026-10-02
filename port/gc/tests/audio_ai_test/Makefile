# Stand-alone test DOL for the audio output path of the libogc bridge (port/gc/ogc/bridge_audio.c),
# independent of Makefile.gc and the game. Same compiler flags as OGC_CFLAGS in Makefile.gc.
#
#   audio_ai_test.dol  every port/gc/ogc/*.c except main.c, with audio_ai_test.c (a sine-wave producer
#                      paced like the game's audio thread)
#
#   make -C port/gc/tests/audio_ai_test            (inside WSL)
#   cp port/gc/tests/audio_ai_test/build/audio_ai_test.dol /mnt/c/_mmgcport/dolphin/
#   port/gc/tools/run_dolphin.sh 'C:\_mmgcport\dolphin\audio_ai_test.dol' -Seconds 25 -NoSd \
#       -DumpAudio 'C:\_mmgcport\dolphin\audio_ai_test.wav' -ExtraConfig 'Dolphin.DSP.Volume=0'
#   python3 port/gc/tests/audio_ai_test/analyze_wav.py /mnt/c/_mmgcport/dolphin/audio_ai_test.wav --tone 440,1000
#
# Outputs go to build/ and are padded for Dolphin by port/gc/tools/dolpad.py.
.SUFFIXES:

DEVKITPRO ?= /opt/devkitpro
DEVKITPPC ?= $(DEVKITPRO)/devkitPPC
export DEVKITPRO DEVKITPPC
include $(DEVKITPPC)/gamecube_rules

TOP     := $(abspath $(CURDIR)/../../../..)
OGC_DIR := $(TOP)/port/gc/ogc
BUILD   := build

OGC_OBJS  := $(patsubst $(OGC_DIR)/%.c,$(BUILD)/ogc/%.o,$(filter-out $(OGC_DIR)/main.c,$(wildcard $(OGC_DIR)/*.c)))
TEST_OBJS := $(OGC_OBJS) $(BUILD)/audio_ai_test.o

CFLAGS  := $(MACHDEP) -O2 -std=gnu17 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing \
           -I$(LIBOGC_INC) -I$(TOP)/port/gc/include -I$(OGC_DIR)
LDFLAGS := $(MACHDEP)
LIBS    := -L$(LIBOGC_LIB) -liso9660 -lfat -logc -lm

.PHONY: all clean
all: $(BUILD)/audio_ai_test.dol

%.dol: %.elf
	$(DEVKITPRO)/tools/bin/elf2dol $< $@
	python3 $(TOP)/port/gc/tools/dolpad.py $@

$(BUILD)/audio_ai_test.elf: $(TEST_OBJS)
	$(CC) $(LDFLAGS) -Wl,-Map,$(@:.elf=.map) $^ $(LIBS) -o $@

$(BUILD)/ogc/%.o: $(OGC_DIR)/%.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD)

.PRECIOUS: %.elf
-include $(wildcard $(BUILD)/*.d $(BUILD)/ogc/*.d)
