#!/usr/bin/env python3
"""Render the LX suite SVG to the PNG sizes used by Qt and Linux packages.

Requires CairoSVG: python3 -m pip install cairosvg
"""
from pathlib import Path

import cairosvg

ICON_DIR = Path(__file__).resolve().parent
for size in (16, 32, 48, 64, 128, 256, 512, 1024):
    cairosvg.svg2png(
        url=str(ICON_DIR / "compositor-lx.svg"),
        write_to=str(ICON_DIR / f"compositor-lx-{size}.png"),
        output_width=size,
        output_height=size,
    )
