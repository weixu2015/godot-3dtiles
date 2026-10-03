"""Locate *where* two PNGs differ, by decoding both properly and diffing.

Why this exists: an intensity sweep produced three PNGs with different hashes and different file
sizes, yet every audit number was bit-identical. The audit samples a dozen points; the render
changed somewhere it does not look. Guessing at that is how a tuning pass converges on the wrong
knob, so this answers it directly: which pixels moved, by how much, and where they are.

A hand-rolled decoder that silently returns zeros is the failure mode to avoid here - it reads as
"the images are identical" or "everything changed" with total confidence. This one decodes the
real zlib stream, undoes all five PNG filter types, and reports the header it parsed so a wrong
parse is visible in the output rather than in the conclusion.

Usage: png_diff.py <a.png> <b.png> [--grid 24] [--top 12]
"""

from __future__ import annotations

import struct
import sys
import zlib
from pathlib import Path


def read_png(path: Path) -> tuple[int, int, list[list[tuple[int, int, int, int]]]]:
    """Decode an 8-bit RGB/RGBA non-interlaced PNG into rows of pixels.

    Raises on anything it does not implement, so an unsupported variant is an error rather than a
    plausible-looking wrong answer.
    """
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path.name}: not a PNG (bad signature)")

    pos = 8
    width = height = bit_depth = color_type = interlace = -1
    idat = bytearray()
    palette: list[tuple[int, int, int]] = []

    while pos < len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        ctype = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        pos += 12 + length  # length + type + body + CRC

        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _comp, _filt, interlace = struct.unpack(
                ">IIBBBBB", body
            )
        elif ctype == b"PLTE":
            palette = [
                (body[i], body[i + 1], body[i + 2]) for i in range(0, len(body), 3)
            ]
        elif ctype == b"IDAT":
            idat += body
        elif ctype == b"IEND":
            break

    if bit_depth != 8:
        raise ValueError(f"{path.name}: bit depth {bit_depth} unsupported (need 8)")
    if interlace != 0:
        raise ValueError(f"{path.name}: interlaced, unsupported")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}.get(color_type)
    if channels is None:
        raise ValueError(f"{path.name}: colour type {color_type} unsupported")

    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    if len(raw) != (stride + 1) * height:
        raise ValueError(
            f"{path.name}: decompressed {len(raw)} B, expected {(stride + 1) * height}"
        )

    rows: list[list[tuple[int, int, int, int]]] = []
    prev = bytearray(stride)
    off = 0
    for _ in range(height):
        ftype = raw[off]
        line = bytearray(raw[off + 1 : off + 1 + stride])
        off += 1 + stride
        # Undo the per-scanline PNG filter. bpp is the filter's byte-per-pixel stride.
        bpp = max(channels, 1)
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            x = line[i]
            if ftype == 0:
                pass
            elif ftype == 1:
                x += a
            elif ftype == 2:
                x += b
            elif ftype == 3:
                x += (a + b) // 2
            elif ftype == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                x += a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            else:
                raise ValueError(f"{path.name}: unknown filter type {ftype}")
            line[i] = x & 0xFF
        prev = line

        row: list[tuple[int, int, int, int]] = []
        for x in range(width):
            px = line[x * channels : (x + 1) * channels]
            if color_type == 0:
                row.append((px[0], px[0], px[0], 255))
            elif color_type == 4:
                row.append((px[0], px[0], px[0], px[1]))
            elif color_type == 3:
                r, g, b = palette[px[0]]
                row.append((r, g, b, 255))
            else:
                row.append((px[0], px[1], px[2], px[3] if channels == 4 else 255))
        rows.append(row)

    return width, height, rows


def main(argv: list[str]) -> int:
    args = [a for a in argv[1:] if not a.startswith("--")]
    flags = [a for a in argv[1:] if a.startswith("--")]

    def flag(name: str, default: int) -> int:
        for f in flags:
            if f.startswith(f"--{name}="):
                return int(f.split("=", 1)[1])
        return default

    if len(args) < 2:
        print(__doc__)
        return 2

    grid = flag("grid", 24)
    top_n = flag("top", 12)

    wa, ha, a = read_png(Path(args[0]))
    wb, hb, b = read_png(Path(args[1]))
    print(f"A {args[0]}  {wa}x{ha}")
    print(f"B {args[1]}  {wb}x{hb}")
    if (wa, ha) != (wb, hb):
        print("size mismatch - cannot diff")
        return 1

    changed = 0
    total = 0
    max_delta = 0
    max_at = (0, 0)
    cells: dict[tuple[int, int], list[int]] = {}
    for y in range(ha):
        for x in range(wa):
            pa, pb = a[y][x], b[y][x]
            d = max(abs(pa[i] - pb[i]) for i in range(3))
            total += 1
            if d:
                changed += 1
                cell = (y * grid // ha, x * grid // wa)
                bucket = cells.setdefault(cell, [0, 0])
                bucket[0] += 1
                bucket[1] = max(bucket[1], d)
                if d > max_delta:
                    max_delta, max_at = d, (x, y)

    print()
    print(f"changed pixels: {changed} / {total}  ({100.0 * changed / total:.3f}%)")
    print(f"max channel delta: {max_delta} at {max_at}")

    ranked = sorted(cells.items(), key=lambda kv: (-kv[1][1], -kv[1][0]))
    if ranked:
        print()
        print(f"changed regions, {grid}x{grid} grid, ranked by peak delta:")
        for (cy, cx), (n, peak) in ranked[:top_n]:
            x0, x1 = cx * wa // grid, (cx + 1) * wa // grid
            y0, y1 = cy * ha // grid, (cy + 1) * ha // grid
            print(
                f"  cell({cy:2d},{cx:2d})  px=({x0:4d}..{x1:4d},{y0:4d}..{y1:4d})"
                f"  n={n:6d}  peak={peak:3d}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
