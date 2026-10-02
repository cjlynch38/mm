# Host-side test of the renderer's RSP module (port/gc/gfx/gfx_rsp.c).
#
# gfx_rsp.c is compiled with the host gcc against a fake gccore.h (include/), with GFX_HOST_TEST so that
# N64 addresses 0x80000000.. map into a buffer the test allocates. GX, the RDP module, the texture cache
# counters and the bridge are mocked in mock.c. Runs with AddressSanitizer and UBSan (including float to
# int overflow), stopping at the first report. Inside Linux/WSL:
#
#   make -C port/gc/tests/gfx_host run
#   GFX_FUZZ_SEED=7 make -C port/gc/tests/gfx_host run    (another seed for the random display list test)
#   GFX_TEST_VERBOSE=1 make -C port/gc/tests/gfx_host run (print the module's log lines)

ROOT  := $(abspath $(CURDIR)/../../../..)
BUILD := $(ROOT)/build/gc-host-tests/gfx_host
TEST  := $(BUILD)/test_gfx_rsp

CC       := gcc
SANITIZE := -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS   := -std=gnu17 -O1 -g $(SANITIZE) -Wall -Wextra -Werror -DGFX_HOST_TEST
CPPFLAGS := -I$(CURDIR)/include -I$(ROOT)/port/gc/gfx -I$(ROOT)/port/gc/include

HEADERS := $(ROOT)/port/gc/gfx/gfx_internal.h $(ROOT)/port/gc/gfx/gfx_gbi.h $(ROOT)/port/gc/include/gc_bridge.h \
           $(CURDIR)/include/gccore.h $(CURDIR)/test.h
OBJS    := $(BUILD)/gfx_rsp.o $(BUILD)/mock.o $(BUILD)/test_rsp.o

.PHONY: all run clean
all: $(TEST)

run: $(TEST)
	$(TEST)

clean:
	rm -rf $(BUILD)

$(TEST): $(OBJS)
	$(CC) $(SANITIZE) -o $@ $^ -lm

$(BUILD)/gfx_rsp.o: $(ROOT)/port/gc/gfx/gfx_rsp.c $(HEADERS) | $(BUILD)
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<

$(BUILD)/%.o: %.c $(HEADERS) | $(BUILD)
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<

$(BUILD):
	mkdir -p $@
