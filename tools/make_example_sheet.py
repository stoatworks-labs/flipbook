#!/usr/bin/env python3
"""Write `docs/example-sheet.png`: an original 5x4 sprite sheet, so the plugin
has something to play the moment it is installed.

It exists for two reasons beyond looking nice in the README.

**Nothing else in the repo is a sheet anyone would want to look at.** `fbtest
--make-sheet` writes flat colour blocks, which is exactly right for an assertion
and useless as a first impression: somebody who installs the plugin and cannot
find a sheet within ten seconds concludes it does not work.

**Licensing.** Every sprite sheet lying around is somebody's artwork under
somebody's terms. This one is arithmetic, generated here, and carries the
repository's licence like the rest of the source.

The animation is a burst: a ring of shards thrown outward from the centre,
fading and spinning as they go, over transparent. Twenty frames, 128 px square
cells, straight alpha — the case the Alpha key handles and most exported sheets
actually are.

    tools/make_example_sheet.py [--out docs/example-sheet.png]
"""

import argparse
import math
import pathlib
import struct
import zlib

COLUMNS = 5
ROWS = 4
FRAMES = COLUMNS * ROWS
CELL = 128
SHARDS = 14


def write_png(path, width, height, rgba):
    """A PNG writer, so this script needs nothing installed. 8-bit RGBA, one
    IDAT, filter 0 on every row -- the same subset `fbtest` writes and reads."""
    raw = bytearray()
    stride = width * 4
    for y in range(height):
        raw.append(0)
        raw += rgba[y * stride:(y + 1) * stride]

    def chunk(kind, body):
        return (struct.pack(">I", len(body)) + kind + body
                + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF))

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
           + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))
    path.write_bytes(png)


def shard_colour(t, heat):
    """White-hot at the centre of the burst, through orange, to a dim ember.

    `t` is the frame's progress 0..1 and `heat` is the shard's own 0..1, so no
    two shards cool at quite the same moment -- a burst whose pieces all change
    colour together reads as a palette swap rather than as combustion.
    """
    cool = min(1.0, t * 1.35 + heat * 0.25)
    r = 1.0
    g = max(0.0, 0.95 - cool * 0.75)
    b = max(0.0, 0.75 - cool * 1.5)
    return r, g, b


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parent.parent / "docs" / "example-sheet.png")
    args = parser.parse_args()

    width = COLUMNS * CELL
    height = ROWS * CELL
    rgba = bytearray(width * height * 4)

    for frame in range(FRAMES):
        t = frame / (FRAMES - 1)
        ox = (frame % COLUMNS) * CELL
        oy = (frame // COLUMNS) * CELL

        # Eased outward so the burst leaves fast and drifts to a stop, which is
        # what an explosion does and what a linear expansion conspicuously does
        # not.
        travel = 1.0 - (1.0 - t) ** 2.2
        fade = 1.0 if t < 0.45 else max(0.0, 1.0 - (t - 0.45) / 0.55)

        for y in range(CELL):
            for x in range(CELL):
                px = (x - CELL / 2 + 0.5) / (CELL / 2)
                py = (y - CELL / 2 + 0.5) / (CELL / 2)

                acc_r = acc_g = acc_b = acc_a = 0.0

                # The core: bright early, gone by the middle of the run.
                core = max(0.0, 1.0 - t * 2.6)
                if core > 0.0:
                    d = math.hypot(px, py) / (0.12 + travel * 0.30)
                    glow = max(0.0, 1.0 - d * d) ** 1.5
                    a = glow * core
                    if a > 0.0:
                        acc_r += 1.0 * a
                        acc_g += (0.92 - t * 0.4) * a
                        acc_b += (0.70 - t * 0.9) * a
                        acc_a += a

                for i in range(SHARDS):
                    # Evenly around the circle, then jittered. The golden angle
                    # on its own -- the obvious choice, and what this was first
                    # -- distributes points evenly over *many* iterations, and
                    # fourteen is not many: it left a visible bite out of one
                    # side of every frame. Even spacing plus an irrational
                    # jitter gives a closed ring that is still not regular.
                    angle = i * (2.0 * math.pi / SHARDS) + ((i * 0.6180339887) % 1.0) * 0.22
                    reach = 0.30 + ((i * 0.6180339887) % 1.0) * 0.55
                    size = 0.10 + ((i * 0.4142135) % 1.0) * 0.10
                    heat = (i * 0.7548776) % 1.0

                    cx = math.cos(angle) * reach * travel
                    cy = math.sin(angle) * reach * travel

                    dx = px - cx
                    dy = py - cy
                    radius = size * (1.0 - travel * 0.45)
                    if radius <= 0.0:
                        continue

                    d = math.hypot(dx, dy) / radius
                    if d >= 1.0:
                        continue

                    a = (1.0 - d * d) ** 1.2 * fade
                    r, g, b = shard_colour(t, heat)
                    acc_r += r * a
                    acc_g += g * a
                    acc_b += b * a
                    acc_a += a

                if acc_a <= 0.0:
                    continue

                # Back to straight alpha: the accumulation above is additive, so
                # the colour has to be divided back out by the coverage. Leaving
                # it premultiplied here would make the sheet look right in a
                # viewer and wrong through the plugin, which reads straight
                # alpha on purpose (see Sheet.h).
                alpha = min(1.0, acc_a)
                inv = 1.0 / acc_a
                offset = ((oy + y) * width + ox + x) * 4
                rgba[offset + 0] = min(255, int(acc_r * inv * 255 + 0.5))
                rgba[offset + 1] = min(255, int(acc_g * inv * 255 + 0.5))
                rgba[offset + 2] = min(255, int(acc_b * inv * 255 + 0.5))
                rgba[offset + 3] = int(alpha * 255 + 0.5)

    args.out.parent.mkdir(parents=True, exist_ok=True)
    write_png(args.out, width, height, bytes(rgba))
    print(f"wrote {args.out} ({width} x {height}, {COLUMNS} x {ROWS} cells of {CELL} px)")


if __name__ == "__main__":
    main()
