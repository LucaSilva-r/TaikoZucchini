# Standalone FTP EBOOT build.
#
# Produces bin/ftp_eboot.elf, an unsigned PPU executable. Sign/package it with
# the SelfResigner tooling required for real consoles.

CELL_SDK     ?= $(abspath $(CURDIR)/../cell)
CELL_HOST    := $(CELL_SDK)/host-linux
PPU_PREFIX   := $(CELL_HOST)/ppu/bin/ppu-lv2-

PPU_CC       := $(PPU_PREFIX)gcc
PPU_LD       := $(PPU_PREFIX)gcc

INCLUDES := -I$(CELL_SDK)/target/ppu/include \
            -I$(CELL_SDK)/target/common/include \
            -I.

CFLAGS  := -O2 -Wall -Wextra -std=gnu99 -mcpu=cell \
           -fno-builtin -ffunction-sections -fdata-sections \
           $(INCLUDES)
LDFLAGS := -Wl,--gc-sections -L$(CELL_SDK)/target/ppu/lib
LDLIBS  := -lfs_stub -lsysmodule_stub -lsysutil_stub \
           -lgcm_cmd -lgcm_sys_stub \
           -lnet_stub -lnetctl_stub -llv2_stub -lc

BIN_DIR := bin
ELF     := $(BIN_DIR)/ftp_eboot.elf

SRCS := main.c ftp_server.c debug.c rsx_init.c menu_draw.c
OBJS := $(SRCS:.c=.o)

all: $(ELF)

$(ELF): $(OBJS) | $(BIN_DIR)
	$(PPU_LD) $(LDFLAGS) $^ $(LDLIBS) -o $@

%.o: %.c
	$(PPU_CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR):
	mkdir -p $@

clean:
	rm -rf $(BIN_DIR) $(OBJS)

.PHONY: all clean
