# Kimidori Dani RED-Port Restart Evidence

Date: 2026-07-05
Repo: `H:\TaikoZucchini`
Runtime log directory: `H:\RPCS3\rpcs3-blue\log`
Kimidori IDB: `H:\TaikoLocalServer\.tools\kimidori\EBOOT.ELF.i64`
RED IDB: `H:\taiko\red\EBOOT.ELF.i64`

## Purpose

This file records the restart point after multiple failed attempts to port RED
Dani Dojo state/resource behavior into Kimidori.

Important: the current installed build is not a final fix. It is the older
diagnostic keep-alive state used to keep Dani Select alive long enough to log
`RequestFillrect` values. Do not mistake this for the proper RED port.

## Failed Patch Timeline

1. RED-only direct resource-prepare attempt:
   - Hooked Kimidori Dani state2 at `0x000580B0`.
   - Called Kimidori native `sub_93258` for resource types `10, 9, 17, 18`.
   - Intended source rationale: RED `sub_A0F94` prepares types `10, 9, 17, 18`
     before constructing Dani Select.
   - Build/install hash reported for that attempt:
     `6653F884C7FD7CABB64B21F739EECF8E4FADFC28472A392F968D0C091071F1B2`.
   - Runtime result: hang while bulk-registering type-10 resources.

2. Later direct resource-prepare attempt:
   - Moved the hook later to Kimidori `0x00058264`, after Kimidori's two
     native package/catalog loops.
   - Still called Kimidori `sub_93258` for `10, 9, 17, 18`.
   - Build/install hash reported for that later attempt:
     `D52F22006D178F4222D037219E64890B445E1F8A77DD7489A3B986CA1E4DA864`.
   - Runtime result: same hang family. Register dumps showed the same resource
     builder state around the final type-10 range, including
     `0x000A0180..0x000A019F`.

3. Current installed build:
   - The direct `sub_93258` resource-prepare hook was removed.
   - The diagnostic keep-alive for `0x000A0000..0x000A019F` is present again.
   - The `RequestFillrect` diagnostic hook remains present.
   - Installed SPRX hash:
     `ABE543F8098805F237B94E1C540C6695B6A337E37246904BFDDDA76A60F008D8`.
   - This is a diagnostic bypass, not the proper fix.

## Latest Log Facts

Latest checked files:

- `TTY.log`: last write `2026/7/5 22:30:13`, length `215137`
- `RPCS3.log`: last write `2026/7/5 22:30:14`, length `5490751`

The latest TTY was read with `FileShare.ReadWrite` because RPCS3 held the log
open.

Observed `[tz]` tag counts:

- `cs`: 5
- `lk`: 2530
- `rf`: 3
- `ri`: 413
- `rr`: 3
- `rt`: 414
- `s4`: 197

The latest run is not the old bulk-registration hang. It reaches Dani state4,
logs three `RequestFillrect` calls, then continues emitting state4/lookup logs.

Relevant latest TTY lines:

```text
1713:[tz] cs=0x03 ps=0x02 f=0x00
1714:[tz] s4 a=0x35330a80 t=0x00aed240 c=0x34ecdb00
1715:[tz] cs=0x04 ps=0x03 f=0x00
1716:[tz] rf b=0x00000009 u=0x00000000 f=0x000a0000
1717:[tz] rr g=0x35330b0c r=0xd0010260 t=0x328c4fc0
1718:[tz] rf b=0x0000000a u=0x00000152 f=0x000a0000
1719:[tz] rr g=0x35330b0c r=0xd0010260 t=0x328c4fc0
1720:[tz] rf b=0x0000000b u=0x0000025c f=0x000a0000
1721:[tz] rr g=0x35330b0c r=0xd0010260 t=0x328c4fc0
1722:[tz] s4 a=0x35330a80 t=0x00c03230 c=0x34ecdb00
2503:[tz] cs=0x05 ps=0x04 f=0x00
```

Interpretation:

- `AssignMusic` or equivalent Lumen flow is passing real UIDs:
  `0`, `0x152`, and `0x25c`.
- `RequestFillrect` resolves all three to `0x000A0000`, the dummy/base type-10
  resource.
- `rr.g = 0x35330b0c` is the current Kimidori AS-connect object pointer
  (`dword_C0422C` value).
- `rr.r = 0xd0010260` is AS-connect field `+0`, used by `RequestFillrect` as
  the resource catalog root.
- `rr.t = 0x328c4fc0` is `*(root + 0x200)`, the type tree root used by
  Kimidori `sub_92620`.
- Therefore the current title failure is not "bad UID". The UIDs are real.
  The failure is that type-10 lookup maps those UIDs back to the base resource.

Observed repeated lookups after state4:

```text
0x000a0000 561
0x000a00ec 1969
```

Interpretation:

- The runtime repeatedly looks up the dummy base resource and one non-dummy
  title resource (`0x000A00EC`).
- It never looks up `0x000A0152` or `0x000A025C`, matching the fact that the
  type-10 range lookup returned `0x000A0000` before registry lookup.

## RED/Kimidori Binary Facts Checked So Far

RED constructor/setup:

- RED constructor/setup: `sub_A17BC`
- RED AS-connect init: `sub_4B158`
- RED Lumen method registration: `sub_4BC64`
- RED fillrect setup: `sub_510E4`
- RED `RequestFillrect`: `sub_4BAD8`
- RED state2: `sub_A0F94`

Kimidori constructor/setup:

- Kimidori constructor/setup: `sub_5871C`
- Kimidori AS-connect init: `sub_2ED98`
- Kimidori Lumen method registration: `sub_2F8A4`
- Kimidori fillrect setup: `sub_33BE0`
- Kimidori `RequestFillrect`: `sub_2F718`
- Kimidori state2: `sub_5802C`

RED `sub_4B158` and Kimidori `sub_2ED98` both initialize the AS-connect object
without writing field `+0`:

```text
field +12 = owner object
field +4  = 0
field +8  = Lumen/fillrect helper
field +16 = helper object
global AS-connect pointer = this object
field +5  = 0
```

Therefore AS-connect field `+0` must already be initialized before these init
helpers run, or it is inherited from object construction/zeroing/setup outside
the helper.

Latest Kimidori runtime proves field `+0` is nonzero:

```text
dword_C0422C = 0x35330b0c
*(dword_C0422C + 0) = 0xd0010260
*(*(dword_C0422C + 0) + 0x200) = 0x328c4fc0
```

That field is not missing outright. The remaining question is whether it points
to the same catalog root RED would use at the equivalent moment, and whether RED
has an additional type-10 range/list preparation path that is not equivalent to
Kimidori `sub_93258`.

Additional RED/Kimidori setup facts:

- RED `sub_A17BC` constructs the helper at `a1 + 0xE70`, then passes the
  AS-connect object at `a1 + 0xE8C` into `sub_4B158`.
- RED `sub_4D66C(a1 + 0xE70)` writes the helper's internal field at helper
  offset `+0x18`. Because the AS-connect object starts at helper offset
  `+0x1C`, this is not a write to AS-connect field `+0`.
- Kimidori `sub_5871C` constructs the corresponding helper at `a1 + 0x70`,
  then passes the AS-connect object at `a1 + 0x8C` into `sub_2ED98`.
- Kimidori `sub_312C0(a1 + 0x70)` also writes helper offset `+0x18`, not
  AS-connect field `+0`.
- Therefore both versions have the same helper/AS-connect layout relation:
  AS-connect starts `0x1C` bytes after the helper object, and the visible
  helper constructors do not initialize AS-connect field `+0`.

State/table facts gathered while restarting from RED:

- RED Dani state2 OPD: `0x00E4B850 -> sub_A0F94`.
- RED constructor/setup OPD: `0x00E4B860 -> sub_A17BC`.
- RED nearby state handlers include `sub_9EE34`, `sub_9EF94`, and `sub_9F100`.
- RED AS/Lumen method table includes `sub_4B158`, `sub_4B17C`,
  `sub_4B190`, `sub_4B19C`, `sub_4B3C0`, and `sub_4B4BC`.
- Kimidori Dani state2 OPD: `0x00AED3E8 -> sub_5802C`.
- Kimidori constructor/setup OPD: `0x00AED418 -> sub_5871C`.
- Kimidori nearby state handlers include `sub_56430`, `sub_56590`,
  `sub_565B0`, `sub_56624`, `sub_56824`, `sub_56834`, and `sub_56844`.
- The current state4 diagnostic hook logs Kimidori Dani object field
  `+0xD8` as `s4.t` and field `+0xC0` as `s4.c`.
- In the latest run, `s4.t` changes from `0x00AED240` to `0x00C03230` after
  state4 entry, then later to `0x00AED098` after state5. The `0x00C03230`
  value is runtime-patched data, not a normal static OPD table in the IDB.

RED state2 loader facts:

- RED `sub_A0F94` prepares type entries `10`, `9`, `17`, and `18`.
- For each type, it looks up the type entry under the root at `root + 0xA4C`,
  then calls `sub_12B6BC(type_entry + 0x10)`.
- RED `sub_12B6BC` is not structurally equivalent to Kimidori `sub_93258`.
  The RED path's persistent-cache string is an implementation detail of RED's
  resource package loader, not the behavior to port into Kimidori.
- The important RED contract is: if a type entry has byte `+8` set and byte
  `+10` clear, `sub_12B6BC(type_entry + 0x10)` waits for that type's resource
  package/load operation and then marks byte `+10` complete.
- Kimidori `sub_93258` directly iterates list ranges, calls `sub_A90CC`, and
  registers resources synchronously. Forcing that function in Dani state2
  caused the observed hang.
- Kimidori native code calls `sub_93258` from `sub_D53B4`, where it prepares
  type `10` and type `9` from the root's type tree at `root + 0x200`.
- Kimidori `sub_92518` rejects type values above `0x10`. RED's type `17` and
  `18` state2 prepares do not map directly to Kimidori's type universe.
- Therefore the old direct hook that tried to force Kimidori type
  `10, 9, 17, 18` was not a faithful Kimidori-side mapping even though RED
  prepares those four type ids.

Live IDA daemon confirmation:

- RED daemon target `H:\taiko\red\EBOOT.ELF.i64` was connected through
  `AgentSession.connect` using daemon id `633c9dc940286026`.
- Kimidori daemon target
  `H:\TaikoLocalServer\.tools\kimidori\EBOOT.ELF.i64` was connected through
  `AgentSession.connect` using daemon id `0610099bec9b05d3`.
- Kimidori Dani state2 `sub_5802C` has no call to `sub_93258`.
- Kimidori state2 first runs two native dependency loops through
  `sub_4863C8`, then proceeds to Lumen/resource setup at `0x00058264`.
- Kimidori's only direct code caller of `sub_93258` is `sub_D53B4`.
- In `sub_D53B4`, the type-ready sequence is:

```text
type 10: find node under *(root + 0x200), call sub_93258(node + 0x10)
type  9: find node under *(root + 0x200), call sub_93258(node + 0x10)
```

- Kimidori `sub_93258` checks byte `+0x0A` and the package list at
  entry offsets `+0x20/+0x24`, calls `sub_A90CC` for each resource chunk,
  appends produced ranges to the entry's range vector at `+0x0C`, then sets
  byte `+0x0A`.
- Kimidori `sub_934CC` is a separate per-package/resource helper. It is called
  only through `sub_63ADE8`, appends a package descriptor, may call
  `sub_A90CC`, updates the same range vector, and sets byte `+0x0A`.
  It is not the RED Dani state2 operation by itself.

## Current Working Hypothesis

The failed direct hook treated RED `sub_12B6BC` as equivalent to Kimidori
`sub_93258`. Runtime disproved that equivalence:

- Kimidori `sub_93258` synchronously creates/registers the type-10 resource
  family and hangs when forced in Dani state2.
- RED `sub_12B6BC` is a type-entry load/ready path. Its cache-backed storage
  implementation is not the portable fact. The portable fact is that RED Dani
  state2 explicitly makes type entries `10`, `9`, `17`, and `18` ready before
  Dani select requests fillrect/title resources.
- The Kimidori-native precedent found so far is narrower: normal Kimidori code
  makes type entries `10` and `9` ready through `sub_D53B4 -> sub_93258`.

The proper fix must identify what RED has before `RequestFillrect` that makes
UIDs like `0x152` and `0x25c` resolve to non-dummy type-10 resources, without
forcing Kimidori's synchronous `sub_93258` bulk load inside Dani state2.

## Next RED-Only Analysis Steps

1. In RED, trace every write to the Dani AS-connect object field `+0` or its
   constructor zero/init path.
2. In RED, inspect `sub_A0F94` around the four `sub_12B6BC` calls and identify
   what resource-entry fields are required by the later type-10 lookup.
3. In RED, inspect the loaded type-entry/range-list fields that `sub_12A034`
   later consumes. Treat RED's persistent cache as loader implementation
   detail, not a Kimidori patch target.
4. Map that state back to Kimidori by structure, not by assuming
   `sub_93258 == sub_12B6BC`.
5. Do not reintroduce the direct `sub_93258` state2 hook as a candidate fix
   unless RED equivalence is proven.
6. If a new Kimidori test patch is made, it must either:
   - mirror the Kimidori-native `sub_D53B4` type `10/9` ready sequence with
     diagnostics around the type-entry fields and return values, or
   - use the per-package `sub_934CC` path with proven inputs for the Dani
     music UIDs. Do not port RED's cache string or RED-only type ids `17/18`.

## Correction: Cache Is Not The Patch Target

User correction on 2026-07-05: Kimidori does not build RED's persistent cache,
and normal Kimidori operation is fine without that cache. Therefore the cache
path observed inside RED `sub_12B6BC` is not evidence for what to port.

Corrected interpretation:

- RED's `/cache/ST8100-1/` path is an implementation detail of RED's resource
  package loader.
- The portable RED behavior is only that Dani state2 makes the resource
  type-entry range state ready before `RequestFillrect`.
- The Kimidori patch must operate on Kimidori's native resource structures and
  functions. It must not create or depend on a persistent cache.

## Latest RED/Kimidori Daemon Recheck

Daemon mode was used for both IDBs:

- RED: `H:\taiko\red\EBOOT.ELF.i64`
- Kimidori: `H:\TaikoLocalServer\.tools\kimidori\EBOOT.ELF.i64`

RED state2 `sub_A0F94`:

- Reads a ready byte through the object field at `+0xEAC`, guarded by the
  same lock/unlock syscall pattern seen in Kimidori.
- If the ready byte is zero, it returns before resource/UI setup.
- If the ready byte is nonzero, it looks up type entries under
  `root + 0xA4C`.
- It calls `sub_12B6BC(entry + 0x10)` for type 10, type 9, type 17, and
  type 18.
- The relevant type-10 call is at `0x000A166C`.
- After those type-ready calls, it constructs the Dani select UI/Lumen state.

Kimidori state2 `sub_5802C`:

- Reads the equivalent ready byte through object field `+0xAC`, using the same
  lock/unlock pattern.
- If the ready byte is zero, it returns before resource/UI setup.
- If the ready byte is nonzero, it runs two native resource-list loops:
  - first over `*(root + 0x208)`, calling `sub_4863C8`;
  - then over `*(object + 0xA4)`, calling `sub_4863C8`.
- After those loops it proceeds at `0x00058264` into `sub_33460` and the Dani
  select setup.
- It does not prepare the type-10 lookup tree at `root + 0x200` anywhere in
  state2.

Kimidori normal song-select precedent `sub_D53B4`:

- Uses the type tree at `root + 0x200`.
- Finds type 10 and calls `sub_93258(entry + 0x10)` from `0x000D5D28`.
- Finds type 9 and calls `sub_93258(entry + 0x10)` from `0x000D5D14`.
- Then continues building the normal song-select UI.

Latest log interpretation remains:

- Dani's requested UIDs are real (`0x152`, `0x25c`).
- `RequestFillrect` resolves them through type 10.
- The type-10 range lookup returns `0x000A0000`, so later registry lookups only
  see dummy/base resources.
- This is missing type-entry/range readiness before Dani select rendering, not
  a row marker problem, not a fallback-render problem, and not a persistent
  cache problem.

Patch implication:

- The old state2 force loaded RED type ids `10, 9, 17, 18` through Kimidori
  `sub_93258`; that is not source-faithful for Kimidori because Kimidori's
  type lookup rejects values above `0x10`.
- A new patch must either:
  - reproduce only the Kimidori-valid type 10/9 readiness and prove it does
    not hang, or
  - identify the Kimidori per-package path that RED's type-entry readiness
    corresponds to, then call it with proven package inputs.
- The next patch should log type-entry fields before and after any readiness
  call: entry pointer, type id, byte `+8`, byte `+10`, package list
  `+0x20/+0x24`, and range vector `+0x0C/+0x10/+0x14`.

## Installed Passive Type-10 Diagnostic Patch

Implemented a passive diagnostic in
`patches/asm/kimidori_dani_runtime_diag_hooks.S` inside the existing Kimidori
`RequestFillrect` hook.

This patch does not call `sub_93258`, does not modify the type-entry range
vector, and does not change row selection or draw fallback behavior.

New log lines emitted after each `rf`/`rr` pair:

```text
[tz] te t=0x........ n=0x........ e=0x........
[tz] tv f=0x........ b=0x........ e=0x........
[tz] tp b=0x........ e=0x........ c=0x........
[tz] tr b=0x........ e=0x........ c=0x........
```

Field meanings:

- `te.t`: found type id, expected `0x0000000a` for type 10.
- `te.n`: type-tree node pointer.
- `te.e`: type-entry pointer (`node + 0x10`).
- `tv.f`: packed `type, byte+8, byte+9, byte+10`.
- `tv.b`/`tv.e`: range-vector begin/end pointers from entry `+0x10/+0x14`.
- `tp.b`/`tp.e`: package-list begin/end pointers from entry `+0x20/+0x24`.
- `tp.c`: package-list byte count.
- `tr.b`/`tr.e`: first registered range begin/end when the range vector is
  non-empty.
- `tr.c`: repeats the package-list byte count so the range and package evidence
  can be correlated from one screen of TTY output.

Build/install result:

- Build command:
  `nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=token`
- Install command:
  `nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=token RPCS3_DEV_HDD0=H:\RPCS3\rpcs3-blue\dev_hdd0 install`
- Installed SPRX:
  `H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx`
- Local and installed SHA-256:
  `0A31FFFA120031060729351330533401F8900DD76F22CD8EA5F2828F5D0D4DFF`

The game has not been rerun after this install. The expected next runtime
evidence is whether type 10 exists and, if so, whether `tv` shows an empty
range vector with a non-empty package list. That result decides whether the
next patch should safely trigger Kimidori type 10/9 readiness or instead route
through the per-package helper.

## Runtime Result After Passive Type-10 Diagnostic

Latest checked files after the user reran Dani Dojo:

- `TTY.log`: last write `2026/7/5 23:22:43`, length `332825`
- `RPCS3.log`: last write `2026/7/5 23:23:02`, length `6499025`

RPCS3 did not report a new crash in this run. The log ended with a manual
window close:

```text
SYS: All emulation threads have been signaled.
SYS: Objects cleared...
SYS: Quit with main_window::closeEvent. (autoexit=0)
```

Observed `[tz]` tag counts:

- `cs`: 5
- `lk`: 4837
- `rf`: 3
- `ri`: 413
- `rr`: 3
- `rt`: 414
- `s4`: 382
- `te`: 3
- `tp`: 3
- `tr`: 3
- `tv`: 3

Relevant TTY lines:

```text
1594:[INFO] (load) /dev_hdd0/game/SCEEXE001 Kimidori/USRDIR/data/lumendata/packed/dani_select/packeddata.ddp
1596:[INFO] (load) /dev_hdd0/game/SCEEXE001 Kimidori/USRDIR/data/sound/bgm/nsh/JINGLE_DANI.nsh
1599:[tz] cs=0x04 ps=0x03 f=0x00
1600:[tz] rf b=0x00000009 u=0x00000000 f=0x000a0000
1601:[tz] rr g=0x34e6c68c r=0xd0010260 t=0x328c4fc0
1602:[tz] te t=0x0000000a n=0x328c62c0 e=0x328c62d0
1603:[tz] tv f=0x0a010100 b=0x3322a480 e=0x3322a480
1604:[tz] tp b=0x32a0f5c0 e=0x32a0f6bc c=0x000000fc
1605:[tz] tr b=0x00000000 e=0x00000000 c=0x000000fc
1606:[tz] rf b=0x0000000a u=0x00000152 f=0x000a0000
1607:[tz] rr g=0x34e6c68c r=0xd0010260 t=0x328c4fc0
1608:[tz] te t=0x0000000a n=0x328c62c0 e=0x328c62d0
1609:[tz] tv f=0x0a010100 b=0x3322a480 e=0x3322a480
1610:[tz] tp b=0x32a0f5c0 e=0x32a0f6bc c=0x000000fc
1611:[tz] tr b=0x00000000 e=0x00000000 c=0x000000fc
1612:[tz] rf b=0x0000000b u=0x0000025c f=0x000a0000
1613:[tz] rr g=0x34e6c68c r=0xd0010260 t=0x328c4fc0
1614:[tz] te t=0x0000000a n=0x328c62c0 e=0x328c62d0
1615:[tz] tv f=0x0a010100 b=0x3322a480 e=0x3322a480
1616:[tz] tp b=0x32a0f5c0 e=0x32a0f6bc c=0x000000fc
1617:[tz] tr b=0x00000000 e=0x00000000 c=0x000000fc
2399:[tz] cs=0x05 ps=0x04 f=0x00
```

Interpretation:

- Dani select's packed data and JINGLE_DANI resource load requests are present.
- Dani reaches state4 and later state5 under the diagnostic keep-alive build.
- Type 10 exists in Kimidori's `root + 0x200` type tree.
- Type 10's flags are `0x0a010100`: type `0x0a`, byte `+8` set, byte `+9`
  set, byte `+10` clear.
- Type 10's package list is non-empty:
  `0x32a0f5c0..0x32a0f6bc`, byte count `0xfc`.
- Type 10's range vector is empty:
  `0x3322a480..0x3322a480`.
- Therefore the current failure is proven to be missing type-10 range/readiness
  population. `RequestFillrect` receives real UIDs (`0`, `0x152`, `0x25c`),
  but because the type-10 range vector is empty, `sub_92620` resolves all of
  them to `0x000A0000`.

Patch implication:

- The next patch should not change Dani row state, and should not add a draw
  fallback.
- The next patch should target the exact type-10 readiness gap: an entry with
  byte `+8` set, byte `+10` clear, queued packages present, and no ranges.
- The prior broad state2 call of Kimidori `sub_93258` for `10, 9, 17, 18` is
  still rejected by evidence: it hangs and includes RED-only type ids that do
  not map cleanly into Kimidori's type universe.

## Guarded RequestFillrect Type-10 Readiness Experiment

Installed after the latest passive diagnostic result.

Patch location:

- `patches/asm/kimidori_dani_runtime_diag_hooks.S`
- Existing Kimidori `RequestFillrect` hook at `0x0002F85C`

Patch behavior:

- Keep the passive `rf/rr/te/tv/tp/tr` diagnostics.
- Do not change Dani row state.
- Do not add any draw fallback.
- Do not call RED-only type ids `17` or `18`.
- Only call Kimidori native `sub_93258(type10_entry)` when all of these
  runtime guards match the observed broken state:
  - requested UID is nonzero;
  - current resolved fillrect id is `0x000A0000`;
  - type-10 entry exists;
  - packed flags are exactly `0x0A010100`;
  - package byte count is nonzero;
  - range vector begin equals range vector end.
- After the call, rerun Kimidori native
  `sub_92620(&root_local, 10, requested_uid)`.
- If the second resolve returns nonzero, update the saved `r29` value that the
  original draw call will use.

New diagnostic lines:

```text
[tz] tc e=0x........ f=0x........ c=0x........
[tz] td r=0x........ f=0x........ d=0x........
```

Field meanings:

- `tc.e`: type-10 entry passed to `sub_93258`.
- `tc.f`: pre-call packed flags.
- `tc.c`: pre-call package byte count.
- `td.r`: `sub_93258` return value.
- `td.f`: post-call packed flags.
- `td.d`: post-call `sub_92620` resolved fillrect id for the requested UID.

Expected interpretation:

- If TTY stops after `tc` with no `td`, Kimidori `sub_93258` is still hanging
  even when invoked lazily at the first nonzero Dani `RequestFillrect`.
- If `td.f` becomes `0x0A010101` and `td.d` becomes a non-base `0x000A....`
  id, the missing state is exactly type-10 range readiness and the hook has
  proved the source of the dummy-title behavior.
- If `td` appears but `td.f` remains `0x0A010100` or `td.d` remains
  `0x000A0000`, `sub_93258` is not the correct Kimidori equivalent for this
  Dani path and the next investigation must move to the per-package helper
  path.

Build/install result:

- Build command:
  `nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=token`
- Install command:
  `nmake /f Makefile.win TAIKO_ZUCCHINI_API_TOKEN=token RPCS3_DEV_HDD0=H:\RPCS3\rpcs3-blue\dev_hdd0 install`
- Installed SPRX:
  `H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx`
- Local and installed SHA-256:
  `2B649C7A838352EB5F99F0DE891DCD52C273BCCADDCA10B74EF7B835711C71CD`

The game has not been rerun after this guarded readiness install.

## Runtime Result After Guarded Type-10 Readiness Experiment

Latest checked files after the user reran Dani Dojo:

- `TTY.log`: last write `2026/7/6 01:47:34`, length `760948`
- `RPCS3.log`: last write `2026/7/6 01:47:39`, length `9807126`

RPCS3 did not report a new crash in this run. The log ended with a manual
window close:

```text
SYS: All emulation threads have been signaled.
SYS: Objects cleared...
SYS: Quit with main_window::closeEvent. (autoexit=0)
```

Observed `[tz]` tag counts:

- `cs`: 5
- `lk`: 13310
- `rf`: 18
- `ri`: 415
- `rr`: 18
- `rt`: 416
- `s4`: 926
- `tc`: 1
- `td`: 1
- `te`: 18
- `tp`: 18
- `tr`: 18
- `tv`: 18

The guarded call did not hang:

```text
1609:[tz] tc e=0x328c62d0 f=0x0a010100 c=0x000000fc
1610:[tz] td r=0x00000001 f=0x0a010101 d=0x000a0152
```

Interpretation:

- Kimidori `sub_93258(type10_entry)` returned `1`.
- It flipped type-10 flags from `0x0A010100` to `0x0A010101`.
- It populated the range vector from empty to `0x34d12c40..0x34d12c88`
  (`0x48` bytes, nine 8-byte ranges).
- Re-running `sub_92620(&root, 10, 0x152)` resolved `0x152` to
  `0x000A0152`.

Subsequent `RequestFillrect` calls prove the remaining behavior:

```text
5604:[tz] rf b=0x0000000a u=0x00000152 f=0x000a0152
5610:[tz] rf b=0x0000000b u=0x0000025c f=0x000a0000
7168:[tz] rf b=0x00000009 u=0x0000029e f=0x000a0000
7174:[tz] rf b=0x0000000a u=0x0000028e f=0x000a0000
7180:[tz] rf b=0x0000000b u=0x00000214 f=0x000a0000
9282:[tz] rf b=0x00000009 u=0x000002a0 f=0x000a0000
9288:[tz] rf b=0x0000000a u=0x000002a4 f=0x000a0000
9294:[tz] rf b=0x0000000b u=0x00000008 f=0x000a0008
```

Unique type-10 resource lookups after the readiness call:

```text
0x000A00EC -> 0x3172bf40
0x000A0000 -> 0x31719840
0x000A0152 -> 0x31733ec0
0x000A0008 -> 0x3171a240
```

Conclusion from this run:

- The first missing state was real: type 10 had to be made ready before
  non-dummy type-10 titles could resolve.
- The new non-dummy titles are not evidence of a renderer fallback. They are
  valid registry objects for the resolved type-10 ids.
- The remaining wrong-title behavior is an ID/flow problem: Dani is feeding
  raw UIDs such as `0x152` and `0x8` into `RequestFillrect`; once type 10 is
  ready, those raw UIDs resolve to valid but not necessarily intended title
  resources. Other Dani UIDs such as `0x25c`, `0x29e`, `0x28e`, `0x214`,
  `0x2a0`, and `0x2a4` are outside the currently registered Kimidori type-10
  range and still collapse to `0x000A0000`.
- Therefore the next RED-only investigation must find what RED does between
  parsed Dani medley/song data and `RequestFillrect`: either RED remaps Dani
  song identifiers before calling `RequestFillrect`, or RED loads/registers a
  different type-10 range set for Dani than the one Kimidori currently prepares.

## RED RequestFillrect Recheck After Wrong-Title Runtime

Daemon mode was used for RED IDB:

- RED: `H:\taiko\red\EBOOT.ELF.i64`

RED `RequestFillrect` at `sub_4BAD8` is structurally the same as Kimidori
`sub_2F718`:

```text
parse boardIndex argument
parse uid argument
root = *dword_F8218C
resolved = sub_12A09C(&root, 10, uid)
draw resolved id through the fillrect helper
```

RED's resolver returns base-plus-UID only when the UID lands inside a prepared
range; otherwise it returns the type base.

Conclusion:

- The remaining wrong-title problem is not caused by a RED/Kimidori difference
  inside `RequestFillrect`.
- RED must differ before that call: either the Dani Lumen flow passes a
  different id to `RequestFillrect`, or RED's Dani state preparation loads a
  type-10 range set that covers the Dani medley song ids.

## Runtime Data Finding: Kimidori MusicMedleyInfo Unique IDs Are Stale

After the guarded readiness patch, the user observed that some title slots
showed stable, wrong songs across reboots. Example: Dan slot 2 repeatedly
showed `tenyou` rather than dummy or random data.

Local data comparison on 2026-07-06:

- Active Kimidori `musicinfo.xml`:
  `H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data\musicinfo.xml`
- Active Kimidori `musicmedleyinfo.xml`:
  `H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data\musicmedleyinfo.xml`

The mismatch is in the data:

```text
musicmedleyinfo.xml: <musicid>wego</musicid>   <uniqueid>338</uniqueid>
musicinfo.xml:       <musicid>wego</musicid>   <uniqueid>366</uniqueid>
musicinfo.xml:       <musicid>tenyou</musicid> <uniqueid>338</uniqueid>
```

Therefore, once type-10 title resources are actually ready, the medley's stale
numeric ID `338` resolves to the valid title resource for `tenyou`. This is
stable data mismatch, not random memory and not a draw fallback.

Reusable fixer added:

- `tools/fix_musicmedley_uniqueids.py`

Behavior:

- Reads `musicinfo.xml`.
- Treats `musicid` as ground truth.
- Rewrites only `<uniqueid>` values inside `musicmedleyinfo.xml` `<Content>`
  blocks.
- Creates a timestamped backup before writing.
- Supports `--data-dir` or explicit `--musicinfo` / `--musicmedleyinfo`.
- Supports `--dry-run`.

Applied to the active Kimidori runtime data:

```text
python tools\fix_musicmedley_uniqueids.py --data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data"
```

Result:

- Changed medley entries: `48`
- Backup:
  `H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data\musicmedleyinfo.xml.bak-20260706-020852`
- Verification dry-run afterward reported `changed entries: 0`.

Spot checks after rewrite:

```text
wego   338 -> 366
ponpon 372 -> 276
mheart 190 -> 223
m96srb 508 -> 201
```

## Checkpoint Before Adjacent-Dani Duplication Investigation

User runtime report after correcting active Kimidori `musicmedleyinfo.xml`:

- Song titles are now loading.
- A new issue surfaced: only one of two adjacent Dani entries appears to load.
- Example report: 1st Dan and 2nd Dan both show 1st Dan data.

Preserved working state before investigating the new issue:

- Guarded type-10 readiness diagnostic hook remains the current runtime code.
- Active Kimidori medley data was corrected from `musicid` ground truth.
- `tools/fix_musicmedley_uniqueids.py` is the reusable data fixer.
- This evidence file records the findings that led to the current state.

Next investigation target:

- Determine whether adjacent Dani duplication is caused by the parsed medley
  list, by the Dani level/index state passed into Lumen, or by a Kimidori
  state-machine/selection field that RED initializes differently.

## Latest Log After Unlock and XML Correction

Runtime log checked after the user unlocked `H:\RPCS3\rpcs3-blue\log`.

Files:

- `RPCS3.log` last write: `2026-07-06 02:19:49`
- `TTY.log` last write: `2026-07-06 02:12:47`

`RPCS3.log` ended in normal shutdown/close-event lines, not a crash.

`TTY.log` confirms the active run is after the XML correction. The first
visible Dani rows now pass corrected medley IDs into `RequestFillrect`; the
registered IDs resolve to type-10 title resources, while `10tai` UID `0x2`
still collapses to the type base:

```text
[tz] rf b=0x00000009 u=0x00000002 f=0x000a0000
[tz] rf b=0x0000000a u=0x0000016e f=0x000a016e
[tz] rf b=0x0000000b u=0x0000009d f=0x000a009d

[tz] rf b=0x00000009 u=0x00000070 f=0x000a0070
[tz] rf b=0x0000000a u=0x000000a8 f=0x000a00a8
[tz] rf b=0x0000000b u=0x000000e3 f=0x000a00e3

[tz] rf b=0x00000009 u=0x000000fb f=0x000a00fb
[tz] rf b=0x0000000a u=0x00000154 f=0x000a0154
[tz] rf b=0x0000000b u=0x00000013 f=0x000a0013

[tz] rf b=0x00000009 u=0x0000013b f=0x000a013b
[tz] rf b=0x0000000a u=0x00000040 f=0x000a0040
[tz] rf b=0x0000000b u=0x000000f4 f=0x000a00f4

[tz] rf b=0x00000009 u=0x0000016b f=0x000a016b
[tz] rf b=0x0000000a u=0x00000072 f=0x000a0072
[tz] rf b=0x0000000b u=0x00000081 f=0x000a0081
```

These match the corrected active Kimidori medley data for the visible
pre-Dan rows:

```text
00 初級: 10tai=2, wego=366, iaybns=157
01 ４級: erfanc=112, invinv=168, mikuse=227
02 ３級: noshou=251, thbad=340, akb10t=19
03 ２級: siduso=315, chocod=64, natsu=244
04 １級: valvra=363, eva=114, godea2=129
```

Important conclusion:

- Type-10 title resource readiness is working for registered IDs that are
  actually passed to `RequestFillrect`.
- UID `0x2` still collapsing to `0x000A0000` is resource coverage for that
  specific title ID, not the adjacent-Dani duplication.
- The user-reported adjacent-Dani duplication is not visible in the
  `RequestFillrect` trace yet because this log only reached the rows above.
- Registry insert diagnostics show 初段/二段 title resources exist in memory,
  so the next bug is upstream of the title resolver: the parsed medley row list,
  InitData assignment stream, selected row/index state, or Lumen-side list
  binding.

## RED-Only Grounding for the Next Diagnostic

IDA daemon mode was used with:

- Kimidori: `H:\TaikoLocalServer\.tools\kimidori\EBOOT.ELF.i64`
- RED: `H:\taiko\red\EBOOT.ELF.i64`

The relevant RED/Kimidori comparison remains:

- RED InitData (`sub_4C4C0`) and Kimidori InitData (`sub_30054`) both build
  the Dani select list and push it into Lumen.
- Both flows call `AssignDani`, `SetDaniStatus`, and three `AssignMusic`
  operations for normal medley rows.
- RED row records are 20 bytes. Kimidori row records are 16 bytes.
- Kimidori's compiled-out logger `nullsub_172` at `0x00215E24` is a single
  `blr`.
- Kimidori InitData still calls that logger after the relevant Lumen assignment
  operations, passing fixed format strings:
  - `0x00A20DA0`: `* AssignDani %d`
  - `0x00A20DD0`: `SetDaniStatus %s %d %d`
  - `0x00A20DF8`: `AssignMusic %d %d %d %d`
  - `0x00A20E38`: `Can't find MusicInfo uniqueID:%d`

Therefore the next diagnostic should hook only this no-op logger and filter by
those format pointers. This records the exact InitData stream sent to Lumen
without changing row markers, selected state, resource loading, or drawing.

Patch added for the next run:

- `patches/asm/kimidori_dani_initdata_trace_hook.S`
- Inline hook spec:
  `kimidori-st51-v05r00-dani-initdata-trace`
- Hook site: `0x00215E24`
- Signature: one word, `0x4E800020` (`blr`)

Expected new TTY tags:

```text
[tz] ad r=0x........
[tz] as c=0x........ s=0x........ r=0x........
[tz] am u=0x........ g=0x........ c=0x........ s=0x........
[tz] af u=0x........
```

This should answer whether 初段 and 二段 are already duplicated before Lumen, or
whether Lumen selection/state is duplicating an otherwise correct assignment
stream.

Build/install result for this diagnostic patch:

```text
cmd /c vcvars64.bat && nmake /f Makefile.win CELL_SDK=H:\PS3_SDK TAIKO_ZUCCHINI_API_TOKEN=token sprx
```

- Build passed.
- Installed to:
  `H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx`
- Local and installed SHA-256:
  `19C4BDD1481A3C285D5121BC09BA10B56AEA7A5812E24A7E35073AA931ABB37D`

## Higher-Dani Duplication Root Cause: Mixed Medley ID Format

Latest runtime report after the InitData trace:

- Adjacent duplication was broader than first observed.
- Higher Dani ranks, especially `>= 六段`, displayed the same song data.

The diagnostic hook proved this is a data-shape problem, not a new missing
renderer/resource hook:

```text
[tz] ad r=0x00000000
[tz] am u=0x00000002 ...
[tz] am u=0x0000016e ...
[tz] am u=0x0000009d ...

[tz] ad r=0x00000002
[tz] am u=0x00000070 ...
[tz] am u=0x000000a8 ...
[tz] am u=0x000000e3 ...

...

[tz] ad r=0x0000000e
[tz] am u=0x00000107 ...
[tz] am u=0x0000004e ...
[tz] am u=0x000000e3 ...

[tz] ad r=0x0000000f
[tz] as c=0x00a20dc0 s=0x00000000 r=0x00000000
[tz] ad r=0x00000010
[tz] as c=0x00a20dc0 s=0x00000000 r=0x00000000
```

Interpretation:

- Kimidori's row markers are sequential (`0, 1, 2, ...`).
- Active Kimidori `musicinfo.xml` medley rows are sequential:
  `medley000..medley015` map to `20000..20015`.
- Active Kimidori `musicmedleyinfo.xml` was still in a Momoiro-style even-ID
  shape:
  `20000, 20002, 20004, ..., 20030`.
- Therefore only the medleyinfo rows whose top-level ID also existed in
  `musicinfo.xml` could attach songs. The first eight even IDs matched
  `musicinfo.xml` (`20000, 20002, ..., 20014`), then higher rows no longer
  matched and InitData emitted empty Dani rows.

Murasaki reference check:

- `H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Murasaki\USRDIR\data\config\ST5100-1`
  is the Kimidori-era format reference under the Murasaki install.
- Its `musicinfo.xml` and `musicmedleyinfo.xml` are internally consistent and
  sequential for that era.

Reusable normalizer added:

- `tools/normalize_musicmedley_ids.py`

Behavior:

- Reads `musicinfo.xml` medley rows (`musicid=medleyNNN`, `partsset=dojo`) in
  file order.
- Uses those IDs as the target sequence.
- Rewrites only top-level `MusicMedleyInfoData` `<uniqueid>` values in
  `musicmedleyinfo.xml`.
- Supports `--start-id` if a dataset's `musicinfo.xml` also needs medley-row
  renumbering.
- Creates backups only for files it changes.

Applied to active Kimidori runtime data:

```text
python tools\normalize_musicmedley_ids.py --data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data"
```

Changes:

```text
target medley IDs: 20000..20015 (16 rows)
musicinfo changes: 0
musicmedleyinfo changes: 15
01 ４級: 20002 -> 20001
02 ３級: 20004 -> 20002
03 ２級: 20006 -> 20003
04 １級: 20008 -> 20004
05 初段: 20010 -> 20005
06 二段: 20012 -> 20006
07 三段: 20014 -> 20007
08 四段: 20016 -> 20008
09 五段: 20018 -> 20009
10 六段: 20020 -> 20010
11 七段: 20022 -> 20011
12 八段: 20024 -> 20012
13 九段: 20026 -> 20013
14 十段: 20028 -> 20014
15 達人: 20030 -> 20015
```

Backup:

```text
H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data\musicmedleyinfo.xml.bak-20260706-030022
```

Post-write verification:

```text
python tools\normalize_musicmedley_ids.py --data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data" --dry-run
target medley IDs: 20000..20015 (16 rows)
musicinfo changes: 0
musicmedleyinfo changes: 0
no write needed

python tools\fix_musicmedley_uniqueids.py --data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data" --dry-run
changed entries: 0
no write needed
```

Expected next runtime result:

- InitData should emit `AssignMusic` for all 16 active Kimidori Dani medley
  rows through `達人`.
- If another issue remains after this, inspect the fresh TTY stream before
  changing hooks again.

## Correction: Real Kimidori Dani Data Has 22 Entries

User correction after the 16-row normalization:

- The active dump was not the complete real Kimidori Dani set.
- The Murasaki install contains the Kimidori-era reference at:
  `H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Murasaki\USRDIR\data\config\ST5100-1`
- That reference has 22 Dani entries:
  `初級, 十級, 九級, 八級, 七級, 六級, 五級, 四級, 三級, 二級, 一級, 初段, 二段, 三段, 四段, 五段, 六段, 七段, 八段, 九段, 十段, 達人`.

Correct merge strategy:

- `musicinfo.xml`: replace active Kimidori Dani medley `Data` rows with the
  ST5100-1 Dani medley rows as-is.
- `musicmedleyinfo.xml`: use ST5100-1 for rank order, top-level medley IDs,
  rank names, challenge levels, and rows missing from active Kimidori.
- Preserve active Kimidori song contents for ranks that already existed in the
  active data. This keeps Kimidori's existing `達人` songs; the regular
  `souryu` song row is not needed for this strategy.
- For ranks missing from active Kimidori, use the ST5100-1
  `musicmedleyinfo.xml` rows.

Reusable merger added:

- `tools/merge_kimidori_dani_from_st5100.py`

Applied to active Kimidori runtime data:

```text
python tools\merge_kimidori_dani_from_st5100.py --target-data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data" --reference-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Murasaki\USRDIR\data\config\ST5100-1"
```

Backups created:

```text
H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data\musicinfo.xml.bak-20260706-032555
H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data\musicmedleyinfo.xml.bak-20260706-032555
```

Post-merge verification on current active Kimidori data:

```text
python tools\merge_kimidori_dani_from_st5100.py --target-data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data" --reference-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Murasaki\USRDIR\data\config\ST5100-1" --dry-run
target: H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data
reference: H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Murasaki\USRDIR\data\config\ST5100-1
musicmedley preserved Kimidori ranks: 22
  初級, 十級, 九級, 八級, 七級, 六級, 五級, 四級, 三級, 二級, 一級, 初段, 二段, 三段, 四段, 五段, 六段, 七段, 八段, 九段, 十段, 達人
musicmedley inserted ST5100-1 ranks: 0
musicinfo removed target medley rows: 22
musicinfo inserted ST5100-1 medley rows: 22
musicmedley changed: False
musicinfo changed: False
dry run; no files written

python tools\normalize_musicmedley_ids.py --data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data" --dry-run
target medley IDs: 20001..20022 (22 rows)
musicinfo changes: 0
musicmedleyinfo changes: 0
no write needed

python tools\fix_musicmedley_uniqueids.py --data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data" --dry-run
changed entries: 0
no write needed
```

Compile check:

```text
python -m py_compile tools\merge_kimidori_dani_from_st5100.py tools\normalize_musicmedley_ids.py tools\fix_musicmedley_uniqueids.py
```

Result: passed.

Current expected runtime check:

- InitData should now enumerate 22 Dani ranks.
- `musicinfo.xml` Dani medley rows should be the ST5100-1 Kimidori-era rows.
- Existing active Kimidori rank song contents should be preserved where the
  rank already existed, including `達人`.
- Missing lower ranks should use ST5100-1 song contents.

## Correction: Boost Serialization Size And Class IDs Matter

User correction after the first ST5100-1 merge:

- These XML files are boost serialization archives.
- The top-level collection `size` field must match the actual serialized entry
  count.
- `musicinfo.xml` `Data class_id` values must remain sequential in serialized
  file order.

Bug in the first ST5100-1 merge:

```text
musicinfo.xml <size>: 427
actual <Data> blocks: 433
last normal song row: class_id 412
inserted medley rows: class_id 470..491
```

That happened because the script copied ST5100-1 medley `Data` blocks as raw
blocks into active Kimidori's smaller song table. The visible medley IDs were
right, but the boost serialization metadata was not right for the active file.

Fix applied to `tools/merge_kimidori_dani_from_st5100.py`:

- After replacing the medley block range, update the `TaikoAC15 MusicInfo`
  header `<size>` to the actual number of `Data` blocks.
- Resequence every `Data class_id` in file order from the active file's first
  `Data` class id.
- Keep `musicmedleyinfo.xml` header `<size>` tied to the actual
  `MusicMedleyInfoData` block count.

Applied to active Kimidori runtime data:

```text
python tools\merge_kimidori_dani_from_st5100.py --target-data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data" --reference-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Murasaki\USRDIR\data\config\ST5100-1"
```

Backup created:

```text
H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data\musicinfo.xml.bak-20260706-033410
```

Post-fix verification:

```text
python tools\merge_kimidori_dani_from_st5100.py --target-data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data" --reference-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Murasaki\USRDIR\data\config\ST5100-1" --dry-run
musicmedley changed: False
musicinfo changed: False
dry run; no files written

python tools\normalize_musicmedley_ids.py --data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data" --dry-run
target medley IDs: 20001..20022 (22 rows)
musicinfo changes: 0
musicmedleyinfo changes: 0
no write needed

python tools\fix_musicmedley_uniqueids.py --data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data" --dry-run
changed entries: 0
no write needed
```

Direct structure check on active Kimidori data:

```text
musicinfo size 433 data blocks 433 size_ok True
musicinfo class ids 2 434 sequential True
musicmedleyinfo size 22 blocks 22 size_ok True
medley rows 22 first medley000 20001 last medley021 20022
medley class ids 413 434
```

Current expected runtime check is unchanged except the data is now structurally
valid as a boost serialization archive:

- InitData should enumerate 22 Dani ranks.
- The active `musicinfo.xml` medley rows should parse in the active file's
  class-id sequence, not with ST5100-1's original class-id offsets.

## Abort Root Cause: Mixed MusicMedleyInfo Header Version And Content Shape

Runtime abort after the `musicinfo.xml`/Dani data merge:

```text
abort() is called from 0x00000000004470cc
                  from 0x0000000000447074
                  from 0x000000000045a724
                  from 0x000000000070e294
                  from 0x000000000070e8a4
                  from 0x00000000006fb904
                  from 0x00000000004eba2c
                  from 0x0000000000399544
                  from 0x00000000004eaebc
                  from 0x0000000000399544
                  from 0x000000000006be5c
                  from 0x0000000000528a0c
                  from 0x0000000000535e14
                  from 0x00000000000a7bd0
                  from 0x0000000000fa000c
```

IDA daemon mode was used with:

```text
IDA_CLI_DAEMON_DIR=H:\TaikoZucchini\.codex-tmp\ida-daemons
target=H:\TaikoLocalServer\.tools\kimidori\EBOOT.ELF.i64
target id=0610099bec9b05d3
```

Mapped stack frames:

```text
0x4470cc -> sub_4470BC+0x10
0x447074 -> sub_447040+0x34
0x45a724 -> sub_45A6A8+0x7c
0x70e294 -> sub_70DE6C+0x428
0x70e8a4 -> sub_70E884+0x20
0x6fb904 -> sub_6FB8BC+0x48
0x4eba2c -> sub_4EB73C+0x2f0
0x399544 -> sub_399204+0x340
0x4eaebc -> sub_4EAB90+0x32c
0x6be5c  -> sub_6B250+0xc0c
0x528a0c -> sub_5289D8+0x34
0x535e14 -> sub_535330+0xae4
0x0a7bd0 -> sub_A7AEC+0xe4
```

Hex-Rays decompile evidence:

- `sub_7501C8` is the XML end-tag helper and calls `sub_6FB8BC`.
- `sub_6FB8BC` reads the next `>`-terminated XML tag via
  `sub_70E884 -> sub_70DE6C`, then compares it with the expected tag name.
- `sub_70DE6C` throws through `sub_45A6A8` when the archive stream is already
  in an error state or the expected tag cannot be read cleanly.
- `sub_4EB73C` is the medley content item loader in this stack. It parses:
  `musicid`, `uniqueid`, `difficulty`, then conditionally `hidden`, then
  `notes`.
- The conditional branch is:

```text
if (content_version <= 0x20140500)
    load <hidden> ... </hidden>
load <notes> ... </notes>
```

Root cause:

- Active `musicmedleyinfo.xml` kept the old Kimidori/Momoiro-style header:

```text
<version>538054930</version>
```

- `538054930` is below the `0x20140500` cutoff, so Kimidori enters the legacy
  `hidden` branch for every medley `Content` row.
- Rows preserved from the old active data had `<hidden>...</hidden>`.
- Rows inserted from ST5100-1 did not have `<hidden>`, because ST5100-1 uses:

```text
<version>538182913</version>
```

- The merged file was therefore mixed-format:

```text
target before fix:
  version 538054930
  Content blocks 66
  hidden tags 48
  notes tags 66

ST5100-1 reference:
  version 538182913
  Content blocks 66
  hidden tags 0
  notes tags 66
```

Failure mechanism:

- On the first ST5100-1 inserted content row, the loader expected
  `<hidden>... </hidden>` because the header version was old.
- The XML stream actually had `<notes>... </notes>`.
- The `sub_7501C8("hidden")` close-tag check therefore hit the archive error
  path and threw through `sub_70DE6C -> sub_45A6A8`.

Fix applied to `tools/merge_kimidori_dani_from_st5100.py`:

- Copy the ST5100-1 `MusicMedleyInfoHeader` version into active
  `musicmedleyinfo.xml`.
- Normalize medley `Content` hidden-field presence to match ST5100-1. For the
  Kimidori ST5100-1 reference, this means removing all `<hidden>` tags.
- Keep the existing rank/song preservation strategy otherwise unchanged.

Applied to active Kimidori runtime data:

```text
python tools\merge_kimidori_dani_from_st5100.py --target-data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data" --reference-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Murasaki\USRDIR\data\config\ST5100-1"
```

Backup created:

```text
H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data\musicmedleyinfo.xml.bak-20260706-035616
```

Post-fix verification:

```text
python tools\merge_kimidori_dani_from_st5100.py --target-data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data" --reference-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Murasaki\USRDIR\data\config\ST5100-1" --dry-run
musicmedley changed: False
musicinfo changed: False
dry run; no files written

python tools\fix_musicmedley_uniqueids.py --data-dir "H:\RPCS3\rpcs3-blue\dev_hdd0\game\SCEEXE001 Kimidori\USRDIR\data" --dry-run
changed entries: 0
no write needed

direct structure check:
medley version 538182913
medley size 22 blocks 22 size_ok True
content blocks 66 hidden 0 notes 66 hidden_ok True notes_ok True
musicinfo size 433 blocks 433 size_ok True
```

Expected next runtime result:

- The startup abort during medley content deserialization should be gone.
- If runtime still fails, check the fresh TTY/RPCS3 logs before making another
  data or hook change.

## Final Runtime Acceptance And Hook Cleanup

User runtime result after applying the medley header/content-shape fix:

```text
Now it is fully working.
```

Final cleanup decision, corrected after post-cleanup regression:

- Keep the real Kimidori Dani hook path:
  - `kimidori-st51-v05r00-dani-row`
  - `kimidori-st51-v05r00-dani-proc-main`
  - `kimidori-st51-v05r00-dani-resource-retain`
  - `patch_kimidori_dani_state4_service_table`
- Remove the temporary diagnostic hooks and payloads from the final build:
  - change-state trace hook
  - initdata trace hook
  - lookup/state4/registry/fillrect runtime diagnostic hooks
- Remove the temporary `[tz] pm=...` TTY marker from the proc-main hook itself.

The cleanup initially removed the registry-insert hook as if it were only
diagnostic. That was wrong: the hook also performed the retained-resource-family
experiment that runtime had already proven necessary. The production form keeps
only the behavior:

```text
on registry insert:
  if 0x000A0000 <= key < 0x000A01A0 and resource != NULL:
      atomic_increment(*(resource + 4))
```

The final patch is therefore data-shape correction plus the established Kimidori
native Dani hook path and the resource-family retain hook, not a logging build.
