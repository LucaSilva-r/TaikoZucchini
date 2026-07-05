# RED Dani Dojo Full Native Flow

Date: 2026-07-05
Repo: `H:\TaikoZucchini`
Reference binary: `H:\taiko\red\EBOOT.ELF.i64`
Comparison binary: `H:\TaikoLocalServer\.tools\kimidori\EBOOT.ELF.i64`

## Why This Matters

Kimidori can now enter Dani Dojo after the retained-resource-family test patch,
but the three song title boards still show the dummy title. Star counts render
correctly.

The current conclusion is narrower than the previous hypothesis: the full Dani
Lumen callback flow is not absent from Kimidori. Kimidori has the same native
`InitData`, `AssignMusic`, and `RequestFillrect` structure as RED. The remaining
bad surface is the type-10 title/fillrect resource resolution used by
`RequestFillrect`.

Runtime evidence still shows the three title board fillrect resolutions returning
`0x000A0000`, the base/dummy type-10 resource. The previously logged `u=` field
from the fillrect diagnostic should not be trusted, because the hook read the
wrong parent stack slot. The useful field from that run is still `f=0x000A0000`.

## RED State Flow

RED `GameDojoSelect::ChangeState` is `sub_9EE34`.

Important states:

- State 3: `sub_9F760`
- State 4: `sub_9EF94`
- State 5: `sub_9F100`
- State 7: `sub_9FAD8`

RED constructor/setup is `sub_A17BC`.

`sub_A17BC` creates the Dani select object family and installs the native Lumen
surface:

```text
sub_4B158(a1 + 3724, a1, a1 + 52, a1 + 3696)
sub_4BC64(a1 + 3724, *(a1 + 3776))
sub_510E4(a1 + 52, *(a1 + 3776), a2)
sub_9EE34(a1, 1)
```

`sub_4B158` installs the process-global Dani AS-connect pointer:

```text
dword_F8218C = a1 + 3724
(dword_F8218C + 8)  = a1 + 52
(dword_F8218C + 12) = a1
(dword_F8218C + 16) = a1 + 3696
```

The hidden `dword_F8218C + 0` field is also critical: `RequestFillrect` uses it
as the resource catalog root for type-10 lookup. `sub_4B158` does not write that
field, so it must already be initialized by the object construction path.

State 3 (`sub_9F760`) waits for the load gate, then prepares the Dani Lumen
surface:

```text
sub_4B19C(a1 + 3724)    // sets AS-connect ready byte +6
sub_4EBE8(a1 + 52, 1)   // SetVisible on one Lumen helper
sub_4E388(a1 + 52, 1)   // SetVisible on another Lumen helper
sub_4EB20(a1 + 52, 2)   // SetType(2)
sub_9EE34(a1, 4)
```

State 4 (`sub_9EF94`) waits down `a1 + 3792`, then changes to state 5.

State 5 (`sub_9F100`) waits for `a1 + 3728`, which is set by the
`LumenTerminate` native callback. If the selected row type is RED Dani row `30`,
it changes to state 7; otherwise it changes to state 6.

## RED Lumen Native Method Table

The RED Dani select method table starts at `0x00E271AC`. It registers under
`"Lumen"` through `sub_4BC64`.

Relevant entries:

```text
InitData           -> sub_4C4C0
IsInitWait         -> sub_4BD4C
IsReady            -> sub_4BD0C
RequestFillrect    -> sub_4BAD8
NotifyDaniSelect   -> sub_4B4BC
NotifyConfirm      -> sub_4B3C0
LumenTerminate     -> sub_4BCC8
SetSelectedInfo    -> sub_4BD8C
SetMotion          -> sub_4C080
```

`sub_4BCC8` sets byte `dword_F8218C + 4`, which is RED `a1 + 3728`.
`sub_4BD4C` returns byte `dword_F8218C + 5`.
`sub_4BD0C` returns byte `dword_F8218C + 6`.

## RED InitData Flow

RED `InitData` is `sub_4C4C0`.

High-level flow:

1. Calls the Lumen VM receive helper.
2. Reads player/daily status from `*(dword_F8218C + 12)`.
3. Emits `SetDailyMode` when needed.
4. Emits `SetPlayer`.
5. Iterates parsed Dani medley rows from `sub_9EDD0(*(dword_F8218C + 12))`.
6. Emits `AssignDani(row_type)`.
7. Emits `SetDaniStatus(clear_flag, status_sum, row_status)`.
8. For valid rows, emits exactly three `AssignMusic(uid, genre, course, star)`
   calls.
9. Emits `AssignCondition(condition_type, condition_value)` calls.
10. Returns `true` to Lumen.

The RED `AssignMusic` values come from medley content entries:

```text
uid    = *(content + 0x1C)
course = *(content + 0x20)
genre  = genre resolved from the music info record with matching uid
star   = course-table star count for that content/course
```

This explains the current symptom: star count can be correct even when title
resolution fails, because stars are computed from the medley/course data and do
not require the type-10 fillrect lookup to succeed.

## RED RequestFillrect Flow

RED `RequestFillrect` is `sub_4BAD8`.

It extracts:

```text
arg1 boardIndex -> local v11 / stack sp+0x80 in the decompile
arg2 uid        -> local v9  / stack sp+0x78 in the decompile
```

Then:

```text
root = *(dword_F8218C + 0)
resolved = sub_12A09C(&root, 10, uid)
sub_4DF6C(*(dword_F8218C + 8), boardIndex, resolved)
```

`sub_12A09C` is the type-resource lookup:

```text
type tree root = *(root + 0xA4C)
select type 10 tree
resolve uid through sub_12A034
```

`sub_12A034` returns the base resource when the requested UID is not inside any
registered range. For type-10 resources that base is the dummy/default
`0x000A0000`.

`sub_510E4` intentionally seeds every `fillrect_0` through `fillrect_14` to the
first/default type-10 resource before real song UIDs are requested. Seeing
`0x000A0000` at startup is normal. Seeing it after `RequestFillrect` for the
three Dani song title boards means the actual UID lookup is falling back.

## Kimidori Comparison

Kimidori has the same Dani native structure:

```text
ChangeState       sub_56430
Constructor       sub_5871C
State 3           sub_56DA0
State 4           sub_56590
State 5           sub_56624
InitData          sub_30054
RequestFillrect   sub_2F718
AS connect init   sub_2ED98
Method register   sub_2F8A4
Fillrect setup    sub_33BE0
Type lookup       sub_92620
```

Kimidori constructor `sub_5871C` installs the same family:

```text
sub_2ED98(a1 + 140, a1, a1 + 52, a1 + 112)
sub_2F8A4(a1 + 140, *(a1 + 192))
sub_33BE0(a1 + 52, *(a1 + 192), a2)
sub_56430(a1, 1)
```

Kimidori state 3 (`sub_56DA0`) matches RED's state-3 preparation:

```text
sub_2EDDC(a1 + 140)   // sets AS-connect ready byte +6
sub_31F8C(a1 + 52, 1)
sub_327F0(a1 + 52, 1)
sub_32728(a1 + 52, 2)
sub_56430(a1, 4)
```

Kimidori `InitData` (`sub_30054`) also emits the same semantic callbacks:

```text
SetPlayer
AssignDani
SetDaniStatus
AssignMusic(uid, genre, course, star)
AssignCondition
```

Kimidori `RequestFillrect` (`sub_2F718`) matches RED:

```text
root = *(dword_C0422C + 0)
resolved = sub_92620(&root, 10, uid)
sub_31B98(*(dword_C0422C + 8), boardIndex, resolved)
```

Kimidori resource offsets are smaller than RED's:

```text
Kimidori sub_92620: type tree root at root + 0x200
RED      sub_12A09C: type tree root at root + 0xA4C

Kimidori sub_92518: preferred type-10 list at root + 0x1CC/+0x1D0,
                    fallback list at root + 0x210/+0x214
RED      sub_12A11C: preferred type-10 list at root + 0x494/+0x498,
                    fallback list at root + 0xA88/+0xA8C
```

## Current Root Cause Model

The current dummy-title symptom is not explained by a wholly missing
`musicmedleyinfo.xml` parse or missing `InitData` callback. Both RED and
Kimidori contain that flow, and the rendered star counts prove the medley/course
portion is reaching Lumen.

The title-specific failure is after `AssignMusic`:

```text
DaniDetail.SetMusicInfo(info, index)
  -> RequestFillrect(info.uid, boardIndex)
  -> type-10 lookup
  -> dummy/base 0x000A0000
```

The most likely missing state is the type-10 resource range/registry population
or lifetime for the Dani title resources, not the Lumen callback list itself.
The earlier retain-family patch stopped the crash because the same `0x000Axxxx`
family was being released or looked up without a valid registry entry. It did
not populate the UID-to-title range table needed by `sub_92620`.

There are still two runtime cases to split:

1. Native `AssignMusic` is passing the correct music UID, but the type-10 range
   table does not contain that UID, so `sub_923A0` falls back to `0x000A0000`.
2. Native `AssignMusic` is passing the wrong UID despite correct stars, so the
   type-10 table may be valid but the requested UID is outside its registered
   ranges.

The old fillrect diagnostic cannot distinguish these because its logged `u=`
field used the wrong stack slot.

## Patch Implication From State 5

The selected-row type must remain the native Dani marker for the era. RED's
state 5 changes to Dani state 7 only when the selected row type is `30`.
Kimidori has the same structure, but its native predicate is row type `26`
(`0x1A`):

```text
Kimidori sub_56624:
selected_row_type == 26 -> a1+0x14 = 2; ChangeState(7)
otherwise               -> ChangeState(6)
```

The earlier Kimidori patch emitted row type `13` (`0x0D`) and then aliased that
marker in the `Proc_Main` hook. That enters Dani state 7, but the selected row
record no longer satisfies Kimidori's native Dani identity. The proper port is
to append the restored Kimidori row with marker `0x1A` and leave `Proc_Main`
matching only the native marker.

## Next Diagnostic Patch

Patch `sub_2F718` earlier or more precisely, before the call to `sub_92620`, and
log:

```text
dword_C0422C
*(dword_C0422C + 0)           // resource catalog root used by sub_92620
*(*(dword_C0422C + 0) + 0x200) // type tree root
boardIndex
uid from the real second argument local
resolved fillrect after sub_92620
```

The hook should not use the previous parent-frame `uid` slot. In the decompiled
Kimidori and RED `RequestFillrect` functions, the real UID local is the second
argument local corresponding to `v9` (`sp+0x78` in the decompiler's frame view).

If the hook shows real song UIDs and `resolved=0x000A0000`, the next patch should
target type-10 range/resource population or retention. If it shows bad UIDs, the
next patch should target the medley row/content mapping before `AssignMusic`.
