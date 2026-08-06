#!/usr/bin/env python3
"""Convert a little-endian RGB565 framebuffer dump to PNG."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

from PIL import Image


def rgb565_le_to_rgb(data: bytes, width: int, height: int) -> tuple[bytes, int, int]:
    if width <= 0 or height <= 0:
        raise ValueError("width and height must be positive")
    expected = width * height * 2
    if len(data) != expected:
        raise ValueError(f"expected {expected} bytes, got {len(data)}")

    rgb = bytearray(width * height * 3)
    colors: set[int] = set()
    non_black = 0
    for pixel_index in range(width * height):
        source = pixel_index * 2
        value = data[source] | (data[source + 1] << 8)
        colors.add(value)
        if value != 0:
            non_black += 1

        red = (value >> 11) & 0x1F
        green = (value >> 5) & 0x3F
        blue = value & 0x1F
        destination = pixel_index * 3
        rgb[destination] = (red << 3) | (red >> 2)
        rgb[destination + 1] = (green << 2) | (green >> 4)
        rgb[destination + 2] = (blue << 3) | (blue >> 2)

    return bytes(rgb), len(colors), non_black


def convert(source: Path, destination: Path, width: int, height: int) -> dict[str, object]:
    data = source.read_bytes()
    rgb, unique_colors, non_black = rgb565_le_to_rgb(data, width, height)
    destination.parent.mkdir(parents=True, exist_ok=True)
    Image.frombytes("RGB", (width, height), rgb).save(destination, format="PNG")
    return {
        "source": str(source.resolve()),
        "destination": str(destination.resolve()),
        "width": width,
        "height": height,
        "input_bytes": len(data),
        "unique_rgb565_colors": unique_colors,
        "non_black_pixels": non_black,
        "source_sha256": hashlib.sha256(data).hexdigest().upper(),
        "png_sha256": hashlib.sha256(destination.read_bytes()).hexdigest().upper(),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=600)
    args = parser.parse_args()
    print(json.dumps(convert(args.source, args.destination, args.width, args.height), indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
