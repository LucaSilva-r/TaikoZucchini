# EBOOT Inline Hook Loader Design

Date: 2026-06-27

## Context

Taiko Zucchini already patches the game EBOOT during the first-run and repatch
flows. A local RPCS3 imported patch file proves a
White AC15 behavior change using two fixed instruction patches plus an
RPCS3 `calloc` inline hook.

RPCS3 `calloc` semantics are:

- allocate executable code space near the hook site;
- write a branch from the hook site into the allocation;
- fill the allocation with NOPs;
- write a return branch at the end of the allocation to `hook_site + 4`;
- write subsequent offset-0 patch entries into the allocation.

The White patch payload does not rely on the automatic return branch. It ends
with an explicit jump to White's row-iteration continuation at `0x0067DE40`.

## Goal

Add a generic EBOOT-time inline-hook installer that can host version-specific,
SDK-assembled payloads. The first payload targets White's Dani Dojo and
Taikojuku display flow. Later work can add 4-5 more binary versions for the same
feature without rewriting the installer.

## Non-Goals

- Do not install this hook live from the SPRX after game startup.
- Do not require Keystone, Python, or generated patch bytes for normal builds.
- Do not attempt to make one payload binary-compatible across all Taiko
  versions.
- Do not automatically relocate overwritten game instructions. Each hook spec
  is responsible for preserving any original behavior it replaces.

## Architecture

The patcher will gain a reusable inline-hook stage in the existing EBOOT patch
flow. It will run while the decrypted EBOOT is already available as a mutable
buffer, before the patched EBOOT is re-signed and written back.

The installer will append payload bytes to executable space in the EBOOT's RX
`PT_LOAD` segment, write a one-instruction branch from the hook site to the
payload, and update the same SELF/ELF size metadata already handled by the SPRX
loader payload path.

The first hook remains version-specific:

- fixed Dani gate writes keep using the existing self-validating patch logic;
- White's row hook branches from `0x0067EB7C` into the appended payload;
- the payload appends the original row `0x0D`, runs the Taikojuku bind/append
  flow, appends visible row `0x11`, and explicitly jumps to `0x0067DE40`.

## Components

### Inline Hook Installer

The installer owns these responsibilities:

- choose exactly one matching hook spec by validating expected instruction
  signatures;
- reserve RX payload space with proper alignment;
- copy assembled payload bytes into the EBOOT buffer;
- optionally append an automatic return branch for specs that want RPCS3-like
  `hook_site + 4` behavior;
- write the branch from hook site to payload;
- verify the copied payload and hook branch;
- report clear patch-time log messages and error codes.

The branch encoder must support the single-instruction branch forms that fit in
the overwritten hook site. If the appended payload is not branch-reachable, the
installer fails instead of emitting a larger stub over unrelated instructions.

### Hook Spec Table

Each spec describes one binary-specific payload:

- feature identifier, initially the existing `dani_dojo_unlock` patch flag;
- hook site VA;
- expected original words around the hook site;
- optional nearby context signatures for stronger version proof;
- payload start/end symbols;
- payload alignment;
- return mode: explicit payload continuation, or installer-appended auto-return;
- optional continuation VA for logging and validation.

Specs are data additions for future versions. The generic installer should not
need feature-specific branching beyond invoking the matching spec.

### SDK Assembly Payloads

Payload source lives in readable `.S` files under `patches/asm/`.

The payloads are assembled by the PS3 SDK toolchain as part of the normal
`make` and `nmake` builds. They should be emitted into a data/custom section
with global `start` and `end` symbols, not as callable SPRX functions. This
avoids OPD/function-descriptor confusion and lets the patcher copy the exact
instruction bytes.

Payload assembly should avoid unresolved relocations. Game helper addresses can
be expressed as constants/macros and loaded explicitly with instruction
sequences such as `lis`/`ori`/`mtctr`/`bctrl`, matching the existing RPCS3 patch
shape.

## Data Flow

1. The existing EBOOT flow reads, decrypts, and maps the original EBOOT.
2. Existing fixed patches run first, including Dani gate unlock writes.
3. The inline-hook stage checks whether the feature flag is enabled.
4. It scans the spec table and validates signatures against the EBOOT buffer.
5. If exactly one spec matches, it appends that spec's assembled payload to RX
   payload space.
6. It writes the hook-site branch to the appended payload.
7. It updates SELF metadata, ELF program headers, and embedded debug ELF
   headers consistently with existing RX segment growth.
8. The patcher re-signs and writes the patched EBOOT.
9. At runtime, PS3 executes the baked game-code branch and payload directly.

## Error Handling

The installer is conservative:

- no matching spec: log and skip the inline hook;
- multiple matching specs: fail the patch stage;
- hook site context mismatch after a spec partially matches: fail the patch
  stage;
- payload size is zero, unaligned, or otherwise invalid: fail before writes;
- payload cannot be reached by the one-word branch: fail before writes;
- no RX space before the next load segment: fail before writes;
- payload copy or branch verification fails: fail the patch stage.

For White, validation must include more than the single word at `0x0067EB7C`,
because the payload calls hard-coded White helper addresses and expects White's
register/object layout.

## Testing And Verification

Codex verification is limited to build and static checks.

Required local checks:

- run the repo's normal build command available in the current environment;
- confirm SDK assembly payloads compile and link into the SPRX;
- confirm the patcher logs/spec code can identify the payload size and symbols;
- inspect patched output statically when a patchable EBOOT is available, proving
  the hook site branches to the appended payload and payload bytes match the
  assembled source.

Runtime validation is performed by the user on RPCS3, PS3, or S357 hardware.
Runtime success for the first White spec is:

- Dani gate behavior remains enabled;
- the original `0x0D` row behavior is preserved;
- the Taikojuku row `0x11` appears and behaves like the proven RPCS3 patch.

## Rollout

Implement the generic installer and land one White spec first. After White is
statically verified and user runtime validation passes, add additional binary
versions as separate specs and `.S` payloads. Each new version must carry its own
signatures, payload, and runtime acceptance notes.
