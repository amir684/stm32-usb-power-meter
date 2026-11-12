"""Builds the screen renderer on the PC and writes the README screenshots to docs/screenshots.

The pages are drawn by the real code in src/main.cpp and the U8g2 C library, with sample data
from render.cpp, so the images match the LCD pixel for pixel.

Needs: a host g++ (on Windows the MinGW one PlatformIO installs is found automatically),
Pillow (pip install pillow), and one `pio run` first so the U8g2 sources are downloaded.

    python tools/screenshots/make.py [--gxx path/to/g++]
"""
import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
CLIB = ROOT / ".pio" / "libdeps" / "meter" / "U8g2" / "src" / "clib"
BUILD = HERE / "build"
OUT = ROOT / "docs" / "screenshots"

SCALE = 5                  # screen pixels to image pixels
MARGIN = 14
BACKGROUND = (226, 233, 240)
PIXEL_OFF = (214, 222, 232)
PIXEL_ON = (38, 42, 92)


def find_gxx(given):
    if given:
        return Path(given)
    pio = Path.home() / ".platformio" / "packages" / "toolchain-gccmingw32" / "bin" / "g++.exe"
    if pio.exists():
        return pio
    found = shutil.which("g++")
    if found:
        return Path(found)
    sys.exit("No g++ found: pass --gxx")


def run(cmd, env):
    result = subprocess.run([str(c) for c in cmd], env=env)
    if result.returncode:
        sys.exit(f"failed: {' '.join(str(c) for c in cmd[:3])} ...")


def build(gxx):
    if not CLIB.exists():
        sys.exit(f"{CLIB} is missing: run `pio run` once first")
    gcc = gxx.with_name(gxx.name.replace("g++", "gcc"))
    env = dict(os.environ)
    env["PATH"] = str(gxx.parent) + os.pathsep + env["PATH"]   # the compiler's own DLLs

    # The U8g2 C library, compiled once
    objects = BUILD / "u8g2"
    objects.mkdir(parents=True, exist_ok=True)
    for source in sorted(CLIB.glob("*.c")):
        target = objects / (source.stem + ".o")
        if not target.exists() or target.stat().st_mtime < source.stat().st_mtime:
            run([gcc, "-c", "-O1", "-std=gnu99", "-I", CLIB, source, "-o", target], env)

    # main.cpp next to the stand-in headers, so its #include "hw.h" finds those
    sources = BUILD / "src"
    sources.mkdir(parents=True, exist_ok=True)
    for shim in (HERE / "shim").iterdir():
        shutil.copy(shim, sources)
    shutil.copy(ROOT / "include" / "store.h", sources)
    shutil.copy(ROOT / "src" / "main.cpp", sources)

    exe = BUILD / "render.exe"
    run([gxx, "-std=gnu++11", "-O1", "-I", sources, "-I", CLIB, HERE / "render.cpp",
         *sorted(objects.glob("*.o")), "-o", exe, "-static"], env)
    return exe, env


def to_png(pbm, png):
    lines = pbm.read_text().split()
    width, height = int(lines[1]), int(lines[2])
    rows = lines[3:3 + height]
    image = Image.new("RGB", (width * SCALE + 2 * MARGIN, height * SCALE + 2 * MARGIN), BACKGROUND)
    pixels = image.load()
    for y, row in enumerate(rows):
        for x, bit in enumerate(row):
            color = PIXEL_ON if bit == "1" else PIXEL_OFF
            for dy in range(SCALE - 1):   # one image pixel of gap: the LCD dot grid
                for dx in range(SCALE - 1):
                    pixels[MARGIN + x * SCALE + dx, MARGIN + y * SCALE + dy] = color
    image.save(png, optimize=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--gxx")
    args = parser.parse_args()

    exe, env = build(find_gxx(args.gxx))
    frames = BUILD / "frames"
    frames.mkdir(exist_ok=True)
    run([exe, frames], env)

    OUT.mkdir(parents=True, exist_ok=True)
    for pbm in sorted(frames.glob("*.pbm")):
        png = OUT / (pbm.stem + ".png")
        to_png(pbm, png)
        print(f"wrote {png.relative_to(ROOT)}")


if __name__ == "__main__":
    main()
