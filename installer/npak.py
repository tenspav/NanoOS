#!/usr/bin/env python3
"""NPAK v1 asset pack writer (vicenano.pak and the vcworld.pak runtime pack).

Layout (little endian), every offset relative to the pack start:
  header  : char magic[4] "NPAK", u32 version 1, u32 count, u32 data_start
  entries : count x 64 B { char name[32]; u32 offset, size, padded_size,
            crc32, sector (offset / 4096), 0, 0, 0 }
  data    : each entry at a 4096-byte aligned offset, zero padded
The pack ends at the last entry's offset + padded_size. NanoOS's startup.s
relies on that to move a pack appended to kernel.bin (nanoos/src/asset.h).

CLI: npak.py out.pak file [file...]  (entry name = file name)
"""
import hashlib
import struct
import sys
import zlib
from pathlib import Path

ALIGN = 4096


def write_npak(path: Path, entries: list[tuple[str, bytes, str]]) -> list[dict]:
    """entries: (name, data, kind); returns the manifest rows."""
    hdr_size = 16 + 64 * len(entries)
    data_start = (hdr_size + ALIGN - 1) // ALIGN * ALIGN
    header = bytearray(data_start)
    struct.pack_into("<4sIII", header, 0, b"NPAK", 1, len(entries), data_start)
    payload = bytearray()
    cur = data_start
    rows = []
    for i, (name, raw, kind) in enumerate(entries):
        padded = (len(raw) + ALIGN - 1) // ALIGN * ALIGN
        crc = zlib.crc32(raw) & 0xFFFFFFFF
        struct.pack_into("<32sIIIIIIII", header, 16 + i * 64, name.encode("ascii")[:31],
                         cur, len(raw), padded, crc, cur // ALIGN, 0, 0, 0)
        payload += raw + b"\x00" * (padded - len(raw))
        rows.append({
            "name": name,
            "kind": kind,
            "offset": cur,
            "sector_index": cur // ALIGN,
            "size": len(raw),
            "aligned_size": padded,
            "crc32": f"0x{crc:08x}",
            "sha256": hashlib.sha256(raw).hexdigest(),
        })
        cur += padded
    Path(path).write_bytes(bytes(header + payload))
    return rows


if __name__ == "__main__":
    files = [Path(f) for f in sys.argv[2:]]
    rows = write_npak(Path(sys.argv[1]), [(f.name, f.read_bytes(), "data") for f in files])
    print(f"NPAK {sys.argv[1]}: {len(rows)} entries, {Path(sys.argv[1]).stat().st_size} bytes")
