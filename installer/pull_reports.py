#!/usr/bin/env python3
"""pull_reports.py -- read the NanoOS GPU report mailbox (src/report.h, RAM
0x0ae10000) straight out of iPod DRAM over the SCSI peek channel and save
R<id>.txt on the host. R80 is the ViceNano M8 GPU diag.

Why: NanoOS cannot write the iPod's FAT disk (src/nand.c is raw reads only,
with no FTL, and writes are refused), and NanoOSLoader's export needs the
NanoOS app opened plus extra reboots. The firmware's custom SCSI command
(c6 96 02 <addr>, the same one NanoApps' `start peek` / `start trace` use)
reads RAM in ANY mode, the Home screen included. Round 12: 0x0ae10000 is a
free block of a RetailOS allocator (dumps/snap2..6), so it survives the
NanoOS -> RetailOS reset only if nothing was allocated over it; NanoOS also
writes six small R80 mirrors elsewhere (report.h), this script checks the
peek channel first (a dead sg_raw reads as zeros), prints the raw header and
a per-mirror survival map, and saves R80 from whichever copy survived.

Read-only on the device: nothing is written to the iPod disk or RAM, so the
report also stays in RAM (the loader can still export it later).

Loop: ViceNano fails -> Home (NanoOS resets into RetailOS) -> plug the cable
-> python3 nanoos/installer/pull_reports.py

Transport: NanoApps' `start` Transport (iPod on this Linux box, or
--remote user@host / $REMOTE / the saved `start setup` config). The peek
runs `sudo sg_raw`, so sudo may ask for a password.
"""
import argparse
import importlib.machinery
import importlib.util
import struct
import sys
from pathlib import Path

INSTALLER_DIR = Path(__file__).resolve().parent
NANOOS_DIR = INSTALLER_DIR.parent
DEFAULT_NANOAPPS = NANOOS_DIR.parent.parent / "NanoApps"
DEFAULT_OUT = NANOOS_DIR / "reports"

# src/report.h
RPT_BASE = 0x0AE10000
RPT_SLOTS = 5
RPT_DATA = 0x1000
RPT_SLOT_SIZE = 0x30000
RPT_MAGIC = (0x5258424D, 0x00315450)
HDR_WORDS = 3 + 3 * RPT_SLOTS          # magic[2], slot_size, id/len/sum[5]
HDR_FMT = "<%dI" % (HDR_WORDS + 1)     # + hdr_sum
PEEK = 0x200
# report.h round 12: small R80 copies outside 0x0ae10000's allocator block
RPT_MIR_SIZE = 0x4000
RPT_M8_SLOT = 4
RPT_MIRRORS = (0x0AF60000, 0x0ACB0000, 0x0A0D0000, 0x0A300000, 0x0BA00000, 0x0BE60000)
# Peek channel check: RetailOS kernel text, never zero once RetailOS runs.
# A failed sg_raw (sudo, wrong device) reads back as zeros, which used to
# look exactly like "report lost".
CHANNEL_CHECK = 0x08000000


def fnv1a(data):
    s = 0x811C9DC5
    for b in data:
        s = ((s ^ b) * 16777619) & 0xFFFFFFFF
    return s


def parse_header(raw):
    """-> dict, or (None, why)."""
    w = struct.unpack_from(HDR_FMT, raw, 0)
    if (w[0], w[1]) != RPT_MAGIC:
        return None, "no report magic (%08x %08x): nothing written since the last export/clear, " \
                     "or RAM was lost (power-off / long reset)" % (w[0], w[1])
    if w[2] != RPT_SLOT_SIZE:
        return None, "slot_size %#x != %#x (report.h mismatch)" % (w[2], RPT_SLOT_SIZE)
    if w[HDR_WORDS] != fnv1a(raw[:HDR_WORDS * 4]):
        return None, "header checksum mismatch"
    n = RPT_SLOTS
    return {"id": w[3:3 + n], "len": w[3 + n:3 + 2 * n], "sum": w[3 + 2 * n:3 + 3 * n]}, None


def mirror_status(peek, base):
    """-> (text or None, why). A mirror is one R80 copy (report.h)."""
    raw = peek(base, PEEK)
    w = struct.unpack_from(HDR_FMT, raw, 0)
    if (w[0], w[1]) != RPT_MAGIC:
        return None, "gone (%08x %08x %08x %08x)" % w[:4]
    if w[2] != RPT_MIR_SIZE or w[HDR_WORDS] != fnv1a(raw[:HDR_WORDS * 4]):
        return None, "header damaged"
    n, want = w[3 + RPT_SLOTS + RPT_M8_SLOT], w[3 + 2 * RPT_SLOTS + RPT_M8_SLOT]
    if not 0 < n <= RPT_MIR_SIZE:
        return None, "bad len %d" % n
    data = peek(base + RPT_DATA, (n + PEEK - 1) // PEEK * PEEK)[:n]
    if fnv1a(data) != want:
        return None, "text damaged (sum)"
    return data, "OK %d bytes" % n


def read_slots(peek, hdr, only=None):
    """Yield (slot, id, text_bytes, sum_ok) for every filled slot."""
    for i in range(RPT_SLOTS):
        rid, n = hdr["id"][i], hdr["len"][i]
        if not rid or not n or (only and rid not in only):
            continue
        if n > RPT_SLOT_SIZE:
            print("  slot %d (R%d): len %d > slot size, skipped" % (i, rid, n))
            continue
        addr = RPT_BASE + RPT_DATA + i * RPT_SLOT_SIZE
        print("  slot %d -> R%d: %d bytes ..." % (i, rid, n), flush=True)
        data = peek(addr, (n + PEEK - 1) // PEEK * PEEK)[:n]
        yield i, rid, data, fnv1a(data) == hdr["sum"][i]


def save(out, rid, data, ok):
    """R<id>.txt; a different older one rotates to R<id>.prev.txt. A bad sum
    goes to R<id>.bad.txt and leaves the good files alone."""
    out.mkdir(parents=True, exist_ok=True)
    if not ok:
        p = out / ("R%d.bad.txt" % rid)
        p.write_bytes(data)
        return p, "CHECKSUM MISMATCH (RAM reused before the pull?), saved for inspection"
    p = out / ("R%d.txt" % rid)
    if p.exists():
        if p.read_bytes() == data:
            return p, "unchanged (same report as the last pull: no new ViceNano session?)"
        p.replace(out / ("R%d.prev.txt" % rid))
        p.write_bytes(data)
        return p, "new (previous -> R%d.prev.txt)" % rid
    p.write_bytes(data)
    return p, "new"


def load_transport(nanoapps, args):
    start = nanoapps / "start"
    if not start.is_file():
        sys.exit("NanoApps `start` not found at %s (use --nanoapps)" % start)
    loader = importlib.machinery.SourceFileLoader("nanoapps_start", str(start))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    mod = importlib.util.module_from_spec(spec)
    loader.exec_module(mod)
    return mod.detect_transport(args)


def self_test(out):
    """Offline check of the parser/sum/rotation against a fake RAM image."""
    ram = {}
    text = b"== VICENANO M8 GPU R80 ==\nfail 1 rsn 5\n== END R80 ==\n" * 30
    ids, lens, sums = [0] * 5, [0] * 5, [0] * 5
    ids[4], lens[4], sums[4] = 80, len(text), fnv1a(text)
    body = struct.pack("<%dI" % HDR_WORDS, *RPT_MAGIC, RPT_SLOT_SIZE, *ids, *lens, *sums)
    ram[RPT_BASE] = body + struct.pack("<I", fnv1a(body))
    ram[RPT_BASE + RPT_DATA + 4 * RPT_SLOT_SIZE] = text

    def peek(addr, n):
        return (ram.get(addr, b"") + b"\0" * n)[:n]

    hdr, why = parse_header(peek(RPT_BASE, PEEK))
    assert hdr, why
    got = list(read_slots(peek, hdr))
    assert len(got) == 1 and got[0][1] == 80 and got[0][2] == text and got[0][3]
    p, st = save(out, 80, text, True); assert st == "new", st
    p, st = save(out, 80, text, True); assert st.startswith("unchanged"), st
    p, st = save(out, 80, text + b"x", True); assert st.startswith("new (prev"), st
    assert (out / "R80.prev.txt").read_bytes() == text
    p, st = save(out, 80, b"junk", False); assert p.name == "R80.bad.txt"
    assert (out / "R80.txt").read_bytes() == text + b"x"
    bad = bytearray(ram[RPT_BASE]); bad[12] ^= 1
    assert parse_header(bytes(bad))[0] is None
    # round 12 mirror: same header, slot_size RPT_MIR_SIZE, text at +RPT_DATA
    ids, lens, sums = [0] * 5, [0] * 5, [0] * 5
    ids[4], lens[4], sums[4] = 80, len(text), fnv1a(text)
    body = struct.pack("<%dI" % HDR_WORDS, *RPT_MAGIC, RPT_MIR_SIZE, *ids, *lens, *sums)
    ram[RPT_MIRRORS[2]] = body + struct.pack("<I", fnv1a(body))
    ram[RPT_MIRRORS[2] + RPT_DATA] = text
    got, st = mirror_status(peek, RPT_MIRRORS[2])
    assert got == text, st
    assert mirror_status(peek, RPT_MIRRORS[0])[0] is None
    ram[RPT_MIRRORS[2] + RPT_DATA] = b"X" + text[1:]
    assert mirror_status(peek, RPT_MIRRORS[2])[1].startswith("text damaged")
    print("self-test OK (%s)" % out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help="host dir (default %(default)s)")
    ap.add_argument("--id", type=int, action="append", help="only these report ids (e.g. --id 80)")
    ap.add_argument("--nanoapps", type=Path, default=DEFAULT_NANOAPPS)
    ap.add_argument("--local", action="store_true", help="iPod on this Linux machine")
    ap.add_argument("--remote", metavar="USER@HOST", help="iPod on a Linux machine over SSH")
    ap.add_argument("--quiet", action="store_true", help="do not print R80 at the end")
    ap.add_argument("--self-test", metavar="DIR", type=Path, help="offline parser test, writes only DIR")
    args = ap.parse_args()
    if args.self_test:
        return self_test(args.self_test)

    tr = load_transport(args.nanoapps, args)
    print("==> peek channel check at %#010x (%s)" % (CHANNEL_CHECK, tr.desc), flush=True)
    chk = tr.peek(CHANNEL_CHECK, PEEK)
    print("    %s" % chk[:16].hex(" "))
    if not any(chk):
        sys.exit("PEEK CHANNEL DEAD: RetailOS kernel text read back as zeros. The SCSI peek "
                 "failed (sudo password? wrong device? iPod not attached?), so nothing below "
                 "would mean anything. Fix the transport and rerun; the report is untouched.")
    print("==> reading report header at %#010x" % RPT_BASE, flush=True)
    raw = tr.peek(RPT_BASE, PEEK)
    print("    raw " + " ".join("%08x" % x for x in struct.unpack_from("<8I", raw, 0)))
    hdr, why = parse_header(raw)
    # survival map: which copies made it through the reset + RetailOS boot
    print("==> mirrors (round 12 survival map):", flush=True)
    mirror_text = None
    for base in RPT_MIRRORS:
        text, st = mirror_status(tr.peek, base)
        print("    %#010x: %s" % (base, st), flush=True)
        if text and mirror_text is None:
            mirror_text = text
    saved = []
    if hdr:
        for slot, rid, data, ok in read_slots(tr.peek, hdr, args.id):
            p, st = save(args.out, rid, data, ok)
            print("  R%d -> %s: %s" % (rid, p, st))
            saved.append((rid, p, ok))
    else:
        print("  main mailbox: " + why)
    if mirror_text is not None and not any(rid == 80 and ok for rid, _, ok in saved) \
            and (not args.id or 80 in args.id):
        p, st = save(args.out, 80, mirror_text, True)
        print("  R80 (from mirror) -> %s: %s" % (p, st))
        saved.append((80, p, True))
    if not saved:
        sys.exit("no report in the main mailbox or any mirror" +
                 (" for ids %s" % args.id if args.id else ""))
    r80 = [p for rid, p, ok in saved if rid == 80 and ok]
    if r80 and not args.quiet:
        print("\n" + r80[0].read_text(errors="replace"))
    if not all(ok for _, _, ok in saved):
        sys.exit(1)


if __name__ == "__main__":
    main()
