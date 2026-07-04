# Kimidori Dani Runtime Fallback Evidence

Date: 2026-07-04

## Runtime Failure

The latest RPCS3 log still crashes at Kimidori `0x003B2524` with lookup key
`0x000A00EC` and a null lookup result. The log also shows both files loading
successfully before the crash:

```text
/data/lumendata/packed/entry/packeddata.ddp
/data/lumendata/packed/dani_select/packeddata.ddp
```

This is not a missing-file failure. It is the same renderer lookup after the
generic Dani-select packed-data path is reached without the native Dani flow.

## IDA Control-Flow Check

Kimidori `GameDojoSelect::Proc_Main` at `0x005666C` only branches to native
Dani flow when the selected row marker is `0x1A`:

```text
00056668 80090000 lwz       r0, 0(r9)
0005666C 2F80001A cmpwi     cr7, r0, 0x1A
00056670 419E0078 beq       cr7, loc_566E8
```

The restored dormant row case emits marker `0x0D`:

```text
0057C588 3900000D li        r8, 0xD
0057C58C 4BFFF67C b         loc_57BC08
```

The EBOOT inline hook preserves marker `0x1A` and also handles marker `0x0D`.
However, the runtime log still reaches the generic `dani_select` packed-data
loader, which is consistent with the original compare still being live in the
tested process.

## Fallback Patch Shape

`patches.c` now has a live-memory fallback for Kimidori only:

- require the exact Proc_Main surrounding words at `0x0056650`,
  `0x0056668`, `0x0056670`, and `0x0056674`;
- require the branch at `0x0056670` to still target `0x00566E8`;
- if `0x005666C` is still the original `cmpwi cr7,r0,0x1A`, rewrite it to
  `cmpwi cr7,r0,0x0D`;
- if the EBOOT inline hook is present, `0x005666C` is no longer the original
  compare and the fallback does nothing.

This fallback does not touch resource maps, renderer null checks, or
`GameDojoSelect+0xA4/+0xA8` ownership slots.
