# NanoOS Launcher v0.0.0 Spec

Goal: first bootable NanoOS kernel that shows a simple game-selection launcher and can return to the stock iPod OS.

## Boot

- Build as a raw ARM binary loaded by the existing chainloader at physical DRAM 0x08000000.
- Entry point at 0x08000000, machine mode (MMU off, caches flushed by chainloader).
- No libc. Use only MMIO and a tiny framebuffer helper.

## Display

- Target the iPod nano 7G LCD: 240x432, 32-bit ARGB framebuffer.
- For this first version, draw in portrait mode. The launcher UI is arranged horizontally inside the vertical screen.
- Background: dark grey (#1a1a1a).

## Top bar

- One line of text at the top, centered horizontally:
  `NanoOS v0.0.0 by Tenspav`
- Font: built-in 8x8 bitmap font. White color.
- Also shows the active clock profile: STOCK, SAFE or MAX.

## Clock profile selector

- On first boot, show a simple selector with three options:
  - **STOCK** - 342 MHz CPU / 133 MHz GPU
  - **SAFE** - 684 MHz CPU / 171 MHz GPU
  - **MAX** - 684 MHz CPU / 266 MHz GPU
- Use Volume Up/Down (keyboard Left/Right) to move, Home/Enter to confirm.
- After confirming, write the CPU clock to `0x3C500000` and the GPU clock to `0x3C50001C` using the known register values:
  - STOCK: CPU `0x40009001`, GPU `0xE0010000`
  - SAFE: CPU `0x40009000`, GPU `0xD0030000`
  - MAX: CPU `0x40009000`, GPU `0xE0000000`
- Then proceed to the game launcher. The selected profile stays visible in the top bar.

## Game icons row

- Below the top bar, a horizontal row of circular icons.
- Each icon is a colored circle (radius 32 px) with a white 2-letter label inside.
- Icons in order:
  - StockOS (blue circle, label "iP")
  - ViceNano (orange circle, "VC")
  - PS1Emu (green circle, "P1")
  - Settings (grey circle, "ST")
- Selected icon gets a white ring border and is slightly larger (radius 38 px).
- Navigation: left/right arrow keys on host keyboard map to Volume Down / Volume Up for selection.
- Launch: Home button or Enter confirms and runs the chosen target.

## Actions

- StockOS: return to iPod OS using the safe reset sequence (watchdog + PMGR reset).
- ViceNano / PS1Emu / Settings: for now, just draw a placeholder full-screen message with the target name and "press Home to go back". Home returns to the launcher.

## Input

- Home button: GPIO/PMU event line, active low.
- Volume Up/Down: map to keyboard Left/Right in NanoEmu for testing; on device use real volume buttons.

## Build outputs

- `nanoos/build/kernel.bin` - the raw kernel binary.
- `nanoos/build/kernel.hbapp` - optional homebrew package for direct launch on jailbroken iPod.
- Emulator test script: `nanoos/run-in-nanoemu.sh` that boots the kernel via the chainloader path.

## Files to create

- `nanoos/src/startup.s` - reset vector, stack setup, call kernel_main.
- `nanoos/src/main.c` - launcher logic and scene drawing.
- `nanoos/src/lcd.c` / `nanoos/src/lcd.h` - framebuffer init and pixel draw.
- `nanoos/src/font.c` - 8x8 font bitmap.
- `nanoos/src/input.c` / `input.h` - button polling.
- `nanoos/src/reset.c` - safe return to RetailOS.
- `nanoos/Makefile` - cross compile with the existing arm-none-eabi toolchain.

## Non-goals for v0.0.0

- Real image icons.
- Loading external game binaries.
- Audio, NAND, USB, Bluetooth.
- Landscape panel rotation.
