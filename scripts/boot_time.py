#!/usr/bin/env python3
"""Device: time one boot of the installed Donut Dodo from `load_core`.

    python3 boot_time.py [cold|warm] [secs]      (run as root on the MiSTer)

Loads the menu core, optionally drops the page cache, writes `load_core <newest
DonutDodo_*.rbf>` to /dev/MiSTer_cmd (as the OSD does; MiSTer.ini main= starts
the launcher) and polls every 10 ms without forking: /proc for the engine
process (frt_3.5.2), C_DONE (0x3B000028) through /dev/mem. Prints seconds from
the load_core write to: engine exec, first frame retired by the fabric, 60
frames retired. Then loads the menu core again.
"""
import glob, mmap, os, struct, sys, time

ENGINE = "frt_3.5.2"
C_DONE = 0x3B000028
mode = sys.argv[1] if len(sys.argv) > 1 else "warm"
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 30


def corename():
    try:
        return open("/tmp/CORENAME").read().strip()
    except OSError:
        return ""


def load(path, want, limit=30):
    with open("/dev/MiSTer_cmd", "w") as f:
        f.write(f"load_core {path}\n")
    t = time.monotonic()
    while corename() != want and time.monotonic() - t < limit:
        time.sleep(0.2)


def engine_pid():
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                if open(f"/proc/{d}/comm").read().strip() == ENGINE:
                    return int(d)
            except OSError:
                pass
    return None


if corename() != "MENU":
    load("/media/fat/menu.rbf", "MENU")
    time.sleep(3)
t = time.monotonic()           # the launcher's watchdog stops the engine ~3 s after the core change
while engine_pid() and time.monotonic() - t < 10:
    time.sleep(0.2)
if engine_pid():
    sys.exit("engine still running on the menu core -- stop it first")
rbf = max(glob.glob("/media/fat/_Other/DonutDodo_*.rbf"), key=os.path.getmtime)
os.sync()
if mode == "cold":
    open("/proc/sys/vm/drop_caches", "w").write("3\n")
time.sleep(1)

fd = os.open("/dev/mem", os.O_RDONLY | os.O_SYNC)
page = C_DONE & ~0xFFF
mm = mmap.mmap(fd, 4096, mmap.MAP_SHARED, mmap.PROT_READ, offset=page)
done = lambda: struct.unpack_from("<I", mm, C_DONE - page)[0]

t0 = time.monotonic()
with open("/dev/MiSTer_cmd", "w") as f:
    f.write(f"load_core {rbf}\n")
t_core = t_exec = t_first = t_60 = None
d0 = None
while time.monotonic() - t0 < secs and t_60 is None:
    now = time.monotonic() - t0
    if t_core is None and corename() == "DonutDodo":
        t_core = now
    if t_core is not None and t_exec is None and engine_pid():
        t_exec = now
        d0 = done()            # the new core's C_DONE, before the engine submits
    if t_exec is not None:
        d = done()
        if t_first is None and d != d0:
            t_first = now
        if (d - d0) & 0xFFFFFFFF >= 60:
            t_60 = now
    time.sleep(0.01)
fmt = lambda v: "-" if v is None else f"{v:.2f}"
print(f"mode={mode} rbf={os.path.basename(rbf)} core={fmt(t_core)} exec={fmt(t_exec)} "
      f"first_frame={fmt(t_first)} frames60={fmt(t_60)}")
load("/media/fat/menu.rbf", "MENU")
