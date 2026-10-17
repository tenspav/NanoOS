#!/usr/bin/env python3
"""Boot NanoOS via the RetailOS chainloader and check UI/input/reset.
Artifacts: nanoos/build/screens/<profile> and nanoos/build/nanoemu-<profile>.log.
Requires snap6, the disk/resource images, NanoEmu, and Pillow.
--real[=SPEC]: NanoEmu's real iPod simulation (NANO7_REALDEV: failing PMU
I2C, panel bridge busy hold, DSI latency); adds FPS / I2C / push checks.
"""

import atexit, json, os, re, subprocess, sys, time
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
# --fps: only measure DevMode FPS (confirm / enter by the volume chord, no
# PMU needed), print the DevMode lines and stop
fps_only = '--fps' in sys.argv
args = [a for a in sys.argv[1:] if not a.startswith('--')]
real = next((a.partition('=')[2] or '1' for a in sys.argv[1:]
             if a.startswith('--real')), None)
profile = args[0].upper() if args else 'STOCK'
# real iPod mode: DevMode must reach this FPS, and Home must keep working
MIN_REAL_FPS = 15.0
# GPU rate field CLKCON+0x1c [29:16]; the CPU/system word +0x00 and the
# rest of +0x1c (gates, memory domain [15:0]) must be left as found
GPU_RATE = 0x3fff0000
profiles = {'STOCK': 0x20010000, 'SAFE': 0x10030000, 'MAX': 0x20000000}
if profile not in profiles:
    raise SystemExit('Usage: test_nanoemu.py [STOCK|SAFE|MAX] [--real[=SPEC]]')
tag = profile.lower() + ('-real' if real else '')
out = ROOT / 'nanoos/build/screens' / tag
out.mkdir(parents=True, exist_ok=True)

patch_file = ROOT / 'nanoos/build/chainloader.patch'
emu = ROOT / 'nanoos/build/emu' / tag
(emu / 'state').mkdir(parents=True, exist_ok=True)
(emu / 'screens').mkdir(exist_ok=True)
cmd = [
    'emulator/run.sh',
    '--ramfull', 'dumps/snap6/ram-full.bin',
    '--iram', 'dumps/snap6/iram.bin',
    '--ctx', 'emulator/fixups/snap6-ctx.bin',
    '--overlay', 'dumps/snap6/overlay.bin',
    '--os-poke-file', 'emulator/fixups/snap6-resume.poke',
    '--torn-map', 'dumps/snap6/torn-map.bin',
    '--nand-miss', 'oracle',
    '--nand-disk', 'dumps/ipod-disk.dd',
    '--nand-disk-offset', '49216',
    '--nand-rsrc', 'emulator/rsrc/rsrc-1.1.2.img1',
    '--nand-patch', str(patch_file),
    '-display', 'none',
    '-qmp', 'stdio',
    # private state/screens dirs: the default emulator/state holds the boot
    # state that a guest reset reloads, and other NanoEmu runs share it
    '-M', 'nano7,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100'
          f',state-dir={emu / "state"},screens-dir={emu / "screens"}'
]

log_path = ROOT / f'nanoos/build/nanoemu-{tag}.log'
log_file = open(log_path, 'w')
env = dict(os.environ)
env.pop('NANO7_REALDEV', None)
if real:
    env['NANO7_REALDEV'] = real
evt_path = emu / 'evt.txt'
if fps_only:
    env['NANO7_EVT'] = str(evt_path)     # panel pushes, guest-timed
p = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=log_file, text=True, env=env)
atexit.register(lambda: p.poll() is None and p.terminate())
p.stdout.readline()

def q(ex, args={}):
    p.stdin.write(json.dumps(dict(execute=ex, arguments=args)) + '\n')
    p.stdin.flush()
    while True:
        line = p.stdout.readline()
        if not line: raise RuntimeError('NanoEmu exited; see ' + str(log_path))
        r = json.loads(line)
        if 'return' in r or 'error' in r:
            if 'error' in r:
                raise RuntimeError(f'QMP error {ex}: {r}')
            return r

def key(k):
    # hold 300 ms: a bare sendkey is ~10 ms of guest time, shorter than one
    # NanoOS loop pass (a panel push alone is ~17 ms), so a press can fall
    # between two samples; real presses last far longer
    q('human-monitor-command', {'command-line': f'sendkey {k} 300'})
    # volume acts on release: start the caller's wait after it. QEMU spaces
    # every down/up event of a chord (left-right) by the hold time.
    time.sleep(.3 * (2 * len(k.split('-')) - 1))

def pause(t):
    # real iPod mode: frames take ~150 ms or more, as on the device
    time.sleep(t * 3 if real else t)

def touch(x, y, down=None):
    es = [dict(type='abs', data=dict(axis='x', value=x)),
          dict(type='abs', data=dict(axis='y', value=y))]
    if down is not None:
        es.append(dict(type='btn', data=dict(down=down, button='left')))
    q('input-send-event', dict(events=es))
    time.sleep(.13)

def shot(name, landscape=False):
    ppm = out / (name + '.ppm')
    q('screendump', {'filename': str(ppm)})
    time.sleep(0.3)
    if ppm.exists():
        im = Image.open(ppm).convert('RGB')
        im.save(out / (name + '.png'))
        if landscape:
            assert im.size == (240, 432)
            im = im.transpose(Image.ROTATE_90)
            im.save(out / (name + '_landscape.png'))
        ppm.unlink()
        print(f"Captured {name}.png: size={im.size}, center pixel={im.getpixel((120, 216))}")
        return im
    raise RuntimeError(f"Failed to capture {ppm}")

# 8x8 glyphs from the kernel's font, for reading DevMode values back
FONT = {}
for code, rows in re.findall(r'\[(\d+)\] = \{(0x[^{}]*)\}',
                             (ROOT / 'nanoos/src/font.c').read_text()):
    FONT[tuple(int(v, 0) for v in rows.split(','))] = chr(int(code))

def read_text(im, x, y, cells):
    """OCR one line of white 8x8 cells (unknown glyph = '?')."""
    text = ''
    for i in range(cells):
        rows = tuple(sum(1 << dx for dx in range(8)
                         if im.getpixel((x + i * 8 + dx, y + dy)) == (255, 255, 255))
                     for dy in range(8))
        text += ' ' if not any(rows) else FONT.get(rows, '?')
    return text.rstrip()

def assert_text_visible(im, x, y, text):
    # Every non-space cell must contain ink; catches missing font glyphs.
    for i, char in enumerate(text):
        if char != ' ':
            assert any(im.getpixel((x + i * 8 + dx, y + dy)) == (255, 255, 255)
                       for dx in range(8) for dy in range(8)), (text, char, i)

print("Initializing QMP...")
q('qmp_capabilities')
# Snapshot resume can take longer under host load; let RetailOS accept keys.
time.sleep(8)
shot('01_retailos_boot')

print("Navigating to Home screen...")
key('h')
time.sleep(4)
home = shot('02_retailos_home_p3')
# A resumed snapshot may consume the first Home press just waking the panel.
for _ in range(3):
    if home.getpixel((120, 216)) != (14, 17, 24):
        break
    key('h')
    time.sleep(3)
    home = shot('02_retailos_home_p3')
if home.getpixel((120, 216)) == (14, 17, 24):
    raise RuntimeError('RetailOS did not wake to Home')

print("Swiping to home page 2 (Blocks slot)...")
for x, d in [(4000, True), (9000, None), (14000, None), (19000, None), (24000, None), (29000, False)]:
    touch(x, 16000, d)
time.sleep(5)
shot('03_retailos_home_p2')

print("Tapping chainloader app (Blocks icon)...")
touch(8700, 17500, True)
touch(8700, 17500, False)
time.sleep(5)


selector = shot('04_clock_STOCK', landscape=True)
if fps_only:
    key('left-right')                       # confirm STOCK
    pause(.5)
    key('left')                             # StockOS -> DevMode (wraps)
    pause(.5)
    key('left-right')                       # enter
    pause(4)
    mark = time.time()
    time.sleep(4)
    live = shot('DevMode_fps', landscape=True)
    if os.environ.get('NANOOS_TRACE'):
        print(q('human-monitor-command', {'command-line':
          f'pmemsave 0x08300000 0x2008 "{os.environ["NANOOS_TRACE"]}"'}))
    print(f'DevMode: FPS {read_text(live, 112, 128, 8)} | '
          f'{read_text(live, 8, 144, 52)} | {read_text(live, 8, 192, 52)}')
    q('quit')
    p.wait(timeout=10)
    # pushes per guest second over the last ~4 s (DevMode's own FPS window
    # is 1 s: too few frames to compare runs)
    ram = [int(l.split()[0]) for l in evt_path.read_text().splitlines()
           if ' realdev-ramwr ' in l]
    if len(ram) > 2:
        end = ram[-1]
        win = [t for t in ram if t >= end - 4_000_000]
        print(f'FPS (4 s of pushes): {(len(win) - 1) * 1e6 / (win[-1] - win[0]):.2f}')
    sys.exit(0)
for i, name in enumerate(profiles):
    assert_text_visible(selector, 48 + i * 144, 100, name)
for y, text in [(56, 'Choose clock profile'),
                (140, '342 MHz CPU / 133 MHz GPU'),
                (184, 'Volume / Left-Right to choose'),
                (208, 'Home or Vol- + Vol+ to confirm')]:
    assert_text_visible(selector, (432 - len(text) * 8) // 2, y, text)
# gray PMU diagnostic line at y=228
assert any(selector.getpixel((x, y)) == (128, 128, 128)
           for x in range(432) for y in range(228, 236)), 'no PMU line'
# Left wraps STOCK -> MAX; right wraps MAX -> STOCK, then visits SAFE/MAX.
for direction, name in [('left', 'MAX'), ('right', 'STOCK'),
                        ('right', 'SAFE'), ('right', 'MAX')]:
    key(direction)
    pause(.5)
    current = shot('clock_' + name, landscape=True)
    # the bottom row (y >= 224) is the live PMU diagnostic line
    same = current.crop((0, 0, 432, 224)).tobytes() == selector.crop((0, 0, 432, 224)).tobytes()
    assert same == (name == 'STOCK'), name
for _ in range({'MAX': 0, 'STOCK': 1, 'SAFE': 2}[profile]):
    key('right')
    pause(.5)
shot('clock_confirm_' + profile, landscape=True)
def reg(address):
    result = q('human-monitor-command', {'command-line': f'xp /1wx 0x{address:x}'})['return']
    return int(re.search(r':\s*(0x[0-9a-fA-F]+)', result).group(1), 16)
before = reg(0x3c500000), reg(0x3c50001c)
# STOCK confirms with Enter, SAFE with Home, MAX with the volume chord
# (both buttons = Home, no PMU/I2C needed)
key({'STOCK': 'ret', 'SAFE': 'h', 'MAX': 'left-right'}[profile])
pause(.5)
expected = before[0], before[1] & ~GPU_RATE | profiles[profile]
for address, want in zip((0x3c500000, 0x3c50001c), expected):
    assert reg(address) == want, (hex(address), hex(reg(address)), hex(want))
print(f'CLKCON before {before[0]:#x} {before[1]:#x} -> after {expected[0]:#x} {expected[1]:#x}')
print(f'PASS: {profile} CPU/GPU register values')
launcher = shot('05_launcher_' + profile, landscape=True)
title = 'NanoOS v0.0.2 beta by Tenspav | ' + profile
assert_text_visible(launcher, (432 - len(title) * 8) // 2, 16, title)
assert launcher.getpixel((420, 220)) == (26, 26, 26)
assert launcher.getpixel((43, 90)) == (38, 132, 255)
assert launcher.getpixel((43, 71)) == (255, 255, 255)
# All five circles are visible, evenly spaced, with a centered title.
for x, color in [(43, (38,132,255)), (129, (255,136,0)),
                 (215, (40,184,72)), (301, (128,128,128)),
                 (387, (160,76,224))]:
    assert launcher.getpixel((x,90)) == color
assert any(launcher.getpixel((x,y)) == (255,255,255)
           for x in range(128,304) for y in range(16,24))
assert any(launcher.getpixel((x,y)) == (255,255,255)
           for x in range(35,51) for y in range(108,116))
for target, x, color in [('ViceNano', 129, (255,136,0)),
                         ('PS1Emu', 215, (40,184,72)),
                         ('Settings', 301, (128,128,128)),
                         ('DevMode', 387, (160,76,224))]:
    key('right')
    pause(.5)
    selected = shot(target + '_selected', landscape=True)
    assert selected.getpixel((x,90)) == color
    assert selected.getpixel((x,71)) == (255,255,255)
    key('ret')
    pause(.5)
    if target == 'DevMode':
        dev = shot('DevMode_enter', landscape=True)
        assert_text_visible(dev, (432 - 14 * 8) // 2, 16, 'NanoOS DevMode')
        for y, label in [(48, 'CPU'), (68, 'GPU'), (88, 'RAM'),
                         (108, 'Heap'), (128, 'FPS')]:
            assert_text_visible(dev, 40, y, label)
        # Screen redraws live: the FPS value replaces '--' after 1 s.
        pause(2.5)
        live = shot('DevMode_live', landscape=True)
        fps = [live.getpixel((x, y)) for x in range(112, 200) for y in range(128, 136)]
        assert fps != [dev.getpixel((x, y)) for x in range(112, 200) for y in range(128, 136)]
        fps_text = read_text(live, 112, 128, 8)
        lcd_line = read_text(live, 8, 144, 52)
        i2c_line = read_text(live, 8, 192, 52)
        print(f'DevMode: FPS {fps_text} | {lcd_line} | {i2c_line}')
        if real:
            assert float(fps_text) >= MIN_REAL_FPS, ('FPS', fps_text)
        # Drivers page (DRIVER_PORT.md): Vol+ opens it and runs the probes;
        # then a tap at the panel centre, Power, Play, and time for the BT
        # pad to connect and send its scripted reports (1 s, 2 s, ...)
        key('right')
        pause(1)
        touch(16384, 16384, True)
        touch(16384, 16384, False)
        key('p')
        key('spc')
        pause(3)
        drv = shot('DevMode_drivers', landscape=True)
        lines = {}
        for i in range(8):
            text = read_text(drv, 8, 40 + 18 * i, 53)
            print('  ' + text)
            lines[text.split(' ')[0]] = text.split()
        def field(name, after):
            words = lines[name]
            return words[words.index(after) + 1]
        for name in ['LCD', 'TOUCH', 'BTN', 'AUDIO', 'NAND', 'BT', 'MBX']:
            assert lines[name][1] == 'OK', lines[name]
        assert lines['USB'][1] in ('OK', 'STUB'), lines['USB']
        assert int(field('TOUCH', 'n')) >= 2, lines['TOUCH']
        tx, ty = int(field('TOUCH', 'x')), int(field('TOUCH', 'y'))
        assert abs(tx - 216) <= 3 and abs(ty - 120) <= 3, (tx, ty)
        seen = field('BTN', 'seen')
        assert seen[1] == 'P' and seen[2] == 'L', seen     # Power, Play
        assert field('AUDIO', 'codec') == '2F:80', lines['AUDIO']
        assert field('AUDIO', 'tone') == '22050', lines['AUDIO']
        assert lines['NAND'][2:5] == ['PPN', '16384B', '128pg'], lines['NAND']
        assert lines['NAND'][-1] in ('blank', 'data'), lines['NAND']
        assert lines['BT'][2:4] == ['hid', 'up'], lines['BT']
        assert int(field('BT', 'rep')) >= 2, lines['BT']
        assert field('MBX', 'id') == 'B105F00D' and field('MBX', 'scene') == '1', lines['MBX']
        print('PASS: Drivers page')
        # GPU page (MBX_GPU.md): Vol+ again; the MBX clears to dark blue
        # and draws a Gouraud triangle whose apex moves every frame
        key('right')
        pause(1)
        gpu = shot('DevMode_gpu', landscape=True)
        pause(.5)
        gpu2 = shot('DevMode_gpu2', landscape=True)
        gpu_line = read_text(gpu2, 8, 24, 40)
        print('  GPU: ' + gpu_line)
        assert gpu_line.split()[0] == 'OK', gpu_line
        assert gpu.getpixel((10, 120)) == (8, 24, 64), gpu.getpixel((10, 120))
        tri = gpu.getpixel((216, 180))
        assert tri != (8, 24, 64) and max(tri) > 100, tri
        assert gpu.tobytes() != gpu2.tobytes()          # animated
        print('PASS: GPU page')
        key('h')
        pause(.5)
        back = shot(target + '_back', landscape=True)
        assert back.tobytes() == selected.tobytes()
        continue
    placeholder = shot(target + '_placeholder', landscape=True)
    assert placeholder.getpixel((40,90)) == (26,26,26)
    assert any(placeholder.getpixel((x,y)) == (255,255,255)
               for x in range(432) for y in range(132,140))
    key('h')
    pause(.5)
    back = shot(target + '_back', landscape=True)
    assert back.tobytes() == selected.tobytes()
# Verify left navigation, then wrap right from DevMode to StockOS.
key('left')
pause(.5)
left = shot('left_to_Settings', landscape=True)
assert left.getpixel((301,71)) == (255,255,255)
key('right')
pause(.5)
key('right')
pause(.5)
stock = shot('StockOS_selected', landscape=True)
assert stock.tobytes() == launcher.tobytes()
key('ret')
time.sleep(4)
restored = shot('RetailOS_restored')
assert restored.tobytes() != launcher.tobytes()
q('quit')
p.wait(timeout=10)
log_file.close()
log = log_path.read_text()
assert '[nano7-ui] reset: reloading the boot state' in log
if real:
    m = re.search(r'\[nano7-realdev\] summary: (.*)', log)
    assert m, 'no realdev summary'
    stats = dict((k, int(v)) for k, v in re.findall(r'([a-z-]+) (\d+)', m.group(1)))
    print('realdev:', m.group(1))
    assert stats['pushes'] > 0 and stats['addr'] > 0 and stats['nack'] > 0
    assert stats['short'] == 0 and stats['torn'] == 0 and stats['no-bridge'] == 0, stats
print(f'PASS: {profile}{" REAL-IPOD" if real else ""} selector, clocks, chainloader boot, landscape launcher, navigation, all placeholders, DevMode live screen, Home back, StockOS reset')
