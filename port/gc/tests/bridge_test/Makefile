# Stand-alone test DOLs for the libogc half of the platform layer (port/gc/ogc), independent of
# Makefile.gc and the game. Same compiler flags as OGC_CFLAGS in Makefile.gc.
#
#   bridge_test.dol  every port/gc/ogc/*.c except main.c, with bridge_test.c (the bridge tests)
#   boot_test.dol    every port/gc/ogc/*.c including main.c, with boot_stub.c standing in for
#                    the libultra side's gc_ultra_boot (the real boot sequence)
#
#   make -C port/gc/tests/bridge_test            (inside WSL)
#   port/gc/tools/run_dolphin.sh port/gc/tests/bridge_test/build/bridge_test.dol -Seconds 25
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

OGC_OBJS    := $(patsubst $(OGC_DIR)/%.c,$(BUILD)/ogc/%.o,$(filter-out $(OGC_DIR)/main.c,$(wildcard $(OGC_DIR)/*.c)))
BRIDGE_OBJS := $(OGC_OBJS) $(BUILD)/bridge_test.o
BOOT_OBJS   := $(OGC_OBJS) $(BUILD)/ogc/main.o $(BUILD)/boot_stub.o

CFLAGS  := $(MACHDEP) -O2 -std=gnu17 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing \
           -I$(LIBOGC_INC) -I$(TOP)/port/gc/include -I$(OGC_DIR)
LDFLAGS := $(MACHDEP)
LIBS    := -L$(LIBOGC_LIB) -liso9660 -lfat -logc -lm

.PHONY: all clean
all: $(BUILD)/bridge_test.dol $(BUILD)/boot_test.dol

%.dol: %.elf
	$(DEVKITPRO)/tools/bin/elf2dol $< $@
	python3 $(TOP)/port/gc/tools/dolpad.py $@

$(BUILD)/bridge_test.elf: $(BRIDGE_OBJS)
	$(CC) $(LDFLAGS) -Wl,-Map,$(@:.elf=.map) $^ $(LIBS) -o $@

$(BUILD)/boot_test.elf: $(BOOT_OBJS)
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
