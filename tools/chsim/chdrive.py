"""Drive a CHGame sketch - in the simulator or on the device - with a script.

    python chdrive.py --sim <sketch dir> <script> <outdir>
    python chdrive.py --device [--port COMx] <script> <outdir>

--id names the game's handshake reply (default CHCS, CHChess).

Both targets speak the same serial debug protocol (see the sketch's
src/debug/Debug.h), so one script produces comparable screenshots from each.

Script lines (# comments allowed):
    wait N              advance N frames
    free SECONDS        run free (real time) for a while, then lockstep again
    tap BTN[+BTN] [H]   hold for H frames (default 3), then release, then 1 frame
    hold BTN[+BTN]      keep held until `release`
    release
    snap NAME           save NAME.png
    gif NAME N [EVERY]  record N frames (every EVERY-th) to NAME.gif
    say TEXT            send TEXT as a raw protocol line (game hooks: seed, deck)
    perf                print the device's PERF line
Buttons: A B UP DOWN LEFT RIGHT START SELECT
"""
import argparse
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


from fbimage import sheet, to_image  # noqa: E402

BUTTONS = {"A": 1, "B": 2, "UP": 4, "DOWN": 8, "LEFT": 16, "RIGHT": 32, "START": 64, "SELECT": 128}


def mask_of(spec):
    m = 0
    for part in spec.upper().split("+"):
        if part:
            m |= BUTTONS[part]
    return m


class SimTransport:
    def __init__(self, exe):
        self.p = subprocess.Popen([str(exe)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.PIPE, bufsize=0)
        self.bugs = []
        threading.Thread(target=self._err, daemon=True).start()

    def _err(self):
        for line in self.p.stderr:
            t = line.decode("latin-1").rstrip()
            self.bugs.append(t)
            sys.stderr.write(t + "\n")

    def send(self, line):
        self.p.stdin.write((line + "\n").encode())
        self.p.stdin.flush()

    def read(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.p.stdout.read(n - len(buf))
            if not chunk:
                raise EOFError("simulator exited")
            buf += chunk
        return buf

    def readline(self):
        buf = b""
        while not buf.endswith(b"\n"):
            buf += self.read(1)
        return buf.decode("latin-1").strip()

    def close(self):
        self.p.stdin.close()
        rc = self.p.wait(10)
        return rc


class SerialTransport:
    def __init__(self, port=None):
        sys.path.insert(0, str(HERE.parent))           # tools/serialcap.py
        from serialcap import open_port
        self.s = open_port(port)
        self.s.timeout = 1
        time.sleep(0.3)
        self.s.reset_input_buffer()
        self.bugs = []

    def send(self, line):
        self.s.write((line + "\n").encode())

    def read(self, n):
        buf = b""
        end = time.time() + 10
        while len(buf) < n and time.time() < end:
            buf += self.s.read(n - len(buf))
        if len(buf) < n:
            raise TimeoutError(f"wanted {n} bytes, got {len(buf)}")
        return buf

    def readline(self, timeout=30.0):
        buf = b""
        end = time.time() + timeout
        while not buf.endswith(b"\n"):
            if time.time() > end:
                raise TimeoutError("no reply from device")
            buf += self.s.readline()
        return buf.decode("latin-1").strip()

    def close(self):
        self.s.close()
        return 0


class Driver:
    def __init__(self, t, ident="CHCS"):
        self.t = t
        self.ident = ident

    def expect(self, prefix, tries=50):
        for _ in range(tries):
            line = self.t.readline()
            if line.startswith(prefix):
                return line
        raise RuntimeError(f"never saw {prefix!r}")

    def cmd(self, line, prefix="OK"):
        self.t.send(line)
        return self.expect(prefix)

    def handshake(self, tries=20):
        # A freshly uploaded board may still be enumerating; replies sent
        # before the host opened the port are dropped, so ask until answered.
        for _ in range(tries):
            self.t.send("?")
            try:
                line = self.t.readline(1.0) if isinstance(self.t, SerialTransport) else self.t.readline()
                if line.startswith(self.ident):
                    return line
            except TimeoutError:
                time.sleep(0.3)
        raise RuntimeError("device never answered '?'")

    def frames(self, n):
        if n > 0:
            self.cmd(f"N {n}")

    def buttons(self, mask):
        self.cmd(f"K {mask:x}")

    def shot(self):
        self.t.send("S")
        hdr = self.expect("FB ")
        n = int(hdr.split()[2])
        return self.t.read(n)

    def run(self, script, outdir, scale=3):
        outdir = Path(outdir)
        outdir.mkdir(parents=True, exist_ok=True)
        self.handshake()
        self.cmd("L1")
        snaps = []
        for raw in Path(script).read_text().splitlines():
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            op, *args = line.split()
            if op == "wait":
                self.frames(int(args[0]))
            elif op == "tap":
                self.buttons(mask_of(args[0]))
                self.frames(int(args[1]) if len(args) > 1 else 3)
                self.buttons(0)
                self.frames(1)
            elif op == "hold":
                self.buttons(mask_of(args[0]))
            elif op == "release":
                self.buttons(0)
            elif op == "snap":
                im = to_image(self.shot(), scale)
                im.save(outdir / f"{args[0]}.png")
                snaps.append((args[0], im))
            elif op == "gif":
                name, count = args[0], int(args[1])
                every = int(args[2]) if len(args) > 2 else 1
                frames = []
                for _ in range(count):
                    frames.append(to_image(self.shot(), 2))
                    self.frames(every)
                frames[0].save(outdir / f"{name}.gif", save_all=True, append_images=frames[1:],
                               duration=int(1000 * every / 60), loop=0)
            elif op == "say":
                self.t.send(" ".join(args))
                for _ in range(50):
                    line = self.t.readline()
                    if line.startswith("LAPS"):
                        print(line)
                    if line.startswith("OK"):
                        break
                    if line.startswith("ERR"):
                        raise SystemExit(f"game refused: {raw.strip()}")
            elif op == "free":
                # Run in real time for N seconds (device timing is only
                # meaningful free-running), then return to lockstep.
                self.cmd("L0")
                time.sleep(float(args[0]))
                self.cmd("L1")
            elif op == "step":
                # One frame at a time, as the free-running game does (N k runs
                # up to three logic ticks per drawn frame, like a slow frame's
                # catch-up).
                for _ in range(int(args[0])):
                    self.frames(1)
            elif op == "cal":
                # Simulator: host time of the primitives the CHGfx benchmark
                # measured on the board -> device ns per host ns.
                self.t.send("Q")
                vals = [int(v) for v in self.expect("CAL").split()[1:]]
                self.expect("OK")
                device_us = [93, 4, 100, 246, 263]   # benchmark-results.txt, 12 bpp run
                ratios = [d * 1000.0 / max(h, 1) for d, h in zip(device_us, vals)]
                self.ratio = sum(ratios) / len(ratios)
                print(f"calibration: device/host = {self.ratio:.1f} (clear, hline, blit16, text24, circle: {[round(r) for r in ratios]})")
            elif op == "perf":
                line = self.cmd("P", "PERF")
                label = " ".join(args)
                kv = dict(f.split("=") for f in line.split()[1:] if "=" in f)
                if getattr(self, "ratio", None) and "pcrnd" in kv:
                    avg = int(kv["pcrnd"]) * self.ratio / 1e6
                    mx = int(kv["pcmax"]) * self.ratio / 1e6
                    print(f"perf {label}: est. device render avg {avg:.1f} ms, max {mx:.1f} ms ({kv['frames']} frames)")
                else:
                    print(line)
            elif op == "prof":
                print(self.cmd("T", "PROF"))
            else:
                raise SystemExit(f"bad script line: {raw}")
        if snaps:
            sh = sheet([im for _, im in snaps], cols=4)
            sh.save(outdir / "sheet.png")
        return snaps


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--sim", metavar="SKETCH")
    g.add_argument("--device", action="store_true")
    ap.add_argument("--port")
    ap.add_argument("--id", default="CHCS", help="handshake prefix the game answers '?' with")
    ap.add_argument("-D", dest="defines", action="append", default=[])
    ap.add_argument("script")
    ap.add_argument("outdir")
    a = ap.parse_args()
    if a.sim:
        from chsim import build
        t = SimTransport(build(a.sim, a.defines))
    else:
        t = SerialTransport(a.port)
    d = Driver(t, a.id)
    try:
        d.run(a.script, a.outdir)
    finally:
        if a.device:
            try:
                d.buttons(0)
                d.cmd("L0")
            except Exception:
                pass
        rc = t.close()
    bugs = [b for b in getattr(t, "bugs", []) if b.startswith("BUG")]
    if bugs:
        raise SystemExit(f"{len(bugs)} simulator bug report(s)")
    print(f"ok: {a.outdir}")
    return rc


if __name__ == "__main__":
    main()
