#!/usr/bin/env python3
"""Host-side check for core/scene_track.c's resolve chain.

Replays, against real EBOOT images, exactly what the runtime does:

    "N4game18SequenceControllerE"  (unique string, read-only segments)
      -> typeinfo   = &name_ref - 4        (unique word, writable segments)
      -> vtable     = &ti_ref - 4          (unique word, writable segments)
      -> push slot  = vtable + 8 + 4*2

Each step must be a single hit or the runtime refuses to patch anything. Run:

    python3 tools/check_scene_track_resolve.py "../original elf"/*.elf
"""
import struct
import sys

NAME = b"N4game18SequenceControllerE\x00"
PUSH_SLOT = 2


def segments(data):
    """(vaddr, filesz, file_off, writable) for every PT_LOAD."""
    phoff = struct.unpack_from(">Q", data, 32)[0]
    phentsize, phnum = struct.unpack_from(">HH", data, 54)
    out = []
    for i in range(phnum):
        o = phoff + i * phentsize
        p_type, p_flags = struct.unpack_from(">II", data, o)
        p_off, p_vaddr = struct.unpack_from(">QQ", data, o + 8)
        p_filesz = struct.unpack_from(">Q", data, o + 32)[0]
        if p_type == 1 and p_vaddr and p_filesz >= 16:
            out.append((p_vaddr, p_filesz, p_off, bool(p_flags & 2)))
    return out


def find_str(data, segs, needle):
    hits = []
    for va, sz, off, w in segs:
        if w:
            continue
        blob = data[off:off + sz]
        i = blob.find(needle)
        while i >= 0:
            hits.append(va + i)
            i = blob.find(needle, i + 1)
    return hits


def find_word(data, segs, value):
    want = struct.pack(">I", value)
    hits = []
    for va, sz, off, w in segs:
        if not w:
            continue
        blob = data[off:off + sz]
        i = blob.find(want)
        while i >= 0:
            if i % 4 == 0:
                hits.append(va + i)
            i = blob.find(want, i + 1)
    return hits


def read32(data, segs, va):
    for start, sz, off, _ in segs:
        if start <= va < start + sz - 3:
            return struct.unpack_from(">I", data, off + (va - start))[0]
    return None


def resolve(path):
    data = open(path, "rb").read()
    segs = segments(data)

    names = find_str(data, segs, NAME)
    if len(names) != 1:
        return None, "typeinfo name hits=%d (no RTTI / not unique)" % len(names)

    refs = find_word(data, segs, names[0])
    if len(refs) != 1:
        return None, "typeinfo object hits=%d" % len(refs)
    ti = refs[0] - 4

    vrefs = find_word(data, segs, ti)
    if len(vrefs) != 1:
        return None, "vtable hits=%d" % len(vrefs)
    vtable = vrefs[0] - 4

    top = read32(data, segs, vtable)
    if top != 0:
        return None, "offset_to_top=%s, not a primary vtable" % top

    slot = vtable + 8 + 4 * PUSH_SLOT
    opd = read32(data, segs, slot)
    if opd is None or opd & 3:
        return None, "push slot holds %s" % opd
    code = read32(data, segs, opd)
    if code is None or code & 3:
        return None, "descriptor code %s" % code
    return (ti, vtable, slot, opd, code), None


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    failures = 0
    for path in argv[1:]:
        got, err = resolve(path)
        name = path.rsplit("/", 1)[-1]
        if err:
            print("%-24s -- %s" % (name, err))
            continue
        ti, vtable, slot, opd, code = got
        print("%-24s ti=%08x vtable=%08x slot=%08x opd=%08x push=%08x"
              % (name, ti, vtable, slot, opd, code))
    return failures


if __name__ == "__main__":
    sys.exit(main(sys.argv))
