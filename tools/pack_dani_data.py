#!/usr/bin/env python3
"""Pack the repaired Dani Dojo data XMLs into embeddable .zdf blobs.

The plugin embeds these files so a cabinet can repair its own game data with
no network and no extra assets beside zucchini.sprx. Raw XML would add ~325 KB
to the module, so each file is stored as independently-deflated blocks that the
runtime inflates one block at a time into a small fixed buffer (see
storage/zdf_blob.c) instead of holding a whole file in the PRX heap.

Container layout, all integers big-endian (PPU native):

    magic   "ZDF1"          4
    block   raw block size  4
    total   raw file size   4
    count   block count     4
    index   count * { u32 comp_len, u32 raw_len }
    payload count deflate streams (raw deflate, no zlib header)

Regenerate after changing any XML under assets/dani/:

    python3 tools/pack_dani_data.py
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path

BLOCK = 32 * 1024
MAGIC = b"ZDF1"


def pack(raw: bytes) -> bytes:
    blocks = [raw[i:i + BLOCK] for i in range(0, len(raw), BLOCK)] or [b""]
    comp = [zlib.compress(b, 9)[2:-4] for b in blocks]  # strip zlib hdr/adler

    out = bytearray(struct.pack(">4sIII", MAGIC, BLOCK, len(raw), len(blocks)))
    for c, b in zip(comp, blocks):
        out += struct.pack(">II", len(c), len(b))
    for c in comp:
        out += c
    return bytes(out)


def main(argv: list[str]) -> int:
    root = Path(argv[1]) if len(argv) > 1 else Path(__file__).resolve().parent.parent
    data_dir = root / "assets" / "dani"
    xmls = sorted(data_dir.glob("*.xml"))
    if not xmls:
        print(f"no XML files in {data_dir}", file=sys.stderr)
        return 1

    for xml in xmls:
        raw = xml.read_bytes()
        blob = pack(raw)
        out = xml.with_suffix(".zdf")
        out.write_bytes(blob)
        print(f"{xml.name}: {len(raw)} -> {len(blob)} bytes "
              f"({len(raw) / len(blob):.1f}x) -> {out.name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
