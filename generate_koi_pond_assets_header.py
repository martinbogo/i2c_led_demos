#!/usr/bin/env python3
"""Generate a C++ header with raw koi pond assets extracted from koi_pond.py."""

from __future__ import annotations

import ast
import base64
import re
from pathlib import Path

from PIL import Image

ROOT = Path(__file__).resolve().parent
SOURCE = ROOT / "koi_pond.py"
OUTPUT = ROOT / "koi_pond_assets.h"

ASSET_NAMES = [
    "EMBEDDED_POND_BACKGROUND_JPEG_B64",
    "EMBEDDED_LILYPAD_1_PNG_B64",
    "EMBEDDED_LILYPAD_2_PNG_B64",
    "EMBEDDED_LILYPAD_3_PNG_B64",
]

LILY_SIZES = {
    "EMBEDDED_LILYPAD_1_PNG_B64": (134, 105),
    "EMBEDDED_LILYPAD_2_PNG_B64": (141, 104),
    "EMBEDDED_LILYPAD_3_PNG_B64": (128, 102),
}


def extract_constant(text: str, name: str) -> str:
    pattern = rf"{name}\s*=\s*\((.*?)\n\)"
    match = re.search(pattern, text, re.DOTALL)
    if not match:
        raise RuntimeError(f"Could not find {name} in {SOURCE}")
    inner = match.group(1)
    pieces = []
    for literal in re.findall(r'"(?:[^"\\]|\\.)*"', inner):
        pieces.append(ast.literal_eval(literal))
    return "".join(pieces)


def format_bytes(values: bytes, bytes_per_line: int = 12) -> str:
    lines = []
    for index in range(0, len(values), bytes_per_line):
        chunk = values[index:index + bytes_per_line]
        line = ", ".join(f"0x{value:02X}" for value in chunk)
        lines.append(f"    {line}")
    return ",\n".join(lines)


def emit_asset(var_name: str, image: Image.Image) -> str:
    if image.mode not in {"RGB", "RGBA"}:
        raise ValueError(f"Unsupported mode for {var_name}: {image.mode}")
    data = image.tobytes()
    channels = len(image.mode)
    return f"""inline constexpr std::uint16_t {var_name}_WIDTH = {image.width};
inline constexpr std::uint16_t {var_name}_HEIGHT = {image.height};
inline constexpr std::uint8_t {var_name}_CHANNELS = {channels};
inline constexpr std::array<std::uint8_t, {len(data)}> {var_name}_DATA = {{
{format_bytes(data)}
}};
"""


def main() -> None:
    source_text = SOURCE.read_text(encoding="utf-8")

    decoded = {}
    for name in ASSET_NAMES:
        encoded = extract_constant(source_text, name)
        decoded[name] = base64.b64decode(encoded)

    bg = Image.open(__import__("io").BytesIO(decoded["EMBEDDED_POND_BACKGROUND_JPEG_B64"])).convert("RGB")
    bg = bg.resize((240, 240), Image.Resampling.LANCZOS)

    lily_assets = []
    for name in (
        "EMBEDDED_LILYPAD_1_PNG_B64",
        "EMBEDDED_LILYPAD_2_PNG_B64",
        "EMBEDDED_LILYPAD_3_PNG_B64",
    ):
        image = Image.open(__import__("io").BytesIO(decoded[name])).convert("RGBA")
        image = image.resize(LILY_SIZES[name], Image.Resampling.LANCZOS)
        lily_assets.append((name, image))

    header = f"""#ifndef KOI_POND_ASSETS_H
#define KOI_POND_ASSETS_H

#include <array>
#include <cstdint>

namespace koi_pond_assets {{

{emit_asset('POND_BACKGROUND', bg)}
{emit_asset('LILYPAD_1', lily_assets[0][1])}
{emit_asset('LILYPAD_2', lily_assets[1][1])}
{emit_asset('LILYPAD_3', lily_assets[2][1])}

}}  // namespace koi_pond_assets

#endif  // KOI_POND_ASSETS_H
"""
    OUTPUT.write_text(header, encoding="utf-8")
    print(f"Wrote {OUTPUT}")


if __name__ == "__main__":
    main()
