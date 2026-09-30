"""Build, upload and drive CHChess on the attached CHGame.

    python tools/device.py build [--debug]         compile (release by default)
    python tools/device.py upload [--debug]        compile + upload
    python tools/device.py run SCRIPT OUTDIR       debug build, upload, run a chdrive script
    python tools/device.py shot OUT.png            screenshot of a running debug build

The game must be built with opt=osstd (the IDE's default "Smallest") and
periph=game (the default Peripherals setting of CHGame core 0.2.2+): it
does not fit in the 50,944-byte application region at -O2 or with the full
peripheral set. The debug protocol is enabled through build.extra_flags,
which is empty on this platform.
"""
import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SKETCH = HERE.parent
CHSIM = HERE / "chsim"
FQBN = "CHGame:ch32v:CHGame:opt=osstd,rtlib=nano,periph=game"
# CHChess needs link-time optimisation to fit. Board packages with the
# "Smallest + LTO" Optimize option (opt=oslto) do it from the menu; this flag
# does the same on any CHGame package (0.2.2 has no such option).
LTO = "-flto"


def build(debug):
    out = SKETCH / "build" / ("debug" if debug else "release")
    cmd = ["arduino-cli", "compile", "-b", FQBN, "--build-path", str(out)]
    flags = LTO + (" -DCHCH_DEBUG=1" if debug else "")
    cmd += ["--build-property", f"build.extra_flags={flags}"]
    cmd.append(str(SKETCH))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout[-3000:] + r.stderr[-3000:])
        raise SystemExit("compile failed")
    subprocess.run([sys.executable, str(HERE / "check_size.py"), str(out), "--top", "0"])
    return out


def upload(out, port=None):
    sys.path.insert(0, str(HERE))
    from serialcap import find_port
    port = port or find_port()
    if not port:
        raise SystemExit("no CHGame found on USB (VID 16C0:27DD): plug it in, or pass --port")
    r = subprocess.run(["arduino-cli", "upload", "-b", "CHGame:ch32v:CHGame", "-p", port,
                        "--input-dir", str(out), str(SKETCH)], capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("upload failed")
    print(r.stdout.strip().splitlines()[-1])


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("build", "upload"):
        p = sub.add_parser(name)
        p.add_argument("--debug", action="store_true")
        p.add_argument("--port")
    p = sub.add_parser("run")
    p.add_argument("script")
    p.add_argument("outdir")
    p.add_argument("--port")
    p = sub.add_parser("shot")
    p.add_argument("out")
    p.add_argument("--port")
    a = ap.parse_args()
    if a.cmd == "build":
        build(a.debug)
    elif a.cmd == "upload":
        upload(build(a.debug), a.port)
    elif a.cmd == "run":
        upload(build(True), a.port)
        cmd = [sys.executable, str(CHSIM / "chdrive.py"), "--device", a.script, a.outdir]
        if a.port:
            cmd[3:3] = ["--port", a.port]
        raise SystemExit(subprocess.run(cmd).returncode)
    elif a.cmd == "shot":
        sys.path.insert(0, str(CHSIM))
        from chdrive import Driver, SerialTransport
        from fbimage import to_image
        d = Driver(SerialTransport(a.port))
        d.handshake()
        to_image(d.shot(), 3).save(a.out)
        print(a.out)


if __name__ == "__main__":
    main()
