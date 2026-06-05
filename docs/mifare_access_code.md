# MIFARE Banapassport Access Codes

Older Taiko arcade builds accept Banapassport cards through the MIFARE Classic
path. The game does not read a literal 20 digit access code from the card.
Instead, it validates an encrypted `NBGIC` block, extracts a profile index and a
32-bit card id, then derives the 20 digit access code locally.

To make a virtual MIFARE card match a QR/manual access code, generate or accept
only access codes that are encodable by this algorithm.

## Data Tables

The game stores eight `NBGIC` profiles. Do not hardcode build-specific VAs; find
the table bundle by content.

Profile key records:

- Eight records tagged `NBGIC0` through `NBGIC7`.
- Record stride: `0x88`.
- Per-profile Blowfish key bytes: record offset `+0x08`, length `0x38`.

Access-code profile records:

- Eight records.
- Record stride: `0x48`.
- Offset `+0x00`: big-endian `profile_id`.
- Offset `+0x04`: three ASCII access-code prefix bytes.
- Offset `+0x08`: 56-byte bit permutation.
- Offset `+0x40`: big-endian 64-bit add constant.

Known prefixes by profile index:

```text
0 -> 300
1 -> 302
2 -> 303
3 -> 304
4 -> 305
5 -> 306
6 -> 307
7 -> 308
```

Blowfish seed tables:

- S boxes: standard Blowfish S-box seed beginning with `d1310ba6 98dfb5ac`.
- P array: standard Blowfish P-array seed beginning with `243f6a88 85a308d3`.

## Generate A 20 Digit Access Code

Inputs:

- `profile`: integer `0..7`
- `card_id`: unsigned 32-bit integer
- `rec`: access-code profile record for `profile`

Helpers:

```text
be32(x)       = read x as big-endian u32
be64(x)       = read x as big-endian u64
bswap32(x)    = byte-swap u32
xor_bytes(x)  = xor of the four bytes of x in big-endian order
set_bits(buf, bit_offset, width, value) writes MSB-first into a 56-bit buffer
```

Algorithm:

```text
profile_id = be32(rec + 0x00)
prefix     = ascii(rec + 0x04, 3)
perm       = rec + 0x08
add_const  = be64(rec + 0x40)

x = xor_bytes(profile_id) xor xor_bytes(card_id)
check2 = (x - 3 * floor(x / 11)) & 3

packed56 = 0
set_bits(packed56,  0,  2, check2)
set_bits(packed56,  2, 10, ((profile_id >> 2) & 0xff) | ((profile_id & 3) << 8))
set_bits(packed56, 12,  4, 0)
set_bits(packed56, 16, 32, bswap32(card_id))
set_bits(packed56, 48,  8, x)

permuted56 = 0
for src_bit in 0..55:
    if packed56[src_bit] == 1:
        dst_bit = perm[src_bit] % 56
        permuted56[dst_bit] = 1

decimal_value = (u56_to_integer(permuted56) + add_const) mod 2^64
access_code = prefix + decimal_value formatted as zero-padded 17 decimal digits
```

The result is exactly 20 ASCII decimal digits.

## Validate / Invert A 20 Digit Access Code

To check whether a user-supplied code is MIFARE-encodable:

1. Match the first three digits against the profile prefixes.
2. Parse the remaining 17 digits as `decimal_value`.
3. Compute `permuted56 = (decimal_value - add_const) mod 2^64`, then keep the
   low 56 bits.
4. Invert the permutation: for every source bit, read `permuted56[perm[src] %
   56]` and write it to `packed56[src]`.
5. Extract and verify the fields:
   - bits `0..1` equal `(xor - 3 * floor(xor / 11)) & 3`
   - bits `2..11` equal `((profile_id >> 2) & 0xff) | ((profile_id & 3) << 8)`
   - bits `12..15` are zero
   - bits `16..47` are `bswap32(card_id)`
   - bits `48..55` equal `xor_bytes(profile_id) xor xor_bytes(card_id)`

If any check fails, a stock game cannot derive that access code from a pure
MIFARE card.

## Build MIFARE Block 1

After inverting the 20 digit code to `(profile, card_id)`, build block 1:

```text
plain[0..3] = bswap32(card_id) as big-endian bytes
plain[4]    = 0
plain[5]    = 0
plain[6]    = 0
plain[7]    = plain[0] xor plain[1] xor ... xor plain[6]
```

Encrypt `plain` with Blowfish ECB using the profile key:

1. Initialize P and S from the standard Blowfish seed tables in the game.
2. XOR the P array with the 0x38-byte profile key, repeated as needed.
3. Run the normal Blowfish key schedule.
4. Encrypt the 8-byte payload. The game's implementation byte-swaps both input
   words before encryption and byte-swaps both output words after encryption.

Final block:

```text
block1[0]    = 0x00
block1[1]    = 0x02
block1[2..6] = "NBGIC"
block1[7]    = '0' + profile
block1[8..15]= encrypted payload
```

The MIFARE authentication key observed in Taiko is:

```text
60 90 D0 06 32 F5
```

## Test Vectors

These vectors are useful for checking access-code packing and inversion:

```text
profile=0, card_id=0x12345678 -> 30028550227180294100
profile=3, card_id=0xDEADBEEF -> 30458881161934160218
```

The arbitrary code `12345678901234567890` is not MIFARE-encodable.
