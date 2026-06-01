# Bootstrap EBOOT.BIN build.
#
# Produces bin/eboot.elf — an unsigned PPU executable. The user signs
# it offline using their existing scetool/SELF_Resigner toolchain (the
# same one that signs the normal PRX-aware EBOOT) and ships the
# resulting EBOOT.BIN alongside zucchini.sprx.

CELL_SDK     ?= $(abspath $(CURDIR)/../cell)
CELL_HOST    ?= $(firstword $(wildcard $(CELL_SDK)/host-win32 $(CELL_SDK)/host-linux) $(CELL_SDK)/host-linux)
PPU_PREFIX   := $(CELL_HOST)/ppu/bin/ppu-lv2-

PPU_CC       := $(PPU_PREFIX)gcc
PPU_LD       := $(PPU_PREFIX)gcc

INCLUDES := -I$(CELL_SDK)/target/ppu/include \
            -I$(CELL_SDK)/target/common/include

CFLAGS  := -O2 -Wall -Wextra -std=gnu99 -mcpu=cell $(INCLUDES)
LDFLAGS := -L$(CELL_SDK)/target/ppu/lib
LDLIBS  := -lsysmodule_stub -lio_stub -lc

BIN_DIR := bin
ELF     := $(BIN_DIR)/eboot.elf

SRCS := main.c
OBJS := $(SRCS:.c=.o)

$(ELF): $(OBJS) | $(BIN_DIR)
	$(PPU_LD) $(LDFLAGS) $^ $(LDLIBS) -o $@

%.o: %.c
	$(PPU_CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BIN_DIR) $(OBJS)

.PHONY: clean
