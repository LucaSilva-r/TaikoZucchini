Date: 2026-07-04

# Kimidori Dani Native Flow Restoration Design

## Context

Kimidori already contains most of the pre-RED Dani Dojo code. The current patch
restores visibility by allowing the dormant type-9 row path and appending the
visible Dani row, but runtime still crashes after entering Dani select:

- crash site: Kimidori `0x003B2524`;
- failing lookup: `sub_3BB70C(&dword_F70280, 0x000A00EC, &out)`;
- failure mode: `out == 0`, followed by an unchecked dereference;
- asset evidence: `0x000A00EC` belongs to the shared `entry` packed resources,
  not to `dani_select`;
- negative evidence: overwriting `GameDojoSelect` root/shared pointer slots
  corrupts ownership and must not be repeated.

The fix therefore needs to restore the native control flow that made entry
resources available before Dani select renders. It must not invent a separate
resource ownership model.

## Goal

Restore Kimidori Dani Dojo by re-enabling native logic that is present but
disabled or unreachable.

The patch may use EBOOT-time inline hooks, but each hook must recreate an
existing native branch, state transition, or helper call. If a branch was
compiled away, the hook should synthesize that branch into the original native
target. It should not hand-build resources, overwrite unrelated object fields,
or install a standalone resource graft.

## Non-Goals

- Do not add Taikojuku behavior.
- Do not patch around the renderer null dereference.
- Do not insert a fake texture/object into the global resource map.
- Do not write `entry` resources into `GameDojoSelect` persistent root slots
  such as `a1+0xA4` / `a1+0xA8`.
- Do not rely on runtime-only hooks when the existing EBOOT inline-hook
  substrate can restore the branch statically.

## Native Flow Model

Kimidori and Murasaki both retain these native pieces:

- `GameDojoSelect::ChangeState`;
- `GameDojoSelect::Proc_Main`;
- the Dani-select loader state;
- `entry/packeddata.ddp` loader logic in the separate entry state machine;
- the Lumen and packed-data references required by Dani select.

The row restoration alone is insufficient because it exposes Dani select without
restoring all state that native Dani selection expects. The implementation must
therefore locate the disabled native decision point that should prepare or
retain the `entry` resources before the Dani-select Lumen draws `DON_ENTRY_*`
content.

## Preferred Fix Shape

The preferred patch is a Kimidori-specific EBOOT inline hook that restores the
native Dani transition branch.

1. Keep the existing pre-RED emit-gate hook and Kimidori dormant-row hook as the
   visibility layer.
2. Compare Kimidori and Murasaki around `GameDojoSelect::Proc_Main`, state 6,
   state 7, and the callback that ultimately enters state 1.
3. Identify the row/state marker that native Dani selection should test. Do not
   assume visible row `0x0D` is the selected marker unless the backing-row code
   proves it.
4. Patch the disabled/missing branch so the restored Dani row enters the same
   native special transition path as the supported era.
5. Let native state code perform native setup and cleanup.

If the needed branch target is no longer reachable by changing a condition or
immediate, the inline payload should do the same native work explicitly:

- preserve the caller frame and TOC;
- test the native selected-row/state value;
- branch to the existing Kimidori native target when the test matches;
- otherwise run the original instruction(s) and return to the normal path.

## Fallback Fix Shape

If the state comparison proves that the native transition path is already
correct and only an earlier entry-resource preload was compiled away, add a
second Kimidori-specific inline hook at the native preload site.

That hook may call existing Kimidori loader helpers such as the path builder and
shared resource loader, but only in the same ownership pattern used by native
code. The hook must store retained objects only into fields that native code
already owns for that resource lifetime, or branch into the native state
function that performs that storage.

This fallback is still branch restoration: it reintroduces a removed native load
step. It is not a manual resource-map insertion.

## Patch Boundaries

Implementation should remain inside the existing EBOOT inline-hook system:

- add Kimidori-only `.S` payloads under `patches/asm/`;
- add validated Kimidori signatures in `eboot_patcher/eboot_inline_specs.c`;
- patch payload addresses through the existing magic-word replacement pattern
  where constants need to be version-specific;
- leave existing White and Murasaki payloads untouched except for shared
  declarations/build-list updates if needed.

The signature should validate more than the hook instruction. It should include
nearby branch/state context so the hook cannot install on the wrong AC15 build.

## Verification

Agent verification is build/static unless runtime validation is explicitly
requested.

Required static checks:

- IDA evidence showing the exact restored branch or native helper call;
- static guard that no resource-root overwrite hook remains;
- static guard that the Kimidori hook preserves non-Dani path behavior;
- full Windows or GNU Make build with the PS3 SDK;
- optional patched-EBOOT inspection proving the hook branch and payload bytes
  were installed at the expected addresses.

Runtime acceptance remains user-run on RPCS3, PS3, or S357 hardware:

- entering restored Kimidori Dani Dojo no longer crashes at `0x003B2524`;
- no new crash appears at the previous root-slot corruption site;
- normal Kimidori song-select and normal play still work;
- Dani remains first-song-only according to the existing pre-RED gate behavior.
