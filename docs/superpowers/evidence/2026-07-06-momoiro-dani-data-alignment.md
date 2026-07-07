# Momoiro Dani Data Alignment Check

Date: 2026-07-06

## Targets

- IDB: `H:\taiko\momorio\EBOOT.ELF.i64`
- Active runtime data:
  `H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Momoiro\USRDIR\data`
- Prior baseline:
  `docs/superpowers/evidence/2026-07-05-kimidori-dani-red-port-restart.md`

IDA was run through `ida-cli` daemon mode. The default daemon path attempted to
use `\\wsl$\Debian\tmp\.ida-cli\daemons`, which was unavailable from this
PowerShell session, so the recheck used:

```text
IDA_CLI_DAEMON_DIR=H:\TaikoZucchini\.codex-tmp\ida-daemons
```

Backend probe result for the Momoiro target:

```text
database_opened=True
ida_available=True
backend=idalib
```

## Binary Findings

Momoiro's route strings are present as `v04r00` and `v01r00`. Dani-select
strings are also present:

```text
0x009B3838 N4game9animation17Lumen_dani_selectE
0x009CC248 dani_select/dani_select.lm
0x009CEA70 /data/lumendata/packed/dani_select/packeddata.ddp
0x009B3538 AssignDani
0x009B3558 SetDaniStatus
0x009B34E8 AssignMusic
```

The existing Momoiro Dani unlock anchors match the current patch table.

Count gate at `0x00528464`:

```text
0x00528464 lwz   r9, 0xC(r11)
0x00528468 cmpwi cr7, r9, 0
0x0052846C beq   cr7, loc_528488
0x00528470 xori  r9, r9, 9
```

Emit gate at `0x00528534`:

```text
0x00528534 cmpwi  cr7, r0, 0
0x00528538 beq    cr7, loc_5285D0
0x0052853C cmpwi  cr7, r0, 9
0x00528540 beq    cr7, loc_5285D0
0x00528544 cmplwi cr7, r0, 9
```

Dormant type-9 row case at `0x005293F8`:

```text
0x005293F8 li r8, 0xC
0x005293FC b  loc_528550
```

This is the key difference from Kimidori: Momoiro's dormant Dani row marker is
`0x0C`, not Kimidori's `0x0D`. The current source already reflects that in
`MOMOIRO_ROW_WORDS` and the `momoiro-v04r00-dani-emit-gate` inline spec. The
pre-RED emit hook for Momoiro resumes at `0x00528544` and skips to
`0x005285D0`, with state read through `r25 + 0x18`.

No Kimidori-style row-marker alias was proven necessary from this IDB check.

## Active Data Checks

The active Momoiro data is internally aligned with the Momoiro/even-ID shape.

`musicinfo.xml`:

```text
header size: 380
Data blocks: 380
medley rows: 15
medley unique IDs: 20000, 20002, 20004, ..., 20028
```

`musicmedleyinfo.xml`:

```text
version: 538054930
header size: 15
MusicMedleyInfoData rows: 15
Content rows: 45
hidden tags: 45
notes tags: 45
top-level medley IDs: 20000, 20002, 20004, ..., 20028
```

The `musicmedleyinfo.xml` version is an old-format archive, and every content
row has the old-format `<hidden>` field. That matches the Kimidori abort
finding from the previous evidence: old-format medley content must keep
`hidden`.

Dry-run validation:

```text
python tools\normalize_musicmedley_ids.py --data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Momoiro\USRDIR\data" --dry-run

target medley IDs: 20000..20028 (15 rows)
musicinfo changes: 0
musicmedleyinfo changes: 0
no write needed
```

```text
python tools\fix_musicmedley_uniqueids.py --data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Momoiro\USRDIR\data" --dry-run

changed entries: 0
no write needed
```

Direct cross-check:

```text
musicinfo header size 380, Data blocks 380
musicmedley header size 15, rows 15, Content blocks 45
content uniqueid mismatches by musicid: 0
```

## Conclusion

No active Momoiro data rewrite was applied. The current files already satisfy
the two Kimidori-derived data invariants:

- top-level Dani medley IDs match the active `musicinfo.xml` medley rows;
- each `musicmedleyinfo.xml` content `uniqueid` matches `musicinfo.xml` by
  `musicid`.

Momoiro should stay in its native 15-row, even-ID medley format. Do not apply
Kimidori/ST5100 sequential-medley repair logic to this active Momoiro data
unless a later runtime log proves a different concrete mismatch.

## Hook-Parity Recheck

After the data pass, the user reported that Momoiro soft-locks while loading
Dani Dojo, matching Kimidori's pre-full-patch symptom class. That changes the
working diagnosis: the active Momoiro data is aligned, but the Momoiro EBOOT
patch set did not yet include the later Kimidori runtime hooks.

Current source state before this fix:

```text
Momoiro present:
- momoiro-v04r00-dani-emit-gate

Kimidori full patch present:
- kimidori-st51-v05r00-dani-row
- kimidori-st51-v05r00-dani-proc-main
- kimidori-st51-v05r00-dani-type10-ready
- kimidori-st51-v05r00-dani-resource-retain
- patch_kimidori_dani_state4_service_table
```

Momoiro does not need the Kimidori row-marker rewrite as-is. The Momoiro
dormant row case is already native marker `0x0C`:

```text
0x005293F8 li r8, 0xC
0x005293FC b  loc_528550
```

The proven Momoiro resource-retain analogue is:

```text
0x003BCEC8 addi   r3, r31, 0x80
0x003BCECC clrldi r4, r30, 32
0x003BCED0 clrldi r3, r3, 32
0x003BCED4 clrldi r5, r28, 32
0x003BCED8 bl     sub_3955CC
0x003BCEDC nop
0x003BCEE0 lwz    r9, 0x1C(r31)
0x003BCEE4 mr     r29, r3
```

This matches the Kimidori resource-retain hook context, with Momoiro's native
callee changed from Kimidori `0x003BF608` to Momoiro `0x003955CC`.

Implemented patch:

- Added `patches/asm/momoiro_dani_resource_retain_hook.S`.
- Added `momoiro-v04r00-dani-resource-retain` to
  `eboot_patcher/eboot_inline_specs.c`.
- Wired the new assembly object into `Makefile` and `Makefile.win`.
- The hook preserves the native `sub_3955CC` call, then increments the retained
  resource reference for the same Dani title-resource family Kimidori needed:
  `0x000A0000 <= key < 0x000A01A0`.

Static validation:

```text
IDA target: H:\taiko\momorio\EBOOT.ELF.i64
0x003BCED8 word: 0x4BFD86F5
0x003BCED8 branch target: 0x003955CC
```

Targeted compiler validation, because this PowerShell shell has the PS3
compiler but not `nmake` or `make` on PATH:

```text
H:\PS3_SDK\host-win32\ppu\bin\ppu-lv2-gcc.exe ... \
  -c patches\asm\momoiro_dani_resource_retain_hook.S \
  -o %TEMP%\momoiro_dani_resource_retain_hook.o

H:\PS3_SDK\host-win32\ppu\bin\ppu-lv2-gcc.exe ... \
  -c eboot_patcher\eboot_inline_specs.c \
  -o %TEMP%\eboot_inline_specs.o
```

Both targeted compiles exited `0`. The assembly object exports:

```text
taiko_momoiro_dani_resource_retain_hook_start
taiko_momoiro_dani_resource_retain_hook_end
```

## Remaining Hook Gap

The Momoiro `RequestFillrect` path is structurally similar to Kimidori but not
byte-identical:

```text
0x0002E3DC li   r4, 0xA
0x0002E3F4 bl   sub_7CD20
0x0002E400 clrldi r31, r3, 32
0x0002E410 bl   sub_46E7F0
0x0002E414 nop
0x0002E464 mr   r5, r31
0x0002E474 bl   sub_309FC
```

Important Momoiro differences from Kimidori:

- Momoiro type resolver `sub_7CD20` uses `root + 0x220`, not Kimidori
  `root + 0x200`.
- The requested UID is not still in a stable register at the candidate hook
  site `0x0002E414`; it must be reconstructed from the RequestFillrect parse
  stack slots.
- The Momoiro native equivalent of Kimidori's `sub_93258(type10_entry)` has not
  been proven yet. A wrong builder call was a known Kimidori dead end, so no
  speculative Momoiro type10-ready hook was installed in this pass.

The next hook to port, if the resource-retain fix still soft-locks, is the
guarded Momoiro RequestFillrect/type10-ready hook at `0x0002E414`, after proving
the native type-entry builder that flips type-10 flags and populates the range
vector.

## Runtime Result: Resource-Retain Alone Was Insufficient

User runtime result after the resource-retain build:

```text
Your patch results in nothing in log and it still hangs without any change
```

Fresh log inspection with shared-read access showed the patched SPRX and EBOOT
path were active. The earlier run did install both Momoiro hooks:

```text
[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-emit-gate
[patch] inline hook site=0x00528540
[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-resource-retain
[patch] inline hook site=0x003bced8
```

The runtime log also reached the Dani/Song Select load region:

```text
VO_DANISELECT.nub
data/lumendata/packed/song_select/packeddata.ddp
```

Conclusion: resource retention was applied, but it was not the missing Momoiro
piece. The failure remained aligned with Kimidori's missing type-10 readiness
class.

## Momoiro Type-10 Ready Mapping

IDA daemon mode was used against:

```text
H:\taiko\momorio\EBOOT.ELF.i64
```

Momoiro `RequestFillrect` is `sub_2E2C8`. Its type-10 lookup sequence is:

```text
0x0002E3D4 lwz    r29, off_AC117C
0x0002E3D8 srawi  r5, r5, 1
0x0002E3DC li     r4, 0xA
0x0002E3E4 addi   r3, r1, 0x70
0x0002E3E8 lwz    r9, dword_B8F158(r29)
0x0002E3EC lwz    r0, 0(r9)
0x0002E3F0 stw    r0, 0x70(r1)
0x0002E3F4 bl     sub_7CD20
0x0002E400 clrldi r31, r3, 32
0x0002E410 bl     sub_46E7F0
0x0002E414 nop
0x0002E464 mr     r5, r31
0x0002E474 bl     sub_309FC
```

`sub_7CD20` uses Momoiro's type tree at `root + 0x220`:

```text
0x0007CD20 lwz  r11, 0(r3)
0x0007CD24 lwz  r10, 0x220(r11)
...
0x0007CD90 addi r3, r3, 0x10
0x0007CD9C b    sub_7CAA0
```

`sub_7CAA0` is the final range lookup helper. Its entry layout differs from
Kimidori: base id at entry `+0`, range begin at entry `+0x14`, and range end
at entry `+0x18`.

The native Momoiro root builder is `sub_22FC4(root)`. Its first operation is
to use the same tree:

```text
0x00023008 addi r20, r3, 0x21C
0x00023010 lwz  r6, 4(r20)   ; *(root + 0x220)
```

`sub_22FC4` later calls `sub_7D980` with a type-entry-derived pointer while
iterating queued texture/resource packs. This is safer than calling
`sub_7D980` directly from the hook, because `sub_7D980` needs extra package
state supplied by the root builder loop.

## Implemented Momoiro Type-10 Ready Hook

Added:

```text
patches/asm/momoiro_dani_type10_ready_hook.S
```

Inline spec:

```text
momoiro-v04r00-dani-type10-ready
hook site: 0x0002E414
return:    0x0002E418
```

The hook is guarded. It only runs the builder when all of these are true:

- requested UID after Momoiro's native `uid >> 1` transform is nonzero;
- current resolved fillrect id is exactly `0x000A0000`;
- the resource root exists;
- type 10 exists in `root + 0x220`;
- type 10's range begin equals range end, so the range vector is still empty.

When guarded in, it calls:

```text
sub_22FC4(root)
sub_7CD20(&root, 10, requested_uid)
```

If the second lookup returns a nonzero, non-dummy id, the hook replaces the
saved `r31` value so Momoiro's original `sub_309FC(..., r31)` draw path
consumes the corrected type-10 resource id.

Self-validating signature words:

```text
0x0002E400 0x787F0020  clrldi r31,r3,32
0x0002E404 0x7FC507B4  extsw r5,r30
0x0002E408 0x38610084  addi r3,r1,0x84
0x0002E40C 0x7FE6FB78  mr r6,r31
0x0002E410 branch-link target 0x0046E7F0
0x0002E414 0x60000000  nop
0x0002E418 0x8001009C  lwz r0,0x9C(r1)
```

Build helper:

```text
.codex-tmp\build-win.cmd
```

The helper uses:

```text
C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat
```

It defaults `CELL_SDK`/`SCE_PS3_ROOT` to `H:\PS3_SDK`, runs
`nmake /f Makefile.win`, and runs `nmake /f Makefile.win install` after a
successful normal build.

Build/install command used:

```text
cmd /c .codex-tmp\build-win.cmd
```

Result:

```text
build: passed
install: copied bin\zucchini.sprx to H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx
```

Installed artifact:

```text
local:     H:\TaikoZucchini\bin\zucchini.sprx
installed: H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx
size:      1003009
SHA-256:   6E9A2509006951CC6FF7B897C41C29DF9AACEA818CCA567ED4144018107FC7DB
SHA-1:     669232B33A1F2F2C1471D09334F866976A08F0F4
```

The current Momoiro `USRDIR\zucchini_hash` still contains the prior EBOOT patch
hash, so the next Momoiro launch should repatch the EBOOT and log:

```text
[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-type10-ready
```

## Runtime Correction: Loading-Screen Stall, Not Full Freeze

User retest after the type-10-ready build:

```text
No still the same soft lock. More specifically it is not really complete
frozen. Instead, it stuck on Dani Dojo loading screen but cannot enter the
next scene. Maybe we need to start from Dani Dojo lumen.
```

Fresh log inspection of the retest run showed the game was still alive until
shutdown, but no Dani Select Lumen package was requested:

```text
0:01:50.260389  open data/sound/se/VO_DANISELECT.nub
0:01:50.262732  open data/sound/bgm/nub/JINGLE_GENRE.nub
0:01:50.274979  open data/lumendata/packed/song_select/packeddata.ddp
0:01:50.616105  close data/lumendata/packed/song_select/packeddata.ddp
```

There were no log hits for:

```text
data/lumendata/packed/dani_select/packeddata.ddp
JINGLE_DANI.nub
AssignDani
AssignMusic
SetDaniStatus
NotifyDaniSelect
```

That means the current stall is before Dani Select Lumen execution. Dani Select
Lumen exists and has the expected wait/callback shape, but the runtime never
loads it in the failing trace.

## Dani Select Lumen Boundary

Momoiro Dani Select Lumen was dumped from:

```text
H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Momoiro\USRDIR\data\lumendata\packed\dani_select\packeddata.ddp
```

The `dani_select` script defines the downstream native callback surface:

```text
AssignDani
SetDaniStatus
AssignMusic
Callback_AssignDani
Callback_SetDaniStatus
Callback_AssignMusic
Callback_AssignCondition
NotifyDaniSelect
NotifyConfirm
WAIT_RECIEVE_DATA
WAIT_READY
Wait_DaniTitle
```

In `Main.Proc`, Dani Select waits on `Resource.VerifyRecieveData()` before it
can move from `WAIT_RECIEVE_DATA` to `WAIT_READY`. That is a real later risk:
if `dani_select/packeddata.ddp` starts loading but hangs inside the scene, the
next boundary is the native `AssignDani` / `SetDaniStatus` / `AssignMusic`
population path.

The current trace does not reach that boundary. The native transition remains
on the Song Select / genre path because it requests `JINGLE_GENRE` and reloads
`song_select`, not `JINGLE_DANI` and `dani_select`.

## Momoiro Native Transition Recheck

IDA daemon mode recheck of `H:\taiko\momorio\EBOOT.ELF.i64` confirmed the
native top-level row dispatcher still uses Momoiro's Lumen constants:

```text
0x000B65B4 lwz   r31, 0(r9)
0x000B65B8 cmpwi cr7, r31, 0xD
0x000B65C0 cmpwi cr7, r31, 0xC
0x000B65C4 bge   cr7, loc_B7874
...
0x000B7894 li    r9, 4
0x000B7898 li    r0, 0xA
0x000B789C stw   r9, 0x14(r27)
0x000B78A0 stw   r0, 0x10(r27)
```

So `0x0C` is still the correct row type for normal Dani Dojo. The failing log
is therefore not consistent with state 6 consuming row type `0x0C`.

## Momoiro Row-Emission Diagnostic Patch

The shared pre-RED emit hook was replaced for Momoiro only with a
Momoiro-specific payload:

```text
patches/asm/momoiro_dani_emit_select_hook.S
```

Installed inline specs:

```text
momoiro-v04r00-dani-emit-gate
site:   0x00528540
return: 0x00528544

momoiro-v04r00-dani-select-row-diag
site:   0x000B65B4
return: 0x000B65B8
```

The emit hook now lets Momoiro type-9 rows reach the native jump-table case:

```text
0x005293F8 li r8, 0xC
0x005293FC b  loc_528550
```

It also emits a TTY diagnostic only when a type-9 row reaches the gate:

```text
[tz] me type9
```

The state-6 selector hook preserves the original `lwz r31,0(r9)` and logs the
row type consumed by the native transition dispatcher:

```text
[tz] ms row=0x0000000C
```

Expected interpretation on the next retest:

- `[tz] me type9` absent: the row builder is not seeing the dormant Dani row.
- `[tz] me type9` present but `[tz] ms row=0x0000000C` absent: the row is built
  but not selected through state 6.
- `[tz] ms row=0x0000000C` present and the log still loads `song_select`: the
  native dispatcher branch target or mode/state update needs rechecking.
- `dani_select/packeddata.ddp` loads and then stalls: move downstream to
  Dani Select Lumen's `WAIT_RECIEVE_DATA` / callback population path.

## Build And Install

Build helper used:

```text
cmd /c .codex-tmp\build-win.cmd
```

The helper uses:

```text
C:\Program Files\Microsoft Visual Studio\18\Insiders\VC\Auxiliary\Build\vcvars64.bat
```

Result:

```text
build: passed
install: copied bin\zucchini.sprx to H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx
```

Installed artifact:

```text
local:     H:\TaikoZucchini\bin\zucchini.sprx
installed: H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx
size:      1003769
SHA-1:     3F17EB3DA53C97FD50C3AF6C079704D75FB5AE8F
SHA-256:   24A942F4462DC8C6C6853220C3A015E033FA4EE94E638DCFE6ECA939195FDBD9
```

The current Momoiro hash marker still contains the previous SPRX hash:

```text
669232b33a1f2f2c1471d09334f866976a08f0f4
5887e2acf496a8a062c3a8700e8b084305ad988a
```

Because the installed SPRX hash is now
`3f17eb3da53c97fd50c3af6c079704d75fb5ae8f`, the next Momoiro launch should
force the EBOOT patcher to rebuild from `EBOOT_ORIGINAL.BIN` and install the
new Momoiro inline specs, including:

```text
[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-emit-gate
[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-select-row-diag
```

## Runtime Log Check After Retest

Checked:

```text
H:\RPCS3\rpcs3-blue\log\RPCS3.log
LastWriteTime: 2026-07-07 04:21:49
Length: 3989093
```

The retest did pick up the rebuilt Momoiro EBOOT and installed all current
Momoiro Dani inline hooks:

```text
13718 [patch] Dan-i Dojo binary: Momoiro v04r00
13719 [patch] Dan-i Dojo count gate=0x00528470
13720 [patch] Dan-i Dojo emit hook gate=0x00528540
13721 [patch] Dan-i Dojo dormant case=0x005293f8
13722 [patch] Dan-i Dojo emit gate left for inline hook
13729 [patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-emit-gate
13736 [patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-select-row-diag
13743 [patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-resource-retain
13750 [patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-type10-ready
13855 [eboot] wrote zucchini_hash
```

The runtime diagnostics fired at the Dani loading stall:

```text
36393 [tz] me type9
36461 [tz] ms row=0x0000000c
37030 [tz] me type9
37031 [tz] ms row=0x0000000c
```

Nearby file activity shows `VO_DANISELECT.nub`, then the normal song-select
packed data, not Dani Select:

```text
36210 open data/sound/se/VO_DANISELECT.nub
36419 open data/lumendata/packed/song_select/packeddata.ddp
```

No matches were present for:

```text
dani_select/packeddata.ddp
JINGLE_DANI
AssignDani
AssignMusic
SetDaniStatus
NotifyDaniSelect
```

Interpretation: the previous "row not emitted" and "row not selected" theories
are now false. Momoiro emits the dormant Dani type-9 row and the native state-6
selector consumes row type `0x0C`. The remaining break is after the row
dispatch, before DojoSelect construction opens `dani_select/packeddata.ddp`.
The next narrow diagnostic target is the state-10 helper gate around
`0x000B6978`/`0x000B69C0`, which can bounce back to state 8 before state 12
builds DojoSelect.

## State-Transition Diagnostic Hooks

IDA daemon recheck of `H:\taiko\momorio\EBOOT.ELF.i64` confirmed the next
transition boundaries:

```text
0x000B7894 li  r9,4
0x000B7898 li  r0,0xA
0x000B789C stw r9,0x14(r27)
0x000B78A0 stw r0,0x10(r27)

0x000B69A8 clrldi r3,r3,32
0x000B69AC addi   r4,r1,0x94
0x000B69B0 bl     sub_72788
0x000B69B8 clrlwi r3,r3,24
0x000B69BC cmpwi  cr7,r3,0
0x000B69C0 beq    cr7,loc_B70DC
0x000B69EC li     r0,0xB
0x000B69F0 stw    r0,0x10(r27)

0x000B6E74 bl sub_527880  ; mode 4 DojoSelect constructor path
0x000B6C94 bl sub_527880  ; mode 5 adjacent path
```

Added:

```text
patches/asm/momoiro_dani_state_diag_hooks.S
```

New Momoiro-only inline specs:

```text
momoiro-v04r00-dani-state10-set-diag
site:   0x000B7894
return: 0x000B7898
log:    [tz] m6->10

momoiro-v04r00-dani-state10-result-diag
site:   0x000B69C0
success return: 0x000B69C4
failure return: 0x000B70DC
log:    [tz] m10 ok
log:    [tz] m10 fail

momoiro-v04r00-dani-dojo-ctor4-diag
site:   0x000B6E74
return: 0x000B6E78
log:    [tz] ctor4

momoiro-v04r00-dani-dojo-ctor5-diag
site:   0x000B6C94
return: 0x000B6C98
log:    [tz] ctor5
```

Expected interpretation for the next retest:

- `[tz] ms row=0x0000000c` but no `[tz] m6->10`: state 6 read the row but did
  not enter the native Dani branch target at `0x000B7874`.
- `[tz] m6->10` then `[tz] m10 fail`: the `sub_72788` readiness gate returned
  false and the game bounced back through `loc_B70DC` to state 8.
- `[tz] m10 ok` but no `[tz] ctor4`: state 10 passed, but the state 11/12
  transition did not reach the normal DojoSelect construction path.
- `[tz] ctor4` but no `dani_select/packeddata.ddp`: DojoSelect construction was
  reached, so the next boundary is inside `sub_527880` or its asset-open path.
- `[tz] ctor5` during normal row `0x0C` selection would indicate an unexpected
  mode mismatch.

Build helper used:

```text
cmd /c .codex-tmp\build-win.cmd
```

Result:

```text
build: passed
install: copied bin\zucchini.sprx to H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx
```

Installed artifact:

```text
local:     H:\TaikoZucchini\bin\zucchini.sprx
installed: H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx
size:      1006441
SHA-1:     4A8A6767FD3B1C5EC39079F80D6B92B66E99515E
SHA-256:   DF5A407C7199CDF60679FE0EDC56161A095A0EF630EEF7CE34C7197203558F9D
```

The current Momoiro hash marker still contains the previous SPRX hash:

```text
3f17eb3da53c97fd50c3af6c079704d75fb5ae8f
884cd8ae42fa179720dd84660d7cb8c809f53e66
```

Because the installed SPRX hash is now
`4a8a6767fd3b1c5ec39079f80d6b92b66e99515e`, the next Momoiro launch should
force the EBOOT patcher to rebuild from `EBOOT_ORIGINAL.BIN` and install the
new state-transition diagnostic specs.

## Runtime Log Check After State Diagnostics

Checked:

```text
H:\RPCS3\rpcs3-blue\log\RPCS3.log
LastWriteTime: 2026-07-07 05:08:01
Length: 6215715
```

The retest did force the Momoiro EBOOT repatch and installed the new state
diagnostics:

```text
13674 [eboot] repatching EBOOT_ORIGINAL.BIN for current patch state
13714 [patch] Dan-i Dojo binary: Momoiro v04r00
13725 [patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-emit-gate
13732 [patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-select-row-diag
13739 [patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-state10-set-diag
13746 [patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-state10-result-diag
13753 [patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-dojo-ctor4-diag
13760 [patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-dojo-ctor5-diag
13767 [patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-resource-retain
13774 [patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-type10-ready
13878 [eboot] repatch flow rc=0x00000000
13879 [eboot] wrote zucchini_hash
```

Runtime diagnostics at the Dani loading stall:

```text
54018 [tz] me type9
54139 [tz] ms row=0x0000000c
54141 [tz] m6->10
54143 [tz] m10 ok
```

Negative matches:

```text
[tz] m10 fail
[tz] ctor4
[tz] ctor5
dani_select/packeddata.ddp
JINGLE_DANI
AssignDani
AssignMusic
SetDaniStatus
NotifyDaniSelect
```

Nearby file activity still shows the wrong Lumen package:

```text
53833 open data/sound/se/VO_DANISELECT.nub
53840 open data/sound/bgm/nub/JINGLE_GENRE.nub
54042 open data/lumendata/packed/song_select/packeddata.ddp
54060 close data/lumendata/packed/song_select/packeddata.ddp
```

Interpretation: the state-10 readiness-gate theory is now false. Momoiro
emits the Dani row, state 6 dispatches row `0x0C`, state 6 enters the native
mode-4/state-10 branch, and state 10's `sub_72788` gate returns success.
However, neither the mode-4 nor mode-5 `sub_527880` DojoSelect constructor call
is reached. The remaining boundary is state 11/state 12 transition after
`0x000B69EC`, especially the state-11 countdown at `0x000B69F8` and the state
12 mode dispatch before `0x000B6E74`.

The current Momoiro hash marker now contains the state-diagnostic SPRX hash:

```text
4a8a6767fd3b1c5ec39079f80d6b92b66e99515e
2ca82b0a2982a19d5eb0257963b63eab9f7ca42b
```

## Post-State10 Control-Flow Correction

IDA daemon recheck of `H:\taiko\momorio\EBOOT.ELF.i64` corrected the
post-`m10 ok` interpretation. For mode 4/5, state 10 does not immediately use
the generic state-11 fallthrough path. It first branches to `loc_B7AA4`:

```text
0x000B69C4 lwz    r9,0x14(r27)
0x000B69C8 li     r11,0x3C
0x000B69CC addi   r0,r9,-4
0x000B69D0 cmplwi cr7,r0,1
0x000B69D4 ble    cr7,loc_B69DC
0x000B69D8 li     r11,0x78
0x000B69DC addi   r0,r9,-4
0x000B69E0 stw    r11,0x210(r27)
0x000B69E4 cmplwi cr7,r0,1
0x000B69E8 ble    cr7,loc_B7AA4
```

The mode-4/5 path then does a transition setup and returns to the generic
state-11 setup:

```text
0x000B7AA4 addi   r3,r25,0xE4
0x000B7AA8 li     r4,6
0x000B7AAC clrldi r3,r3,32
0x000B7AB0 li     r5,7
0x000B7AB4 bl     sub_118BE0
0x000B7ABC b      loc_B69EC

0x000B69EC li     r0,0xB
0x000B69F0 stw    r0,0x10(r27)
```

State 11 counts down, then sets state 8:

```text
0x000B69F8 lwz    r9,0x210(r3)
0x000B69FC addi   r9,r9,-1
0x000B6A00 cmpwi  cr7,r9,0
0x000B6A04 stw    r9,0x210(r3)
0x000B6A08 bge    cr7,loc_B5FF8
0x000B6A0C li     r0,-1
0x000B6A10 li     r9,8
0x000B6A14 stw    r0,0x210(r3)
0x000B6A18 stw    r9,0x10(r3)
```

State 8 is the next wait gate before state 12:

```text
0x000B6648 addi   r3,r3,0x13C
0x000B6650 bl     sub_115B18
0x000B6658 clrlwi r3,r3,24
0x000B665C cmpwi  cr7,r3,0
0x000B6660 beq    cr7,loc_B5FF8
0x000B6670 bl     sub_119244
0x000B667C bl     sub_118AA0
0x000B6684 li     r0,0xC
0x000B6688 stw    r0,0x10(r27)
```

State 12 then dispatches by mode:

```text
0x000B61B4 addi   r3,r3,0xA4
0x000B61C0 bl     sub_1117DC
0x000B61C8 lwz    r29,0x14(r27)
0x000B61D0 cmplwi cr7,r29,5
...
0x000B6E74 bl     sub_527880 ; mode 4 DojoSelect constructor
0x000B6C94 bl     sub_527880 ; mode 5 adjacent path
```

Added the next Momoiro-only diagnostic specs:

```text
momoiro-v04r00-dani-state10-prep-diag
site:   0x000B7AA4
return: 0x000B7AA8
log:    [tz] m10 prep

momoiro-v04r00-dani-state11-expire-diag
site:   0x000B6A0C
return: 0x000B6A10
log:    [tz] m11->8

momoiro-v04r00-dani-state8-result-diag
site:   0x000B6660
success return: 0x000B6664
failure return: 0x000B5FF8
log:    [tz] m8 ok
log:    [tz] m8 wait

momoiro-v04r00-dani-state8-to12-diag
site:   0x000B6688
return: 0x000B668C
log:    [tz] m8->12

momoiro-v04r00-dani-state12-entry-diag
site:   0x000B61B4
return: 0x000B61B8
log:    [tz] m12 ent

momoiro-v04r00-dani-state12-mode-diag
site:   0x000B61C8
return: 0x000B61CC
log:    [tz] m12 m4
log:    [tz] m12 m5
log:    [tz] m12 m?
```

Expected interpretation for the next retest:

- `m10 ok` but no `m10 prep`: the mode-4 branch at `0x000B69E8` did not take
  the expected Momoiro setup path.
- `m10 prep` but no `m11->8`: state 11 is not counting down to expiry.
- Repeated `m8 wait` and no `m8->12`: state 8 is blocked on `sub_115B18`.
- `m8->12` but no `m12 ent`: state 12 was written but not dispatched.
- `m12 m4` but no `ctor4`: state 12 mode dispatch is breaking before the
  mode-4 constructor call.

Build notes:

```text
cmd /c .codex-tmp\build-win.cmd
first attempt: failed; missing register aliases r25/r27/r29 in momoiro_dani_state_diag_hooks.S
second attempt: passed
install: copied bin\zucchini.sprx to H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx
```

Installed artifact:

```text
local:     H:\TaikoZucchini\bin\zucchini.sprx
installed: H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx
size:      1010457
SHA-1:     54CC7BD15EFECA534CB3DCEE817D97D64E2A3851
SHA-256:   4A2C4A015CA2200C9B93132D7A2E962A2CEA91878BA17BF2ABEBD3671B384817
```

The current Momoiro hash marker still contains the previous diagnostic SPRX
hash:

```text
4a8a6767fd3b1c5ec39079f80d6b92b66e99515e
2ca82b0a2982a19d5eb0257963b63eab9f7ca42b
```

Because the installed SPRX hash is now
`54cc7bd15efeca534cb3dcee817d97d64e2a3851`, the next Momoiro launch should
force another EBOOT repatch and install the new state-8/state-12 diagnostics.

## Runtime Log Check After State8 Diagnostics

Checked the latest RPCS3 logs:

```text
H:\RPCS3\rpcs3-blue\log\RPCS3.log
LastWriteTime: 2026-07-07 05:31:55
Length: 4283156

H:\RPCS3\rpcs3-blue\log\TTY.log
LastWriteTime: 2026-07-07 05:31:54
Length: 19367
```

The installed diagnostics are active. The clean `TTY.log` sequence is:

```text
388:[tz] me type9
389:[tz] ms row=0x0000000c
390:[tz] m6->10
391:[tz] m10 ok
392:[tz] m10 prep
393:[tz] m11->8
394:[tz] m8 wait
...
622:[tz] m8 wait
```

Clean `TTY.log` marker counts:

```text
[tz] me type9 = 1
[tz] ms row = 1
[tz] m6->10 = 1
[tz] m10 ok = 1
[tz] m10 prep = 1
[tz] m11->8 = 1
[tz] m8 wait = 229
[tz] m8 ok = 0
[tz] m8->12 = 0
[tz] m12 ent = 0
[tz] m12 m4 = 0
[tz] m12 m5 = 0
```

The relevant RPCS3 file-access lines around the transition show the expected
generic song-select assets load successfully before the state8 loop:

```text
38021:... sys_fs_open(path="/dev_hdd0/game/SCEEXE001 Momoiro/USRDIR/data/sound/se/VO_DANISELECT.nub", ...)
38022:... sys_fs_open(): fd=4, Regular file, '/dev_hdd0/game/SCEEXE001 Momoiro/USRDIR/data/sound/se/VO_DANISELECT.nub', ...
38024:... sys_fs_close(fd=4): Regular file, '/dev_hdd0/game/SCEEXE001 Momoiro/USRDIR/data/sound/se/VO_DANISELECT.nub', ...
38029:... sys_fs_open(path="/dev_hdd0/game/SCEEXE001 Momoiro/USRDIR/data/sound/bgm/nub/JINGLE_GENRE.nub", ...)
38030:... sys_fs_open(): fd=4, Regular file, '/dev_hdd0/game/SCEEXE001 Momoiro/USRDIR/data/sound/bgm/nub/JINGLE_GENRE.nub', ...
38230:... sys_fs_open(path="/dev_hdd0/game/SCEEXE001 Momoiro/USRDIR/data/lumendata/packed/song_select/packeddata.ddp", ...)
38231:... sys_fs_open(): fd=5, Regular file, '/dev_hdd0/game/SCEEXE001 Momoiro/USRDIR/data/lumendata/packed/song_select/packeddata.ddp', ...
38250:... sys_fs_close(fd=5): Regular file, '/dev_hdd0/game/SCEEXE001 Momoiro/USRDIR/data/lumendata/packed/song_select/packeddata.ddp', ...
38525:... sys_fs_close(fd=4): Regular file, '/dev_hdd0/game/SCEEXE001 Momoiro/USRDIR/data/sound/bgm/nub/JINGLE_GENRE.nub', ...
```

Interpretation:

- Type9 selection data is being emitted (`me type9`, row `0x0c`).
- State6 reaches the type10 transition request (`m6->10`).
- The type10 wait succeeds (`m10 ok`).
- The Momoiro mode-specific prep path at `0x000B7AA4` executes (`m10 prep`).
- State11 expires and writes state8 (`m11->8`).
- State8 never passes `sub_115B18(this + 0x13C)`.
- State12 and the mode-4/mode-5 constructors are never reached in this run.

Current narrowed failure boundary: the loader/wait object rooted at
`this + 0x13C` remains not-ready after the state10 prep path. The next
IDA-backed check should inspect what state10 prep queues through
`sub_118BE0(this + 0xE4, 6, 7)` and how that relates to the state8 wait object
checked by `sub_115B18(this + 0x13C)`.

## Kimidori/RED Reference Check For Momoiro Missing Piece

Question: can Kimidori or RED be used as the reference for what Momoiro is
missing?

Answer: yes, but only at the behavior/sequence level. Kimidori is the closer
control because it is the previous accepted patch family. RED is useful as a
native Dani reference, but its object layout differs too much to use raw offsets
directly.

Live Momoiro data already contains the relevant Dani scene and sound assets:

```text
H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Momoiro\USRDIR\data\lumendata\packed\dani_select\packeddata.ddp | 3322036
H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Momoiro\USRDIR\data\sound\bgm\nsh\JINGLE_DANI.nsh | 2048
H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Momoiro\USRDIR\data\sound\bgm\nub\JINGLE_DANI.nub | 505168
```

The same broad asset families exist in active Momoiro, Kimidori, and RED:

```text
lumendata/packed/dani_result
lumendata/packed/dani_select
lumendata/packed/enso_dojo
lumendata/packed/song_select
sound/bgm/*/JINGLE_DANI.*
```

IDA pointer-table comparison:

```text
Momoiro dani_select path string:
  0x009CEA70 /data/lumendata/packed/dani_select/packeddata.ddp
  pointer table entry: 0x00B8B924

Momoiro song_select path string:
  0x009D0188 /data/lumendata/packed/song_select/packeddata.ddp
  pointer table entry: 0x00B8BA9C

Kimidori dani_select path string:
  0x00A3FCC8 /data/lumendata/packed/dani_select/packeddata.ddp
  pointer table entry: 0x00C00200

Kimidori song_select path string:
  0x00A414E8 /data/lumendata/packed/song_select/packeddata.ddp
  pointer table entry: 0x00C0038C

RED dani_select path string:
  0x00D89218 /data/lumendata/packed/dani_select/packeddata.ddp
  pointer table entry: 0x00F7B19C

RED song_select path string:
  0x00D8ADA0 /data/lumendata/packed/song_select/packeddata.ddp
  pointer table entry: 0x00F7B364
```

The state10 prep call is structurally the same across all three binaries:

```text
Momoiro:
0x000B7AA4 addi r3,r25,0xE4
0x000B7AA8 li   r4,6
0x000B7AB0 li   r5,7
0x000B7AB4 bl   sub_118BE0

Kimidori:
0x000D8124 addi r3,r25,0xE4
0x000D8128 li   r4,6
0x000D8130 li   r5,7
0x000D8134 bl   sub_136A48

RED:
0x00178EA8 li   r4,6
0x00178EAC clrldi r3,r30,32
0x00178EB0 li   r5,7
0x00178EB4 bl   sub_1D6548
```

The state8 wait-to-state12 shape is also structurally the same, but the wait
object offset is binary-specific:

```text
Momoiro:
0x000B6648 addi r3,r3,0x13C
0x000B6650 bl   sub_115B18
0x000B6660 beq  cr7,loc_B5FF8
0x000B6670 bl   sub_119244
0x000B667C bl   sub_118AA0
0x000B6688 stw  r0,0x10(r27) ; state 12

Kimidori:
0x000D6F0C addi r3,r3,0x148
0x000D6F14 bl   sub_1337B4
0x000D6F24 beq  cr7,loc_D63B0
0x000D6F34 bl   sub_13759C
0x000D6F40 bl   sub_136904
0x000D6F4C stw  r0,0x10(r26) ; state 12

RED:
0x00177C54 addi r31,r3,0xE80
0x00177C60 bl   sub_1D3DEC
0x00177C70 beq  cr7,def_177670
0x00177C7C bl   sub_1D710C
0x00177C88 bl   sub_1D6440
0x00177C94 stw  r0,0x10(r27) ; state 12
```

Conclusion:

- Momoiro is not missing the `dani_select` or `JINGLE_DANI` files.
- Momoiro is not obviously missing the state10 `6,7` queue call; that call
  matches Kimidori and RED.
- The latest runtime log shows Momoiro is missing the successful completion
  observed in the reference flow: it never passes the state8 wait, never writes
  state12, and therefore never requests `dani_select/packeddata.ddp` or
  `JINGLE_DANI`.
- The next patch should not copy RED offsets. The useful reference is the
  Kimidori/RED sequence: state8 wait object becomes ready, cleanup runs on the
  `+0xE4` object, state12 dispatches mode 4, then DaniSelect construction
  requests `dani_select` and `JINGLE_DANI`.

## Deeper State8 Diagnostic Build

User direction: do not stop at evidence recording unless the issue is solved;
continue into more diagnostics or candidate patches.

IDA check of the state8 callee showed that `sub_115B18` is a one-word readiness
check:

```text
0x00115B18 lwz   r9,0x9C(r3)
0x00115B1C li    r3,1
0x00115B20 addi  r9,r9,1
0x00115B24 cmplwi cr7,r9,1
0x00115B28 ble   cr7,loc_115B30
0x00115B2C li    r3,0
```

For the Momoiro state8 call, `r3 = state + 0x13C`, so the blocking word is:

```text
state + 0x13C + 0x9C = state + 0x1D8
```

`sub_118BE0(this + 0xE4, 6, 7)` only queues work if the queue object is active:

```text
0x00118BEC lbz   r0,4(r3)
0x00118BF4 bne   cr7,loc_118C18
0x00118BF8 lwz   r0,0x18(r3)
0x00118C0C beq   cr7,loc_118C18
0x00118C10 bl    sub_633C8
```

That makes the next diagnostic split concrete:

- if `q04` is nonzero or `q18` is zero after `sub_118BE0`, the state10 queue
  call did not actually submit the transition resource request;
- if the queue fields look active but `m8s` stays positive, the state8 wait
  object is watching a stuck/wrong readiness word.

Added diagnostics:

```text
taiko_momoiro_dani_state10_after_queue_diag_hook_start/end
site:   0x000B7AB8
return: 0x000B7ABC
replaces: nop
logs:
  [tz] q04=0xXXXXXXXX
  [tz] q18=0xXXXXXXXX
  [tz] q2c=0xXXXXXXXX
  [tz] q30=0xXXXXXXXX
  [tz] q34=0xXXXXXXXX

taiko_momoiro_dani_state8_result_diag_hook_start/end
existing site: 0x000B6660
additional wait-path log:
  [tz] m8s=0xXXXXXXXX
```

New inline spec:

```text
momoiro-v04r00-dani-state10-after-queue-diag
site:   0x000B7AB8
return: 0x000B7ABC
signature:
  0x000B7AB8 60000000  nop
  0x000B7ABC 4BFFEF30  b loc_B69EC
  0x000B7AC0 39E00000  li r15,0
  0x000B7AC4 4BFFFB44  b loc_B7608
```

Build/install:

```text
cmd /c .codex-tmp\build-win.cmd
result: passed
install: copied bin\zucchini.sprx to H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx
```

Installed artifact:

```text
SHA-1:   17403C0980F80DA893C43276B19E754CED368F14
SHA-256: 44886410B1143E16A62D75977477FC6E1158363A6065CF20F071AB593F9167D8
```

The current Momoiro `zucchini_hash` still contains the previous diagnostic SPRX
hash:

```text
54cc7bd15efeca534cb3dcee817d97d64e2a3851
ab31bf625018751081bbe5af59521a1ef852d92e
```

Because the installed SPRX is now
`17403c0980f80da893c43276b19e754ced368f14`, the next Momoiro launch should
force another EBOOT repatch and install this deeper diagnostic set.

Next runtime interpretation:

- `q04 != 0`: the queue object is already closed/inactive when state10 tries to
  queue `(6,7)`.
- `q18 == 0`: the queue object has no backing resource dispatcher.
- `q04 == 0` and `q18 != 0`, but `m8s` stays positive: the queue request exists,
  but the state8 wait object is not being completed.
- `m8s == 0xffffffff` or `m8s == 0`: state8 should pass; if it still does not,
  then the hook or branch condition must be rechecked.

## Runtime Log Check After Queue/Wait Diagnostics

Checked the latest logs:

```text
H:\RPCS3\rpcs3-blue\log\RPCS3.log
LastWriteTime: 2026-07-07 05:57:23
Length: 4261690

H:\RPCS3\rpcs3-blue\log\TTY.log
LastWriteTime: 2026-07-07 05:57:22
Length: 32835
```

The new diagnostics were installed:

```text
13968:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-emit-gate
13975:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-select-row-diag
13982:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-state10-set-diag
13989:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-state10-result-diag
13996:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-state10-prep-diag
14003:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-state10-after-queue-diag
14010:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-state11-expire-diag
14017:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-state8-result-diag
14024:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-state8-to12-diag
14031:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-state12-entry-diag
14038:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-state12-mode-diag
14045:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-dojo-ctor4-diag
14052:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-dojo-ctor5-diag
14059:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-resource-retain
14066:[patch] inline hook installed: dani_dojo_unlock / momoiro-v04r00-dani-type10-ready
```

Clean `TTY.log` counts:

```text
[tz] me type9 = 1
[tz] ms row = 1
[tz] m6->10 = 1
[tz] m10 ok = 1
[tz] m10 prep = 1
[tz] q04 = 1
[tz] q18 = 1
[tz] q2c = 1
[tz] q30 = 1
[tz] q34 = 1
[tz] m11->8 = 1
[tz] m8 wait = 486
[tz] m8s = 486
[tz] m8 ok = 0
[tz] m8->12 = 0
[tz] m12 ent = 0
```

The one-shot queue diagnostic immediately after
`sub_118BE0(this + 0xE4, 6, 7)`:

```text
395:[tz] me type9
396:[tz] ms row=0x0000000c
397:[tz] m6->10
398:[tz] m10 ok
399:[tz] m10 prep
400:[tz] q04=0x00000000
401:[tz] q18=0x332c23c0
402:[tz] q2c=0x00000000
403:[tz] q30=0x00000000
404:[tz] q34=0x33366900
405:[tz] m11->8
406:[tz] m8 wait
407:[tz] m8s=0x00000002
```

The wait status distribution:

```text
480 x [tz] m8s=0x00000002
3 x [tz] m8s=0x00000001
3 x [tz] m8s=0x00000003
```

Relevant file-load timing:

```text
36020:... VO_DANISELECT.nub opened
36023:... VO_DANISELECT.nub closed
36028:... JINGLE_GENRE.nub opened
36229:... lumendata/packed/song_select/packeddata.ddp opened
36247:... lumendata/packed/song_select/packeddata.ddp closed
36279:... [tz] q04=0x00000000
36315:... JINGLE_GENRE.nub closed
36351:... [tz] m11->8
36353:... [tz] m8 wait
36355:... [tz] m8s=0x00000002
```

Interpretation:

- The state10 queue object is active enough to submit work:
  `q04 == 0` and `q18 != 0`.
- State8 is not blocked because the queue object was closed or missing a
  dispatcher.
- The blocking state is now precisely the wait object status word:
  `state + 0x1D8 == 2` for nearly the entire hang.
- `sub_115B18` only reports ready for status `0xffffffff` or `0`, so status
  `2` loops forever in the current path.
- No state12/DaniSelect constructor markers appear, so `dani_select` is still
  not requested in this run.

## Candidate Patch: Force Mode-4 State8 Status-2 Ready

Added a constrained candidate patch to the existing Momoiro state8 diagnostic
hook. If the native readiness call returns false, but:

```text
state + 0x14 == 4
state + 0x1D8 == 2
```

then the hook logs:

```text
[tz] m8 force
[tz] m8s=0x00000002
```

and branches to the native state8 success path at `0x000B6664`. This is a
deliberate force-ready experiment, scoped to Momoiro mode 4 only, intended to
test whether the remaining scene path can reach state12 and request
`dani_select/packeddata.ddp` and `JINGLE_DANI`.

Build/install:

```text
cmd /c .codex-tmp\build-win.cmd
result: passed
install: copied bin\zucchini.sprx to H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx
```

Installed artifact:

```text
SHA-1:   8EDB0ECE173643A7C4637F0197FD8754DA52C79F
SHA-256: 431D210446C428CF9A20965F8E0C882AFDC6DD16A5A9A62063396444F060FC88
```

Current Momoiro `zucchini_hash` still contains the previous diagnostic SPRX
hash:

```text
17403c0980f80da893c43276b19e754ced368f14
d65bbe67e6513c9362bf1aba5fb3010158aa62e1
```

Because the installed SPRX is now
`8edb0ece173643a7c4637f0197fd8754da52c79f`, the next Momoiro launch should
force another EBOOT repatch and install this force-ready test.

Next runtime interpretation:

- `[tz] m8 force` followed by `[tz] m8->12` / `[tz] m12 ent`: the wait-status
  force reaches the state12 scene path.
- `[tz] m12 m4` and `[tz] ctor4`: the mode dispatch and DaniSelect constructor
  are now reached.
- `dani_select/packeddata.ddp` and `JINGLE_DANI` after `ctor4`: the state8 wait
  was the blocking transition.
- crash or new hang immediately after `[tz] m8 force`: status `2` was not safe
  to treat as complete, and the next diagnostic should trace the callback that
  is supposed to move the wait object out of status `2`.

## Force-Ready Runtime Result

The force-ready experiment did reach the next scene path. Latest observed TTY
markers after the user run:

```text
[tz] me type9
[tz] ms row=0x0000000c
[tz] m6->10
[tz] m10 ok
[tz] m10 prep
[tz] q04=0x00071280
[tz] q18=0x33844440
[tz] q2c=0x00000000
[tz] q30=0x00000000
[tz] q34=0x33071300
[tz] m11->8
[tz] m8 force
[tz] m8s=0x00000002
[tz] m8->12
[tz] m12 ent
[tz] m12 m4
[tz] ctor4
```

Relevant resource opens after the force:

```text
JINGLE_DANI.nub opened
lumendata/packed/dani_select/packeddata.ddp opened
lumendata/packed/dani_select/packeddata.ddp closed
```

Interpretation: state8's wait object status was the immediate blocker. The
force does not explain the real completion path; it only proves that once
state8 is allowed to continue, the mode-4 state12 path constructs DaniSelect
and requests the expected Dani resources.

## Normal Ready State: Momoiro, Kimidori, RED

IDA target: `H:\taiko\momorio\EBOOT.ELF.i64`.

Momoiro state8 uses `sub_115B18(this + 0x13C)`:

```text
0x00115B18 lwz   r9,0x9C(r3)
0x00115B20 addi  r9,r9,1
0x00115B24 cmplwi cr7,r9,1
```

This is only ready for wait status `0xffffffff` or `0`. The status word for
the Dani state8 wait object is `state + 0x13C + 0x9C == state + 0x1D8`.

Momoiro `sub_116360(wait, n4)` is the status setter. It writes:

```text
wait + 0x9C = n4
wait + 0xA0 = status handler table
wait + 0xA4 = 0
```

Momoiro status-2 handler at `sub_116B40` consumes completion bytes:

```text
if (*(uint8_t *)(wait + 0xAB)) {
    sub_116360(wait, (((uint8_t)*(wait + 0xAA) - 1) >> 31) + 3);
    *(uint8_t *)(wait + 0xAB) = 0;
}
```

So status 2 becomes status 3 on success (`+0xAA == 0`) or status 4 on failure.
Momoiro status-1 handler at `sub_118984` is the normal ready transition; it
calls `sub_116360(wait, 0)`.

The completion callbacks use global `dword_B92650` as the active wait pointer:

```text
sub_116B98(wait): stores wait into dword_B92650
sub_7B6D8():      sets wait + 0xAB = 1 and wait + 0xAA = 0
sub_7B7B0(result): sets wait + 0xAB = 1 and wait + 0xAA from result
```

Kimidori and RED have the same normal design:

```text
Kimidori ready check:       sub_1337B4(this + 0x148), ready only for -1/0
Kimidori status setter:     sub_134B34
Kimidori status-2 handler:  sub_1352E4, same +AB/+AA -> status 3/4 logic
Kimidori callbacks:         sub_82F54 / sub_8301C via dword_C07FE0

RED ready check:            sub_1D3DEC(this + 0xE80), ready only for -1/0
RED status setter:          sub_1D4E04
RED status-2 handler:       sub_1D559C, same +AB/+AA -> status 3/4 logic
RED callbacks:              sub_102480 / sub_102558 via dword_F84CC4
```

Conclusion: yes, there is a real ready state. For Momoiro, Kimidori, and RED,
the wait object normally reaches status `0`; treating status `2` as ready is
only a bypass.

## Diagnostic Build: Trace Real Ready Chain

Removed the temporary state8 force-ready branch and added Momoiro diagnostics
for the normal wait-object chain:

```text
[tz] ns =0x........  new wait status written by sub_116360
[tz] h2b=0x........  status-2 +0xAB completion flag
[tz] h2a=0x........  status-2 +0xAA result byte
[tz] h1r            status-1 handler just wrote status 0
[tz] cbok           success callback sub_7B6D8 entered
[tz] cbr=0x........  result callback sub_7B7B0 entered, r3 result value
```

Hook sites verified with IDA:

```text
0x001163D4: 2F9E0002 cmpwi cr7,r30,2
0x00116B58: 881F00AB lbz r0,0xAB(r31)
0x001189B4: 893D00A8 lbz r9,0xA8(r29)
0x0007B6D8: 8122AE80 lwz r9,(off_AC36A0 - 0xAC8820)(r2)
0x0007B7B0: 8122AE80 lwz r9,(off_AC36A0 - 0xAC8820)(r2)
```

Build/install:

```text
cmd /c .codex-tmp\build-win.cmd
result: passed
install: copied bin\zucchini.sprx to H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx
```

Installed artifact:

```text
SHA-1:   1CC381CE8D8E7DA83E4FAF60E108F5A6BA1600ED
SHA-256: 2F44F8F48256360FF33857F342653EB42B58D9D2F9C09153248D6F0583FEE200
```

Next runtime interpretation:

- If no `[tz] cbok` or `[tz] cbr=...` appears, the request completion callback
  is not firing for Momoiro Dani.
- If callbacks appear but `[tz] h2b=0x00000001` does not, the callback is not
  writing the active wait object expected by the status-2 handler.
- If `[tz] h2b=0x00000001` appears but no status 3/4 or status 1 follows, the
  status handler dispatch is not advancing normally.
- If `[tz] h1r` and `[tz] ns =0x00000000` appear but state8 still logs
  `m8s=0x00000002`, a different wait object is being read by the state8 ready
  check than the one completed by the network callback chain.

## Runtime Log Check: Ready State Re-Arms Before State8

Latest checked logs:

```text
H:\RPCS3\rpcs3-blue\log\TTY.log   LastWriteTime 2026-07-07 06:41:29
H:\RPCS3\rpcs3-blue\log\RPCS3.log LastWriteTime 2026-07-07 06:41:34
```

All Momoiro Dani diagnostic hooks installed, including the normal ready-chain
hooks:

```text
momoiro-v04r00-dani-network-status-set-diag
momoiro-v04r00-dani-network-status2-diag
momoiro-v04r00-dani-network-status1-diag
momoiro-v04r00-dani-network-callback-success-diag
momoiro-v04r00-dani-network-callback-result-diag
```

Marker counts:

```text
320 x [tz] h2b=0x00000000
174 x [tz] h2a=0x00000001
147 x [tz] h2a=0x00000000
142 x [tz] m8s=0x00000002
142 x [tz] m8 wait
  2 x [tz] ns =0x00000002
  2 x [tz] ns =0x00000000
  1 x [tz] ns =0x00000003
  1 x [tz] ns =0x00000001
  1 x [tz] cbr=0xd000ed1c
  1 x [tz] h2b=0x00000001
  1 x [tz] h1r
```

Chronological slice:

```text
[tz] me type9
[tz] ns =0x00000000
[tz] ns =0x00000002
...
[tz] ms row=0x0000000c
[tz] m6->10
[tz] m10 ok
[tz] m10 prep
[tz] q04=0x00000000
[tz] q18=0x3337c4c0
[tz] q2c=0x00000000
[tz] q30=0x00000000
[tz] q34=0x332c2300
...
[tz] cbr=0xd000ed1c
[tz] h2b=0x00000001
[tz] h2a=0x00000001
[tz] ns =0x00000003
[tz] ns =0x00000001
[tz] ns =0x00000000
[tz] h1r
[tz] ns =0x00000002
...
[tz] m11->8
[tz] m8 wait
[tz] m8s=0x00000002
```

Resource timing remains the pre-state12 path only:

```text
VO_DANISELECT.nub opened/closed
JINGLE_GENRE.nub opened/closed
lumendata/packed/song_select/packeddata.ddp opened/closed
```

No `JINGLE_DANI` or `lumendata/packed/dani_select/packeddata.ddp` appears in
the no-bypass run.

Interpretation:

- The request completion path is not missing entirely: result callback
  `sub_7B7B0` fires once.
- The status-2 handler sees completion once: `h2b=1`.
- Status advances through `3 -> 1 -> 0`, and the status-1 handler marker
  confirms the normal ready write happened.
- Immediately after status1 writes ready, the same chain writes status `2`
  again before state11 expires and state8 begins polling.
- Therefore the current blocker is not "no ready state exists"; it is that
  Momoiro re-arms another SongSelectNetwork request before the Dani state8
  check, leaving `state + 0x1D8 == 2` when the scene machine reaches state8.

IDA for `sub_118984` explains the immediate re-arm candidate:

```text
sub_118984(wait):
    sub_116360(wait, 0)
    if (wait[0xA8]) {
        if (wait[0xA9]) {
            sub_118184(wait + 0x20, wait + 0x5C)
            sub_116A24(wait)
            wait[0xA9] = 0
            wait[0xA8] = 0
        }
    } else {
        sub_1159C4(wait + 0x0C)
        if (sub_115A20(wait + 0x0C))
            sub_116A24(wait)
        else
            sub_116708(wait)
    }
```

`sub_116A24` can call `sub_116708(wait)`, and `sub_116708` writes status `2`
on a successful `sub_10BAB8(...)` request submission.

## Diagnostic Build: Trace Status1 Re-Arm Request

Added narrower diagnostics:

```text
[tz] 1a8=0x........  status1 wait +0xA8
[tz] 1a9=0x........  status1 wait +0xA9
[tz] v10=0x........  status1 wait +0x10 vector begin
[tz] v14=0x........  status1 wait +0x14 vector end
[tz] v1c=0x........  status1 wait +0x1C cursor/count
[tz] rqi=0x........  sub_116708 request index from sub_115A64(wait + 0x0C)
[tz] rq2            sub_116708 successful request submission re-armed status 2
```

New hook sites verified in IDA:

```text
0x00116790: 815C00E8 lwz r10,0xE8(r28)
0x00116A18: 4BFFF949 bl sub_116360
```

Build/install:

```text
cmd /c .codex-tmp\build-win.cmd
first attempt: failed, missing local r28 register alias in the assembly payload
fix: added `.set r28,28`
second attempt: passed
install: copied bin\zucchini.sprx to H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx
```

Installed artifact:

```text
SHA-1:   4198E67E5DF6954AAC674AEC0DE0190BD2EFC538
SHA-256: 5189F0DCE5499D2F443A85716FB5B97E23A6070D5845C7930D6AE0545F509EEF
```

Next runtime interpretation:

- `[tz] h1r` followed by `[tz] rqi=...` and `[tz] rq2` confirms the status1
  handler itself re-arms status2 through `sub_116A24 -> sub_116708`.
- `v1c` compared to `(v14 - v10) / 4` tells whether the `wait + 0x0C`
  request vector has been exhausted before `sub_116A24` starts another request.
- If `1a8/1a9` are nonzero, the re-arm uses the alternate copied-result branch.
- If `rqi` identifies a repeated request index, compare that index against
  Kimidori/RED request tables and Momoiro's song-select Lumen records.

## Runtime Log Check: Request Vector Resets And Re-Arms Inside State8

Log files checked:

```text
H:\RPCS3\rpcs3-blue\log\TTY.log   LastWriteTime=2026-07-07 21:09:10 Length=40328
H:\RPCS3\rpcs3-blue\log\RPCS3.log LastWriteTime=2026-07-07 21:09:11 Length=5162303
```

Marker counts:

```text
381 [tz] h2b=0x00000000
236 [tz] h2a=0x00000001
167 [tz] m8 wait
165 [tz] m8s=0x00000002
147 [tz] h2a=0x00000000
  3 [tz] rqi=0x00000000
  3 [tz] rq2
  3 [tz] ns =0x00000002
  3 [tz] ns =0x00000000
  2 [tz] h1r
  2 [tz] h2b=0x00000001
  2 [tz] ns =0x00000003
  2 [tz] ns =0x00000001
  2 [tz] 1a8=0x00000000
  2 [tz] 1a9=0x00000000
  2 [tz] v10=0x33790b40
  2 [tz] v14=0x33790b44
  2 [tz] v1c=0x00000000
```

The important second completion happens while state8 is already polling:

```text
[tz] cbr=0xd000ed1c
[tz] m8 wait
[tz] m8s=0x00000002
[tz] h2b=0x00000001
[tz] h2a=0x00000001
[tz] ns =0x00000003
[tz] m8 wait
[tz] m8s=0x00000003
[tz] ns =0x00000001
[tz] m8 wait
[tz] m8s=0x00000001
[tz] ns =0x00000000
[tz] h1r
[tz] 1a8=0x00000000
[tz] 1a9=0x00000000
[tz] v10=0x33790b40
[tz] v14=0x33790b44
[tz] v1c=0x00000000
[tz] rqi=0x00000000
[tz] rq2
[tz] ns =0x00000002
[tz] m8 wait
[tz] m8s=0x00000002
```

Interpretation:

- The request vector has exactly one u32 entry: `v14 - v10 == 4`.
- `1a8/1a9 == 0`, so the normal status1 handler path runs.
- `sub_1159D4(wait + 0x0C)` in `sub_116A24` resets the cursor to zero.
- The same request index `0` is immediately submitted again.
- State8 does see status `3`, `1`, and `0` transiently, but the status1 chain
  re-arms status `2` before state8 gets a stable ready frame.

This changes the working hypothesis from "ready does not exist" to "ready is
overwritten by a repeated request submission while state8 is polling".

## Candidate Patch: Skip State8 Mode4 Status2 Re-Arm

Patched the `momoiro-v04r00-dani-network-request-status2-diag` hook at
Momoiro `0x00116A18`, replacing the successful `sub_116360(wait, 2)` re-arm
with a state/mode guard:

```text
if (*(wait - 0x12C) == 8 && *(wait - 0x128) == 4) {
    log "[tz] rqsk"
    return to 0x00116A1C without calling sub_116360(wait, 2)
} else {
    log "[tz] rq2"
    call sub_116360(wait, 2)
    return to 0x00116A1C
}
```

The guard uses the known relationship `wait == state + 0x13C`; therefore:

```text
wait - 0x12C == state + 0x10   // Dani state machine state
wait - 0x128 == state + 0x14   // Dani mode
```

Build/install artifact currently deployed:

```text
bin\zucchini.sprx LastWriteTime=2026-07-07 21:12:26 Length=1017337
H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx LastWriteTime=2026-07-07 21:12:26 Length=1017337

SHA-1:   D92953DB64510042566EFAED2EA1141C6CCE3682
SHA-256: 1954F3C7043B86807AB84753C2A878790CDCD01187B80550C287767369F17BAC
```

The installed and local SPRX hashes match.

## Latest Log Check: Current Log Predates Candidate Patch

After the candidate patch build/install, the current log directory still only
contains the 21:09 capture:

```text
H:\RPCS3\rpcs3-blue\log\RPCS3.log.gz LastWriteTime=2026-07-07 21:09:12
H:\RPCS3\rpcs3-blue\log\RPCS3.log    LastWriteTime=2026-07-07 21:09:11
H:\RPCS3\rpcs3-blue\log\TTY.log      LastWriteTime=2026-07-07 21:09:10
```

That capture contains the diagnostic hook installation lines and three native
re-arms:

```text
446  [tz] rqi=0x00000000
447  [tz] rq2
749  [tz] h1r
755  [tz] rqi=0x00000000
756  [tz] rq2
1336 [tz] h1r
1342 [tz] rqi=0x00000000
1343 [tz] rq2
```

It contains no `[tz] rqsk`, no `[tz] m8 ok`, no `[tz] m8->12`, no state12
markers, and no `JINGLE_DANI` or `dani_select` resource opens.

Conclusion: this latest log is not a validation run for the 21:12 candidate
skip build. The next run should be interpreted as follows:

- `[tz] rqsk` followed by `[tz] m8 ok` / `[tz] m8->12` means the re-arm skip is
  the missing Momoiro transition.
- `[tz] rqsk` without state8 advance means state8 has another predicate besides
  network status.
- no `[tz] rqsk` in a new log means the guard condition is wrong or RPCS3 did
  not load the 21:12 installed SPRX.

## Runtime Acceptance: State8 Advances To Dani Dojo

User runtime result after the 21:12 candidate build:

```text
"Now whatever has changed works."
```

Fresh logs checked:

```text
H:\RPCS3\rpcs3-blue\log\TTY.log   LastWriteTime=2026-07-07 21:38:43 Length=137267
H:\RPCS3\rpcs3-blue\log\RPCS3.log LastWriteTime=2026-07-07 21:38:55 Length=6043853
```

Installed artifact for this accepted run:

```text
bin\zucchini.sprx LastWriteTime=2026-07-07 21:12:26 Length=1017337
H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx LastWriteTime=2026-07-07 21:12:26 Length=1017337

SHA-1:   D92953DB64510042566EFAED2EA1141C6CCE3682
SHA-256: 1954F3C7043B86807AB84753C2A878790CDCD01187B80550C287767369F17BAC
```

Marker counts from the fresh TTY log:

```text
  1 [tz] rqsk
 16 [tz] rq2
 17 [tz] rqi=
 17 [tz] ns =0x00000000
 16 [tz] ns =0x00000002
 16 [tz] h1r
 16 [tz] h2b=0x00000001
106 [tz] m8 wait
104 [tz] m8s=0x00000002
  1 [tz] m8 ok
  1 [tz] m8->12
  1 [tz] m12 ent
  1 [tz] m12 m4
  1 [tz] ctor4
```

The decisive transition:

```text
6398 [tz] m8 wait
6399 [tz] m8s=0x00000002
6402 [tz] cbr=0xd000ed1c
6403 [tz] m8 wait
6404 [tz] m8s=0x00000002
6405 [tz] h2b=0x00000001
6406 [tz] h2a=0x00000001
6407 [tz] ns =0x00000003
6408 [tz] m8 wait
6409 [tz] m8s=0x00000003
6410 [tz] ns =0x00000001
6411 [tz] m8 wait
6412 [tz] m8s=0x00000001
6413 [tz] ns =0x00000000
6414 [tz] h1r
6415 [tz] 1a8=0x00000000
6416 [tz] 1a9=0x00000000
6417 [tz] v10=0x33032180
6418 [tz] v14=0x33032184
6419 [tz] v1c=0x00000000
6420 [tz] rqi=0x00000000
6421 [tz] rqsk
6422 [tz] m8 ok
6423 [tz] m8->12
6424 [tz] m12 ent
6425 [tz] m12 m4
6426 [tz] ctor4
6427 [tz] cbr=0xd000ed1c
```

Resource proof from `RPCS3.log` after the state12/constructor transition:

```text
50977 sys_fs_open(path="/dev_hdd0/game/SCEEXE001 Momoiro/USRDIR/data/sound/bgm/nub/JINGLE_DANI.nub")
50978 sys_fs_open(path="/dev_hdd0/game/SCEEXE001 Momoiro/USRDIR/data/lumendata/packed/dani_select/packeddata.ddp")
50979 sys_fs_open(): fd=4, Regular file, ".../JINGLE_DANI.nub", Pos/Size: 0/493.328KB
50980 sys_fs_open(): fd=5, Regular file, ".../dani_select/packeddata.ddp", Pos/Size: 0/3.16814MB
51003 sys_fs_close(fd=5): Regular file, ".../dani_select/packeddata.ddp", Pos/Size: 3.16814MB/3.16814MB
```

Conclusion:

- Momoiro did reach the native ready state before the patch.
- The failure was the status1 completion chain immediately re-submitting the
  single request index and restoring status `2` while Dani state8 was polling.
- Skipping only the state8/mode4 `sub_116360(wait, 2)` re-arm leaves the ready
  state visible to native state8.
- Native state8 then advances to state12, mode4 dispatch constructs the Dani
  Dojo select scene, and the expected Dani Dojo resources are opened.
