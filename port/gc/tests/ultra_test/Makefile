# ultra_test: stand-alone test DOL for the libultra OS core (port/gc/ultra: threads, message
# queues, events, interrupt mask, time, timers) on the real libogc bridge (port/gc/ogc).
#
#   make -C port/gc/tests/ultra_test             (inside WSL; the N64 build must exist)
#   port/gc/tools/run_dolphin.sh port/gc/tests/ultra_test/ultra_test.dol -NoSd -Seconds 25
#
# All objects are compiled by Makefile.gc, so they get exactly the game/platform flags;
# this file only links them (libogc crt0 + linker script via -mogc), runs elf2dol and pads
# the DOL for Dolphin.

ROOT      := $(abspath $(CURDIR)/../../../..)
DEVKITPRO ?= /opt/devkitpro
DEVKITPPC ?= $(DEVKITPRO)/devkitPPC
CC        := $(DEVKITPPC)/bin/powerpc-eabi-gcc
ELF2DOL   := $(DEVKITPRO)/tools/bin/elf2dol
DOLPAD    := python3 $(ROOT)/port/gc/tools/dolpad.py
BUILD_DIR := build/gc-n64-us

# The OS core under test. The test provides stand-ins for bootproc() and __gcViInit().
ULTRA_O := $(addprefix $(BUILD_DIR)/port/gc/ultra/,thread.o mesg.o event.o intmask.o time.o timer.o globals.o \
                                                    boot.o)
# The parts of the bridge the core uses (no SD, ROM, pads or saves)
OGC_O   := $(addprefix $(BUILD_DIR)/port/gc/ogc/,bridge_log.o bridge_sync.o bridge_thread.o bridge_time.o \
                                                  bridge_mem.o bridge_video.o bridge_reset.o)
# The game's own usleep/msleep (osSetTimer + osRecvMesg)
GAME_O  := $(BUILD_DIR)/src/boot/libc64/sleep.o
TEST_O  := $(BUILD_DIR)/port/gc/tests/ultra_test/ultra_test.o

ALL_O := $(ULTRA_O) $(OGC_O) $(GAME_O) $(TEST_O)

.PHONY: all objects clean

all: ultra_test.dol

objects:
	$(MAKE) --no-print-directory -C $(ROOT) -f Makefile.gc $(ALL_O)

ultra_test.elf: objects
	$(CC) -mogc -mcpu=750 -meabi -mhard-float -Wl,-Map,ultra_test.map $(addprefix $(ROOT)/,$(ALL_O)) \
	    -L$(DEVKITPRO)/libogc/lib/cube -logc -lm -o $@

ultra_test.dol: ultra_test.elf
	$(ELF2DOL) $< $@
	$(DOLPAD) $@

clean:
	rm -f ultra_test.elf ultra_test.dol ultra_test.map
	rm -f $(ROOT)/$(TEST_O) $(ROOT)/$(TEST_O:.o=.d)
