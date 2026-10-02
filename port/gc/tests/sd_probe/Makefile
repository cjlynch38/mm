# sd_probe: stand-alone libogc test DOL for the SD / Dolphin dev-disc / USB Gecko
# paths. Build inside WSL with devkitPPC:  make -C port/gc/tests/sd_probe
# Output: port/gc/tests/sd_probe/sd_probe.dol (padded for Dolphin by dolpad.py).
.SUFFIXES:

ifeq ($(strip $(DEVKITPPC)),)
DEVKITPRO ?= /opt/devkitpro
export DEVKITPRO
export DEVKITPPC := $(DEVKITPRO)/devkitPPC
endif

include $(DEVKITPPC)/gamecube_rules

TARGET   := $(notdir $(CURDIR))
BUILD    := build
SOURCES  := source
INCLUDES :=

CFLAGS   = -g -O2 -Wall -Wextra $(MACHDEP) $(INCLUDE)
LDFLAGS  = -g $(MACHDEP) -Wl,-Map,$(notdir $@).map

LIBS     := -liso9660 -lfat -logc -lm
LIBDIRS  :=

ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT  := $(CURDIR)/$(TARGET)
export VPATH   := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR := $(CURDIR)/$(BUILD)
# elf2dol leaves the last section unpadded; Dolphin 2609 rejects such DOLs.
export DOLPAD  := $(abspath $(CURDIR)/../../tools/dolpad.py)

CFILES := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))

export LD           := $(CC)
export OFILES       := $(CFILES:.c=.o)
export INCLUDE      := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                       $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                       -I$(CURDIR)/$(BUILD) -I$(LIBOGC_INC)
export LIBPATHS     := -L$(LIBOGC_LIB) $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).elf $(OUTPUT).dol

else

DEPENDS := $(OFILES:.o=.d)

$(OUTPUT).dol: $(OUTPUT).elf
	$(SILENTMSG) output ... $(notdir $@)
	$(SILENTCMD)elf2dol $< $@
	$(SILENTCMD)python3 $(DOLPAD) $@ > /dev/null
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

endif
