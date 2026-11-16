#!/usr/bin/env python3
"""Pack canonical NanoOS ABI1 game image; no install/deploy side effects.

This checkout lacks NanoOS/game/mkgame.py. Preserve the exact gameabi.h header
and game.c loader protocol instead: 32 bytes, 4-byte image alignment, CRC-32 of
[32,size). The input is the linked app.bin, including its assembly header.
"""
import pathlib
import struct
import sys
import zlib


def pack(raw):
    data = bytearray(raw)
    if len(data) < 32:
        raise ValueError("truncated ngame_hdr")
    data.extend(b"\0" * (-len(data) % 4))
    magic, abi, load, _, bss_end, entry, _, reserved = struct.unpack_from("<8I", data)
    if (magic, abi, load, reserved) != (0x314D474E, 1, 0x0B000000, 0):
        raise ValueError("unexpected canonical ABI1 header")
    if not load + 32 <= (entry & ~1) < load + len(data):
        raise ValueError("entry outside image")
    if not load + len(data) <= bss_end <= 0x0BA00000:
        raise ValueError("invalid BSS extent")
    struct.pack_into("<I", data, 12, len(data))
    struct.pack_into("<I", data, 24, zlib.crc32(data[32:]) & 0xFFFFFFFF)
    return data


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: mkgame.py app.bin game.bin")
    image = pack(pathlib.Path(sys.argv[1]).read_bytes())
    pathlib.Path(sys.argv[2]).write_bytes(image)
    hdr = struct.unpack_from("<8I", image)
    print(f"NGM1 ABI{hdr[1]} load={hdr[2]:#010x} size={hdr[3]} "
          f"bss_end={hdr[4]:#010x} entry={hdr[5]:#010x} crc={hdr[6]:08x}")
