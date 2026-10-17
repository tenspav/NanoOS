# RetailOS MBX initialization reverse engineering

Investigation: 2026-10-16. Local files only; no iPod access or modification.

RetailOS supplies TA buffer pointers, clipping bounds and scene configuration
before `+0x800`, then a 20-pair ISP/render configuration ending in `+0x680 = 1`.
NanoOS currently supplies neither the TA buffers nor most of that render list.
The emulator does not consume these buffers, so successful emulated rendering
does not validate hardware initialization.

NanoOS comparisons below refer to the committed `nanoos/src/mbx.c` at
`HEAD` when this investigation began. During the investigation, independent
uncommitted edits to `mbx.c`/`mbx.h` appeared that copy the traced PB pointers
and switch the render kick to `808`. Those edits are outside this report's
changes; they do not establish owned buffer initialization or the ordinary
RetailOS `6ec`/pair-list/`680` sequence documented below.

## Evidence and limits

- Dump: `dumps/snap6/ram-full.bin`, 64 MiB, mapped at CPU PA `0x08000000`.
  SHA-256: `a7eb8e4ebbc237c1cc4c89e32fa2671a4e9eb883aaa6d1e544eb9a07d29236b2`.
  All code addresses below are Thumb instruction addresses, without bit 0.
- Disassembled the dump with Python Capstone 5.0.7. File offset is
  `address - 0x08000000`. Important entry points: `0x080dde00` (build setup
  lists), `0x0812b600` (driver init), `0x0812c134` (scene setup),
  `0x0813645a` (render scheduler), `0x083f1a88` (apply register pairs),
  `0x08195e50` (alternate/partial render path), `0x084850e4` (MMU roots).
- Concrete runtime values come from the existing
  [gltest frame trace](../docs/research/mbx-gltest-frame.txt), captured from
  RetailOS running in the emulator using snap6. Its header explicitly says
  **frame 11**, not the first frame after boot. Lines 498–504 prepare the
  following scene; lines 464–483 configure the render of the scene at line 9.
  They establish a consistent frame configuration, not one contiguous
  first-boot capture. No new execution trace was collected for this report.
- A RAM snapshot contains code and current RAM, not historical MMIO writes.
  Consequently the literal first boot render's complete values/order cannot
  be certified from this input. Below is the statically verified initialization
  path plus a concrete observed pre-render setup. Pointer values are allocations
  from that trace, not universal constants to copy into NanoOS.
- Existing `docs/research/gpu-deep-dive.md` helps locate code, but its detailed
  cache-bit, depth-bit and tile-size interpretations are not treated as facts.
  This report distinguishes observed words from inferred register roles.

## Address discovery

Core base is `0x3b000000`; FIFO0 is `0x3b800000`; FIFO2/segment commit is
`0x3bc00000`. The dump constructs these as ARM immediates rather than always
storing literal base words: `0x0812b6f6`, `0x0812b6fe`, `0x0812b708` add these
constants to the platform mapping bias. Core window size is `0x4000`, FIFO
window size `0x200000`. FIFO1 `0x3ba00000` is loaded from `0x0812b958`.
Aligned literal matches also occur in mapping records around `0x08fd1e40`
and live driver records around `0x09984990`; raw unaligned byte matches
throughout the dump include unrelated data and are not reliable xrefs.

`0x081805dc` streams words to the fixed FIFO pointer, not successive MMIO
addresses. `0x08180b3a` writes the segment commit word (observed value 1) to
FIFO2. End of scene is FIFO0 word `0xf0000000`.

## Global initialization, before scenes

All offsets in the tables are relative to `0x3b000000`.

| Register | Value/action | Evidence and qualification |
|---|---|---|
| `+0x080` | `0x111`, delay, `0`, delay; alternate branch reverses these | `0x0812b864–0x0812b8ca`. Reset loop, not just one unconditional pulse. Branch depends on driver object `+4`; delays receive `0x1f4` at `0x083f4a2a`. Do not infer time units here. |
| `+0x134` | `0xffff` during reset loop | Store at `0x0812b8c2`; loop counter starts at `0x10` and uses unsigned decrement/branch. |
| `+0x85c` | `0x80` | Direct store at `0x0812b8d2`. Scene setup subsequently changes this. |
| `+0x130` | enable mask `0x8775` | Init at `0x0812b90e` calls `0x080c1c50`, which ORs/clears the mask according to its third argument; observed enabled result is `0x8775`. |
| `+0x1000..+0x101c` | `0x0995c000 + n*0x1000`, `n=0..7`, for snap6 | `0x084850fc–0x08485116` reads table object `+0x20+4*n`, clears CPU alias bit 31 and writes each root. Also seeded in emulator reset. These are physical pointers. |
| `+0x1020` | clear bit 0, poll; set bit 0, poll | `0x08485118–0x08485142` calls `0x08131390` for masked updates and `0x08484e94`/`0x08484df4` for waits. This is not evidence that RetailOS simply writes NanoOS's `0x101`. |

NanoOS already fills eight page tables and enables translation, but assumes
PTE physical addresses with no flags. The emulator masks PTE page addresses
and does not validate all hardware descriptor attributes. The above evidence
does not settle valid hardware PTE flags or cold-start clock/power sequencing.

## Scene start: concrete missing TA configuration

Observed next-scene setup at trace lines 498–504, before the next `+0x800=1`:

| Order | Register / full address | Observed value | Interpretation / NanoOS replacement |
|---|---|---|---|
| 1 | `+0x85c` / `0x3b00085c` | `0x00e068e1` | Scene configuration; preserve selected live bits, see below. |
| 2 | `+0x840` / `0x3b000840` | `0x002f0300` | TA buffer pointer from buffer object `+0x3c`; allocate owned GPU memory. |
| 3 | `+0x844` / `0x3b000844` | `0x002f1304` | Selected scene buffer pointer; also becomes ISP `+0x608`. Its exact start/end semantics remain unproven. |
| 4 | `+0x84c` / `0x3b00084c` | `0x00f00000` | Width `240 << 16` in this path. |
| 5 | `+0x850` / `0x3b000850` | `0x01b00000` | Height `432 << 16` in this path. |
| 6 | `+0x854` / `0x3b000854` | `0x3727c5ac` | Float approximately `1e-5`; exact function is not established. |
| 7 | `+0x858` / `0x3b000858` | `0x3727c5ac` | Same constant. |
| 8 | `+0x800` / `0x3b000800` | `1` | Scene begin, direct store `0x0812c394`. |

The list builder `0x080dde5c–0x080dde9a` creates the six pairs
`840,844,84c,850,854,858` at graphics context `gc+4`. It also puts an `85c`
pair at `gc+0x34`, but the ordinary begin path applies only **six** pairs
(`0x0812c37e–0x0812c388`) after separately updating `85c`.
The update at `0x0812c348–0x0812c35c` is exactly:

```c
CORE(0x85c) = (desired & ~0x388u) | (CORE(0x85c) & 0x388u) | 0x80u;
```

The initial desired word is constructed at `0x080dde28–0x080dde48`:

```c
nx = buffer_object[0x0c/4];
ny = buffer_object[0x10/4];
desired = 0x000400e1u |
          (((nx - 1) << 19) & 0x07f00000u) | ((ny - 1) << 10);
```

Later context state adjusts this (notably bits `0x40000`/`0x80000` at
`0x0812c220–0x0812c280`). Thus `0x00e068e1` is a useful observed word,
not a decoded hardware cache-control specification.

## Render list: concrete values and a trigger-order correction

The ordinary scheduler writes `+0x6ec` at **`0x081365c8`, before applying
the pairs** at `0x0813666e`. In the trace `+0x6ec = 0` is line 463, then
the following ordered list appears at lines 464–483. The written value is
selected from the scene object's indexed words (`0x081365bc–0x081365c8`);
there is no evidence here that it is a job-array address.

| Order | Register | Observed value | Role / confidence |
|---|---|---|---|
| 1 | `+0x608` | `0x002f1304` | Same selected scene buffer pointer as TA `+0x844`; likely ISP region/parameter input. |
| 2 | `+0x610` | `0x00030000` | Opaque ISP configuration. Builder default is `0x00030001` at literal `0x080ddfdc`; runtime changes matter. |
| 3 | `+0x614` | `0x1e3ce508` | Constant from literal `0x080ddfe0`; exact format unknown. |
| 4 | `+0x618` | `0x1e3ce508` | Same constant. |
| 5 | `+0x61c` | `0x00020007` | Constant from literal `0x080ddfd0`; exact format unknown. |
| 6 | `+0x63c` | `0` | Scene setup supplies byte-reordered clear colour (`gc+0x330`), `0x0812c14e–0x0812c164`, `0x0812c2aa`. Not a proven depth register. |
| 7 | `+0x644` | `0` | Builder uses `0xc` when context `+0x15d0` is nonzero, otherwise 0. |
| 8 | `+0x648` | `1` | Opaque enable/configuration. |
| 9 | `+0x64c` | `0` | Opaque configuration. |
| 10 | `+0x650` | `0xe` | Output format in this trace; not all format mappings proved here. |
| 11 | `+0x654` | `0x00ef0000` | `(width-1)<<16`; NanoOS already writes. |
| 12 | `+0x658` | `0x01af0000` | `(height-1)<<16`; NanoOS already writes. |
| 13 | `+0x65c` | `0x00335180` | GPU VA of target; replace with `MBX_VA(target)`. |
| 14 | `+0x660` | `0xf0` | Byte pitch divided by four in builder `0x080ddf76–0x080ddf7a`. |
| 15 | `+0x674` | `0` | 0 without optional auxiliary buffer; alternative is its address (`0x080ddeb4–0x080ddeda`). |
| 16 | `+0x6d0` | `1` | Opaque configuration. |
| 17 | `+0x6dc` | `0x3f800001` | Scene modifies context word with OR 1 (`0x0812c2d6–0x0812c2e6`); builder initially uses 1. |
| 18 | `+0x860` | `0x001a001d` | `(ny-1)<<16 | (nx-1)`, built at `0x080dde3e`, stored at `0x080ddf98`. |
| 19 | `+0x60c` | `0x002f4000` | Buffer object's `+0x24`; likely buffer boundary/configuration pointer, exact limit semantics unproved. |
| 20 | `+0x680` | `1` | Last pair; strong candidate for actual ordinary ISP launch/commit. |

`0x083f1a88` is an ordinary CPU loop: load `(offset,value)` and
`str value,[core,offset]` at `0x083f1a92`. It does not queue an array for
the GPU to fetch. The list itself is constructed at `0x080ddedc–0x080ddfaa`
and patched at scene start (`0x0812c29e–0x0812c2e6`).

The emulator instead calls `mbxr_render()` on **any** `+0x6ec` write and
models completion with a timer. This disagrees with the observed write order;
its comment describing pairs-before-6ec is also contradicted by the dump.
NanoOS currently writes target registers then `+0x6ec=1`, matching that
emulator shortcut. For hardware, reproduce the observed selector-before-pairs
sequence with `+0x680=1` last; keep emulator compatibility as an explicit
separate path until its launch model is corrected.

The alternate/partial-render routine `0x08195e50` writes `+0x80c` from
region coordinates and `+0x808=1` (`0x08195ef0/0x08195ef6`), then polls
status bit 4 before modifying region entries and replaying the render list.
It is not evidence that every ordinary frame needs an extra `808` kick.

## What the parameter/region evidence actually establishes

The buffer object used by the builders supplies:

| Object field | Consumer |
|---|---|
| `+0x08` | Active buffer index. |
| `+0x0c`, `+0x10` | Grid counts used in `85c` and `860`. |
| `+0x24` | Render register `60c`. |
| `+0x30 + 4*index` | TA `844` and ISP `608`, both explicitly patched from the same field at `0x0812c2b2–0x0812c2d4`. |
| `+0x3c` | TA `840`. |
| `+0xac + 4*index` | CPU pointer to region entries in `0x08195e6a–0x08195e7a` and `0x08136610–0x08136634`. |

Region entries have **8-byte stride**: `0x0813662c–0x08136634` ORs
`0x40000000` into each first word, using `[pointer,index,lsl #3]`.
The partial path also modifies their first words with region flags
(`0x08195fb8–0x08195fc0`). This is evidence that owned, initialized region
memory is needed, not just writes of pointer registers.

The traced pointers differ by `0x1004` (`844-840`); `60c-844 = 0x2cfc`.
These differences alone do **not** prove a PB allocation size, free-list
format, inclusive/exclusive boundary, or that `840` is the entire allocation
base. The trace values were not found as a live matching buffer object in
the original snapshot (the gltest allocation occurred after resume).
An aligned `0x002f4000` match at `0x09974bbc` is merely part of a monotonic
page-address array, not proof of the scene object's location.

For 240x432, `860` numerically describes a 30x27 grid. This is consistent
with 8x16 pixel units, but neither the physical raster tile size nor the
full region allocation dimensions are established by this one value.
Do not size a region allocation solely as `30*27*8` without tracing the
object's count fields `+0x14,+0x18,+0x1c,+0x20` and their product in
`0x08136610–0x08136626`.

## Suggested NanoOS changes (proposal, not implemented)

1. In `mbx_init`, reserve persistent, suitably aligned DRAM for TA parameters,
   region records and any allocation metadata. Map all of it in NanoOS's GPU
   MMU; use GPU VAs for these register pointers and physical addresses for
   page-table roots. Do not reuse RetailOS VAs `0x002f....` with NanoOS's
   different address mapping. Add the reset/`85c=0x80` setup only with a
   deliberate handoff/reset policy; reset invalidates inherited setup.
   Keep IRQ enables 0 for a polling driver unless IRQ 53 is installed.
2. Add dimensions to scene configuration (e.g. `mbx_begin(w,h)` or a configured
   surface object). `mbx_begin()` currently knows neither dimension. Before
   `800=1`, acknowledge stale events, publish initialized buffer memory, apply
   the `85c` masked update, and write the six TA pairs in the order above.
   Use `w<<16,h<<16` for the observed normal-size clipping path and retain
   the two `0x3727c5ac` words. Other size/clip modes need their own analysis.
3. In `mbx_render`, after EOL and the bit-4 wait, select the scene with `6ec`
   **before** writing the complete 20-pair list; use the observed scalar
   defaults as a bring-up template, replace allocation-dependent pointers,
   target, dimensions, pitch and region grid, and write `680=1` last.
   `608` must refer to the same selected scene memory as `844`. Wait for
   `status & 0x44 == 0x44` before reusing buffers. Do not add `808` blindly.
4. Keep buffers alive through completion and apply correct CPU cache cleaning
   if data caching is enabled. Current NanoOS startup leaves D-cache off;
   `dsb` orders stores but would not replace cleaning a future cached buffer.
   Add FIFO occupancy checks using `12c[23:16]` and depth `0x7b`, including
   EOL/commit submission, rather than relying on the emulator's zero occupancy.
5. Treat overflow bit `0x20` as failure for this first implementation. The
   emulator's comments identify the RetailOS partial-render/overflow path;
   reporting successful completion there would hide exhausted TA memory.

This is enough to identify the missing register sets and correct launch
ordering, but **not yet a safe complete hardware buffer allocator**. The
remaining prerequisite is to follow the buffer object's allocator and CPU
initializers to recover alignment, allocation sizes, free-list seeds and
region words, then observe the first complete scene with those contents.
The software renderer (`nano7_mbxr.c:mbxr_render`) only reads target/size/pitch
and MMU tables from core registers; triangles and depth live in host arrays.
It cannot validate those missing hardware memory structures.

## Reproduce the static checks

From the repository root (requires the already available `capstone` module):

```python
from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB
from pathlib import Path
ram = Path('dumps/snap6/ram-full.bin').read_bytes()
md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
for address, size in [(0x080dde00, 0x1c6), (0x0812b600, 0x354),
                      (0x0812c134, 0x2aa), (0x0813645a, 0x226),
                      (0x08195e50, 0x216), (0x083f1a88, 0x12),
                      (0x084850e4, 0x6e)]:
    off = address - 0x08000000
    for ins in md.disasm(ram[off:off+size], address):
        print(f'{ins.address:08x} {ins.mnemonic:8} {ins.op_str}')
```

Literal pools must be read as little-endian data, not decoded as instructions.
The frame values above can be checked with
`rg -n 'CORE' docs/research/mbx-gltest-frame.txt`.
