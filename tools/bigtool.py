#!/usr/bin/env python3
"""Minimal BIGF reader: list or extract entries.

BIG format (Westwood/EA):
  'BIGF'                  4 bytes
  archive size            4 bytes, little-endian
  entry count             4 bytes, big-endian
  first entry offset      4 bytes, big-endian
  then per entry: offset (4, BE), size (4, BE), NUL-terminated name

Entry payloads are stored exactly as a loose file would be on disk (RefPack-compressed .map files
included), so extracting the raw bytes yields a file the engine loads the same way.
"""
import struct
import sys


def entries(path):
    with open(path, "rb") as f:
        data = f.read(16)
        if data[:4] != b"BIGF":
            raise SystemExit(f"not a BIGF archive: {path}")
        count = struct.unpack(">I", data[8:12])[0]
        f.seek(16)
        out = []
        for _ in range(count):
            off, size = struct.unpack(">II", f.read(8))
            name = b""
            while True:
                c = f.read(1)
                if c in (b"\x00", b""):
                    break
                name += c
            out.append((name.decode("latin1"), off, size))
        return out


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: bigtool.py <archive> list [substr] | extract <name> <dest>")
    archive, mode = sys.argv[1], sys.argv[2]
    ents = entries(archive)
    if mode == "list":
        needle = sys.argv[3].lower() if len(sys.argv) > 3 else ""
        for name, off, size in ents:
            if needle in name.lower():
                print(f"{size:>10}  {name}")
        return
    if mode == "extract":
        name, dest = sys.argv[3], sys.argv[4]
        for n, off, size in ents:
            if n.lower() == name.lower():
                with open(archive, "rb") as f:
                    f.seek(off)
                    blob = f.read(size)
                with open(dest, "wb") as f:
                    f.write(blob)
                print(f"extracted {size} bytes -> {dest}")
                return
        raise SystemExit(f"no such entry: {name}")
    raise SystemExit(f"unknown mode {mode}")


main()
