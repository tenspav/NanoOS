#!/usr/bin/env python3
"""Boot NanoOS via the RetailOS chainloader and check UI/input/reset.
Artifacts: nanoos/build/screens/<profile> and nanoos/build/nanoemu-<profile>.log.
Requires snap6, the disk/resource images, NanoEmu, and Pillow.
"""

import atexit, json, re, subprocess, sys, time
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
profile = sys.argv[1].upper() if len(sys.argv) > 1 else 'STOCK'
profiles = {'STOCK': (0x40009001, 0xe0010000),
            'SAFE': (0x40009000, 0xd0030000),
            'MAX': (0x40009000, 0xe0000000)}
if profile not in profiles:
    raise SystemExit('Usage: test_nanoemu.py [STOCK|SAFE|MAX]')
out = ROOT / 'nanoos/build/screens' / profile.lower()
out.mkdir(parents=True, exist_ok=True)

patch_file = ROOT / 'nanoos/build/chainloader.patch'
emu = ROOT / 'nanoos/build/emu' / profile.lower()
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

log_path = ROOT / f'nanoos/build/nanoemu-{profile.lower()}.log'
log_file = open(log_path, 'w')
p = subprocess.Popen(cmd, cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                     stderr=log_file, text=True)
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
    q('human-monitor-command', {'command-line': f'sendkey {k}'})

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
for i, name in enumerate(profiles):
    assert_text_visible(selector, 48 + i * 144, 100, name)
for y, text in [(56, 'Choose clock profile'),
                (140, '342 MHz CPU / 133 MHz GPU'),
                (184, 'Volume / Left-Right to choose'),
                (208, 'Home / Enter to confirm')]:
    assert_text_visible(selector, (432 - len(text) * 8) // 2, y, text)
# Left wraps STOCK -> MAX; right wraps MAX -> STOCK, then visits SAFE/MAX.
for direction, name in [('left', 'MAX'), ('right', 'STOCK'),
                        ('right', 'SAFE'), ('right', 'MAX')]:
    key(direction)
    time.sleep(.5)
    current = shot('clock_' + name, landscape=True)
    if name == 'STOCK':
        assert current.tobytes() == selector.tobytes()
    else:
        assert current.tobytes() != selector.tobytes()
for _ in range({'MAX': 0, 'STOCK': 1, 'SAFE': 2}[profile]):
    key('right')
    time.sleep(.5)
shot('clock_confirm_' + profile, landscape=True)
key('h' if profile == 'SAFE' else 'ret')
time.sleep(.5)
for address, expected in zip((0x3c500000, 0x3c50001c), profiles[profile]):
    result = q('human-monitor-command', {'command-line': f'xp /1wx 0x{address:x}'})['return']
    actual = int(re.search(r':\s*(0x[0-9a-fA-F]+)', result).group(1), 16)
    assert actual == expected, (hex(address), hex(actual), hex(expected))
print(f'PASS: {profile} CPU/GPU register values')
launcher = shot('05_launcher_' + profile, landscape=True)
title = 'NanoOS v0.0.0 by Tenspav | ' + profile
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
    time.sleep(.5)
    selected = shot(target + '_selected', landscape=True)
    assert selected.getpixel((x,90)) == color
    assert selected.getpixel((x,71)) == (255,255,255)
    key('ret')
    time.sleep(.5)
    if target == 'DevMode':
        dev = shot('DevMode_enter', landscape=True)
        assert_text_visible(dev, (432 - 14 * 8) // 2, 16, 'NanoOS DevMode')
        for y, label in [(48, 'CPU'), (68, 'GPU'), (88, 'RAM'),
                         (108, 'Heap'), (128, 'FPS')]:
            assert_text_visible(dev, 40, y, label)
        # Screen redraws live: the FPS value replaces '--' after 1 s.
        time.sleep(2.5)
        live = shot('DevMode_live', landscape=True)
        fps = [live.getpixel((x, y)) for x in range(112, 200) for y in range(128, 136)]
        assert fps != [dev.getpixel((x, y)) for x in range(112, 200) for y in range(128, 136)]
        key('h')
        time.sleep(.5)
        back = shot(target + '_back', landscape=True)
        assert back.tobytes() == selected.tobytes()
        continue
    placeholder = shot(target + '_placeholder', landscape=True)
    assert placeholder.getpixel((40,90)) == (26,26,26)
    assert any(placeholder.getpixel((x,y)) == (255,255,255)
               for x in range(432) for y in range(132,140))
    key('h')
    time.sleep(.5)
    back = shot(target + '_back', landscape=True)
    assert back.tobytes() == selected.tobytes()
# Verify left navigation, then wrap right from DevMode to StockOS.
key('left')
time.sleep(.5)
left = shot('left_to_Settings', landscape=True)
assert left.getpixel((301,71)) == (255,255,255)
key('right')
time.sleep(.5)
key('right')
time.sleep(.5)
stock = shot('StockOS_selected', landscape=True)
assert stock.tobytes() == launcher.tobytes()
key('ret')
time.sleep(4)
restored = shot('RetailOS_restored')
assert restored.tobytes() != launcher.tobytes()
q('quit')
p.wait(timeout=10)
log_file.close()
assert '[nano7-ui] reset: reloading the boot state' in log_path.read_text()
print(f'PASS: {profile} selector, clocks, chainloader boot, landscape launcher, navigation, all placeholders, DevMode live screen, Home back, StockOS reset')
