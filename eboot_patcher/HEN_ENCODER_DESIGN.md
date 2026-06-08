# On-device HEN (retail 3.XX STD) EBOOT encoder — design

## Problem

The arcade `EBOOT_ORIGINAL.BIN` files we work with are **debug-signed**
(`sce_header.key_revision == 0x8000`). The current on-device patcher
(`eboot_flow.c`) is **format-preserving**: it patches and re-emits the same
debug self. DEX/CFW boot debug selfs; HEN-enabled CEX rejects them.

To boot on HEN the patched EBOOT must be a **retail "3.XX STD"** self:

| field            | value                       |
|------------------|-----------------------------|
| key_revision     | `0x0004` (fw 3.40–3.42)      |
| self_type        | `APP` (4) — non-NPDRM "STD"  |
| self_fw_version  | `0x0003004000000000`        |
| self_app_version | `0x0001000000000000`        |
| auth_id          | `0x1010000001000003`        |
| vendor_id        | `0x01000002`                |
| ctrl/cap flags   | scetool defaults (see below)|

This is the same output as TrueAncestor's `[3.XX STD]` / the bundled
`tools/SELF_Resigner_Linux` scetool. The sprx + bootstrap take this path on
the PC build (`scripts/resign_hen.sh`); the **patched game EBOOT** must be
produced on the console, so we port scetool's encode path into the PRX.

Converting debug → retail-STD is **not** a re-encrypt. The debug self's
metadata layout differs; we must rebuild a fresh retail SCE around the patched
ELF. That means porting scetool's `sce_create_ctxt_build_self` /
`self_build_self` / `sce_layout_ctxt` path — not just the sign+encrypt step we
already have in `self_encrypt.c`.

## What already exists on-device (reuse)

- `self_parse.c` — parse an SCE buffer into `self_ctx_t`.
- `self_decrypt.c` / `self_npdrm.c` — decrypt metadata + body (to recover the
  patched ELF from the debug self). Debug self bodies are often cleartext.
- `elf_extract.c` — pull the ELF image back out of a decrypted self.
- `self_encrypt.c` — `calculate_hashes`, `sign_header` (real rev-04 ECDSA via
  `sce_ecdsa`/`sce_curve`), `encrypt_metadata_block`, `encrypt_body`. These
  operate on an **already-laid-out** SCE buffer — exactly the back half of
  scetool's `sce_encrypt_ctxt`. **Reusable as-is** for the new encoder.
- `sce_ecdsa` / `sce_curve` / `sce_rand` / mbedtls AES+SHA1+HMAC.
- `key_load.c` — loads one scetool-format keyset.

## What must be added (the encoder)

New `self_build.c` mirroring scetool's `sce.cpp`/`self.cpp` **encode** half,
trimmed for STD APP, **no compression** (avoids zlib), **no NPDRM**, **no
add_shdrs** (matches the resigner's non-DRM CEX command):

1. `build_ctxt_from_elf(elf, elf_len)` ← `sce_create_ctxt_build_self`
   - Fresh SCE header: magic, version 2, type SELF.
   - Copy ELF ehdr + phdrs into the header region.
   - Per phdr: a `section_info_t` (encrypted=1 for PT_LOAD / PT_PS3_PRX_RELOC
     (0x700000A1) / 0x700000A8, else 0).
   - Data sections + `metadata_section_header_t[]`: include only the
     encrypted-LOAD-type phdrs, **dedupe identical p_offset** (skip_sections),
     all `hashed`, `compressed=NOT`.

2. config fill ← `self_build_self`
   - app_info: auth_id/vendor_id/self_type=APP/version as table above.
   - control infos (in order):
     - FLAGS (`type=1`, size `0x28`, next=1), 0x20 bytes zero.
     - DIGEST_40 (`type=2`, size `0x38`, next=0): digest1 = the static
       `0x627CB180…E4` constant, digest2 = `sha1(elf, elf_len)`, fw_version = 0
       (APP).
   - optional headers:
     - CAP_FLAGS (`type=1`, size `0x28`, next=0): unk3=unk4=0,
       flags=`0x7B` (SYSDBG|RETAIL|DEBUG|REFTOOL|0x3), unk6=1, unk7=0x20000.
   - sce_version: header_type=1, present=0 (NOT_PRESENT), size=0x10, unk3=0.

3. `layout_ctxt` ← `sce_layout_ctxt` + `_sce_fixup_keys` + `_sce_fixup_ctxt`
   - Offsets, each `ALIGN(.,0x10)`, in order: sce_header, self_header,
     app_info, ehdr, phdrs, section_info[], sce_version, control_infos,
     metadata_info, metadata_header, metadata_section_headers, **keys**,
     optional_headers, signature, then pad to `ALIGN(.,0x80)` = header_len.
   - keys table: per encrypted section 0x80 (8 slots: 6 HMAC + key + iv),
     per non-encrypted 0x60 (6 HMAC). Fill random. Set sha1/key/iv indices.
   - data sections start at header_len, each `ALIGN(.,0x10)`; set
     metash.data_offset/size + section_info.offset; data_len accumulates.
   - metadata_header: sig_input_length=off_sig, unknown_0=1,
     opt_header_size=Σ oh sizes, key_count from fixup_keys.
   - self_header offsets (app_info/elf/phdr/section_info/sce_version/
     control_info + control_info_size).

4. `build_header` ← `_sce_build_header`: allocate `header_len` buffer, copy all
   the above structs in (big-endian native on PPU — no byteswap), then the
   section data is written after header_len.

5. **Reuse `self_encrypt.c`**: point a `self_ctx_t` at the freshly built buffer
   (set sceh/selfh/ai/si/metai/metah/metash/keys pointers via `self_parse`
   after we synthesize a metadata_info key/iv), set `ctx.decrypted=1`, then call
   `self_encrypt(&ctx, &ks_rev04)`. It runs calculate_hashes → encrypt_body →
   sign_header (rev-04 ECDSA) → encrypt_metadata_block. Identical to scetool's
   `sce_encrypt_ctxt` back half.

   Note: `metadata_info.key`/`iv` must be randomized before encrypt (scetool
   fills `ctxt->metai` random; our decrypt path reads it from the input). Add a
   `metai` randomize step for the build path.

## Keys

`key_load.c` currently forces `parse_revision_zero` and loads ONE keyset. The
HEN path needs **two**:

- **decrypt keyset** — matches the input debug self. Debug selfs use the
  well-known debug erk/riv (key_revision 0x8000); body is often cleartext so
  decrypt may be a no-op, but metadata still needs the debug metadata keys.
- **encrypt keyset** — appldr **revision 0004** (erk/riv/pub/priv/ctype). This
  revision has `priv=YES` in the bundled `data/keys`, so real ECDSA signing
  works on-device.

Plan: extend `key_load` to accept a target revision (param), load the rev-04
appldr keyset for encrypt. Keep the existing single-keyset load for decrypt
(debug). Ship the rev-04 appldr entry (+ curves) to
`/dev_hdd0/plugins/taiko/keys` via `export_scetool_keyset.py` (already exists).

## eboot_flow changes

- Add a config switch (taiko_config.cfg, e.g. `target_signing = hen|dex`).
- HEN path: **do not** take the `self_has_clear_load_sections` shortcut.
  Instead: decrypt → `elf_extract` → patch ELF (patches_apply + sprx loader) →
  `self_build` (encode retail STD) → write. fix_elf SDK lowering applies to the
  extracted ELF before encode (lower sys_process_param sdk_version >0x32 to
  0x33), matching `scripts/resign_hen.sh`.
- DEX path: unchanged (format-preserving, current behavior).
- `expected_patched_hash` still tracks the final bytes so re-patch is skipped.

## Memory

Uncompressed STD encode roughly doubles on-disk size vs the compressed retail
original. The encode buffer must hold `header_len + Σ aligned section sizes`
(~ELF size, up to ~18 MB). Allocate via `sys_memory_allocate` (64K pages),
separate from the BSS heap (keep the 384K `HEAP_SIZE` per
`sprx_heap_size_real_hw`). Free promptly.

## Off-device de-risking

The encoder is plain C (mbedtls + sce_ecdsa/sce_curve, already host-portable).
Build a host harness: feed a decrypted ELF, run `self_build` + `self_encrypt`
with the rev-04 keyset, then verify the output with the bundled scetool
(`scetool -i` shows key rev 0x0004 / APP; `scetool --decrypt` round-trips).
Get it byte-plausible on host **before** touching real HW, where a single wrong
byte = silent no-boot.

## Reference (bundled scetool source)

`tools/SELF_Resigner_Linux/src/tool/scetool_source/`:
- `sce.cpp`: `sce_create_ctxt_build_self`, `sce_layout_ctxt`,
  `_sce_fixup_ctxt`, `_sce_fixup_keys`, `_sce_build_header`,
  `_sce_calculate_hashes`, `_sce_sign_header`, `_sce_encrypt_header`,
  `_sce_encrypt_data`, `sce_encrypt_ctxt`, `sce_write_ctxt`.
- `self.cpp`: `self_build_self`, `_build_self_64`, `_create_control_infos`,
  `_create_optional_headers`, `_fill_sce_version`, `_add_phdr_section`,
  `_set_cap_flags`, the static control digest/flags constants.
