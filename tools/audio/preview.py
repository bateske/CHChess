"""Render CHBlackjack's music and effects to WAV on the PC, from the real code.

    python tools/audio/preview.py OUTDIR [--src DIR]

Compiles src/audio/Audio.cpp + Music.cpp (or the copies in --src) with a model
of the piezo timer (tools/audio/host), then writes one WAV per song and sound
mode and one per effect. For each it prints how often a sounding tone was cut
off mid-cycle and restarted (audible clicks) and a hash of the pin waveform,
so two versions of the code can be compared exactly.
"""
import argparse
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE.parent / "chsim"))
from chsim import find_cxx  # noqa: E402

SONGS = ["title", "victory", "broke"]
SONG_MS = [15000, 4000, 12000]
MODES = {1: "arpeggio", 2: "lead"}
SFX = ["deal", "flip", "chip", "cursor", "select", "deny", "win", "blackjack", "bust", "push",
       "lose", "peek", "shuffle", "coin", "split", "double", "insurance", "broke", "reveal", "whoosh"]


def build(src, out):
    exe = out / "harness.exe"
    cmd = find_cxx() + ["-std=gnu++17", "-O2", "-w", f"-I{HERE / 'host'}",
                        str(HERE / "host" / "harness.cpp"),
                        str(src / "Audio.cpp"), str(src / "Music.cpp"), "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("build failed")
    return exe


def run(exe, wav, mode, kind, index, ms, log=None):
    args = [str(exe), str(wav), str(mode), kind, str(index), str(ms)] + ([str(log)] if log else [])
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(r.stderr)
    return r.stdout.strip()


def main():
    p = argparse.ArgumentParser()
    p.add_argument("outdir")
    p.add_argument("--src", default=str(ROOT / "src" / "audio"))
    a = p.parse_args()
    out = Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    exe = build(Path(a.src), out)
    for i, name in enumerate(SONGS):
        for mode, mname in MODES.items():
            stem = f"music_{name}_{mname}"
            print(f"{stem:28s}", run(exe, out / f"{stem}.wav", mode, "song", i, SONG_MS[i], out / f"{stem}.log"))
    for i, name in enumerate(SFX):
        stem = f"sfx_{name}"
        print(f"{stem:28s}", run(exe, out / f"{stem}.wav", 1, "sfx", i, 2500))


if __name__ == "__main__":
    main()
