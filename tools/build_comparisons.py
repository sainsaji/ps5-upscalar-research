#!/usr/bin/env python3
"""Build every comparison image and metrics table from the full-resolution
captures, losslessly.

Input  comparisons/<set>/full/<mode>.png   3840x2160 scanout captures
Output comparisons/<set>/crop_<mode>.png  1:1 crop of the most detailed window
       comparisons/<set>/strip.png        those crops side by side
       comparisons/<set>/zoom_<mode>.png  3x nearest-neighbour zoom of one mode, 720x405 -
                                          narrow enough for GitHub to show 1:1
       comparisons/<set>/zoom.png         all zooms on one sheet
       comparisons/<set>/grid.png         network x (Off | AI | Sharp) grid
       comparisons/<set>/metrics.md       difference statistics

Everything is PNG (lossless). Crops are pixel-exact copies of the capture and
zooms use nearest-neighbour, so no image here is resampled or recompressed.

    python tools/build_comparisons.py                  # all sets
    python tools/build_comparisons.py bbb-540p-web     # one set
    python tools/build_comparisons.py --import-bmp DIR SET   # BMPs from upcompare
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont, ImageStat

ROOT = Path(__file__).resolve().parents[1]
COMP = ROOT / "comparisons"

# Capture file name -> display label. Order is the order images appear in.
MODES = {
    "off": "Off (bilinear)",
    "sharp": "Sharp (FSR1)",
    "ai-standard": "AI Standard (Anime4K S)",
    "ai-large": "AI Large (Anime4K M)",
    "ai-maximum": "AI Maximum (Anime4K UL)",
}
BMP_NAMES = {"off": "off", "sharp": "sharp", "ai": "ai-standard",
             "ai_large": "ai-large", "ai_max": "ai-maximum"}

CROP_W, CROP_H = 640, 360     # 1:1 crop
ZOOM_W, ZOOM_H = 240, 135     # region shown at 3x in zoom.png


def font(size: int):
    for name in ("arialbd.ttf", "Arial Bold.ttf", "DejaVuSans-Bold.ttf", "LiberationSans-Bold.ttf"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default(size=size)


def busiest_window(img: Image.Image, w: int, h: int) -> tuple[int, int]:
    """Top-left of the w x h window with the most edge energy - a flat or
    out-of-focus area shows no upscaler at all."""
    edges = np.asarray(img.convert("L").filter(ImageFilter.FIND_EDGES), np.float64)
    ii = edges.cumsum(0).cumsum(1)
    ii = np.pad(ii, ((1, 0), (1, 0)))
    H, W = edges.shape
    best, pos = -1.0, (0, 0)
    for y in range(0, H - h + 1, h // 6):
        for x in range(0, W - w + 1, w // 6):
            s = ii[y + h, x + w] - ii[y, x + w] - ii[y + h, x] + ii[y, x]
            if s > best:
                best, pos = s, (x, y)
    return pos


def labelled(tiles: list[tuple[str, Image.Image]], cols: int, pad: int = 12,
             head: int = 44) -> Image.Image:
    tw, th = tiles[0][1].size
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new("RGB", (cols * tw + (cols + 1) * pad, rows * (th + head + pad) + pad),
                      (18, 18, 20))
    d = ImageDraw.Draw(sheet)
    f = font(24)
    for i, (label, im) in enumerate(tiles):
        x = pad + (i % cols) * (tw + pad)
        y = pad + (i // cols) * (th + head + pad)
        d.text((x + 4, y + 8), label, fill=(235, 235, 235), font=f)
        sheet.paste(im, (x, y + head))
    return sheet


def diff_stats(a: np.ndarray, b: np.ndarray) -> tuple[float, float]:
    d = np.abs(a.astype(np.int16) - b.astype(np.int16))
    return float(d.mean()), float(100.0 * (d.max(-1) > 8).mean())


def build(set_dir: Path) -> None:
    full = set_dir / "full"
    modes = [m for m in MODES if (full / f"{m}.png").exists()]
    imgs = {m: Image.open(full / f"{m}.png").convert("RGB") for m in modes}
    info = json.loads((set_dir / "set.json").read_text()) if (set_dir / "set.json").exists() else {}

    x, y = busiest_window(imgs["off"], CROP_W, CROP_H)
    crops = {m: imgs[m].crop((x, y, x + CROP_W, y + CROP_H)) for m in modes}
    for m, c in crops.items():
        c.save(set_dir / f"crop_{m}.png", optimize=True)
    labelled([(MODES[m], crops[m]) for m in modes], cols=len(modes)).save(
        set_dir / "strip.png", optimize=True)

    # 3x nearest-neighbour zoom of the busiest sub-window of the crop.
    zx, zy = busiest_window(crops["off"], ZOOM_W, ZOOM_H)
    zooms = [(MODES[m], crops[m].crop((zx, zy, zx + ZOOM_W, zy + ZOOM_H))
              .resize((ZOOM_W * 3, ZOOM_H * 3), Image.NEAREST)) for m in modes]
    labelled(zooms, cols=min(3, len(zooms))).save(set_dir / "zoom.png", optimize=True)
    # One file per mode: GitHub scales anything wider than its ~830 px column,
    # which blurs a sheet. 720 px wide renders pixel for pixel.
    for m, (_, z) in zip(modes, zooms):
        z.save(set_dir / f"zoom_{m}.png", optimize=True)

    # Grid: one row per AI network present, columns Off | AI | Sharp.
    nets = [m for m in ("ai-standard", "ai-large", "ai-maximum") if m in modes]
    tiles = []
    for n in nets:
        tiles += [(f"Off", crops["off"]), (MODES[n], crops[n]), ("Sharp (FSR1)", crops["sharp"])]
    labelled(tiles, cols=3).save(set_dir / "grid.png", optimize=True)

    arr = {m: np.asarray(imgs[m]) for m in modes}
    lines = [f"# {info.get('title', set_dir.name)} - difference statistics", "",
             f"Full frame, {imgs['off'].size[0]}x{imgs['off'].size[1]}. "
             f"Crop window: x={x} y={y} {CROP_W}x{CROP_H}.", "",
             "| Mode | Mean abs diff vs Off (0-255) | Pixels changed by > 8 |",
             "|---|---|---|"]
    for m in modes[1:]:
        md, ch = diff_stats(arr[m], arr["off"])
        lines.append(f"| {MODES[m]} | {md:.2f} | {ch:.1f}% |")
    if len(nets) > 1:
        lines += ["", "| Network pair | Mean abs diff | Pixels changed by > 8 |", "|---|---|---|"]
        for i in range(len(nets)):
            for j in range(i + 1, len(nets)):
                md, ch = diff_stats(arr[nets[j]], arr[nets[i]])
                lines.append(f"| {MODES[nets[j]]} vs {MODES[nets[i]]} | {md:.2f} | {ch:.2f}% |")
    (set_dir / "metrics.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"  {set_dir.name}: {len(modes)} modes, crop at {x},{y}")


def import_bmp(src: Path, name: str) -> None:
    out = COMP / name / "full"
    out.mkdir(parents=True, exist_ok=True)
    for bmp, mode in BMP_NAMES.items():
        p = src / f"{bmp}.bmp"
        if p.exists():
            Image.open(p).convert("RGB").save(out / f"{mode}.png", optimize=True)
            print(f"  {p.name} -> {name}/full/{mode}.png")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("sets", nargs="*")
    ap.add_argument("--import-bmp", nargs=2, metavar=("DIR", "SET"))
    a = ap.parse_args()
    if a.import_bmp:
        import_bmp(Path(a.import_bmp[0]), a.import_bmp[1])
        return 0
    sets = [COMP / s for s in a.sets] or sorted(p.parent for p in COMP.glob("*/full"))
    for s in sets:
        build(s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
