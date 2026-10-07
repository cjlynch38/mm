# Host tests of the SD card code in port/gc/ogc, with AddressSanitizer and UBSan. Run inside Linux/WSL:
#
#   make -C port/gc/tests/sd_host run
#
#   test_fat_map        fat_map.c (raw ROM reads from the SD card, storage.c) on FAT12/16/32 images that mkfat.py
#                       builds: with and without an MBR, 512 B to 64 KB clusters, fragmented files, long names,
#                       nested directories. The images are sparse files (about 100 MB on disk).
#   test_save_rotation  save_rotation.c (the save files, bridge_save.c) on a model of the card that loses power at
#                       every write and rename of a store.

ROOT  := $(abspath $(CURDIR)/../../../..)
BUILD := $(ROOT)/build/gc-host-tests/sd_host
OGC   := $(ROOT)/port/gc/ogc

CC       := gcc
SANITIZE := -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer
CFLAGS   := -std=gnu17 -O1 -g $(SANITIZE) -Wall -Wextra -Wno-unused-parameter -Werror -I$(OGC)

.PHONY: all run clean
all: $(BUILD)/test_fat_map $(BUILD)/test_save_rotation $(BUILD)/images/manifest.txt

run: all
	$(BUILD)/test_fat_map $(BUILD)/images/manifest.txt
	$(BUILD)/test_save_rotation

clean:
	rm -rf $(BUILD)

$(BUILD)/test_fat_map: test_fat_map.c $(OGC)/fat_map.c $(OGC)/fat_map.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_fat_map.c $(OGC)/fat_map.c

$(BUILD)/test_save_rotation: test_save_rotation.c $(OGC)/save_rotation.c $(OGC)/save_rotation.h | $(BUILD)
	$(CC) $(CFLAGS) -o $@ test_save_rotation.c $(OGC)/save_rotation.c

$(BUILD)/images/manifest.txt: mkfat.py | $(BUILD)
	python3 mkfat.py $(BUILD)/images

$(BUILD):
	mkdir -p $@
