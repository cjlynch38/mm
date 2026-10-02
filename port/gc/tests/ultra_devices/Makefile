# Host-side test of the libultra device files in port/gc/ultra (vi sp pi cache cont motor ai flash voice).
#
# The real files are compiled with the host gcc and the decomp's headers (as in Makefile.gc), linked
# against mocks of gc_bridge.h and of the libultra core, and run with AddressSanitizer and UBSan,
# three times: no save on "SD", an existing save, and a save that cannot be read. Run inside Linux/WSL:
#
#   make -C port/gc/tests/ultra_devices run

ROOT  := $(abspath $(CURDIR)/../../../..)
BUILD := $(ROOT)/build/gc-host-tests/ultra_devices
TEST  := $(BUILD)/test_ultra_devices

CC       := gcc
SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
CFLAGS   := -std=gnu17 -O1 -g $(SANITIZE) -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Werror

# Files built with the decomp's headers. On a 64-bit host N64 addresses such as K0BASE are cast to
# pointers of a different size, which is harmless here.
GAME_CPPFLAGS := -nostdinc -I$(ROOT)/include -I$(ROOT)/include/libc -I$(ROOT)/src -I$(ROOT) \
                 -I$(ROOT)/port/gc/include -I$(CURDIR) -include $(ROOT)/port/gc/include/gc_prelude.h \
                 -D_LANGUAGE_C -DF3DEX_GBI_2 -DF3DEX_GBI_PL -DGBI_DOWHILE -DMM_VERSION=N64_US \
                 -DNON_MATCHING -DAVOID_UB -DTARGET_GC
GAME_CFLAGS   := -funsigned-char -fno-strict-aliasing -fwrapv -Wno-unknown-pragmas -Wno-multichar \
                 -Wno-builtin-declaration-mismatch -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast

# Files built with the host's headers
HOST_CPPFLAGS := -I$(ROOT)/port/gc/include -I$(CURDIR)

ULTRA_NAMES := vi sp pi cache cont motor ai flash voice
ULTRA_OBJS  := $(foreach n,$(ULTRA_NAMES),$(BUILD)/ultra_$(n).o)
GAME_OBJS   := $(ULTRA_OBJS) $(BUILD)/mock_ultra.o $(BUILD)/test_main.o
HOST_OBJS   := $(BUILD)/mock_bridge.o

.PHONY: all run clean
all: $(TEST)

run: $(TEST)
	$(TEST)
	MOCK_SAVE=1 $(TEST)
	MOCK_SAVE=e $(TEST)

clean:
	rm -rf $(BUILD)

$(TEST): $(GAME_OBJS) $(HOST_OBJS)
	$(CC) $(SANITIZE) -o $@ $^ -lpthread

$(BUILD)/ultra_%.o: $(ROOT)/port/gc/ultra/%.c $(ROOT)/port/gc/include/gc_bridge.h \
                    $(ROOT)/port/gc/include/gc_ultra_internal.h | $(BUILD)
	$(CC) -c $(GAME_CPPFLAGS) $(CFLAGS) $(GAME_CFLAGS) -o $@ $<

$(BUILD)/mock_ultra.o $(BUILD)/test_main.o: $(BUILD)/%.o: %.c mock.h | $(BUILD)
	$(CC) -c $(GAME_CPPFLAGS) $(CFLAGS) $(GAME_CFLAGS) -o $@ $<

$(BUILD)/mock_bridge.o: mock_bridge.c mock.h | $(BUILD)
	$(CC) -c $(HOST_CPPFLAGS) $(CFLAGS) -o $@ $<

$(BUILD):
	mkdir -p $@
