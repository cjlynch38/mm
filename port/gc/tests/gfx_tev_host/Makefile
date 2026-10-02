# Tests of the combiner/blender translation in port/gc/gfx/gfx_tev.c. Inside Linux/WSL:
#
#   make -C port/gc/tests/gfx_tev_host run    host test
#   make -C port/gc/tests/gfx_tev_host dol    Dolphin test DOL (build/gc-host-tests/gfx_tev_host/dol/dol_tev.dol)
#
# Host test: gfx_tev.c is compiled with the host gcc against a stand-in <gccore.h> (this directory) and a GX
# mock that records its calls and models the GameCube pixel pipeline; test_tev.c compares that against a model
# of the N64 RDP (rdp_model.c) for the combine and render modes MM uses (tev_cases.h). Runs with
# AddressSanitizer and UBSan.
#
# Dolphin test: gfx_tev.c with the real libogc, drawing the same tables through GX and reading the pixels
# back (dol_tev.c). It needs Dolphin's software renderer with EFB copies to RAM; see dol_tev.c.

ROOT  := $(abspath $(CURDIR)/../../../..)
BUILD := $(ROOT)/build/gc-host-tests/gfx_tev_host
TEST  := $(BUILD)/test_tev

CC       := gcc
SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
CFLAGS   := -std=gnu17 -O1 -g $(SANITIZE) -Wall -Wextra -Wno-unused-parameter -Werror
# This directory first: its gccore.h replaces libogc's
CPPFLAGS := -I$(CURDIR) -I$(ROOT)/port/gc/gfx -I$(ROOT)/port/gc/include

OBJS := $(BUILD)/gfx_tev.o $(BUILD)/mock_gx.o $(BUILD)/rdp_model.o $(BUILD)/test_tev.o
HDRS := gccore.h mock_gx.h rdp_model.h tev_cases.h $(ROOT)/port/gc/gfx/gfx_internal.h $(ROOT)/port/gc/gfx/gfx_gbi.h

.PHONY: all run dol clean
all: $(TEST)

run: $(TEST)
	$(TEST)

clean:
	rm -rf $(BUILD)

$(TEST): $(OBJS)
	$(CC) $(SANITIZE) -o $@ $^

$(BUILD)/gfx_tev.o: $(ROOT)/port/gc/gfx/gfx_tev.c $(HDRS) | $(BUILD)
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<

$(BUILD)/%.o: %.c $(HDRS) | $(BUILD)
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<

$(BUILD):
	mkdir -p $@

# Dolphin test DOL: devkitPPC with Makefile.gc's OGC_CFLAGS (libogc's gccore.h, not the stand-in)
DEVKITPRO  ?= /opt/devkitpro
PPC_CC     := $(DEVKITPRO)/devkitPPC/bin/powerpc-eabi-gcc
ELF2DOL    := $(DEVKITPRO)/tools/bin/elf2dol
MACHDEP    := -DGEKKO -mogc -mcpu=750 -meabi -mhard-float
PPC_CFLAGS := $(MACHDEP) -O2 -std=gnu17 -Wall -Wextra -Wno-unused-parameter -Werror -fno-strict-aliasing               -I$(DEVKITPRO)/libogc/include -I$(ROOT)/port/gc/gfx -I$(ROOT)/port/gc/include
DOL_BUILD  := $(BUILD)/dol
DOL_OBJS   := $(DOL_BUILD)/gfx_tev.o $(DOL_BUILD)/rdp_model.o $(DOL_BUILD)/dol_tev.o

dol: $(DOL_BUILD)/dol_tev.dol

$(DOL_BUILD)/dol_tev.dol: $(DOL_OBJS)
	$(PPC_CC) $(MACHDEP) -Wl,-Map,$(DOL_BUILD)/dol_tev.map $^ -L$(DEVKITPRO)/libogc/lib/cube -logc -lm 	    -o $(DOL_BUILD)/dol_tev.elf
	$(ELF2DOL) $(DOL_BUILD)/dol_tev.elf $@
	python3 $(ROOT)/port/gc/tools/dolpad.py $@

$(DOL_BUILD)/gfx_tev.o: $(ROOT)/port/gc/gfx/gfx_tev.c $(ROOT)/port/gc/gfx/gfx_internal.h | $(DOL_BUILD)
	$(PPC_CC) -c $(PPC_CFLAGS) -o $@ $<

$(DOL_BUILD)/%.o: %.c rdp_model.h tev_cases.h $(ROOT)/port/gc/gfx/gfx_internal.h | $(DOL_BUILD)
	$(PPC_CC) -c $(PPC_CFLAGS) -o $@ $<

$(DOL_BUILD):
	mkdir -p $@
