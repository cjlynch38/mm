# gfx_fb_host: host test of the renderer's framebuffer effects (port/gc/gfx/gfx_fb.c), compiled with the host gcc
# against a stand-in <gccore.h> (include/) whose GX calls gfx_fb_host_test.c implements on a model of the EFB: texture
# copies in their tiled formats (box filter, RGB565 / RGB5A3 / R8 / Z24X8), the quads gfx_fb.c draws back. N64 RAM is a
# buffer (GFX_HOST_TEST: address a lives at a + gGfxHostRamBias). Runs with AddressSanitizer and UBSan.
#
#   make -C port/gc/tests/gfx_fb_host run      (inside WSL)
#
# Covers the frame's texture for backgrounds (the same texels as the RAM path, the frame's RAM left pending), pending
# frame rows during off-screen passes, off-screen images written to RAM and kept as textures (checked against RAM
# once per task), and the z-buffer's depth.

ROOT  := $(abspath $(CURDIR)/../../../..)
BUILD := $(ROOT)/build/gc-host-tests/gfx_fb_host
TEST  := $(BUILD)/test_gfx_fb

CC       := gcc
SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS   := -std=gnu17 -O1 -g $(SANITIZE) -Wall -Wextra -Werror -Wno-unused-parameter -fno-strict-aliasing \
            -DGFX_HOST_TEST
CPPFLAGS := -I$(CURDIR)/include -I$(ROOT)/port/gc/gfx -I$(ROOT)/port/gc/include

HEADERS := $(ROOT)/port/gc/gfx/gfx_internal.h $(ROOT)/port/gc/gfx/gfx_gbi.h $(ROOT)/port/gc/include/gc_bridge.h \
           $(CURDIR)/include/gccore.h $(CURDIR)/include/gc_ogc.h $(CURDIR)/include/ogc/lwp_watchdog.h
SRCS    := $(ROOT)/port/gc/gfx/gfx_fb.c $(CURDIR)/gfx_fb_host_test.c

.PHONY: all run clean
all: $(TEST)

run: $(TEST)
	$(TEST)

clean:
	rm -rf $(BUILD)

$(TEST): $(SRCS) $(HEADERS) | $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $(SRCS) -lm

$(BUILD):
	mkdir -p $@
