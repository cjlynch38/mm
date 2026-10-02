# gfx_tex_host: host unit tests for the renderer's RDP and texture modules (port/gc/gfx/gfx_rdp.c and
# gfx_tex.c), compiled with the host gcc against a stand-in <gccore.h> (include/gccore.h).
#
#   make -C port/gc/tests/gfx_tex_host run      (inside WSL)
#
# Covers N64 format decoding, GX tiled layouts, wrap/mirror/clamp/mask, TMEM load bookkeeping, texture and
# fill rectangle decoding, other modes, and the texture cache. See gfx_tex_host_test.c.

ROOT     := ../../../..
CC       := gcc
SANITIZE ?= -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS   := -std=gnu17 -O1 -g -Wall -Wextra -Wno-unused-parameter -fno-strict-aliasing $(SANITIZE) \
            -Iinclude -I$(ROOT)/port/gc/gfx -I$(ROOT)/port/gc/include

SRCS := gfx_tex_host_test.c $(ROOT)/port/gc/gfx/gfx_tex.c $(ROOT)/port/gc/gfx/gfx_rdp.c
DEPS := $(SRCS) include/gccore.h $(ROOT)/port/gc/gfx/gfx_internal.h $(ROOT)/port/gc/gfx/gfx_gbi.h

.PHONY: all run clean

all: gfx_tex_host_test

gfx_tex_host_test: $(DEPS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) -lm

run: gfx_tex_host_test
	./gfx_tex_host_test

clean:
	rm -f gfx_tex_host_test
