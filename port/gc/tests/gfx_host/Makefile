# Host-side test of the renderer's RSP and S2DEX2 modules (port/gc/gfx/gfx_rsp.c, gfx_s2dex.c).
#
# gfx_rsp.c and gfx_s2dex.c are compiled with the host gcc against a fake gccore.h (include/), with
# GFX_HOST_TEST so that N64 addresses 0x80000000.. map into a buffer the test allocates. GX, the RDP module,
# the texture cache, gfx_fb.c and the bridge are mocked in mock.c. Runs with AddressSanitizer and UBSan
# (including float to int overflow), stopping at the first report. Inside Linux/WSL:
#
#   make -C port/gc/tests/gfx_host run
#   GFX_FUZZ_SEED=7 make -C port/gc/tests/gfx_host run    (another seed for the random display list test)
#   GFX_TEST_VERBOSE=1 make -C port/gc/tests/gfx_host run (print the module's log lines)
#
# test_njpeg checks the CPU JPEG task (port/gc/game/njpeg_cpu.c), compiled with the game's headers like the
# shim that builds its OSTask (njpeg_shim.c); the test itself uses the host's.

ROOT  := $(abspath $(CURDIR)/../../../..)
BUILD := $(ROOT)/build/gc-host-tests/gfx_host
TEST  := $(BUILD)/test_gfx_rsp
NJPEG := $(BUILD)/test_njpeg

CC       := gcc
SANITIZE := -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS   := -std=gnu17 -O1 -g $(SANITIZE) -Wall -Wextra -Werror -DGFX_HOST_TEST
CPPFLAGS := -I$(CURDIR)/include -I$(ROOT)/port/gc/gfx -I$(ROOT)/port/gc/include

HEADERS := $(ROOT)/port/gc/gfx/gfx_internal.h $(ROOT)/port/gc/gfx/gfx_gbi.h $(ROOT)/port/gc/include/gc_bridge.h \
           $(CURDIR)/include/gccore.h $(CURDIR)/test.h
OBJS    := $(BUILD)/gfx_rsp.o $(BUILD)/gfx_s2dex.o $(BUILD)/mock.o $(BUILD)/test_rsp.o $(BUILD)/test_s2dex.o

GAME_FLAGS := -funsigned-char -Wno-builtin-declaration-mismatch -I$(ROOT)/include -I$(ROOT)/include/libc               -I$(ROOT)/src -I$(ROOT)/port/gc/include -D_LANGUAGE_C -DF3DEX_GBI_2 -DTARGET_GC -DNON_MATCHING -DAVOID_UB
NJPEG_OBJS := $(BUILD)/njpeg_cpu.o $(BUILD)/njpeg_shim.o $(BUILD)/test_njpeg.o

.PHONY: all run clean
all: $(TEST) $(NJPEG)

run: $(TEST) $(NJPEG)
	$(TEST)
	$(NJPEG)

clean:
	rm -rf $(BUILD)

$(TEST): $(OBJS)
	$(CC) $(SANITIZE) -o $@ $^ -lm

$(NJPEG): $(NJPEG_OBJS)
	$(CC) $(SANITIZE) -o $@ $^ -lm

$(BUILD)/njpeg_cpu.o: $(ROOT)/port/gc/game/njpeg_cpu.c | $(BUILD)
	$(CC) -c $(GAME_FLAGS) $(CFLAGS) -o $@ $<

$(BUILD)/njpeg_shim.o: njpeg_shim.c | $(BUILD)
	$(CC) -c $(GAME_FLAGS) $(CFLAGS) -o $@ $<

$(BUILD)/test_njpeg.o: test_njpeg.c | $(BUILD)
	$(CC) -c $(CFLAGS) -o $@ $<

$(BUILD)/gfx_%.o: $(ROOT)/port/gc/gfx/gfx_%.c $(HEADERS) | $(BUILD)
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<

$(BUILD)/%.o: %.c $(HEADERS) | $(BUILD)
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<

$(BUILD):
	mkdir -p $@
