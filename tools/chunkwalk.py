#!/usr/bin/env python3
"""Generic recursive chunk walker for NFSU2 .BIN/.BUN files.

Format: repeating (magic: u32 LE, size: u32 LE) headers. magic==0 is
padding (no recursion). magic with top nibble 0x8 contains nested chunks
covering [data_start, data_start+size).
"""
import struct
import sys


def walk(f, start, end, depth=0):
    f.seek(start)
    while f.tell() < end:
        pos = f.tell()
        hdr = f.read(8)
        if len(hdr) < 8:
            break
        magic, size = struct.unpack("<II", hdr)
        data_start = f.tell()
        print(f"{'  ' * depth}{pos:#010x} magic={magic:#010x} size={size:#x}")
        if magic != 0 and (magic >> 28) == 8:
            walk(f, data_start, data_start + size, depth + 1)
        f.seek(data_start + size)


def _selftest():
    import io

    # inner: one padding chunk (magic 0, size 4 -> skip 4 bytes)
    inner = struct.pack("<II", 0, 4) + b"\x00" * 4
    inner += struct.pack("<II", 0x39000, 2) + b"hi"
    outer = struct.pack("<II", 0x80001111, len(inner)) + inner
    buf = io.BytesIO(outer)
    seen = []
    orig_print = print
    import builtins

    builtins.print = lambda *a, **k: seen.append(" ".join(str(x) for x in a))
    try:
        walk(buf, 0, len(outer))
    finally:
        builtins.print = orig_print
    assert len(seen) == 3, seen
    assert "magic=0x80001111" in seen[0]
    assert "magic=0x00000000" in seen[1]
    assert "magic=0x00039000" in seen[2]
    print("selftest ok")


if __name__ == "__main__":
    if len(sys.argv) == 2 and sys.argv[1] == "--test":
        _selftest()
        sys.exit(0)
    if len(sys.argv) != 2:
        sys.exit(f"usage: {sys.argv[0]} <file.bin|.bun> | --test")
    with open(sys.argv[1], "rb") as f:
        f.seek(0, 2)
        walk(f, 0, f.tell())
