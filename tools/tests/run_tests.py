"""Build and run the host tests (engine port and game logic).

    python tools/tests/run_tests.py
"""
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(HERE.parent / "chsim"))
from chsim import find_cxx  # noqa: E402

# test_chess.cpp #includes the engine; the game logic is compiled beside it.
SOURCES = [HERE / "test_chess.cpp", ROOT / "src" / "game" / "Match.cpp"]


def main():
    exe = HERE / "build" / "test_chess.exe"
    exe.parent.mkdir(exist_ok=True)
    srcs = [str(s) for s in SOURCES if s.exists()]
    cmd = find_cxx() + ["-std=gnu++17", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
                        "-Wno-unused-function", "-Wno-unused-variable", "-Wno-unknown-pragmas", "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
                        "-DCHTEST", *srcs, "-o", str(exe)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit("build failed")
    if r.stderr.strip():
        sys.stderr.write(r.stderr)
    raise SystemExit(subprocess.run([str(exe)]).returncode)


if __name__ == "__main__":
    main()
