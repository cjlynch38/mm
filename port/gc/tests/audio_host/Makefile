# Host test of the software audio microcode (port/gc/audio/aud_ucode.c), see aud_test.c.
#
# aud_ucode.c is compiled with the host gcc and AUD_HOST_TEST (RAM addresses map into a fake 16 MiB
# RDRAM), against a fixed-width stand-in for PR/ultratypes.h (include/). The microcode's data comes
# from the N64 build's extracted/n64-us/incbin. Runs with AddressSanitizer and UBSan. Inside WSL:
#
#   make -C port/gc/tests/audio_host run
#   make -C port/gc/tests/audio_host run AUD_TEST_ARGS="7 500"     (seed, iterations)
#
# Reference: with CXD4=<path to a mupen64plus-rsp-cxd4 source tree> (CC0, not part of this repository),
# the test also runs MM's real microcode (aspMainText) on cxd4's RSP interpreter and requires DMEM and
# RAM to match bit for bit:
#
#   curl -L https://github.com/mupen64plus/mupen64plus-rsp-cxd4/archive/refs/heads/master.tar.gz | tar xz -C ~/ref
#   make -C port/gc/tests/audio_host run CXD4=~/ref/mupen64plus-rsp-cxd4-master
#
# Tasks captured from the game (AUD_CAPTURE builds, see port/gc/audio/aud_task.c; audcap.py converts the
# log): the test replays them (best with CXD4), the benchmark (aud_bench.c) times them. Captures hold
# game data from the ROM: keep them outside the repository.
#
#   make -C port/gc/tests/audio_host run CXD4=... AUD_TEST_ARGS="--replay capture.bin [more.bin ...]"
#   make -C port/gc/tests/audio_host bench CAPTURE=capture.bin [BENCH_ARGS=<repeats>]

ROOT  := $(abspath $(CURDIR)/../../../..)
BUILD := $(ROOT)/build/gc-host-tests/audio_host
TEST  := $(BUILD)/aud_test
BENCH := $(BUILD)/aud_bench
CXD4  ?=
CAPTURE ?=
AUD_TEST_ARGS ?=
BENCH_ARGS ?=

CC       := gcc
SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
CFLAGS   := -std=gnu17 -O1 -g $(SANITIZE) -Wall -Wextra -Wno-unused-parameter -Werror -fwrapv -fno-strict-aliasing
CPPFLAGS := -DAUD_HOST_TEST -I$(CURDIR)/include -I$(ROOT)/port/gc/audio -I$(ROOT)/include \
            -DAUD_EXTRACTED_DIR=\"$(ROOT)/extracted/n64-us\"
# The benchmark: optimized, without the sanitizers
BENCH_CFLAGS := -std=gnu17 -O2 -Wall -Wextra -Wno-unused-parameter -Werror -fwrapv -fno-strict-aliasing

HEADERS := $(ROOT)/port/gc/audio/aud_ucode.h $(CURDIR)/include/PR/ultratypes.h $(CURDIR)/lle.h
OBJS    := $(BUILD)/aud_ucode.o $(BUILD)/aud_test.o

ifneq ($(CXD4),)
CXD4_SRC   := $(abspath $(CXD4))
CXD4_OBJS  := $(addprefix $(BUILD)/cxd4/,su.o vu.o multiply.o add.o select.o logical.o divide.o)
CXD4_FLAGS := -std=gnu99 -O2 -w -DPLUGIN_API_VERSION=0x0101 -I$(CXD4_SRC)
CPPFLAGS   += -DAUD_LLE
OBJS       += $(BUILD)/lle.o $(CXD4_OBJS)
# The objects depend on which reference is built in
FLAVOR     := $(BUILD)/flavor-cxd4
else
FLAVOR     := $(BUILD)/flavor-none
endif

.PHONY: all run bench clean
all: $(TEST)

run: $(TEST)
	$(TEST) $(AUD_TEST_ARGS)

bench: $(BENCH)
	@[ -n "$(CAPTURE)" ] || { echo "make bench needs CAPTURE=<capture.bin> (see the top of this Makefile)"; exit 2; }
	$(BENCH) $(CAPTURE) $(BENCH_ARGS)

clean:
	rm -rf $(BUILD)

$(TEST): $(OBJS)
	$(CC) $(SANITIZE) -o $@ $^ -lm

$(BENCH): $(BUILD)/bench/aud_ucode.o $(BUILD)/bench/aud_bench.o
	$(CC) -o $@ $^

$(FLAVOR): | $(BUILD)
	rm -f $(BUILD)/flavor-* $(BUILD)/aud_test.o
	touch $@

# The test's copy keeps 2 sets of ADPCM tables instead of 32, so that tables get replaced (also during a
# command) far more often than in the game
$(BUILD)/aud_ucode.o: $(ROOT)/port/gc/audio/aud_ucode.c $(HEADERS) | $(BUILD)
	$(CC) -c $(CPPFLAGS) -DAUD_ADPCM_SETS=2 $(CFLAGS) -o $@ $<

$(BUILD)/aud_test.o: aud_test.c $(HEADERS) $(FLAVOR) | $(BUILD)
	$(CC) -c $(CPPFLAGS) $(CFLAGS) -o $@ $<

$(BUILD)/bench/aud_ucode.o: $(ROOT)/port/gc/audio/aud_ucode.c $(HEADERS) | $(BUILD)/bench
	$(CC) -c $(filter-out -DAUD_LLE,$(CPPFLAGS)) $(BENCH_CFLAGS) -o $@ $<

$(BUILD)/bench/aud_bench.o: aud_bench.c $(HEADERS) | $(BUILD)/bench
	$(CC) -c $(filter-out -DAUD_LLE,$(CPPFLAGS)) $(BENCH_CFLAGS) -o $@ $<

$(BUILD)/lle.o: lle.c lle.h | $(BUILD)
	$(CC) -c -std=gnu99 -O1 -g -Wall -Wno-unused-function -I$(CXD4_SRC) -DPLUGIN_API_VERSION=0x0101 -o $@ $<

$(BUILD)/cxd4/su.o: $(CXD4_SRC)/su.c | $(BUILD)/cxd4
	$(CC) -c $(CXD4_FLAGS) -o $@ $<

$(BUILD)/cxd4/%.o: $(CXD4_SRC)/vu/%.c | $(BUILD)/cxd4
	$(CC) -c $(CXD4_FLAGS) -o $@ $<

$(BUILD) $(BUILD)/cxd4 $(BUILD)/bench:
	mkdir -p $@
