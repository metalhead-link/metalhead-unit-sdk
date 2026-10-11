# Shared build rules for loadable units. A unit's Makefile sets NAME and SRCS and includes
# this file:
#
#   NAME := vumeter
#   SRCS := vumeter.c
#   include ../unit.mk
#
# Output: build/$(NAME).unt (upload it with the web UI, or copy it to /units/ on the SD card)

CROSSPATH ?= /Applications/ARM/bin
PREFIX := $(CROSSPATH)/arm-none-eabi-
CC := $(PREFIX)gcc
OBJDUMP := $(PREFIX)objdump

UNITS_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

# Same libgcc variant as the firmware (ARM, not Thumb; see LD_OPTS in wormhole/Makefile).
LIBGCC ?= /Applications/ARM/lib/gcc/arm-none-eabi/14.3.1/arm/v5te/hard/libgcc.a

CFLAGS := -mcpu=cortex-a8 -mfpu=neon -mfloat-abi=hard -marm \
		-O2 -g -Wall -std=gnu11 \
		-fpie -ffreestanding -fno-common -fno-builtin-printf \
		-fno-exceptions -fno-unwind-tables -fno-asynchronous-unwind-tables \
		-ffunction-sections -fdata-sections \
		-I$(UNITS_DIR)/include $(EXTRA_CFLAGS)

LDFLAGS := -nostdlib -nostartfiles -pie \
		-Wl,--no-dynamic-linker -Wl,-T,$(UNITS_DIR)/unit.ld -Wl,--gc-sections \
		-Wl,-z,max-page-size=16 -Wl,--build-id=none -Wl,--no-warn-rwx-segments

OBJS := $(patsubst %.c,build/%.o,$(SRCS)) build/unitrt.o

all: build/$(NAME).unt

build/%.o: %.c $(UNITS_DIR)/include/unit_abi.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# No loop-to-memcpy/memset pattern matching here: it would turn these into calls to themselves.
build/unitrt.o: $(UNITS_DIR)/common/unitrt.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -fno-tree-loop-distribute-patterns -c -o $@ $<

build/$(NAME).elf: $(OBJS) $(UNITS_DIR)/unit.ld
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

build/$(NAME).unt: build/$(NAME).elf $(UNITS_DIR)/tools/mkunit.py
	python3 $(UNITS_DIR)/tools/mkunit.py $< $@

disasm: build/$(NAME).elf
	$(OBJDUMP) -d -r $<

clean:
	rm -rf build

.PHONY: all clean disasm
