# Cell SDK SPRX build (PSL1GHT path retired).
#
# Toolchain runs through Wine wrappers under cell/host-linux/ produced by
# cell/setup_wine_wrappers.sh. The wrappers point CELL_HOST_PATH at the
# Windows .exe binaries with $WINEPREFIX = repo's wine_prefix/.
#
# Build:
#   make CELL_SDK=$(realpath ../cell)
# or set CELL_SDK in the environment.

CELL_SDK     ?= $(abspath $(CURDIR)/../cell)
CELL_HOST    := $(CELL_SDK)/host-linux
PPU_PREFIX   := $(CELL_HOST)/ppu/bin/ppu-lv2-
SPU_PREFIX   := $(CELL_HOST)/spu/bin/spu-lv2-

PPU_CC       := $(PPU_PREFIX)gcc
PPU_CXX      := $(PPU_PREFIX)g++
PPU_LD       := $(PPU_PREFIX)g++   # link via g++ for -mprx
PPU_OBJCOPY  := $(PPU_PREFIX)objcopy
SPU_CC       := $(SPU_PREFIX)gcc
SPU_LD       := $(SPU_PREFIX)gcc
PPU_PRX_STRIP := $(CELL_HOST)/bin/ppu-lv2-prx-strip
MAKE_FSELF    := $(CELL_HOST)/bin/make_fself

MBEDTLS_DIR := vendor/mbedtls
QUIRC_DIR   := vendor/quirc

INCLUDES := -I$(CELL_SDK)/target/ppu/include \
            -I$(CELL_SDK)/target/common/include \
            -I. \
            -Iconfig \
            -Icore \
            -Ipatches \
            -Ihooks \
            -Iinput \
            -Iqr \
            -Iqr_spu \
            -Inetwork \
            -Istorage \
            -Ibpreader \
            -Icards \
            -Ieboot_patcher \
            -Imod_menu \
            -Iftp \
            -I$(MBEDTLS_DIR)/include \
            -I$(MBEDTLS_DIR)/library \
            -I$(QUIRC_DIR)/lib

CFLAGS  := -O2 -Wall -Wextra -std=gnu99 -mcpu=cell \
           -mprx -fno-builtin -ffunction-sections -fdata-sections \
           $(INCLUDES)

# -mprx + -zgenprx + -zgenstub tell the toolchain to emit the PRX-shape
# ELF (proper SCE_PPURELA segment, sceModuleInfo, lib.ent/lib.stub layout).
# --gc-sections drops mbedTLS objects whose features are disabled in
# mbedtls_config.h so the SPRX stays small.
LDFLAGS := -mprx -zgenprx -zgenstub \
           -Wl,--strip-unused-data \
           -Wl,--gc-sections \
           -L$(CELL_SDK)/target/ppu/lib

# libc:        memcpy/memcmp (frozen subset usable in PRX context)
# libfs_stub:  cellFsOpen/Read/Close
# libsysmodule_stub: cellSysmoduleLoadModule (if needed; harmless to keep)
LDLIBS  := -lfs_stub -lsysmodule_stub -lsysutil_game_stub -lsysutil_stub \
           -lm_stub -lcamera_stub -lio_stub -lgcm_cmd -lgcm_sys_stub \
           -lc -lnet_stub -lnetctl_stub -llv2_stub

TARGET_NAME := zucchini
BIN_DIR := bin
PRX     := $(BIN_DIR)/$(TARGET_NAME).prx
SPRX    := $(BIN_DIR)/$(TARGET_NAME).sprx
SYM     := $(BIN_DIR)/$(TARGET_NAME).sym
BOOTSTRAP_EBOOT := bootstrap_eboot/bin/eboot.elf
FTP_EBOOT := ftp_eboot/bin/ftp_eboot.elf

SRCS    := core/main.c core/debug.c core/libc_stubs.c core/patch_ui.c core/rsx_init.c core/overlay.c eboot_fpt.c \
           mod_menu/menu.c mod_menu/menu_draw.c mod_menu/menu_pad.c mod_menu/menu_actions.c \
           mod_menu/menu_osk.c \
           ftp/ftp_server.c \
           config/runtime.c config/cfg_file.c \
           patches/patches.c patches/patch_target.c \
           eboot_patcher/self_parse.c eboot_patcher/self_decrypt.c \
           eboot_patcher/self_encrypt.c eboot_patcher/self_npdrm.c \
           eboot_patcher/elf_extract.c eboot_patcher/key_load.c \
           eboot_patcher/sce_bn.c eboot_patcher/sce_ecdsa.c \
           eboot_patcher/sce_curve.c eboot_patcher/sce_segmap.c \
           eboot_patcher/sprx_loader_patch.c \
           eboot_patcher/sce_rand.c eboot_patcher/eboot_flow.c \
           storage/data00000_redirect.c \
           storage/usio_backup.c \
           storage/usrdir_path.c \
           storage/param_sfo_fix.c \
           hooks/camera_diag.c hooks/bpreader_hook.c \
           hooks/chassisinfo_hook.c storage/chassisinfo_synth.c \
           storage/chassisinfo_schema.c \
           core/game_version.c \
           input/pad_input.c input/kb_input.c input/taiko_frame.c \
           qr/camera_qr.c qr/qr_spu_host.c \
           bpreader/bpreader_serial.c \
           cards/card_store.c cards/card_picker.c \
           network/certs.c \
           network/online_diag.c \
           hooks/http_hook.c hooks/cell_http_shim.c \
           hooks/dns_hook.c hooks/socket_hook.c \
           hooks/video_out_hook.c \
           network/uri.c network/http_client.c network/version_check.c
OBJS    := $(SRCS:.c=.o)

SPU_QR_ELF := $(BIN_DIR)/qr_spu.elf
SPU_QR_PPU_OBJ := $(BIN_DIR)/qr_spu_elf.o
SPU_QR_SRCS := qr_spu/qr_spu_main.c \
               $(QUIRC_DIR)/lib/decode.c \
               $(QUIRC_DIR)/lib/identify.c \
               $(QUIRC_DIR)/lib/version_db.c
SPU_QR_OBJS := $(BIN_DIR)/qr_spu_main.spu.o \
               $(BIN_DIR)/quirc_decode.spu.o \
               $(BIN_DIR)/quirc_identify.spu.o \
               $(BIN_DIR)/quirc_version_db.spu.o
OBJS += $(SPU_QR_PPU_OBJ)

SPU_QR_CFLAGS := -Os -Wall -Wextra -std=gnu99 -DNDEBUG \
                 -DQUIRC_FLOAT_TYPE=float -DQUIRC_USE_TGMATH \
                 -I$(CELL_SDK)/target/spu/include \
                 -Iqr_spu -I$(QUIRC_DIR)/lib
SPU_QR_LDFLAGS := -L$(CELL_SDK)/target/spu/lib
SPU_QR_LDLIBS := -lm


# mbedTLS sources. Keep this explicit for PRX builds: compiling the whole
# library and relying on dead stripping still leaves a much larger module on
# this toolchain.
MBEDTLS_SRC_NAMES := \
	aes.c asn1parse.c asn1write.c base64.c bignum.c bignum_core.c bignum_mod.c \
	bignum_mod_raw.c chacha20.c chachapoly.c cipher.c cipher_wrap.c \
	constant_time.c ctr_drbg.c ecdh.c ecdsa.c ecp.c ecp_curves.c \
	ecp_curves_new.c entropy.c entropy_poll.c error.c gcm.c hmac_drbg.c \
	md.c oid.c pem.c pk.c pk_ecc.c pk_wrap.c pkparse.c platform.c \
	platform_util.c poly1305.c rsa.c rsa_alt_helpers.c sha1.c sha256.c sha512.c \
	ssl_ciphersuites.c ssl_client.c ssl_debug_helpers_generated.c ssl_msg.c \
	ssl_tls.c ssl_tls12_client.c version.c version_features.c x509.c \
	x509_crt.c
MBEDTLS_SRCS    := $(addprefix $(MBEDTLS_DIR)/library/,$(MBEDTLS_SRC_NAMES))
MBEDTLS_OBJS    := $(MBEDTLS_SRCS:.c=.o)
OBJS            += $(MBEDTLS_OBJS)

QUIRC_SRC_NAMES := quirc.c decode.c identify.c version_db.c
QUIRC_SRCS      := $(addprefix $(QUIRC_DIR)/lib/,$(QUIRC_SRC_NAMES))
QUIRC_OBJS      := $(QUIRC_SRCS:.c=.o)
OBJS            += $(QUIRC_OBJS)

all: $(SPRX) $(BOOTSTRAP_EBOOT) $(FTP_EBOOT)

bootstrap: $(BOOTSTRAP_EBOOT)
ftp-eboot: $(FTP_EBOOT)

$(BOOTSTRAP_EBOOT): bootstrap_eboot/Makefile bootstrap_eboot/main.c
	$(MAKE) -C bootstrap_eboot CELL_SDK=$(CELL_SDK)

$(FTP_EBOOT): ftp_eboot/Makefile ftp_eboot/main.c ftp_eboot/ftp_server.c ftp_eboot/ftp_server.h ftp_eboot/debug.c ftp_eboot/debug.h ftp_eboot/rsx_init.c ftp_eboot/rsx_init.h ftp_eboot/menu_draw.c ftp_eboot/menu_draw.h ftp_eboot/menu_font.h ftp_eboot/menu_font_20.h ftp_eboot/menu_font_28.h
	$(MAKE) -C ftp_eboot CELL_SDK=$(CELL_SDK)

$(BIN_DIR):
	mkdir -p $@

$(SYM): $(OBJS) | $(BIN_DIR)
	$(PPU_LD) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

$(PRX): $(SYM)
	$(PPU_PRX_STRIP) $< -o $@

$(SPRX): $(PRX)
	$(MAKE_FSELF) $< $@

%.o: %.c
	$(PPU_CC) $(CFLAGS) -c $< -o $@

$(BIN_DIR)/qr_spu_main.spu.o: qr_spu/qr_spu_main.c qr_spu/qr_spu_shared.h $(QUIRC_DIR)/lib/quirc.h $(QUIRC_DIR)/lib/quirc_internal.h | $(BIN_DIR)
	$(SPU_CC) $(SPU_QR_CFLAGS) -c $< -o $@

$(BIN_DIR)/quirc_decode.spu.o: $(QUIRC_DIR)/lib/decode.c $(QUIRC_DIR)/lib/quirc.h $(QUIRC_DIR)/lib/quirc_internal.h | $(BIN_DIR)
	$(SPU_CC) $(SPU_QR_CFLAGS) -c $< -o $@

$(BIN_DIR)/quirc_identify.spu.o: $(QUIRC_DIR)/lib/identify.c $(QUIRC_DIR)/lib/quirc.h $(QUIRC_DIR)/lib/quirc_internal.h | $(BIN_DIR)
	$(SPU_CC) $(SPU_QR_CFLAGS) -c $< -o $@

$(BIN_DIR)/quirc_version_db.spu.o: $(QUIRC_DIR)/lib/version_db.c $(QUIRC_DIR)/lib/quirc_internal.h | $(BIN_DIR)
	$(SPU_CC) $(SPU_QR_CFLAGS) -c $< -o $@

$(SPU_QR_ELF): $(SPU_QR_OBJS) | $(BIN_DIR)
	$(SPU_LD) $(SPU_QR_LDFLAGS) $(SPU_QR_OBJS) $(SPU_QR_LDLIBS) -o $@

$(SPU_QR_PPU_OBJ): $(SPU_QR_ELF) | $(BIN_DIR)
	$(PPU_OBJCOPY) -I binary -O elf64-powerpc-celloslv2 -B powerpc \
		--set-section-align .data=7 \
		--set-section-pad .data=128 \
		--rename-section .data=.spu_image.$<,readonly,contents,alloc \
		$< $@

# Build mbedTLS sources with -Os to keep size down; behaviour identical
# to the main CFLAGS otherwise. Kept separate so future flag tweaks for
# the vendored tree don't bleed into our own code.
#
# All mbedTLS .o files depend on mbedtls_config.h: editing a
# MBEDTLS_CIPHER_MODE_* / similar macro changes struct layout (e.g.
# cipher_base_t grows a slot), and silently mixing old vs new .o causes
# `mbedtls_cipher_free` to read past the struct and crash mid-handshake.
MBEDTLS_CONFIG_H := $(MBEDTLS_DIR)/include/mbedtls/mbedtls_config.h
$(MBEDTLS_DIR)/library/%.o: $(MBEDTLS_DIR)/library/%.c $(MBEDTLS_CONFIG_H)
	$(PPU_CC) $(CFLAGS) -Os -c $< -o $@

# quirc: NDEBUG to neutralize asserts. -include quirc_math_shim.h to
# substitute manual rint/fabs/sqrt; libm calls return the input register
# unchanged at PRX init time on this firmware.
$(QUIRC_DIR)/lib/%.o: $(QUIRC_DIR)/lib/%.c
	$(PPU_CC) $(CFLAGS) -Os -DNDEBUG -include qr/quirc_math_shim.h -c $< -o $@

config/runtime.o: config/runtime.c config/runtime.h config/cfg_file.h config.h core/debug.h storage/usrdir_path.h input/pad_input.h input/kb_input.h storage/chassisinfo_schema.h
config/cfg_file.o: config/cfg_file.c config/cfg_file.h
core/main.o:      core/main.c      config.h config/runtime.h patches/patches.h network/certs.h core/debug.h hooks/http_hook.h hooks/dns_hook.h hooks/socket_hook.h storage/data00000_redirect.h hooks/camera_diag.h core/overlay.h network/version_check.h cards/card_picker.h
mod_menu/menu.o: mod_menu/menu.c mod_menu/menu.h config/runtime.h
mod_menu/menu_pad.o: mod_menu/menu_pad.c mod_menu/menu_pad.h input/kb_input.h
mod_menu/menu_actions.o: mod_menu/menu_actions.c mod_menu/menu_actions.h config/runtime.h
eboot_fpt.o:      eboot_fpt.c      eboot_fpt.h core/debug.h
storage/data00000_redirect.o: storage/data00000_redirect.c storage/data00000_redirect.h config.h core/debug.h core/icache.h eboot_fpt.h config/runtime.h hooks/chassisinfo_hook.h
hooks/camera_diag.o: hooks/camera_diag.c hooks/camera_diag.h config.h core/debug.h core/icache.h eboot_fpt.h config/runtime.h
qr/camera_qr.o:   qr/camera_qr.c   qr/camera_qr.h qr/qr_spu_host.h qr_spu/qr_spu_shared.h config.h core/debug.h qr/qr_selftest_data.h $(QUIRC_DIR)/lib/quirc.h
qr/qr_spu_host.o: qr/qr_spu_host.c qr/qr_spu_host.h qr_spu/qr_spu_shared.h core/debug.h
bpreader/bpreader_serial.o: bpreader/bpreader_serial.c bpreader/bpreader_serial.h
cards/card_store.o:  cards/card_store.c cards/card_store.h config/cfg_file.h core/debug.h
cards/card_picker.o: cards/card_picker.c cards/card_picker.h cards/card_store.h qr/camera_qr.h hooks/bpreader_hook.h bpreader/bpreader_serial.h core/overlay.h input/taiko_frame.h input/kb_input.h config/runtime.h mod_menu/menu_pad.h mod_menu/menu_osk.h core/debug.h
hooks/bpreader_hook.o: hooks/bpreader_hook.c hooks/bpreader_hook.h config.h core/debug.h core/icache.h eboot_fpt.h config/runtime.h
hooks/chassisinfo_hook.o: hooks/chassisinfo_hook.c hooks/chassisinfo_hook.h storage/chassisinfo_synth.h storage/chassisinfo_schema.h core/game_version.h eboot_fpt.h core/debug.h
core/game_version.o: core/game_version.c core/game_version.h core/debug.h
storage/chassisinfo_synth.o: storage/chassisinfo_synth.c storage/chassisinfo_synth.h storage/chassisinfo_schema.h config.h config/runtime.h core/debug.h
storage/chassisinfo_schema.o: storage/chassisinfo_schema.c storage/chassisinfo_schema.h
patches/patches.o:   patches/patches.c   config.h config/runtime.h patches/patches.h core/icache.h core/debug.h
network/certs.o:     network/certs.c     config.h network/certs.h core/icache.h core/debug.h
core/debug.o:     core/debug.c     core/debug.h config.h config/runtime.h
core/overlay.o:   core/overlay.c   core/overlay.h core/debug.h eboot_fpt.h mod_menu/menu_font_20.h mod_menu/menu_font.h
input/pad_input.o: input/pad_input.c input/pad_input.h input/kb_input.h config/cfg_file.h config/runtime.h core/debug.h
input/kb_input.o:  input/kb_input.c  input/kb_input.h  input/pad_input.h config/cfg_file.h config/runtime.h core/debug.h
network/online_diag.o:   network/online_diag.c   network/online_diag.h config.h core/debug.h
hooks/http_hook.o:      hooks/http_hook.c      hooks/http_hook.h core/icache.h core/debug.h network/http_client.h hooks/cell_http_shim.h eboot_fpt.h config/runtime.h config.h
hooks/dns_hook.o:       hooks/dns_hook.c       hooks/dns_hook.h core/icache.h core/debug.h eboot_fpt.h config/runtime.h config.h
hooks/socket_hook.o:    hooks/socket_hook.c    hooks/socket_hook.h core/icache.h core/debug.h eboot_fpt.h network/http_client.h config/runtime.h config.h
hooks/video_out_hook.o: hooks/video_out_hook.c hooks/video_out_hook.h eboot_fpt.h config/runtime.h config.h core/debug.h
network/uri.o:            network/uri.c            network/uri.h
network/http_client.o:    network/http_client.c    network/http_client.h network/uri.h core/debug.h config/runtime.h config.h
network/version_check.o:  network/version_check.c  network/version_check.h network/http_client.h config/version.h core/debug.h core/overlay.h
hooks/cell_http_shim.o: hooks/cell_http_shim.c hooks/cell_http_shim.h network/http_client.h core/debug.h config/runtime.h config.h

RPCS3_DEV_HDD0 ?= $(HOME)/.config/rpcs3/dev_hdd0
RPCS3_PLUGIN_DIR ?= $(RPCS3_DEV_HDD0)/plugins/taiko

install: $(SPRX)
	@mkdir -p $(RPCS3_PLUGIN_DIR)
	cp $(SPRX) $(RPCS3_PLUGIN_DIR)/zucchini.sprx
	@echo "installed -> $(RPCS3_PLUGIN_DIR)/zucchini.sprx"

clean:
	rm -f $(OBJS) $(SPU_QR_OBJS) $(SPU_QR_ELF) $(SYM) $(PRX) $(SPRX)
	$(MAKE) -C bootstrap_eboot clean
	$(MAKE) -C ftp_eboot clean

.PHONY: all bootstrap ftp-eboot clean install
