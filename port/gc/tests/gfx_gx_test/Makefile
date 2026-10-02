# Stand-alone test DOL for the renderer's GX backend (port/gc/gfx/gfx_gx.c), independent of Makefile.gc
# and the game. It links gfx_gx.c and the real bridge_video.c with test-only stubs of the RSP/RDP state,
# gfx_tev_apply, gfx_tex_bind and the bridge (gfx_gx_test.c), draws a fixed scene and checks EFB pixels.
#
#   make -C port/gc/tests/gfx_gx_test            (inside WSL)
#   port/gc/tools/run_dolphin.sh port/gc/tests/gfx_gx_test/build/gfx_gx_test.dol -NoSd -Seconds 8 \
#       -Screenshot /tmp/gfx_gx_test.png -GeckoLog /tmp/gfx_gx_test.log
#
# The pixel checks read the EFB from the CPU (GX_PeekARGB/GX_PeekZ). Dolphin 2609 skips CPU EFB access by
# default, so they only run when Dolphin is started with -C Graphics.Hacks.EFBAccessEnable=True; otherwise
# the log says so and only the screenshot tells. Expected log: "gfx_gx_test: 29 of 29 checks passed".
#
# Same compiler flags as GFX_CFLAGS in Makefile.gc. Output goes to build/, padded by dolpad.py.
.SUFFIXES:

DEVKITPRO ?= /opt/devkitpro
DEVKITPPC ?= $(DEVKITPRO)/devkitPPC
export DEVKITPRO DEVKITPPC
include $(DEVKITPPC)/gamecube_rules

TOP   := $(abspath $(CURDIR)/../../../..)
BUILD := build

OBJS := $(BUILD)/gfx_gx.o $(BUILD)/gfx_fb.o $(BUILD)/bridge_video.o $(BUILD)/gfx_gx_test.o

CFLAGS  := $(MACHDEP) -O2 -std=gnu17 -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing \
           -I$(LIBOGC_INC) -I$(TOP)/port/gc/include -I$(TOP)/port/gc/gfx -I$(TOP)/port/gc/ogc
LDFLAGS := $(MACHDEP)
LIBS    := -L$(LIBOGC_LIB) -logc -lm

.PHONY: all clean
all: $(BUILD)/gfx_gx_test.dol

%.dol: %.elf
	$(DEVKITPRO)/tools/bin/elf2dol $< $@
	python3 $(TOP)/port/gc/tools/dolpad.py $@

$(BUILD)/gfx_gx_test.elf: $(OBJS)
	$(CC) $(LDFLAGS) -Wl,-Map,$(@:.elf=.map) $^ $(LIBS) -o $@

$(BUILD)/gfx_gx.o: $(TOP)/port/gc/gfx/gfx_gx.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/gfx_fb.o: $(TOP)/port/gc/gfx/gfx_fb.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/bridge_video.o: $(TOP)/port/gc/ogc/bridge_video.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

clean:
	rm -rf $(BUILD)

.PRECIOUS: %.elf
-include $(wildcard $(BUILD)/*.d)
