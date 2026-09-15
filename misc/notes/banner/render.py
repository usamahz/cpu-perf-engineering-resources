#!/usr/bin/env python3
"""Render a painting as a grid of glyphs coloured by the paint underneath.

    uv run --with pillow python3 render.py --out out.png [options]

Base image: the Met's CC0 scan of Hokusai's Under the Wave off Kanagawa
(object 45434, DP130155.jpg, 3859x2594). The banner is a crop of it.
"""
import argparse
import math
import random
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageEnhance, ImageFilter, ImageFont

HERE = Path(__file__).resolve().parent
SRC = HERE / "great-wave-met-DP130155.jpg"  # Met open access, CC0, object 45434
FONT_REGULAR = "/System/Library/Fonts/Menlo.ttc"

HEX = "0123456789abcdef"
# Tokens from the repository's own hot loops (benchmarks 02, 03, 07, 13).
ASM = (
    "fmla ldp stp csel fadd cmhi bic uaddw subs b.ne ldr str add mul sdot "
    "v0.4s v1.4s v2.4s v3.4s q17 q18 x8 x9 w9 s0 s8 lsl #4 #16 #32 "
    "cbz b.lo fmul dup movi ushr and orr eor prfm ret"
).split()


def sample_grid(img, cols, rows):
    """Mean colour per cell via a box-filtered resize."""
    small = img.resize((cols, rows), Image.Resampling.BOX)
    return small.load()


def lum(c):
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]


def boost(c, sat, gain, lift):
    r, g, b = c
    l = lum(c)
    r = l + (r - l) * sat
    g = l + (g - l) * sat
    b = l + (b - l) * sat
    out = []
    for v in (r, g, b):
        v = v * gain + lift
        out.append(max(0, min(255, int(v))))
    return tuple(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--width", type=int, default=3000)
    ap.add_argument("--height", type=int, default=1100)
    ap.add_argument("--crop", default="0.0,0.08,1.0,0.86",
                    help="fractional box x0,y0,x1,y1 of the source to use; aspect is forced to width:height around its centre")
    ap.add_argument("--cell", type=int, default=13, help="cell width in px; height is cell*1.25")
    ap.add_argument("--font-size", type=int, default=0, help="0 = fit the cell")
    ap.add_argument("--mode", choices=["hex", "asm", "mixed", "bits"], default="hex")
    ap.add_argument("--base", type=float, default=0.32, help="brightness of the paint under the glyphs, 0 = black")
    ap.add_argument("--base-hi", type=float, default=0.0,
                    help="if set, base brightness for the lightest paint (paper); --base then applies to the darkest ink, "
                         "interpolated on a blurred luminance so paper stays paper and ink stays ink")
    ap.add_argument("--base-blur", type=float, default=6.0, help="blur radius (px) of the luminance map used by --base-hi")
    ap.add_argument("--sat", type=float, default=1.15)
    ap.add_argument("--gain", type=float, default=1.10)
    ap.add_argument("--lift", type=float, default=6)
    ap.add_argument("--bold", action="store_true")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--jitter", type=float, default=0.0, help="random cell brightness jitter 0..1")
    ap.add_argument("--gap", type=int, default=1, help="px gap between rows (scanline look)")
    ap.add_argument("--dim-low", type=float, default=0.0,
                    help="dim glyphs whose cell luminance is below this (0..255) to keep dark water quiet")
    a = ap.parse_args()

    random.seed(a.seed)
    src = Image.open(SRC).convert("RGB")
    W, H = src.size
    x0, y0, x1, y1 = [float(v) for v in a.crop.split(",")]
    bx0, by0, bx1, by1 = x0 * W, y0 * H, x1 * W, y1 * H
    # force the banner aspect inside the requested box, centred
    want = a.width / a.height
    bw, bh = bx1 - bx0, by1 - by0
    if bw / bh > want:
        nw = bh * want
        bx0 += (bw - nw) / 2
        bx1 = bx0 + nw
    else:
        nh = bw / want
        by0 += (bh - nh) / 2
        by1 = by0 + nh
    crop = src.crop((int(bx0), int(by0), int(bx1), int(by1))).resize((a.width, a.height), Image.Resampling.LANCZOS)

    cw = a.cell
    ch = int(round(a.cell * 1.25))
    cols = a.width // cw
    rows = a.height // ch
    cells = sample_grid(crop, cols, rows)

    # base layer: the paint, darkened, so the picture reads through the grid
    if a.base_hi:
        # print feel: darken the ink more than the paper. factor = base + (base_hi - base) * L
        L = crop.convert("L").filter(ImageFilter.GaussianBlur(a.base_blur))
        lo = int(round(a.base * 255))
        hi = int(round(a.base_hi * 255))
        factor = L.point(lambda v: lo + (hi - lo) * v // 255)
        base = ImageChops.multiply(crop, Image.merge("RGB", (factor, factor, factor)))
    else:
        base = ImageEnhance.Brightness(crop).enhance(a.base)
    base = ImageEnhance.Color(base).enhance(1.05)
    img = base.copy()
    draw = ImageDraw.Draw(img)

    fsize = a.font_size or int(cw * 1.45)
    font = ImageFont.truetype(FONT_REGULAR, fsize, index=1 if a.bold else 0)

    # glyph streams: hex as a memory dump, asm as a listing
    hexstream = "".join(random.choice(HEX) for _ in range(cols * rows * 2))
    asmstream = " ".join(random.choice(ASM) for _ in range(cols * rows))
    bitstream = "".join(random.choice("01") for _ in range(cols * rows))

    hi = 0
    ai = 0
    for r in range(rows):
        y = r * ch + a.gap
        for c in range(cols):
            col = cells[c, r]
            L = lum(col)
            if a.mode == "hex":
                g = hexstream[hi]; hi += 1
            elif a.mode == "bits":
                g = bitstream[hi]; hi += 1
            elif a.mode == "asm":
                g = asmstream[ai] if ai < len(asmstream) else " "; ai += 1
            else:  # mixed: mnemonics on the bright foam, hex elsewhere
                if L > 150:
                    g = asmstream[ai] if ai < len(asmstream) else " "; ai += 1
                else:
                    g = hexstream[hi]; hi += 1
            if g == " ":
                continue
            colour = boost(col, a.sat, a.gain, a.lift)
            if a.dim_low and L < a.dim_low:
                f = 0.55 + 0.45 * (L / a.dim_low)
                colour = tuple(int(v * f) for v in colour)
            if a.jitter:
                f = 1.0 + random.uniform(-a.jitter, a.jitter)
                colour = tuple(max(0, min(255, int(v * f))) for v in colour)
            draw.text((c * cw + 1, y - 1), g, font=font, fill=colour)

    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.suffix.lower() in (".jpg", ".jpeg"):
        img.save(out, quality=88, optimize=True, subsampling=0)
    elif out.suffix.lower() == ".webp":
        img.save(out, quality=86, method=6)
    else:
        img.save(out, optimize=True)
    print(f"wrote {out} {img.size} cells {cols}x{rows} font {fsize}")


if __name__ == "__main__":
    main()
