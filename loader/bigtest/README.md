# bigload host tests

**HARDWARE-UNVERIFIED.** Nothing here has run on a device. The emulator cannot
run RetailOS, so the RetailOS-side loader (chainloader.c, hb_fs_read_scatter,
hb_os_alloc staging) has only host tests: the planner (`../bigload.c`) against a
mock heap/file, plus a cross-compile + `mkrelocapp.py` link check.

Run: `./run.sh` (builds `build/bigtest` with ASan+UBSan, `-Wall -Wextra -Werror`).

## What is covered
Exact chunk list/order, 128 KiB chunks with remainder, descriptor outside the
game region, rejection (kept allocated) of blocks in/straddling
[0x0b000000,0x0ba00000), below 0x08800000, unaligned, 32-bit overflow, past
DRAM end; NULL mid-way; allocator that only returns bad blocks (bounded by
24 leaks / 128 attempts); short read, size mismatch, bad magic/abi/header,
bad CRC, size 0 / <32 / not multiple of 4 / > region; a host copy of
game.c's `valid_header` + chunk checks accepts every success case.

## Limits and failure behaviour
* Staging is bounded by the **free RetailOS heap** (largest block, ~2.6 MiB
  total per docs/research/ram-map.md), NOT by the kernel's 1 MiB asset arena.
  Games needing more than the free heap cannot be staged this way.
* Max game size is also limited to 64 chunks x 128 KiB = 8 MiB (kernel limit
  of 64 chunks); sizes between 8 and 10 MiB are rejected (too-many-chunks).
* Any failure (heap exhausted, leak/attempt cap, short read, header or CRC
  error) frees the legit chunks, the descriptor and the tracked rejected
  blocks, then the chainloader writes 0/0 to 0x22009014/0x22009018 and boots
  with the embedded payload as before. One exception: a rejected block beyond
  the leak cap is left allocated (never freed).
* Rejected blocks are kept allocated during retries (so the allocator can't
  return them again) and freed only at the end of a failed attempt.
* Every launch writes /Apps/Data/NanoOS/loader.txt (heap before/after, reason,
  chunks, leaks, bytes, descriptor address); write failure is ignored.
* The CRC is verified in the loader before handoff; the kernel verifies again.
* The 32-byte-only image cannot pass the kernel's header check (no valid entry);
  the planner mirrors that check, so such a file is rejected as bad-header.
