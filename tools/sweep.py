#!/usr/bin/env python3
"""Render every parameter at two settings and fail if any made no difference.

**This is the only thing in the repo that catches a dead control**, and it is
not a theoretical risk. A GLSL uniform whose name does not match the C++ is
silently ignored -- `glGetUniformLocation` returns -1 and `glUniform` on -1 is a
documented no-op -- so a slider can be stone dead while everything compiles,
links, loads and renders.

None of `fbtest`'s checks would notice. `--frames`, `--copies`, `--aspect`,
`--seam` and `--key` each exercise one configuration and assert the picture is
right *in it*; a control that never reaches the shader makes every one of them
agree perfectly on the wrong thing.

## Two things this file exists to get right

**Most parameters do nothing in the default configuration, correctly.** Spread
and Stagger need more than one copy. Seed needs Scatter. Tolerance needs a key
mode that is not Alpha. Phase needs Manual sync. Every one of those is a false
failure waiting to happen, and `CONTEXT` below is what stops it: each entry is
the setting that gives the parameter under test something to do.

**Two settings can be the same picture.** Rotation is the trap here: the range
is -180 to +180 degrees, so sweeping the slider from 0.0 to 1.0 renders the
sprite at -180 and then at +180, which is the same frame to the last bit. A
sweep of the obvious two ends therefore reports a perfectly working Rotation as
dead. `ENDS` overrides the pair for every parameter where the two ends are not
two pictures.

Usage::

    tools/sweep.py [--build BUILD_DIR] [--verbose]
"""

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile
import zlib

REPO = pathlib.Path(__file__).resolve().parent.parent

# Parameters that only do anything on the effect bundle, so they are swept with
# `--effect` (which renders over a synthetic clip) rather than against the
# source, where they are correctly inert.
EFFECT_ONLY = {"Mix", "Sheet From"}

# Applied to every render. Native fit at the default scale puts a 100 px cell on
# screen at about 2.7x, which is the magnification Sampling needs to be visible
# at all -- nearest and bilinear agree exactly at 1:1.
BASE = [
    "Fit=0",
    "Scale=0.5",
]

# The setting that gives a parameter something to do. Without these the sweep
# would report a dozen working controls as dead, and be right to.
CONTEXT = {
    # Phase is read only under Manual sync; under Free the clock wins.
    "Phase": ["Sync=3"],
    # Rate is per-frame arithmetic on the clock, so Manual sync makes it inert.
    "Rate": ["Sync=0"],
    # Loop and Once are the same picture whenever the clock happens to land on
    # the last frame of the run -- Loop wraps to it and Once clamps to it, and
    # they agree exactly. With the whole 20-cell sheet and the default rate the
    # sweep's fixed 6.25 s does precisely that, and reported a working Mode as
    # dead. A run length that does not divide the clock's landing point is the
    # fix, and it is why this entry exists rather than a different --time: any
    # single time is one coincidence away from the same false failure.
    "Mode": ["Frame Count=7"],
    # A field of copies, or Spread, Stagger and Arrange have nothing to arrange.
    "Arrange": ["Copies=16"],
    "Spread": ["Copies=16"],
    "Stagger": ["Copies=16"],
    # Seed feeds a hash that only Scatter consumes.
    "Seed": ["Copies=16", "Arrange=2"],
    # The key colour and its tolerance are ignored under Alpha and None.
    "Key Colour": ["Key=2", "Tolerance=0.7"],
    "Key Colour_Green": ["Key=2", "Tolerance=0.7"],
    "Key Colour_Blue": ["Key=2", "Tolerance=0.7"],
    "Tolerance": ["Key=2"],
    "Softness": ["Key=2", "Tolerance=0.5"],
    # Key itself is swept Alpha against None, which are the same picture on an
    # opaque cell. Cell 0 of the test sheet is the half-alpha one, so the sweep
    # is pinned to it.
    "Key": ["Sync=3", "Phase=0.0", "Start Frame=0", "Frame Count=1"],
    # The background colour is invisible while the background is transparent.
    "Background": ["Background Alpha=1.0"],
    "Background_Green": ["Background Alpha=1.0"],
    "Background_Blue": ["Background Alpha=1.0"],
    "Background Alpha": ["Background=1.0", "Background_Green=0.5"],
}

# Where the two obvious ends are not two pictures, or are not both legal.
ENDS = {
    # -180 and +180 degrees are the same rotation. Zero against a quarter turn.
    "Rotation": ("0.5", "0.75"),
    # Integer parameters: 0 is outside the range of most of them, and the top of
    # the range is rarely a useful picture.
    "Columns": ("1", "5"),
    "Rows": ("1", "4"),
    "Start Frame": ("0", "7"),
    "Frame Count": ("0", "3"),
    "Copies": ("1", "9"),
    # Dropdowns whose 0 and 1 are two adjacent entries; sweep the whole range.
    "Sync": ("0", "3"),
    "Mode": ("0", "2"),
    "Fit": ("0", "2"),
    "Arrange": ("0", "2"),
    "Key": ("0", "3"),
    "Sampling": ("0", "1"),
    "Sheet From": ("0", "1"),
    "Preset": ("0", "3"),
}


def read_png(path):
    """Decode a PNG to raw bytes. Enough of the format for our own writer's
    output -- 8-bit RGBA, one IDAT, filter 0 on every row."""
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("not a PNG")

    pos = 8
    width = height = 0
    idat = b""
    while pos < len(data):
        length = int.from_bytes(data[pos:pos + 4], "big")
        kind = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
        elif kind == b"IDAT":
            idat += body
        pos += 12 + length

    raw = zlib.decompress(idat)
    stride = width * 4
    out = bytearray()
    for y in range(height):
        start = y * (stride + 1)
        if raw[start] != 0:
            raise ValueError("unexpected PNG filter")
        out += raw[start + 1:start + 1 + stride]
    return width, height, bytes(out)


def difference(a, b):
    """Mean absolute difference per channel, 0..255."""
    if len(a) != len(b):
        return 255.0
    return sum(abs(x - y) for x, y in zip(a, b)) / len(a)


def render(fbtest, out, sheet, settings, extra, effect=False):
    args = [str(fbtest), "--out", str(out), "--size", "480x270", "--time", "6.25",
            "--file", str(sheet), "--columns", "5", "--rows", "4"]
    if effect:
        args.append("--effect")
    for setting in BASE + extra + settings:
        args += ["--set", setting]
    result = subprocess.run(args, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"fbtest failed: {result.stderr.strip() or result.stdout.strip()}")
    return read_png(out)[2]


def parameters(fbtest):
    result = subprocess.run([str(fbtest), "--list"], capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError(f"fbtest --list failed: {result.stderr.strip()}")

    found = []
    for line in result.stdout.splitlines():
        # `--list` pads its columns, so two or more spaces is the separator.
        # Splitting on single whitespace would break every parameter whose name
        # contains a space, which is most of them.
        parts = re.split(r"\s{2,}", line.strip())
        if len(parts) < 3 or not parts[0].isdigit():
            continue
        found.append((parts[1].strip(), parts[2].strip()))
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build", type=pathlib.Path, default=REPO / "build")
    parser.add_argument("--verbose", action="store_true")
    args = parser.parse_args()

    fbtest = args.build / "fbtest"
    if not fbtest.exists():
        print(f"{fbtest} not found -- build with -DFLIPBOOK_BUILD_TOOLS=ON",
              file=sys.stderr)
        return 1

    found = parameters(fbtest)
    if not found:
        print("fbtest --list returned no parameters", file=sys.stderr)
        return 1

    dead = []
    checked = 0

    with tempfile.TemporaryDirectory() as scratch:
        scratch = pathlib.Path(scratch)

        # The DETAILED sheet, not the flat one. Flip is invisible on a flat cell
        # because a flat square is its own mirror image, and Sampling is
        # invisible because nearest and bilinear agree exactly inside a region
        # of constant colour -- so the flat sheet would report both as dead.
        sheet = scratch / "sheet.png"
        subprocess.run([str(fbtest), "--make-sheet", str(sheet), "--detail"],
                       capture_output=True, text=True, check=True)

        low = scratch / "low.png"
        high = scratch / "high.png"

        for name, kind in found:
            # An event has no value to sweep. It is momentary -- the plugin
            # zeroes it the instant it arrives -- so two renders of it are two
            # renders of nothing, and it would report as dead every time.
            # Reset's effect is on the clock base, which `--frames` covers.
            if kind == "event":
                if args.verbose:
                    print(f"  skip {name:22s} (an event has no value to sweep)")
                continue

            # The About block's credit line is a text parameter. The host shows
            # it as a labelled value and it never reaches a pixel, so sweeping
            # it as a float reports it dead every time and buries a real one.
            if kind == "text":
                if args.verbose:
                    print(f"  skip {name:22s} (a text label, not a value)")
                continue

            if kind == "file":
                # Swept as "a sheet that loads" against "a path that does not",
                # which exercises the failure path rather than only the happy
                # one. The name is not in kNames, so it goes through --file.
                before = render(fbtest, low, sheet, [], [])
                missing = scratch / "nonexistent.png"
                after = render(fbtest, high, missing, [], [])
                delta = difference(before, after)
                checked += 1
                if delta < 0.01:
                    dead.append((name, delta))
                    print(f"  DEAD {name:22s} a loaded sheet and a missing one render the same")
                elif args.verbose:
                    print(f"  ok   {name:22s} mean delta {delta:.3f}")
                continue

            ends = ENDS.get(name, ("0.0", "1.0"))
            extra = CONTEXT.get(name, [])
            effect = name in EFFECT_ONLY

            before = render(fbtest, low, sheet, [f"{name}={ends[0]}"], extra, effect)
            after = render(fbtest, high, sheet, [f"{name}={ends[1]}"], extra, effect)
            delta = difference(before, after)
            checked += 1

            if delta < 0.01:
                dead.append((name, delta))
                print(f"  DEAD {name:22s} {ends[0]} and {ends[1]} render identically")
            elif args.verbose:
                print(f"  ok   {name:22s} mean delta {delta:.3f}")

    print(f"{checked} parameters swept, {len(dead)} dead")
    if dead:
        print("\nA parameter that changes nothing is usually a uniform name that does not\n"
              "match the C++, which is silent everywhere else. Before assuming that, check\n"
              "whether it needs a CONTEXT entry -- most of these controls are inert in the\n"
              "default configuration on purpose.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
