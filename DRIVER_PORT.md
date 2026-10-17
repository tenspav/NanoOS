# NanoOS driver port — plan, progress, blockers

Goal: NanoOS runs bare-metal apps without RetailOS services. It boots in
NanoEmu (`run-in-nanoemu.sh`, also `--real` = NANO7_REALDEV), shows DevMode,
and every driver is exercised by a small test call with a DevMode read-out.

Emulator validation only. Nothing here writes NAND, flashes, or changes
persistent state on a device.

## Ground rules (from the existing kernel)

- MMU and D-cache off, IRQs masked (`cpsid aif`), no hardware divide, no
  libgcc. So: polled drivers, divisions only by constants, every wait has a
  USEC deadline plus a poll backstop (timer.h), and nothing blocks the main
  loop for long (input is sampled every pass, see sessions 76/77).
- Code and data must end below 0x08010000 (kernel.ld assert, real-iPod
  memory model). Today 14.5 KB, so the budget is fine.
- Peripherals are in the state RetailOS left them at the chainloader
  handoff. Drivers re-use that state where possible and do not reprogram
  clocks, PLLs or pads they don't own.
- Register maps come from the NanoEmu models (`emulator/machine/hw/arm/`).
  Those models were built from RetailOS reverse engineering, so they are the
  best spec we have. Where a model is thinner than the hardware, the driver
  says so.

## Test surface

DevMode gets a second page, **Drivers**: Vol+ (Right) opens it, Vol- (Left)
goes back to the stats page, and Home leaves DevMode as before. Opening the
page runs the one-shot probes (codec, NAND, USB, MBX, tone) and starts the
Bluetooth state machine. While the page is open, touch, buttons and BT are
polled every pass. Each driver has one status line:
`NAME STATUS details`. STATUS is `OK`, `--` (running), `ERR` or `STUB`.
`test_nanoemu.py` OCRs the page, sends a touch, Power and Play, and checks
each line, plus the emulator's exit summaries (I2S bytes/peak, MBX scenes,
BT HID reports).

## Drivers

| # | Driver | Files | Hardware (NanoEmu model) | Scope now |
|---|--------|-------|--------------------------|-----------|
| 1 | LCD push | lcd.c | LCD ctrl 0x38900000, bridge 0x38300000, DSI 0x3d800000 | done earlier; add a bounded `lcd_flush`, re-arm the controller window after consecutive timeouts, read-out |
| 2 | Touch | spi.c, touch.c | SPI port 2 0x3d200000 (Samsung SPI-style), ready GPIO 0x26 | polled query (16 B, `EA 01 00`), fetch (41 B, `EA 01 01`), raw→panel→landscape coordinates, down/move/up |
| 3 | Buttons | buttons.c, input.c | PMU I2C1 0x73 reg7 bit4 Home / bit5 Power, reg8 bit1 Play (active-low); GPIO 0x3cf000a4 bit0 Vol+, bit1 Vol- | expose levels for all 5 buttons, PMU temperature (reg 0x3c/0x3d, emulator formula) |
| 4 | Audio | spi.c, audio.c | codec on SPI0 0x3c300000 (`6c/6d msb lsb 00 data`), I2S0 0x3ca00000 TX FIFO +0x10 | codec read probe (reg 0x2f), time-paced CPU-fed 440 Hz tone, 0.5 s |
| 5 | NAND | nand.c | FMC 0x38a00000, PPN NAND, 2 CEs, 16 KB pages = 4×4 KB sectors | ID (`0x90`), parameter page (`0x92`), raw sector read (`0x07` + `0x7a`) into DRAM. **Write is a refusing stub** |
| 6 | USB | usb.c | DWC2 OTG 0x38400000, PHY 0x3c400000; VBUS = PMU reg5 bit6 | probe only (GSNPSID, VBUS). NanoEmu has no DWC2 model, see blockers |
| 7 | Bluetooth HID | bt.c | UART1 0x3db00000 (H4 HCI), fake peer "NanoPad" 01:44:41:50:4e:02 | HCI Reset → Create_Connection → L2CAP PSM 0x11 + 0x13 → HID input reports (`A1 btnLo btnHi X Y Z Rz`) |
| 8 | MBX GPU | mbx.c | core 0x3b000000 (ID +0xff0 = 0xb105f00d), TA FIFO 0x3b800000, status +0x12c / ack +0x134 | identify, begin scene (+0x800), end-of-list word to the TA FIFO, poll TA/render done (bit 4) |

Shared: `spi.c` (the touch and codec ports are the same Samsung SPI block
in the model: flush, length, START edge, TX bytes, done bit, RX pops) and
`fmt.c` (line formatting for the read-outs).

### Order of work (each step keeps `make all chainloader` green)

1. fmt.c + spi.c + the Drivers page skeleton (all lines `STUB`).
2. Buttons (input.c levels) + touch.
3. Audio (codec probe + tone).
4. NAND (ID, params, sector read).
5. MBX.
6. USB probe.
7. Bluetooth HID state machine.
8. LCD robustness.
9. test_nanoemu.py Drivers checks; run STOCK, SAFE, MAX and `--real`.

## Known limits by design

- No interrupts yet, so touch, BT and buttons are polled. The IRQ plumbing
  (VIC 0x38e00000, GPIO groups 0x39700080..) is the next kernel step;
  every driver keeps its "event pending" check in one function so it can
  move to an ISR.
- Audio is CPU-fed and paced by USEC. NanoEmu has no FIFO back-pressure; on
  the device the TX FIFO depth and status bits are unknown, so samples may
  be dropped there. DMA (engine 1 ch 2, DST 0x3ca00010) is the proper path
  and is documented by the model; it's next.
- Codec: read-only probe. NanoEmu's codec has no ID register, and writing
  unknown registers of the real codec is avoided.
- MBX: TA path only. The render-job path (+0x6ec) goes through the GPU
  MMU, whose page tables (roots +0x1000..) belong to RetailOS.

## Progress log

### 2026-10-16 — all 8 drivers in, Drivers page, emulator-verified

Done (files in `src/`):

| Driver | Status | What the Drivers page shows (NanoEmu STOCK) |
|--------|--------|------------------------------------------|
| LCD | robust push kept; `lcd_flush` bounded (200 ms); scan-out window re-armed after 3 pushes in a row with an expired wait (`rc`) | `LCD OK fr 551 to 0 hd 0 rc 0` |
| Touch | working: query/fetch, checksums, landscape coordinates, down/move/up | `TOUCH OK n 2 x 216 y 119 up` (tap at panel centre) |
| Buttons | working: Home, Power, Play, Vol+, Vol- levels + "seen" latch, PMU temperature | `BTN OK now ..... seen .PL.. 33C` (H P L + - = Home Power pLay Vol+ Vol-) |
| Audio | codec read probe + CPU-fed 440 Hz tone, 0.5 s | `AUDIO OK codec 2F:80 tone 22050 fr`; emulator exit: `peak 6000 / 32767` |
| NAND | ID, parameter page, raw 4 KB sector read by DMA (sentinel-checked) | `NAND OK PPN 16384B 128pg rd p0 blank` |
| USB | probe only (no emulator model) | `USB STUB id FFFFFFFF vbus 1 no core` |
| BT HID | working host stack against NanoEmu's pad: HCI, ACL, L2CAP signalling, HID interrupt reports | `BT OK hid up rep 9 btn 0000 x 0 y 0` |
| MBX | identify + empty scene through the TA FIFO, render-done polled | `MBX OK id B105F00D scene 1 in 674 us` |

Build changes:
- `-mno-unaligned-access` for the whole kernel. With the MMU off all memory
  is Strongly-ordered, and GCC had merged the touch report's byte loads into
  an unaligned `ldrh`. That raised an alignment fault into RetailOS's IRAM
  vector (pc 0x220000ac) and restarted the kernel. Any future byte-packed
  parsing would have hit the same thing.
- Size: the test overlays the chainloader package on Blocks.hbapp, which
  caps it at **20708 bytes** (`disk_patch.py` can't grow files). Drivers took
  the kernel from 14.5 KB to 24.4 KB. Fixed by building main, devmode and
  the new modules as Thumb-2 (-O2: -Os calls libgcc's `__aeabi_uidivmod`
  even for constant divisors), and DevMode now uses fmt.c instead of inlined
  copies (devmode_draw went from 3.3 KB to well under 1 KB). The kernel is
  now 17.4 KB and the package 19.6 KB, leaving about 1.1 KB of headroom.
  lcd/input/timer/font stay ARM -O2.
- `test_nanoemu.py` opens the Drivers page after the DevMode check, taps the
  panel centre, sends Power (`p`) and Play (`spc`), waits for the BT pad
  script, then OCRs and asserts every line.

Verification (NanoEmu, kernel 1c00c138, 17452 bytes; package 19576 bytes):
- `run-in-nanoemu.sh STOCK`, `SAFE`, `MAX`: PASS, Drivers page included.
  DevMode FPS 58-59, unchanged.
- `run-in-nanoemu.sh STOCK --real` (failing PMU I2C, bridge busy hold, slow
  uncached DRAM): PASS. DevMode 26.4 FPS (min 15). Every driver line OK;
  USB `vbus -` because the PMU read for it failed, which is expected under
  the modelled 40% NACK rate. BT got 11 reports.
- `apps/NanoOSLoader/test_nanoemu.py`: PASS on the 2nd try. Try 1 stopped
  before NanoOS ("RetailOS home screen never appeared"), the host-load
  flake already seen in session 78. `--missing`: PASS.
- Not tested on the device.

## Blockers / next steps

1. **USB (mass storage / file transfer): blocked on the emulator.** NanoEmu
   maps the DWC2 core as a plain register file (GSNPSID reads 0xFFFFFFFF,
   GRSTCTL always 0x80000000, GINTSTS never set). A device-mode stack (core
   soft reset, FIFO sizing, EP0 enumeration, bulk IN/OUT, then BOT + SCSI
   over a RAM disk or a vendor file-transfer class) needs a `nano7_usb.c`
   model first: GSNPSID 0x4F54xxxx, GHWCFG1-4, W1C GINTSTS, SETUP delivery
   by DMA on EP0, and a host side (usbredir or a QMP-fed script). The bootrom
   DFU flow (docs/research/bootrom.md:56-85) is the reference sequence.
2. **NAND FTL (logical blocks) and write.** The raw path works, but NanoEmu
   only serves disk data through RetailOS's FMSS sequencer path, keyed by
   RetailOS's own FTL tables. So a bare-metal read of a real page returns
   0xff unless something programmed it into the NAND store. Next: either
   teach the model's oracle to answer direct-path reads (an LBA from the
   metadata), or have NanoOS read RetailOS's sftl tables before the handoff
   tears them down. **Write stays refused** (`nand_write_sector` returns
   -1): the same kernel boots on the real iPod, and programming the flash
   RetailOS's FTL owns is a brick risk. If it's ever needed, it must be gated
   to NanoEmu at runtime and reviewed.
3. **BT: real pads.** Done: host-initiated connect to a known address, no
   security. Missing for real hardware: Inquiry with name/CoD filtering,
   SSP pairing (NanoEmu's peer supports numeric comparison and PIN) with
   link-key storage, and accepting the pad's own reconnect (Connection
   Request → Accept). After a reset the BT chip on the device may also need
   its RetailOS bring-up (power GPIO, baud, Broadcom patchram), none of
   which NanoEmu checks.
4. **Audio by DMA.** CPU feeding works in NanoEmu, which has no FIFO status.
   On the device, use engine 1 channel 2 (0x38200140: SRC, DST 0x3ca00010,
   NEXT 0, CTRL 0x84249000, COUNT, then 0x8b01; poll 0x38200004 bit 2), as
   RetailOS does.
5. **Interrupts.** Every driver is polled. The VIC (0x38e00000) and the GPIO
   groups (touch ready = GPIO 0x26, group 1 bit 30; volume = group 2; PMU
   event = GPIO 0x56) are next, so touch and BT stop costing poll time.
6. **MBX beyond the empty scene.** Draw calls need the render-job path
   (+0x6ec) with our own GPU page tables (+0x1000.. roots), plus clock and
   power gates on the device (NanoEmu checks none).
7. **Device safety of the Drivers page.** The page runs only when the user
   opens it (Vol+ in DevMode); normal boot and the launcher don't touch the
   new hardware. Everything it does is reads, a BT controller reset, an
   I2S tone and an empty GPU scene. None of it is persistent. Untested on
   the device: the worst expected outcome is a hang (a gated block not
   answering), which a reboot clears.
