#!/usr/bin/env python3
"""NanoOS Loader round trip in NanoEmu (emulator only, never a device).

RetailOS (snap6) -> loader app in the Game Boy slot (a raw app needs an
HBAppKind 1 slot: Blocks is a GL app, which gets no framebuffer) -> opening it
boots NanoOS (no Home press) -> kernel read from disk -> hw_shutdown_all + SRAM
handoff -> NanoOS clock selector -> launcher -> Settings and DevMode -> NanoOS
StockOS -> reset back to RetailOS.

The dumped disk has no /Apps/NanoOS, and disk_patch.py can only overlay
existing files, so the emulator build of the loader reads the kernel from
/Apps/Data/Blocks/world.bin with nanoos/build/kernel.bin laid over it (both
in a patch file; the disk image is never written).

  --missing   instead boot the device build (/Apps/NanoOS/kernel.bin) and
              check that a missing kernel shows the error screen and Home
              takes RetailOS back to its Home screen, no handoff.

Artifacts: apps/NanoOSLoader/build-emu/{screens/,nanoemu.log}.
"""
import atexit, json, re, subprocess, sys, time
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[2]
APP = ROOT / 'apps/NanoOSLoader'
EMU = APP / 'build-emu'
MISSING = '--missing' in sys.argv
out = EMU / ('screens-missing' if MISSING else 'screens')
out.mkdir(parents=True, exist_ok=True)

KERNEL_SLOT = '/Apps/Data/Blocks/world.bin'
subprocess.check_call(['make', '-s', '-C', str(ROOT / 'nanoos'), 'all'])
if MISSING:
    subprocess.check_call(['make', '-s', '-C', str(APP)])
    app = APP / 'build/NanoOSLoader.hbapp'
    repl = [f'/Apps/Executables/Game Boy.hbapp={app}']
else:
    subprocess.check_call(['make', '-s', '-C', str(APP), 'BUILD=build-emu',
                           f'KERNEL_PATH={KERNEL_SLOT}', '-B'])
    app = EMU / 'NanoOSLoader.hbapp'
    repl = [f'/Apps/Executables/Game Boy.hbapp={app}',
            f'{KERNEL_SLOT}={ROOT / "nanoos/build/kernel.bin"}']
patch = EMU / ('missing.patch' if MISSING else 'loader.patch')
subprocess.check_call(['python3', 'emulator/tools/disk_patch.py',
                       'dumps/ipod-disk.dd', *repl, '-o', str(patch)], cwd=ROOT)

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
    '--nand-patch', str(patch),
    '--pcsample', '250',             # proves where the CPU is after the handoff
    '-display', 'none',
    '-qmp', 'stdio',
    # private state/screens dirs: the default emulator/state holds the boot
    # state that a guest reset reloads, and other NanoEmu runs share it
    '-M', 'nano7,mmu=0x08b28000,torn-ignore-pc=0x093d2a80:0x093d4100'
          f',state-dir={EMU / "state"},screens-dir={EMU / "emu-screens"}'
]
(EMU / 'state').mkdir(exist_ok=True)
(EMU / 'emu-screens').mkdir(exist_ok=True)
log_path = EMU / ('nanoemu-missing.log' if MISSING else 'nanoemu.log')
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
        if not line:
            raise RuntimeError('NanoEmu exited; see ' + str(log_path))
        r = json.loads(line)
        if 'return' in r or 'error' in r:
            if 'error' in r:
                raise RuntimeError(f'QMP error {ex}: {r}')
            return r


def tail_log():
    log_file.flush()
    return log_path.read_text(errors='replace')


PMU_NAME = {'right': 'volup', 'left': 'voldown', 'h': 'home', 'ret': 'home'}
lost_keys = 0


def key(k, wait=.6, hold=300):
    """press a key; hold 300 ms by default: the app level-samples buttons once
    per frame and RetailOS can stall its frame callback (see loader.c), while a
    bare sendkey is only ~10 ms of guest time (hold=0: bare, as the other snap6
    tests send keys to RetailOS itself).
    Under host load QEMU sometimes never delivers a sendkey to the machine.
    The PMU model logs every press it gets, so a press it never got is resent;
    one it got but the app ignored still fails the caller's check."""
    global lost_keys
    cmd = f'sendkey {k} {hold}' if hold else f'sendkey {k}'
    tag = f'button {PMU_NAME[k]} pressed' if k in PMU_NAME else None
    for _ in range(3):
        n = tail_log().count(tag) if tag else 0
        q('human-monitor-command', {'command-line': cmd})
        if not tag:
            break
        end = time.time() + 3
        while time.time() < end and tail_log().count(tag) == n:
            time.sleep(.1)
        if tail_log().count(tag) > n:
            break
        lost_keys += 1
        print(f'  ({k}: not delivered by QEMU, resending)')
    else:
        raise AssertionError(f'{k}: QEMU never delivered the key')
    time.sleep(wait)


def touch(x, y, down=None):
    es = [dict(type='abs', data=dict(axis='x', value=x)),
          dict(type='abs', data=dict(axis='y', value=y))]
    if down is not None:
        es.append(dict(type='btn', data=dict(down=down, button='left')))
    q('input-send-event', dict(events=es))
    time.sleep(.13)


def grab():
    """portrait screenshot, not saved"""
    ppm = out / 'grab.ppm'
    q('screendump', {'filename': str(ppm)})
    im = Image.open(ppm).convert('RGB')
    assert im.size == (240, 432)
    return im


def save(name, im):
    """save portrait + the upright landscape view the launcher is drawn in
    (same transform as nanoos/test_nanoemu.py); returns the landscape one"""
    im.save(out / (name + '.png'))
    land = im.transpose(Image.ROTATE_90)
    land.save(out / (name + '_landscape.png'))
    print(f'  {name}')
    return land


def wait_for(name, pred, timeout=10.0, every=.25, stable=False):
    """poll the screen until pred(landscape) holds (stable: on two identical
    consecutive grabs, i.e. no animation running); the host is often loaded
    (other NanoEmu runs), so state is awaited instead of fixed sleeps"""
    end = time.time() + timeout
    last = None
    while True:
        im = grab()
        land = im.transpose(Image.ROTATE_90)
        if pred(land):
            if not stable or (last is not None and last.tobytes() == im.tobytes()):
                return save(name, im)
            last = im
        else:
            last = None
        if time.time() > end:
            save(name + '_TIMEOUT', im)
            raise AssertionError(f'{name}: not reached in {timeout:.0f} s')
        time.sleep(every)


WHITE, BG = (255, 255, 255), (26, 26, 26)
# NanoOS launcher icons (nanoos/src/main.c, 5 slots)
ICONS = [(43, (38, 132, 255)), (129, (255, 136, 0)), (215, (40, 184, 72)),
         (301, (128, 128, 128)), (387, (160, 76, 224))]


def is_launcher(im, sel):
    """NanoOS launcher: the five circles, the white ring on `sel` only"""
    for i, (x, color) in enumerate(ICONS):
        if im.getpixel((x, 90)) != color:
            return False
        if (im.getpixel((x, 72)) == WHITE) != (i == sel):
            return False
    return im.getpixel((420, 10)) == BG


def white_in(im, y0, y1, x0=0, x1=432):
    return any(im.getpixel((x, y)) == WHITE for x in range(x0, x1)
               for y in range(y0, y1))


def home_page(land):
    """RetailOS home page from the white page dot (1..3), 0 = not home"""
    im = land.transpose(Image.ROTATE_270)
    for page, x in [(1, 105), (2, 119), (3, 133)]:
        if im.getpixel((x, 421)) == WHITE and im.getpixel((120, 216)) != BG:
            return page
    return 0


def swipe(right):
    xs = [4000, 9000, 14000, 19000, 24000, 29000]
    if not right:
        xs.reverse()
    for i, x in enumerate(xs):
        touch(x, 16000, True if i == 0 else False if i == len(xs) - 1 else None)


def to_home(tag):
    """Home until RetailOS shows its home screen (the snapshot resumes inside
    DumpRAM; a press can be lost while RetailOS is busy)"""
    for _ in range(3):
        key('h', 0, hold=0)
        try:
            return wait_for(tag, lambda im: home_page(im) > 0, 8)
        except AssertionError:
            pass
    raise AssertionError(tag + ': RetailOS home screen never appeared')


# draw_busy(): "BUSY..." (y=40) and the progress bar track (y=150)
busy = lambda im: (white_in(im, 40, 48, 180, 252)
                   and im.getpixel((370, 158)) in ((64, 64, 64), (40, 184, 72)))


def open_loader(tag):
    """home page 2, tap the Game Boy slot; returns the log length at the tap"""
    print('Opening the loader (home page 2, Game Boy slot)...')
    for _ in range(6):
        # decide only on a settled home screen, never mid page-slide
        page = home_page(wait_for(tag + '_home', lambda im: home_page(im) > 0,
                                  10, .5, stable=True))
        if page == 2:
            break
        swipe(page > 2)
    else:
        raise AssertionError(tag + ': could not reach home page 2')
    mark = len(tail_log())
    touch(24600, 17500, True)
    touch(24600, 17500, False)
    return mark


TITLE = 'NanoOS v0.0.2 beta by Tenspav'
q('qmp_capabilities')
time.sleep(3)
save('01_retailos_boot', grab())
to_home('02_retailos_home')
mark = open_loader('03')

if MISSING:
    wait_for('04_missing_kernel', lambda im: any(
        im.getpixel((x, y)) == (255, 80, 80)
        for x in range(100, 332) for y in range(80, 88)), 20, .1)
    key('h', 0)
    wait_for('05_retailos_home', lambda im: home_page(im) > 0)
    q('quit')
    p.wait(timeout=10)
    assert 'reset: reloading the boot state' not in tail_log()
    if lost_keys:
        print(f'note: {lost_keys} key press(es) were lost inside QEMU and resent')
    print('PASS: missing kernel -> error screen -> Home -> RetailOS home, no handoff/reset')
    sys.exit(0)

print('Loader boots NanoOS on open: BUSY -> kernel load -> handoff -> clock selector...')
# BUSY is up only a few frames, so catching it is best effort; the handoff
# itself is asserted below
selector = lambda im: (not busy(im) and all(im.getpixel(xy) == BG for xy in
                           [(4, 4), (427, 4), (4, 235), (427, 235), (216, 80)])
                       and white_in(im, 16, 24)
                       and white_in(im, 56, 64) and white_in(im, 100, 108))
busy_seen = False
end = time.time() + 20
while time.time() < end:
    im = grab()
    land = im.transpose(Image.ROTATE_90)
    if busy(land):
        if not busy_seen:
            save('04_busy', im)
        busy_seen = True
    elif selector(land):
        break
    time.sleep(.03)
print(f'  BUSY frame {"captured" if busy_seen else "not captured (on screen only a few frames)"}')
sel = wait_for('05_nanoos_clock_selector', selector, 15)
time.sleep(1)
# the PC sampler (4 Hz) must keep finding the CPU inside the loaded kernel
# image at 0x08000000, in SVC mode as the SRAM trampoline leaves it (the
# sampler prints the mode bits only), and no reset may have happened
kernel_end = 0x08000000 + (ROOT / 'nanoos/build/kernel.bin').stat().st_size
log = tail_log()
samples = [(int(pc, 16), int(cpsr, 16)) for pc, cpsr in re.findall(
    r'\[nano7-pc\] pc=0x([0-9a-f]+) cpsr=0x([0-9a-f]+)', log[mark:])]
last = samples[-4:]
assert len(last) == 4 and all(0x08000000 <= pc < kernel_end and cpsr & 0x1f == 0x13
                              for pc, cpsr in last), \
    f'CPU not in NanoOS after the handoff: {[(hex(a), hex(b)) for a, b in last]}'
print(f'  PC sampler: last {len(last)} samples in NanoOS '
      f'(0x08000000..0x{kernel_end:08x}, SVC): {[hex(pc) for pc, _ in last]}')
assert 'reset: reloading the boot state' not in log[mark:], 'reset instead of handoff'
assert 'button home pressed' not in log[mark:], 'boot needed a Home press'

print('Inside NanoOS: STOCK profile, Settings, DevMode, StockOS -> reset...')
key('ret', 0)
launch = wait_for('06_nanoos_launcher', lambda im: is_launcher(im, 0), 10)
assert white_in(launch, 16, 24, (432 - len(TITLE + ' | STOCK') * 8) // 2,
                (432 + len(TITLE + ' | STOCK') * 8) // 2)
for i in range(1, 4):
    key('right', 0)
    wait_for(f'07_nanoos_sel_{i}', lambda im, i=i: is_launcher(im, i))
key('ret', 0)
wait_for('08_nanoos_settings', lambda im: im.getpixel((40, 90)) == BG
         and white_in(im, 132, 140))
key('h', 0)
wait_for('09_nanoos_back', lambda im: is_launcher(im, 3))
key('right', 0)
wait_for('10_nanoos_devmode_sel', lambda im: is_launcher(im, 4))
key('ret', 0)
wait_for('11_nanoos_devmode', lambda im: im.getpixel((40, 90)) == BG
         and white_in(im, 48, 56, 40, 64) and white_in(im, 128, 136, 112, 200), 10)
key('h', 0)
wait_for('12_nanoos_back', lambda im: is_launcher(im, 4))
key('right', 0)
wait_for('13_nanoos_stockos', lambda im: is_launcher(im, 0))
key('ret', 0)
wait_for('14_retailos_restored', lambda im: not is_launcher(im, 0), 15)
to_home('15_retailos_home')

q('quit')
p.wait(timeout=10)
log_file.close()
log = log_path.read_text(errors='replace')
assert '[nano7-ui] reset: reloading the boot state' in log[mark:], 'no reset back to RetailOS'
if lost_keys:
    print(f'note: {lost_keys} key press(es) were lost inside QEMU and resent')
print('PASS: loader opens -> BUSY -> kernel load -> shutdown + SRAM handoff (no Home) -> '
      'NanoOS clock selector -> launcher v0.0.2 beta -> Settings -> DevMode -> '
      'StockOS -> reset -> RetailOS')
