"""Render CHChess's sound effects to WAV on the PC, from the real code.

    python tools/audio/preview.py OUTDIR [--src DIR]

Compiles src/audio/Audio.cpp (or the copy in --src) with a model of the
piezo timer (tools/audio/host), then writes one WAV per effect. For each it
prints how often a sounding tone was cut off mid-cycle and restarted
(audible clicks) and a hash of the pin waveform, so two versions of the code
can be compared exactly.
"""
import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE.parent / "chsim"))
from chsim import find_cxx  # noqa: E402

# Must match enum class Sfx in src/audio/Audio.h.
SFX = ["cursor", "select", "deny", "land", "hop", "capture", "coin", "check", "castle", "promote",
       "whoosh", "flip", "mate", "win", "lose", "draw", "turn", "title", "tick", "tock"]


def build(src, out):
    exe = out / "harness.exe"
    cmd = find_cxx() + ["-std=gnu++17", "-O2", "-w", f"-I{HERE / 'host'}",
                        str(HERE / "host" / "harness.cpp"), str(src / "Audio.cpp"), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("build failed")
    return exe


def main():
    p = argparse.ArgumentParser()
    p.add_argument("outdir")
    p.add_argument("--src", default=str(ROOT / "src" / "audio"))
    a = p.parse_args()
    out = Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    exe = build(Path(a.src), out)
    for i, name in enumerate(SFX):
        stem = f"sfx_{name}"
        r = subprocess.run([str(exe), str(out / f"{stem}.wav"), str(i), "2500"], capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(r.stderr)
        print(f"{stem:18s}", r.stdout.strip())


if __name__ == "__main__":
    main()
