# White Dani/Taikojuku Inline Hook Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the first binary-specific inline-hook spec and SDK-assembled payload for White AC15 Dani Dojo and Taikojuku display flow.

**Architecture:** This plan depends on `docs/superpowers/plans/2026-06-27-eboot-inline-hook-infrastructure.md`. The White payload is linked into the SPRX as read-only payload bytes under `patches/asm/`, the spec registry validates White-only hook and gate signatures, then the generic installer appends the payload to the game EBOOT RX segment and branches from `0x0067EB7C` to it. The payload preserves White's original `0x0D` row append, performs the proven Taikojuku bind/append flow, appends visible row `0x11`, and explicitly jumps to `0x0067DE40`.

**Tech Stack:** Sony Cell SDK PPU assembler via `ppu-lv2-gcc`, C99 spec table, PowerPC instruction words, existing `make` and `nmake` builds, static inspection with `rg` and SDK binutils.

---

## Scope Check

This plan is binary-specific. It adds one White ST71/SCEEXE001 `01.00` spec and one payload. Additional binary versions must be separate specs and payload files added after White static verification and user runtime validation.

## File Structure

- Create `patches/asm/white_dani_taikojuku_hook.S`: exact SDK-assembled payload source for the proven RPCS3 patch instruction stream, with exported start/end symbols.
- Modify `Makefile`: compile `patches/asm/white_dani_taikojuku_hook.S` and clean its object.
- Modify `Makefile.win`: compile the same assembly source under NMAKE and clean its object.
- Modify `eboot_patcher/eboot_inline_specs.c`: replace the empty registry with the White spec table and payload symbol references.

---

### Task 1: Add White Payload Assembly Source

**Files:**
- Create: `patches/asm/white_dani_taikojuku_hook.S`

- [ ] **Step 1: Create the assembly directory**

Run:

```powershell
New-Item -ItemType Directory -Force 'patches\asm' | Out-Null
```

Expected: `patches\asm` exists.

- [ ] **Step 2: Add the payload source**

Create `patches/asm/white_dani_taikojuku_hook.S` with this exact content:

```asm
    .section .rodata.taiko_patch_payload.white_dani_taikojuku,"a",@progbits
    .align 2
    .globl taiko_white_dani_taikojuku_hook_start
    .globl taiko_white_dani_taikojuku_hook_end
    .type taiko_white_dani_taikojuku_hook_start, @object

taiko_white_dani_taikojuku_hook_start:
    /* Prologue and original White 0x0D row append. */
    .long 0x7C0802A6
    .long 0xF821FE81
    .long 0xF8010170
    .long 0xF8410168
    .long 0x386101F8
    .long 0x3880000D
    .long 0x80BD0010
    .long 0x80DD0014
    .long 0x38E0FFFF
    .long 0x3D800067
    .long 0x618C9DC8
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168

    /* Guard against duplicate visible row 0x11 in the local row vector. */
    .long 0x38000000
    .long 0x90010040
    .long 0x90010044
    .long 0x90010048
    .long 0x386101F8
    .long 0x80830000
    .long 0x2F840000
    .long 0x419E02F8
    .long 0x78840020
    .long 0x80A40004
    .long 0x80C40008
    .long 0x7F853040
    .long 0x409C0020
    .long 0x78A70020
    .long 0x81070000
    .long 0x2F880011
    .long 0x419E02D4
    .long 0x38A50010
    .long 0x7F853040
    .long 0x419CFFE8

    /* Resolve and validate the White selected-pack object. */
    .long 0x3D800009
    .long 0x618C542C
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168
    .long 0x2F830000
    .long 0x419E02AC
    .long 0x78630020
    .long 0x80030000
    .long 0x2F800002
    .long 0x409E029C
    .long 0x38600018
    .long 0x3D800085
    .long 0x618CFCC8
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168
    .long 0x2F830000
    .long 0x419E027C

    /* Initialize raw Taikojuku row state. */
    .long 0x7C691B78
    .long 0x38000000
    .long 0x90090000
    .long 0x90090004
    .long 0x90090008
    .long 0x38000001
    .long 0x98090014
    .long 0x98090015
    .long 0x38000000
    .long 0x90010040
    .long 0x90610044
    .long 0x90010048
    .long 0x78690020
    .long 0x90690000
    .long 0x90690004
    .long 0x90690008

    /* Load selected raw pack number from White's +0x2D0 fields. */
    .long 0x39400001
    .long 0x81750008
    .long 0x2F8B0000
    .long 0x419E0050
    .long 0x796B0020
    .long 0x880B0000
    .long 0x2F800000
    .long 0x419E001C
    .long 0x812B0028
    .long 0x2F890000
    .long 0x419E0010
    .long 0x79290020
    .long 0x814902D0
    .long 0x48000028
    .long 0x396B002C
    .long 0x880B0000
    .long 0x2F800000
    .long 0x419E0018
    .long 0x812B0028
    .long 0x2F890000
    .long 0x419E000C
    .long 0x79290020
    .long 0x814902D0
    .long 0x2F8A0001
    .long 0x419C000C
    .long 0x2F8A000A
    .long 0x409D0008
    .long 0x39400001

    /* Resolve raw pack through White helper 0x00954550. */
    .long 0x38610010
    .long 0x7D445378
    .long 0x3D800009
    .long 0x618C5450
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168

    /* Clamp and copy the returned raw music list. */
    .long 0x81010010
    .long 0x81210014
    .long 0x7F884840
    .long 0x409C017C
    .long 0x91010018
    .long 0x7D284850
    .long 0x7D291E70
    .long 0x2F890000
    .long 0x409D0168
    .long 0x2F890003
    .long 0x409D0008
    .long 0x39200003
    .long 0x9121001C
    .long 0x38000000
    .long 0x90010030
    .long 0x38C10080
    .long 0x90C10034
    .long 0x55271838
    .long 0x7CE63A14
    .long 0x90E10038
    .long 0x90E1003C
    .long 0x7D2903A6
    .long 0x38A00000
    .long 0x81010018
    .long 0x79080020
    .long 0x80080000
    .long 0x90060000
    .long 0x90A60004
    .long 0x39080008
    .long 0x91010018
    .long 0x38C60008
    .long 0x4200FFE0

    /* Bind music indices through White helpers. */
    .long 0x38610040
    .long 0x38810030
    .long 0x3D800008
    .long 0x618C7C88
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168
    .long 0x39610040
    .long 0x91610070
    .long 0x38610070
    .long 0x389301E8
    .long 0x3D800008
    .long 0x618C6B80
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168

    /* Compute backing-vector append metadata and extra=-1 semantics. */
    .long 0x81350000
    .long 0x2F890000
    .long 0x419E00C0
    .long 0x792A0020
    .long 0x816A0004
    .long 0x39000000
    .long 0x2F8B0000
    .long 0x419E0010
    .long 0x800A0008
    .long 0x7D0B0050
    .long 0x7D081670
    .long 0x38F301E8
    .long 0x80A70004
    .long 0x38C00000
    .long 0x2F850000
    .long 0x419E001C
    .long 0x80070008
    .long 0x7C050050
    .long 0x7C001670
    .long 0x3D804F72
    .long 0x618CC235
    .long 0x7CC061D6
    .long 0x38000000
    .long 0x90010050
    .long 0x91010054
    .long 0x3800FFFF
    .long 0x90010058
    .long 0x9121005C
    .long 0x90C10060
    .long 0x38610050
    .long 0x38810030
    .long 0x3D800067
    .long 0x618CA2F0
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168

    /* Append visible row 0x11 and clean row-local state. */
    .long 0x80C10050
    .long 0x2F860000
    .long 0x419E0030
    .long 0x386101F8
    .long 0x38800011
    .long 0x80A10054
    .long 0x7CA507B4
    .long 0x7CC607B4
    .long 0x38E0FFFF
    .long 0x3D800067
    .long 0x618C9DC8
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168
    .long 0x80610044
    .long 0x2F830000
    .long 0x419E001C
    .long 0x38610040
    .long 0x3D800067
    .long 0x618CAD7C
    .long 0x7D8903A6
    .long 0x4E800421
    .long 0xE8410168

    /* Epilogue and explicit jump to White row-iteration continuation. */
    .long 0xE8010170
    .long 0xE8410168
    .long 0x7C0803A6
    .long 0x38210180
    .long 0x3D800067
    .long 0x618CDE40
    .long 0x7D8903A6
    .long 0x4E800420

taiko_white_dani_taikojuku_hook_end:
    .size taiko_white_dani_taikojuku_hook_start, . - taiko_white_dani_taikojuku_hook_start
```

- [ ] **Step 3: Verify the payload instruction count**

Run:

```powershell
(Select-String -Pattern '^\s*\.long' 'patches\asm\white_dani_taikojuku_hook.S').Count
```

Expected output:

```text
219
```

- [ ] **Step 4: Commit**

```bash
git add patches/asm/white_dani_taikojuku_hook.S
git commit -m "Add White Dani Taikojuku hook payload"
```

---

### Task 2: Build the Assembly Payload

**Files:**
- Modify: `Makefile`
- Modify: `Makefile.win`

- [ ] **Step 1: Add the assembly object to GNU Make**

In `Makefile`, add these lines after `OBJS    := $(SRCS:.c=.o)`:

```make
ASM_SRCS := patches/asm/white_dani_taikojuku_hook.S
ASM_OBJS := $(ASM_SRCS:.S=.o)
OBJS += $(ASM_OBJS)
```

Add this compile rule after the existing `%.o: %.c` rule:

```make
%.o: %.S
	$(PPU_CC) $(CFLAGS) -c $< -o $@
```

Add this dependency line near the other patcher and patch dependencies:

```make
patches/asm/white_dani_taikojuku_hook.o: patches/asm/white_dani_taikojuku_hook.S
```

Modify the `clean` rule so the first `rm -f` line includes `$(ASM_OBJS)`:

```make
	rm -f $(OBJS) $(ASM_OBJS) $(SPU_QR_OBJS) $(SPU_QR_ELF) $(SYM) $(PRX) $(SPRX)
```

Modify the `clean-prx` rule the same way:

```make
	rm -f $(OBJS) $(ASM_OBJS) $(SPU_QR_OBJS) $(SPU_QR_ELF) $(SYM) $(PRX) $(SPRX)
```

- [ ] **Step 2: Add the assembly object to NMAKE**

In `Makefile.win`, add this variable block after `MAIN_OBJS = $(SRCS:.c=.o)`:

```make
ASM_OBJS = patches\asm\white_dani_taikojuku_hook.o
```

Replace the current `OBJS = ...` line with this line:

```make
OBJS = $(MAIN_OBJS) $(SPU_QR_PPU_OBJ) $(MBEDTLS_OBJS) $(QUIRC_OBJS) $(ASM_OBJS)
```

Add this explicit assembly compile rule near the other explicit object rules:

```make
patches\asm\white_dani_taikojuku_hook.o: patches\asm\white_dani_taikojuku_hook.S
	$(PPU_CC) $(CFLAGS) -c patches\asm\white_dani_taikojuku_hook.S -o $@
```

Modify the `clean-prx` object cleanup line so it also removes assembly objects:

```make
	-del /Q core\*.o config\*.o patches\*.o patches\asm\*.o hooks\*.o input\*.o qr\*.o qr_spu\*.o network\*.o storage\*.o bpreader\*.o cards\*.o eboot_patcher\*.o mod_menu\*.o ftp\*.o vendor\mbedtls\library\*.o vendor\quirc\lib\*.o *.o 2>NUL
```

- [ ] **Step 3: Build**

Run:

```bat
nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: `patches\asm\white_dani_taikojuku_hook.o` is compiled and included in the `zucchini.sym` link. GNU Make equivalent:

```bash
make CELL_SDK=/path/to/cell TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: `patches/asm/white_dani_taikojuku_hook.o` is compiled.

- [ ] **Step 4: Inspect exported symbols**

Run with the SDK objdump available in the current environment:

```powershell
$cellSdk = if ($env:CELL_SDK) { $env:CELL_SDK } else { $env:SCE_PS3_ROOT }
& "$cellSdk\host-win32\ppu\bin\ppu-lv2-objdump.exe" -t 'patches\asm\white_dani_taikojuku_hook.o' | Select-String 'taiko_white_dani_taikojuku_hook'
```

Expected output includes both symbols:

```text
taiko_white_dani_taikojuku_hook_start
taiko_white_dani_taikojuku_hook_end
```

- [ ] **Step 5: Commit**

```bash
git add Makefile Makefile.win
git commit -m "Build White inline hook payload"
```

---

### Task 3: Populate the White Inline Hook Spec

**Files:**
- Modify: `eboot_patcher/eboot_inline_specs.c`

- [ ] **Step 1: Replace the empty registry**

Replace the entire content of `eboot_patcher/eboot_inline_specs.c` with this exact content:

```c
#include <stddef.h>
#include <stdint.h>

#include "eboot_inline_specs.h"
#include "eboot_inline_hook.h"
#include "config/runtime.h"

extern const uint8_t taiko_white_dani_taikojuku_hook_start[];
extern const uint8_t taiko_white_dani_taikojuku_hook_end[];

static const uint32_t WHITE_ROW_HOOK_WORDS[] = {
    0x3880000Du, /* li r4,0x0d */
    0x80BD0010u, /* lwz r5,0x10(r29) */
    0x80DD0014u, /* lwz r6,0x14(r29) */
    0x38E0FFFFu, /* li r7,-1 */
};

static const uint32_t WHITE_ROW_HOOK_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint32_t WHITE_DANI_COUNT_GATE_WORDS[] = {
    0x812B000Cu,
    0x2F890000u,
    0x419E001Cu,
    0x69290000u,
};

static const uint32_t WHITE_DANI_COUNT_GATE_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint32_t WHITE_DANI_EMIT_GATE_WORDS[] = {
    0x2F800000u,
    0x419E0000u,
    0x2F800009u,
    0x60000000u,
    0x2B800009u,
};

static const uint32_t WHITE_DANI_EMIT_GATE_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFF0003u,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const eboot_inline_signature_t WHITE_DANI_TAIKOJUKU_SIGNATURES[] = {
    {
        "white row 0x0d hook",
        0x0067EB7Cu,
        WHITE_ROW_HOOK_WORDS,
        WHITE_ROW_HOOK_MASKS,
        sizeof(WHITE_ROW_HOOK_WORDS) / sizeof(WHITE_ROW_HOOK_WORDS[0]),
    },
    {
        "white dani count gate patched",
        0x0067DD30u,
        WHITE_DANI_COUNT_GATE_WORDS,
        WHITE_DANI_COUNT_GATE_MASKS,
        sizeof(WHITE_DANI_COUNT_GATE_WORDS) / sizeof(WHITE_DANI_COUNT_GATE_WORDS[0]),
    },
    {
        "white dani emit gate patched",
        0x0067DE00u,
        WHITE_DANI_EMIT_GATE_WORDS,
        WHITE_DANI_EMIT_GATE_MASKS,
        sizeof(WHITE_DANI_EMIT_GATE_WORDS) / sizeof(WHITE_DANI_EMIT_GATE_WORDS[0]),
    },
};

static const eboot_inline_hook_spec_t INLINE_HOOK_SPECS[] = {
    {
        "dani_dojo_unlock",
        "white-st71-sceexe001-01.00-dani-taikojuku",
        0x0067EB7Cu,
        WHITE_DANI_TAIKOJUKU_SIGNATURES,
        sizeof(WHITE_DANI_TAIKOJUKU_SIGNATURES) /
            sizeof(WHITE_DANI_TAIKOJUKU_SIGNATURES[0]),
        taiko_white_dani_taikojuku_hook_start,
        taiko_white_dani_taikojuku_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0067DE40u,
    },
};

static const size_t INLINE_HOOK_SPEC_COUNT =
    sizeof(INLINE_HOOK_SPECS) / sizeof(INLINE_HOOK_SPECS[0]);

int eboot_inline_hooks_apply(self_ctx_t *ctx) {
    if (!g_cfg.dani_dojo_unlock)
        return 0;
    return eboot_inline_hook_apply(ctx, INLINE_HOOK_SPECS,
                                   INLINE_HOOK_SPEC_COUNT,
                                   "dani_dojo_unlock");
}
```

- [ ] **Step 2: Build**

Run:

```bat
nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: `eboot_patcher\eboot_inline_specs.o` compiles and the link resolves `taiko_white_dani_taikojuku_hook_start` and `taiko_white_dani_taikojuku_hook_end`. GNU Make equivalent:

```bash
make CELL_SDK=/path/to/cell TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: `bin/zucchini.sprx` is produced if local SDK configuration is valid.

- [ ] **Step 3: Verify the White constants are present**

Run:

```bash
rg -n "0x0067EB7C|0x0067DE40|0x0067DD30|0x0067DE00|white-st71" eboot_patcher/eboot_inline_specs.c
```

Expected output includes all five constants and the `white-st71-sceexe001-01.00-dani-taikojuku` binary id.

- [ ] **Step 4: Commit**

```bash
git add eboot_patcher/eboot_inline_specs.c
git commit -m "Add White Dani Taikojuku inline hook spec"
```

---

### Task 4: Verify Static Installer Inputs

**Files:**
- Inspect: `patches/asm/white_dani_taikojuku_hook.S`
- Inspect: `eboot_patcher/eboot_inline_specs.c`
- Inspect: `eboot_patcher/eboot_inline_hook.c`

- [ ] **Step 1: Verify the payload has explicit continuation semantics**

Run:

```bash
rg -n "618CDE40|EBOOT_INLINE_RETURN_EXPLICIT|0x0067DE40" patches/asm/white_dani_taikojuku_hook.S eboot_patcher/eboot_inline_specs.c
```

Expected output includes:

```text
patches/asm/white_dani_taikojuku_hook.S:    .long 0x618CDE40
eboot_patcher/eboot_inline_specs.c:        EBOOT_INLINE_RETURN_EXPLICIT,
eboot_patcher/eboot_inline_specs.c:        0x0067DE40u,
```

- [ ] **Step 2: Verify the spec validates more than the hook word**

Run:

```bash
rg -n "WHITE_DANI_COUNT_GATE_WORDS|WHITE_DANI_EMIT_GATE_WORDS|WHITE_ROW_HOOK_WORDS|WHITE_DANI_TAIKOJUKU_SIGNATURES" eboot_patcher/eboot_inline_specs.c
```

Expected output includes all four names, proving the spec table validates the row hook plus the two patched Dani gate contexts.

- [ ] **Step 3: Verify normal builds do not need external generators**

Run:

```bash
rg -n "keystone|python|incbin|imported_patch" Makefile Makefile.win patches/asm/white_dani_taikojuku_hook.S eboot_patcher/eboot_inline_specs.c
```

Expected: no output.

- [ ] **Step 4: Verify the linked payload size**

Run:

```powershell
$cellSdk = if ($env:CELL_SDK) { $env:CELL_SDK } else { $env:SCE_PS3_ROOT }
$objdump = "$cellSdk\host-win32\ppu\bin\ppu-lv2-objdump.exe"
& $objdump -t 'patches\asm\white_dani_taikojuku_hook.o' | Select-String 'taiko_white_dani_taikojuku_hook'
```

Expected: the object contains both start and end symbols. The source has `219` `.long` entries, so the copied payload size is `876` bytes (`0x36C`) before any installer-added return branch. This White spec uses `EBOOT_INLINE_RETURN_EXPLICIT`, so no auto-return word is appended.

- [ ] **Step 5: Commit only if verification changed code**

If a verification fix changed files, commit the focused fix:

```bash
git add patches/asm/white_dani_taikojuku_hook.S eboot_patcher/eboot_inline_specs.c Makefile Makefile.win
git commit -m "Fix White inline hook static verification"
```

If no files changed, do not create a commit.

---

### Task 5: Patch-Time Static Verification on a White EBOOT

**Files:**
- Inspect runtime log from the device or RPCS3 environment used to run the existing EBOOT patch flow.
- Inspect patched `EBOOT.BIN` only when a patchable White EBOOT is locally available.

- [ ] **Step 1: Run the normal patch flow**

Use the repository's existing first-run bootstrap or runtime repatch path against a White ST71/SCEEXE001 `01.00` EBOOT. The build artifact must be the `zucchini.sprx` produced after Tasks 1-4.

Expected patch-time log lines include:

```text
[patch] Dan-i Dojo count gate
[patch] Dan-i Dojo emit gate
[patch] Dan-i Dojo dormant case
[patch] inline hook installed: dani_dojo_unlock / white-st71-sceexe001-01.00-dani-taikojuku
[patch] inline hook site
[patch] inline payload VA
[patch] inline payload words
[patch] inline continuation
[patch] SPRX loader payload
```

- [ ] **Step 2: Confirm the hook site branch is installed before runtime validation**

If the patched White `EBOOT.BIN` is available as a local file, inspect the instruction at VA `0x0067EB7C` with the same SELF/ELF mapping logic used by `sce_segmap_build`. The expected installed instruction must be a relative unconditional branch where:

```text
(word & 0xFC000003) == 0x48000000
branch_target(word, 0x0067EB7C) == inline payload VA from the patch log
```

- [ ] **Step 3: Confirm payload bytes match linked source**

If the patched White `EBOOT.BIN` is available as a local file, inspect the payload VA from the patch log and confirm the first and last words match the linked source:

```text
payload[0x000] == 0x7C0802A6
payload[0x004] == 0xF821FE81
payload[0x364] == 0x7D8903A6
payload[0x368] == 0x4E800420
```

Expected: the payload length copied for White is `0x36C` bytes and no auto-return word follows it.

- [ ] **Step 4: Record runtime validation boundary**

Runtime validation is performed by the user on RPCS3, PS3, or S357 hardware. The runtime acceptance notes for White must be limited to these observations:

```text
Dani gate behavior remains enabled.
The original 0x0D row behavior is preserved.
The Taikojuku row 0x11 appears and behaves like the proven RPCS3 patch.
```

Do not claim runtime success from a build, symbol inspection, patch log, or static EBOOT inspection alone.

- [ ] **Step 5: Commit only if patch-time verification changed code**

If static inspection finds a code defect and a fix is made, commit that focused fix:

```bash
git add patches/asm/white_dani_taikojuku_hook.S eboot_patcher/eboot_inline_specs.c eboot_patcher/eboot_inline_hook.c
git commit -m "Fix White inline hook patch-time verification"
```

If runtime notes are recorded outside this repository or no code changed, do not create a commit.

---

## Self-Review

Spec coverage:

- First payload targets White Dani Dojo and Taikojuku display flow: Tasks 1 and 3.
- Fixed Dani gate writes remain in existing self-validating patch logic: Task 3 validates their post-patch contexts instead of replacing them.
- White row hook branches from `0x0067EB7C`: Task 3.
- Payload appends original `0x0D` row and visible `0x11` row: Task 1.
- Payload explicitly jumps to `0x0067DE40`: Tasks 1 and 4.
- SDK assembly is part of normal `make` and `nmake` builds: Task 2.
- No Keystone, Python, or generated patch bytes are required by normal builds: Task 4.
- White validation includes more than one hook-site word: Task 3 validates row hook, count gate, and emit gate signatures.
- Static verification is separated from user runtime validation: Task 5.

Placeholder scan:

- Red-flag no-op wording scan passed for the task steps and code steps.

Type consistency:

- `taiko_white_dani_taikojuku_hook_start` and `taiko_white_dani_taikojuku_hook_end` are exported by the assembly file and declared as `extern const uint8_t[]` in the C registry.
- `WHITE_DANI_TAIKOJUKU_SIGNATURES` uses the `eboot_inline_signature_t` layout defined by the infrastructure plan.
- `INLINE_HOOK_SPECS` uses `EBOOT_INLINE_RETURN_EXPLICIT`, so the generic installer does not append a return branch after the White payload.
