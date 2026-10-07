# sd_ramdisk.dol: the SD card code of port/gc/ogc (raw ROM reads, the SD lock, save files) on the real libfat, with a
# FAT16 and then a sparse FAT32 RAM disk mounted as sd: (Dolphin has no GameCube SD card). Links every
# port/gc/ogc/*.c except main.c, with the same compiler flags as OGC_CFLAGS in Makefile.gc.
#
#   make -C port/gc/tests/sd_ramdisk            (inside WSL)
#   port/gc/tools/run_dolphin.sh port/gc/tests/sd_ramdisk/build/sd_ramdisk.dol -Seconds 30 -NoSd
#
# The log (USB Gecko) ends with "sd_ramdisk: RESULT PASS".
.SUFFIXES:

DEVKITPRO ?= /opt/devkitpro
DEVKITPPC ?= $(DEVKITPRO)/devkitPPC
export DEVKITPRO DEVKITPPC
include $(DEVKITPPC)/gamecube_rules

TOP     := $(abspath $(CURDIR)/../../../..)
OGC_DIR := $(TOP)/port/gc/ogc
BUILD   := build

OGC_OBJS := $(patsubst $(OGC_DIR)/%.c,$(BUILD)/ogc/%.o,$(filter-out $(OGC_DIR)/main.c,$(wildcard $(OGC_DIR)/*.c)))
OBJS     := $(OGC_OBJS) $(BUILD)/sd_ramdisk.o

CFLAGS  := $(MACHDEP) -O2 -std=gnu17 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing \
           -I$(LIBOGC_INC) -I$(TOP)/port/gc/include -I$(OGC_DIR)
LDFLAGS := $(MACHDEP)
LIBS    := -L$(LIBOGC_LIB) -liso9660 -lfat -logc -lm

.PHONY: all clean
all: $(BUILD)/sd_ramdisk.dol

%.dol: %.elf
	$(DEVKITPRO)/tools/bin/elf2dol $< $@
	python3 $(TOP)/port/gc/tools/dolpad.py $@

$(BUILD)/sd_ramdisk.elf: $(OBJS)
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
