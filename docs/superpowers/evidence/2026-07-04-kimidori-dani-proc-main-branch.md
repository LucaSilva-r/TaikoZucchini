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
