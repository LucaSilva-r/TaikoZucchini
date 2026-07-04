# Kimidori Dani Dojo Loading Flow Findings

Date: 2026-07-05
Repo: `H:\TaikoZucchini`
Runtime logs: `H:\RPCS3\rpcs3-blue\log\TTY.log`, `H:\RPCS3\rpcs3-blue\log\RPCS3.log`
Lumen/native evidence root: `H:\TaikoLocalServer\.tools`

## Current Runtime State

The latest retained-resource-family test patch lets Kimidori enter Dani Dojo.
The remaining visible symptom is that the Dani song title boards show dummy
titles. Star counts render correctly.

Relevant code commits:

- `c4ff71d Retain Kimidori Dani resource family for test`
- `7ef9aa7 Trace Kimidori Dani fillrect resolution`

Installed runtime artifact from the fillrect diagnostic run:

- `H:\RPCS3\rpcs3-blue\dev_hdd0\plugins\taiko\zucchini.sprx`
- SHA1: `FD9F20EC18050FA508966C0FE2FE375BAE526828`

## Flow Proven So Far

The Lumen Dani select script is under:

- `H:\TaikoLocalServer\.tools\kimidori\lumen\out\dani_select\entry.disasm.txt`
- `H:\TaikoLocalServer\.tools\kimidori\lumen\out\dani_select\entry.symbols.tsv`

Observed Lumen/native flow:

1. `Main.Startup` calls `cpp.ReceiveInitInfo()`, waits for receive data, then
   checks `resource.VerifyRecieveData()`.
2. Native parsing is expected to use `musicmedleyinfo.xml` to provide Dani pack
   and song data through callbacks such as `Callback_AssignDani`,
   `Callback_SetDaniStatus`, `Callback_AssignMusic`, and
   `Callback_AssignCondition`.
3. `DaniInfo.AssignMusic(uid, genre, course, star)` creates `MusicInfo`, stores
   genre/course, calls `SetStar(star)`, and pushes the object to the Dani music
   list.
4. `DaniDetail.SetMusicInfo(info, index)` calls
   `cpp.RequestFillrect(info.uid, fillrect[index].index)` before updating the
   genre/course/star UI.
5. Native `RequestFillrect` is Kimidori `sub_2F718`. It extracts the board index
   and UID arguments, resolves the UID through a native type-10 lookup, then
   calls the board draw/update function:

   - `sub_92620(&dword_C0422C[0], 10, uid)` resolves the title/fillrect id.
   - `sub_31B98(*(dword_C0422C + 8), boardIndex, resolved)` applies it.

This means the song-title symptom is downstream of Dani medley parsing. Correct
star counts prove at least part of `AssignMusic` data is arriving; title display
also depends on native fillrect/title lookup state.

## Diagnostic Log Evidence

The fillrect diagnostic hook emitted exactly three title-board calls:

```text
[tz] rf b=0x00000009 u=0x34d8a900 f=0x000a0000
[tz] rf b=0x0000000a u=0x00000000 f=0x000a0000
[tz] rf b=0x0000000b u=0x00000000 f=0x000a0000
```

Interpretation:

- `b=9/10/11` are the three Dani song title board slots.
- `f=0x000A0000` for all three means native title/fillrect resolution is not
  producing distinct song title resources for the displayed songs.
- The current `u=` value should not be trusted yet. The hook reads the UID from a
  parent stack slot after the native lookup; the first value looks pointer-like
  and the next two are zero. Before using `u` as proof, add a more precise hook
  before `sub_92620`, around `0x0002F820..0x0002F840`, or re-derive the exact
  stack slot in IDA.

The earlier lookup diagnostics continue to show repeated failed lookups in the
Dani resource family:

```text
[tz] lk k=0x000a00ec o=0x3172bf40 r=0x00000000
[tz] lk k=0x000a0000 o=0x31719840 r=0x00000000
```

The retained-resource-family patch prevents the earlier crash, but the remaining
dummy titles point to missing native state, not only object lifetime.

## Working Hypothesis

Kimidori's Dani entry path is missing more than one native state transition or
initialization block. The missing block likely both:

- keeps/registers the relevant `0x000Axxxx` Dani resources, which explains why
  retaining the family stopped the crash; and
- populates or preserves the type-10 title/fillrect lookup state used by
  `RequestFillrect`, which explains why titles resolve to dummy/default.

Because star counts are correct, `musicmedleyinfo.xml` parsing is not wholly
missing. The likely missing piece is the native Dani state machine path that
connects parsed medley song IDs to the fillrect/title resource registry.

## Next Session Direction

Use RED as the 100% working reference flow first, then Murasaki if RED does not
map cleanly.

Suggested RED comparison targets under `H:\TaikoLocalServer\.tools`:

- `red\lumen\out\dani_select\*`, if present
- `red\lumen\out\song_select\*`
- `red\EBOOT.ELF.i64`
- `murasaki\EBOOT.ELF.i64` as a second reference

Specific next steps:

1. Reconfirm exact Kimidori `RequestFillrect` argument extraction before trusting
   the `u=` diagnostic field.
2. Find RED's equivalent of Kimidori `sub_2F718` / `RequestFillrect` and compare
   the type-10 lookup setup.
3. Trace RED's Dani transition from song select into Dani select, especially the
   native state change that parses `musicmedleyinfo.xml`, registers
   `0x000Axxxx` resources, and prepares fillrect/title mappings.
4. Compare the same transition against Kimidori's patched state-4 path and the
   previous retain-family experiment.
5. Design the next patch from the missing RED/Murasaki transition block rather
   than adding another symptom-level retain or dummy-title workaround.

