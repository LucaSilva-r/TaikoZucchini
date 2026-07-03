# Kimidori Dani Native Flow Restoration Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Restore Kimidori Dani Dojo by routing the restored Kimidori Dani row through the existing native Dani `GameDojoSelect::Proc_Main` branch instead of the generic state-6 path that drops required `entry` resources.

**Architecture:** Keep the existing pre-RED emit-gate hook and Kimidori row hook as the visibility layer. Add one Kimidori-only EBOOT inline hook over `GameDojoSelect::Proc_Main` at `0x005666C`; the payload preserves the native `0x1A` Dani marker and also routes the restored row marker `0x0D` to the existing native Dani target at `0x00566E8`, while all non-Dani markers branch to the original normal path at `0x0056674`. No resource root slots, global resource maps, renderer guards, or synthetic loaders are touched.

**Tech Stack:** C99/GNU99, Sony Cell SDK PPU assembler, existing EBOOT inline-hook substrate, PowerPC branch signatures, IDA-CLI daemon-backed static evidence, `make`/`nmake` builds.

---

## Scope Check

This is one binary-specific subsystem: Kimidori ST51/v05r00 EBOOT inline-hook restoration for Dani Dojo. It deliberately does not add Taikojuku, does not change Murasaki/White/Momoiro specs, and does not implement the fallback entry-preload hook. If Task 2 evidence fails, or if user runtime still crashes at `0x003B2524` after this branch fix, stop and write a separate fallback plan for native entry-preload restoration.

## File Structure

- Create `docs/superpowers/evidence/2026-07-04-kimidori-dani-proc-main-branch.md`: IDA and source evidence proving the restored row marker and native branch target.
- Keep `patches/asm/kimidori_dani_dojo_hook.S`: existing visibility payload that appends row `0x0D`.
- Create `patches/asm/kimidori_dani_proc_main_hook.S`: small Kimidori-only payload that restores the compiled-away Dani decision branch for marker `0x0D`.
- Modify `Makefile`: compile the new assembly payload.
- Modify `Makefile.win`: compile the new assembly payload in Windows builds.
- Modify `eboot_patcher/eboot_inline_specs.c`: declare the new payload symbols, add a strict Kimidori `Proc_Main` signature, and register the new hook spec under `dani_dojo_unlock`.

## Current Baseline Notes

The worktree may already contain uncommitted Kimidori row-visibility changes:

```text
Makefile
Makefile.win
eboot_patcher/eboot_inline_specs.c
patches/asm/kimidori_dani_dojo_hook.S
```

Do not revert those changes. Stage only the files named by each task. Before committing the baseline row hook, verify that it contains only the Kimidori row visibility layer and no entry/root-slot resource patch.

---

### Task 1: Freeze the Existing Kimidori Row Visibility Baseline

**Files:**
- Inspect: `patches/asm/kimidori_dani_dojo_hook.S`
- Inspect: `Makefile`
- Inspect: `Makefile.win`
- Inspect: `eboot_patcher/eboot_inline_specs.c`
- Commit if still dirty: `patches/asm/kimidori_dani_dojo_hook.S`, `Makefile`, `Makefile.win`, `eboot_patcher/eboot_inline_specs.c`

- [ ] **Step 1: Verify the row hook exists**

Run:

```powershell
Test-Path 'patches\asm\kimidori_dani_dojo_hook.S'
```

Expected output:

```text
True
```

- [ ] **Step 2: Verify the row hook appends visible row `0x0D`**

Run:

```powershell
rg -n "li\s+r0,13|stw\s+r0,32\(r1\)|0x0057C588|kimidori-st51-v05r00-dani-row" patches\asm\kimidori_dani_dojo_hook.S eboot_patcher\eboot_inline_specs.c
```

Expected output includes all of these facts:

```text
patches\asm\kimidori_dani_dojo_hook.S:    li      r0,13
patches\asm\kimidori_dani_dojo_hook.S:    stw     r0,32(r1)
eboot_patcher\eboot_inline_specs.c:        "kimidori-st51-v05r00-dani-row",
eboot_patcher\eboot_inline_specs.c:        0x0057C588u,
```

- [ ] **Step 3: Verify no resource-root overwrite is present in the row hook**

Run:

```powershell
rg -n "C00380|dword_C00380|0x000A00EC|0xA4|0xA8|entry/packeddata|resource parent|root slot" patches\asm\kimidori_dani_dojo_hook.S eboot_patcher\eboot_inline_specs.c
```

Expected: no output.

- [ ] **Step 4: Verify both build files include the row hook**

Run:

```powershell
rg -n "kimidori_dani_dojo_hook" Makefile Makefile.win
```

Expected output includes:

```text
Makefile:            patches/asm/kimidori_dani_dojo_hook.S
Makefile:patches/asm/kimidori_dani_dojo_hook.o: patches/asm/kimidori_dani_dojo_hook.S
Makefile.win:           patches\asm\kimidori_dani_dojo_hook.o
Makefile.win:patches\asm\kimidori_dani_dojo_hook.o: patches\asm\kimidori_dani_dojo_hook.S
```

- [ ] **Step 5: Commit the row visibility baseline if those files are still uncommitted**

Run:

```powershell
git status --short -- Makefile Makefile.win eboot_patcher/eboot_inline_specs.c patches/asm/kimidori_dani_dojo_hook.S
```

If the output still shows only the baseline row-hook files, commit them:

```powershell
git add -- Makefile Makefile.win eboot_patcher/eboot_inline_specs.c patches/asm/kimidori_dani_dojo_hook.S
git commit -m "Add Kimidori Dani row visibility hook"
```

Expected commit result includes:

```text
Add Kimidori Dani row visibility hook
```

If the output is empty because the baseline was already committed, do not create a commit for this task.

---

### Task 2: Record Native Branch Evidence

**Files:**
- Create: `docs/superpowers/evidence/2026-07-04-kimidori-dani-proc-main-branch.md`

- [ ] **Step 1: Create the evidence directory**

Run:

```powershell
New-Item -ItemType Directory -Force 'docs\superpowers\evidence' | Out-Null
```

Expected: `docs\superpowers\evidence` exists.

- [ ] **Step 2: Run the IDA proof command**

Run:

```powershell
@'
import os
from ida_cli.agent_bridge import AgentSession

os.environ['IDA_CLI_DAEMON_DIR'] = r'C:\Users\10614\.ida-cli\daemons'
targets = {
    'kimidori': (r'H:\taiko\kimidori\EBOOT.ELF.i64',
                 [0x56650, 0x56654, 0x56658, 0x5665C, 0x56660, 0x56664,
                  0x56668, 0x5666C, 0x56670, 0x56674, 0x56680,
                  0x566E8, 0x566EC, 0x56700]),
    'murasaki': (r'H:\taiko\murasaki\EBOOT.ELF.i64',
                 [0x71000, 0x71004, 0x71008, 0x7100C, 0x71010, 0x71014,
                  0x71018, 0x7101C, 0x71020, 0x71024, 0x71030,
                  0x71098, 0x7109C, 0x710B0]),
}

for name, (target, addrs) in targets.items():
    print('==== ' + name)
    with AgentSession.start(target, daemon=True, require_ida=True, request_timeout_s=240) as ida:
        ida.probe_backend(require_ida=True)
        code = '''
import ida_bytes, idc
addrs = %r
rows = []
for ea in addrs:
    rows.append('%08X %08X %s' % (ea, ida_bytes.get_dword(ea), idc.generate_disasm_line(ea, 0)))
__result__ = '\\n'.join(rows)
''' % (addrs,)
        print(ida.result(code, request_id=name + '.procmain.branch.words', timeout_s=60))
'@ | python -
```

Expected Kimidori output includes:

```text
00056650 38800007 li        r4, 7
00056668 80090000 lwz       r0, 0(r9)
0005666C 2F80001A cmpwi     cr7, r0, 0x1A
00056670 419E0078 beq       cr7, loc_566E8
00056674 E8010090 ld        r0, 0x80+sender_lr(r1)
00056680 38800006 li        r4, 6
000566E8 38000002 li        r0, 2
000566EC 901F0014 stw       r0, 0x14(r31)
00056700 4BFFFD30 b         sub_56430
```

Expected Murasaki output includes:

```text
00071000 38800007 li        r4, 7
00071018 80090000 lwz       r0, 0(r9)
0007101C 2F80001D cmpwi     cr7, r0, 0x1D
00071020 419E0078 beq       cr7, loc_71098
00071024 E8010090 ld        r0, 0x80+sender_lr(r1)
00071030 38800006 li        r4, 6
00071098 38000002 li        r0, 2
0007109C 901F0014 stw       r0, 0x14(r31)
000710B0 4BFFFC58 b         sub_70D08
```

- [ ] **Step 3: Write the evidence file**

Create `docs/superpowers/evidence/2026-07-04-kimidori-dani-proc-main-branch.md` with this content:

````markdown
# Kimidori Dani Proc_Main Branch Evidence

Date: 2026-07-04

## Conclusion

The Kimidori row visibility hook restores a selectable row whose first record word is `0x0D`. Kimidori `GameDojoSelect::Proc_Main` only recognizes native marker `0x1A` as Dani and sends every other marker through generic state 6. The fix should restore the compiled-away decision branch by treating restored marker `0x0D` as an alias for the native Dani branch target at `0x00566E8`.

## Kimidori Proc_Main

Relevant words:

```text
00056650 38800007 li        r4, 7
00056668 80090000 lwz       r0, 0(r9)
0005666C 2F80001A cmpwi     cr7, r0, 0x1A
00056670 419E0078 beq       cr7, loc_566E8
00056674 E8010090 ld        r0, 0x80+sender_lr(r1)
00056680 38800006 li        r4, 6
000566E8 38000002 li        r0, 2
000566EC 901F0014 stw       r0, 0x14(r31)
00056700 4BFFFD30 b         sub_56430
```

Interpretation:

- `0x0056668` loads the selected row marker from the selected 16-byte row record.
- `0x005666C` compares that marker against Kimidori native Dani marker `0x1A`.
- `0x0056670` branches to `0x00566E8` on native Dani.
- `0x00566E8` sets `a1+0x14` to `2`, then reaches `GameDojoSelect::ChangeState(a1, 7)` with `r4` still equal to `7`.
- The non-Dani path at `0x0056674` changes `r4` to `6` and enters generic state 6.

## Murasaki Shape Check

Murasaki has the same branch shape with its own marker:

```text
00071000 38800007 li        r4, 7
00071018 80090000 lwz       r0, 0(r9)
0007101C 2F80001D cmpwi     cr7, r0, 0x1D
00071020 419E0078 beq       cr7, loc_71098
00071024 E8010090 ld        r0, 0x80+sender_lr(r1)
00071030 38800006 li        r4, 6
00071098 38000002 li        r0, 2
0007109C 901F0014 stw       r0, 0x14(r31)
000710B0 4BFFFC58 b         sub_70D08
```

## Row Hook Marker

`patches/asm/kimidori_dani_dojo_hook.S` writes `13` to the first word of the row record before appending it:

```asm
li      r0,13
stw     r0,32(r1)
```

The `Proc_Main` branch hook must therefore preserve native marker `0x1A` and also route restored marker `0x0D` to the same native target. This is branch restoration; it does not load, allocate, or graft resources.

## Negative Boundaries

This evidence does not justify any of these changes:

- writing `entry` resources into `GameDojoSelect` root/shared slots;
- inserting `0x000A00EC` into the resource map;
- guarding the renderer null dereference;
- changing Murasaki, White, or Momoiro hook behavior.
````

- [ ] **Step 4: Commit the evidence**

Run:

```powershell
git add -- docs/superpowers/evidence/2026-07-04-kimidori-dani-proc-main-branch.md
git commit -m "Record Kimidori Dani Proc_Main branch evidence"
```

Expected commit result includes:

```text
Record Kimidori Dani Proc_Main branch evidence
```

---

### Task 3: Add the Kimidori Proc_Main Branch Payload

**Files:**
- Create: `patches/asm/kimidori_dani_proc_main_hook.S`

- [ ] **Step 1: Verify the payload does not already exist**

Run:

```powershell
rg -n "taiko_kimidori_dani_proc_main_hook|kimidori-st51-v05r00-dani-proc-main" patches eboot_patcher
```

Expected: no output.

- [ ] **Step 2: Add the assembly payload**

Create `patches/asm/kimidori_dani_proc_main_hook.S` with this exact content:

```asm
    .section .rodata.taiko_patch_payload.kimidori_dani_proc_main,"a",@progbits
    .align 2
    .globl taiko_kimidori_dani_proc_main_hook_start
    .globl taiko_kimidori_dani_proc_main_hook_end
    .type taiko_kimidori_dani_proc_main_hook_start, @object

    .set r0,0
    .set r12,12
    .set BO_IF_TRUE,12
    .set CR7_EQ,30

taiko_kimidori_dani_proc_main_hook_start:
    /*
     * Hooked over Kimidori 0x005666C:
     *   cmpwi cr7,r0,0x1A
     *
     * The restored row hook emits marker 0x0D. Preserve native marker 0x1A
     * and route both markers to the existing Dani branch target. Every other
     * marker resumes the original non-Dani path at 0x0056674.
     */
    cmpwi   cr7,r0,0x1A
    bc      BO_IF_TRUE,CR7_EQ,kimidori_proc_main_dani
    cmpwi   cr7,r0,0x0D
    bc      BO_IF_TRUE,CR7_EQ,kimidori_proc_main_dani

    lis     r12,0x0005
    ori     r12,r12,0x6674
    mtctr   r12
    bctr

kimidori_proc_main_dani:
    lis     r12,0x0005
    ori     r12,r12,0x66E8
    mtctr   r12
    bctr

taiko_kimidori_dani_proc_main_hook_end:
    .size taiko_kimidori_dani_proc_main_hook_start, . - taiko_kimidori_dani_proc_main_hook_start
```

- [ ] **Step 3: Verify the payload exports start/end symbols**

Run:

```powershell
rg -n "taiko_kimidori_dani_proc_main_hook_start|taiko_kimidori_dani_proc_main_hook_end" patches\asm\kimidori_dani_proc_main_hook.S
```

Expected output includes both symbol names.

- [ ] **Step 4: Verify the payload only branches to the two native Kimidori targets**

Run:

```powershell
rg -n "0x6674|0x66E8|0x0005|0xA4|0xA8|C00380|0x000A00EC" patches\asm\kimidori_dani_proc_main_hook.S
```

Expected output includes `0x6674`, `0x66E8`, and `0x0005`; it must not include `0xA4`, `0xA8`, `C00380`, or `0x000A00EC`.

- [ ] **Step 5: Commit the payload**

Run:

```powershell
git add -- patches/asm/kimidori_dani_proc_main_hook.S
git commit -m "Add Kimidori Dani Proc_Main hook payload"
```

Expected commit result includes:

```text
Add Kimidori Dani Proc_Main hook payload
```

---

### Task 4: Wire the New Payload into Both Builds

**Files:**
- Modify: `Makefile`
- Modify: `Makefile.win`

- [ ] **Step 1: Add the assembly source to GNU Make**

In `Makefile`, update the `ASM_SRCS` block to include `patches/asm/kimidori_dani_proc_main_hook.S` immediately after `patches/asm/kimidori_dani_dojo_hook.S`:

```make
ASM_SRCS := patches/asm/white_dani_taikojuku_hook.S \
            patches/asm/murasaki_dani_taikojuku_hook.S \
            patches/asm/kimidori_dani_dojo_hook.S \
            patches/asm/kimidori_dani_proc_main_hook.S \
            patches/asm/pre_red_dani_emit_gate_hook.S
```

Add this dependency line near the existing assembly dependency lines:

```make
patches/asm/kimidori_dani_proc_main_hook.o: patches/asm/kimidori_dani_proc_main_hook.S
```

- [ ] **Step 2: Add the assembly object to Windows NMAKE**

In `Makefile.win`, update the `ASM_OBJS` block to include `patches\asm\kimidori_dani_proc_main_hook.o` immediately after `patches\asm\kimidori_dani_dojo_hook.o`:

```make
ASM_OBJS = patches\asm\white_dani_taikojuku_hook.o \
           patches\asm\murasaki_dani_taikojuku_hook.o \
           patches\asm\kimidori_dani_dojo_hook.o \
           patches\asm\kimidori_dani_proc_main_hook.o \
           patches\asm\pre_red_dani_emit_gate_hook.o
```

Add this explicit compile rule after the `kimidori_dani_dojo_hook.o` rule:

```make
patches\asm\kimidori_dani_proc_main_hook.o: patches\asm\kimidori_dani_proc_main_hook.S
	$(PPU_CC) $(CFLAGS) -c patches\asm\kimidori_dani_proc_main_hook.S -o $@
```

- [ ] **Step 3: Verify both build files reference the new payload**

Run:

```powershell
rg -n "kimidori_dani_proc_main_hook" Makefile Makefile.win
```

Expected output includes:

```text
Makefile:            patches/asm/kimidori_dani_proc_main_hook.S
Makefile:patches/asm/kimidori_dani_proc_main_hook.o: patches/asm/kimidori_dani_proc_main_hook.S
Makefile.win:           patches\asm\kimidori_dani_proc_main_hook.o
Makefile.win:patches\asm\kimidori_dani_proc_main_hook.o: patches\asm\kimidori_dani_proc_main_hook.S
```

- [ ] **Step 4: Commit the build wiring**

Run:

```powershell
git add -- Makefile Makefile.win
git commit -m "Build Kimidori Dani Proc_Main hook payload"
```

Expected commit result includes:

```text
Build Kimidori Dani Proc_Main hook payload
```

---

### Task 5: Register the Kimidori Proc_Main Inline Hook Spec

**Files:**
- Modify: `eboot_patcher/eboot_inline_specs.c`

- [ ] **Step 1: Add payload extern declarations**

In `eboot_patcher/eboot_inline_specs.c`, add these declarations immediately after the existing Kimidori row-hook declarations:

```c
extern const uint8_t taiko_kimidori_dani_proc_main_hook_start[];
extern const uint8_t taiko_kimidori_dani_proc_main_hook_end[];
```

- [ ] **Step 2: Add strict Proc_Main signature arrays**

In `eboot_patcher/eboot_inline_specs.c`, add this block immediately after `KIMIDORI_DANI_ROW_SIGNATURES`:

```c
static const uint32_t KIMIDORI_DANI_PROC_MAIN_WORDS[] = {
    0x2F80001Au, /* cmpwi cr7,r0,0x1A */
    0u,
    0xE8010090u, /* ld r0,0x90(r1) */
};

static const uint32_t KIMIDORI_DANI_PROC_MAIN_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint8_t KIMIDORI_DANI_PROC_MAIN_MATCH_TYPES[] = {
    EBOOT_INLINE_MATCH_WORD,
    EBOOT_INLINE_MATCH_BRANCH_TARGET,
    EBOOT_INLINE_MATCH_WORD,
};

static const uint32_t KIMIDORI_DANI_PROC_MAIN_BRANCH_TARGETS[] = {
    0u,
    0x00566E8u,
    0u,
};

static const uint32_t KIMIDORI_DANI_PROC_MAIN_CONTEXT_WORDS[] = {
    0x38800007u, /* li r4,7 */
    0x80030028u, /* lwz r0,0x28(r3) */
    0x7D435378u, /* mr r3,r10 */
    0x55292036u, /* slwi r9,r9,4 */
    0x7D290214u, /* add r9,r9,r0 */
    0x79290020u, /* clrldi r9,r9,32 */
    0x80090000u, /* lwz r0,0(r9) */
};

static const uint32_t KIMIDORI_DANI_PROC_MAIN_CONTEXT_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const uint32_t KIMIDORI_DANI_PROC_MAIN_TARGET_WORDS[] = {
    0x38000002u, /* li r0,2 */
    0x901F0014u, /* stw r0,0x14(r31) */
    0xE8010090u, /* ld r0,0x90(r1) */
};

static const uint32_t KIMIDORI_DANI_PROC_MAIN_TARGET_MASKS[] = {
    0xFFFFFFFFu,
    0xFFFFFFFFu,
    0xFFFFFFFFu,
};

static const eboot_inline_signature_t KIMIDORI_DANI_PROC_MAIN_SIGNATURES[] = {
    {
        "kimidori Proc_Main Dani branch",
        0x005666Cu,
        KIMIDORI_DANI_PROC_MAIN_WORDS,
        KIMIDORI_DANI_PROC_MAIN_MASKS,
        sizeof(KIMIDORI_DANI_PROC_MAIN_WORDS) /
            sizeof(KIMIDORI_DANI_PROC_MAIN_WORDS[0]),
        KIMIDORI_DANI_PROC_MAIN_MATCH_TYPES,
        KIMIDORI_DANI_PROC_MAIN_BRANCH_TARGETS,
    },
    {
        "kimidori Proc_Main selected marker load context",
        0x0056650u,
        KIMIDORI_DANI_PROC_MAIN_CONTEXT_WORDS,
        KIMIDORI_DANI_PROC_MAIN_CONTEXT_MASKS,
        sizeof(KIMIDORI_DANI_PROC_MAIN_CONTEXT_WORDS) /
            sizeof(KIMIDORI_DANI_PROC_MAIN_CONTEXT_WORDS[0]),
        NULL,
        NULL,
    },
    {
        "kimidori Proc_Main Dani target context",
        0x00566E8u,
        KIMIDORI_DANI_PROC_MAIN_TARGET_WORDS,
        KIMIDORI_DANI_PROC_MAIN_TARGET_MASKS,
        sizeof(KIMIDORI_DANI_PROC_MAIN_TARGET_WORDS) /
            sizeof(KIMIDORI_DANI_PROC_MAIN_TARGET_WORDS[0]),
        NULL,
        NULL,
    },
};
```

- [ ] **Step 3: Register the hook spec**

In the `INLINE_HOOK_SPECS` array, add this spec immediately after the existing `"kimidori-st51-v05r00-dani-row"` spec:

```c
    {
        "dani_dojo_unlock",
        "kimidori-st51-v05r00-dani-proc-main",
        0x005666Cu,
        KIMIDORI_DANI_PROC_MAIN_SIGNATURES,
        sizeof(KIMIDORI_DANI_PROC_MAIN_SIGNATURES) /
            sizeof(KIMIDORI_DANI_PROC_MAIN_SIGNATURES[0]),
        taiko_kimidori_dani_proc_main_hook_start,
        taiko_kimidori_dani_proc_main_hook_end,
        4u,
        EBOOT_INLINE_RETURN_EXPLICIT,
        0x0056674u,
        NULL,
        NULL,
        { 0u, 0u, 0u, 0u },
    },
```

- [ ] **Step 4: Verify spec references and constants**

Run:

```powershell
rg -n "taiko_kimidori_dani_proc_main_hook|KIMIDORI_DANI_PROC_MAIN|0x005666C|0x00566E8|0x0056674|kimidori-st51-v05r00-dani-proc-main" eboot_patcher\eboot_inline_specs.c
```

Expected output includes all symbol names, all three addresses, and the binary id.

- [ ] **Step 5: Verify the new spec does not mention entry resources or root slots**

Run:

```powershell
rg -n "C00380|dword_C00380|0x000A00EC|0xA4|0xA8|entry/packeddata|resource parent|root slot" eboot_patcher\eboot_inline_specs.c patches\asm\kimidori_dani_proc_main_hook.S
```

Expected: no output.

- [ ] **Step 6: Commit the hook spec**

Run:

```powershell
git add -- eboot_patcher/eboot_inline_specs.c
git commit -m "Restore Kimidori Dani Proc_Main branch"
```

Expected commit result includes:

```text
Restore Kimidori Dani Proc_Main branch
```

---

### Task 6: Build and Static Verification

**Files:**
- Inspect: `patches/asm/kimidori_dani_proc_main_hook.S`
- Inspect: `eboot_patcher/eboot_inline_specs.c`
- Inspect: `Makefile`
- Inspect: `Makefile.win`
- Inspect build output: `bin/zucchini.sprx`

- [ ] **Step 1: Verify the payload assembles through the SDK compiler**

Run from a Visual Studio developer prompt:

```bat
nmake /f Makefile.win CELL_SDK=%SCE_PS3_ROOT% TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: `patches\asm\kimidori_dani_proc_main_hook.o` compiles and `bin\zucchini.sprx` is produced when the local SDK and token configuration are valid.

- [ ] **Step 2: If NMAKE is unavailable, run the GNU Make build**

Run:

```powershell
make CELL_SDK="$env:SCE_PS3_ROOT" TAIKO_ZUCCHINI_API_TOKEN=token
```

Expected: `patches/asm/kimidori_dani_proc_main_hook.o` compiles and `bin/zucchini.sprx` is produced when the local SDK and token configuration are valid.

- [ ] **Step 3: Verify the object exports the payload symbols**

Run:

```powershell
$cellSdk = if ($env:CELL_SDK) { $env:CELL_SDK } else { $env:SCE_PS3_ROOT }
& "$cellSdk\host-win32\ppu\bin\ppu-lv2-objdump.exe" -t 'patches\asm\kimidori_dani_proc_main_hook.o' | Select-String 'taiko_kimidori_dani_proc_main_hook'
```

Expected output includes:

```text
taiko_kimidori_dani_proc_main_hook_start
taiko_kimidori_dani_proc_main_hook_end
```

- [ ] **Step 4: Verify branch target signature matching is used**

Run:

```powershell
rg -n "EBOOT_INLINE_MATCH_BRANCH_TARGET|KIMIDORI_DANI_PROC_MAIN_BRANCH_TARGETS|0x00566E8u" eboot_patcher\eboot_inline_specs.c
```

Expected output shows `KIMIDORI_DANI_PROC_MAIN_MATCH_TYPES` uses `EBOOT_INLINE_MATCH_BRANCH_TARGET` and `KIMIDORI_DANI_PROC_MAIN_BRANCH_TARGETS` includes `0x00566E8u`.

- [ ] **Step 5: Verify only Kimidori owns the new payload**

Run:

```powershell
rg -n "kimidori_dani_proc_main|dani-proc-main|0x005666C" patches eboot_patcher Makefile Makefile.win
```

Expected output references only:

```text
patches\asm\kimidori_dani_proc_main_hook.S
eboot_patcher\eboot_inline_specs.c
Makefile
Makefile.win
```

- [ ] **Step 6: Verify no forbidden resource-graft fix was added**

Run:

```powershell
rg -n "C00380|dword_C00380|0x000A00EC|0xA4|0xA8|entry/packeddata|resource parent|root slot" patches\asm\kimidori_dani_proc_main_hook.S eboot_patcher\eboot_inline_specs.c
```

Expected: no output.

- [ ] **Step 7: Commit build or verification fixes only if files changed**

Run:

```powershell
git status --short -- patches/asm/kimidori_dani_proc_main_hook.S eboot_patcher/eboot_inline_specs.c Makefile Makefile.win
```

If verification forced a code or build-file fix, commit only those files:

```powershell
git add -- patches/asm/kimidori_dani_proc_main_hook.S eboot_patcher/eboot_inline_specs.c Makefile Makefile.win
git commit -m "Fix Kimidori Dani Proc_Main hook verification"
```

If the status output is empty, do not create a commit for this task.

---

### Task 7: Runtime Handoff and Stop Conditions

**Files:**
- Inspect runtime logs from RPCS3, PS3, or S357 hardware.
- Create if runtime results are recorded in-repo: `docs/superpowers/evidence/2026-07-04-kimidori-dani-runtime-notes.md`

- [ ] **Step 1: Record the exact runtime build being tested**

Use the commit hash after Task 6 and the build artifact timestamp:

```powershell
git rev-parse --short HEAD
Get-Item 'bin\zucchini.sprx' | Select-Object FullName,Length,LastWriteTime
```

Expected: one short commit hash and one `bin\zucchini.sprx` file record.

- [ ] **Step 2: User-run runtime acceptance**

On RPCS3, PS3, or S357 hardware, test Kimidori with `dani_dojo_unlock` enabled.

Acceptance observations:

```text
Entering restored Kimidori Dani Dojo no longer crashes at 0x003B2524.
No crash appears at the previous root-slot corruption site.
Normal Kimidori song-select and normal play still work.
Dani remains first-song-only according to the existing pre-RED gate behavior.
```

- [ ] **Step 3: Stop if the same null resource lookup remains**

If runtime still crashes with:

```text
crash site: 0x003B2524
lookup key: 0x000A00EC
out == 0
```

do not add a renderer guard and do not revive the root-slot overwrite. Stop and write a new plan for the spec's fallback path: restoring the native entry-resource preload branch/helper.

- [ ] **Step 4: Commit runtime notes only if notes are recorded in-repo**

If runtime notes are recorded in `docs/superpowers/evidence/2026-07-04-kimidori-dani-runtime-notes.md`, commit them:

```powershell
git add -- docs/superpowers/evidence/2026-07-04-kimidori-dani-runtime-notes.md
git commit -m "Record Kimidori Dani runtime validation notes"
```

If runtime notes are kept outside the repository, do not create a commit for this task.

---

## Self-Review

Spec coverage:

- Restore native logic rather than inventing resource ownership: Tasks 2, 3, and 5 restore the existing `Proc_Main` Dani branch target.
- Inline hooks allowed for compiled-away branch restoration: Task 3 adds a branch payload; Task 5 registers it through the existing EBOOT inline-hook substrate.
- Keep existing visibility layer: Task 1 freezes the row hook and emit-gate baseline.
- Do not assume visible row `0x0D` without proof: Task 2 records why the current row hook emits marker `0x0D` and why `Proc_Main` must branch on it.
- Preserve native Kimidori marker `0x1A`: Task 3 payload tests `0x1A` first.
- No renderer guard, fake resource map entry, or root-slot overwrite: Tasks 3, 5, and 6 include explicit static guards.
- Kimidori-only patch boundary: Tasks 3, 4, 5, and 6 only add `kimidori_dani_proc_main` artifacts and constants.
- Static verification and build evidence: Task 6.
- Runtime acceptance remains user-run: Task 7.
- Fallback entry-preload restoration is not mixed into this branch fix: Scope Check and Task 7 stop condition.

Red flag scan:

- No step uses an unintroduced function, missing file path, or unspecified address.
- All code edits include exact snippets.
- No task asks for a synthetic resource insertion or root/shared pointer overwrite.

Type and symbol consistency:

- Assembly exports `taiko_kimidori_dani_proc_main_hook_start` and `taiko_kimidori_dani_proc_main_hook_end`.
- C declarations use the same symbol names.
- The spec id is consistently `kimidori-st51-v05r00-dani-proc-main`.
- Hook site `0x005666C`, non-Dani continuation `0x0056674`, and Dani target `0x00566E8` match the recorded IDA words.
