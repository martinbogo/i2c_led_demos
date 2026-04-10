#!/usr/bin/env python3
import argparse
from pathlib import Path
import re

LUT_NAMES = ("oled_gray_lut", "lut256")
DEFAULT_PACKED = (
    0x01, 0x01, 0x11, 0x10, 0x10, 0x10, 0x01, 0x01, 0x11, 0x10, 0x01, 0x11, 0x10, 0x10, 0x01, 0x11,
    0x10, 0x01, 0x11, 0x10, 0x01, 0x01, 0x11, 0x10, 0x01, 0x11, 0x10, 0x11, 0x10, 0x01, 0x11, 0x10,
    0x11, 0x10, 0x01, 0x11, 0x01, 0x11, 0x10, 0x01, 0x11, 0x01, 0x11, 0x01, 0x11, 0x01, 0x11, 0x01,
    0x11, 0x01, 0x11, 0x11, 0x10, 0x11, 0x10, 0x11, 0x10, 0x11, 0x10, 0x11, 0x01, 0x11, 0x01, 0x11,
    0x01, 0x11, 0x01, 0x11, 0x11, 0x10, 0x11, 0x10, 0x11, 0x10, 0x11, 0x10, 0x11, 0x01, 0x11, 0x11,
    0x01, 0x11, 0x11, 0x01, 0x11, 0x11, 0x10, 0x11, 0x11, 0x10, 0x11, 0x11, 0x10, 0x11, 0x01, 0x11,
    0x11, 0x01, 0x11, 0x11, 0x01, 0x11, 0x11, 0x10, 0x11, 0x11, 0x10, 0x11, 0x11, 0x10, 0x11, 0x11,
    0x01, 0x11, 0x11, 0x11, 0x01, 0x11, 0x11, 0x54, 0x54, 0x54, 0x54, 0x54, 0x54, 0x54, 0x54, 0x04,
)


def unpack_default_lut() -> list[int]:
    lut = [0]
    value = 0
    for packed in DEFAULT_PACKED:
        value += packed & 0x0F
        lut.append(value)
        if len(lut) >= 256:
            break
        value += packed >> 4
        lut.append(value)
        if len(lut) >= 256:
            break
    if len(lut) != 256:
        raise SystemExit("internal error: default packed LUT did not decode to 256 entries")
    return lut


def load_lut(path: Path) -> tuple[list[int], str]:
    text = path.read_text(encoding="utf-8")
    for name in LUT_NAMES:
        match = re.search(rf"\b{name}\b\s*=\s*\{{(.*?)\}}\s*;?", text, flags=re.S)
        if not match:
            continue
        values = [int(token) for token in re.findall(r"\d+", match.group(1))]
        if len(values) != 256:
            raise SystemExit(f"{path}: {name} must contain exactly 256 values")
        if any(value < 0 or value > 255 for value in values):
            raise SystemExit(f"{path}: {name} values must be in the range 0..255")
        if any(curr < prev for prev, curr in zip(values, values[1:])):
            raise SystemExit(f"{path}: {name} must be monotonic nondecreasing")
        return values, name
    raise SystemExit(f"{path}: no supported LUT found; expected one of: {', '.join(LUT_NAMES)}")


def pack_lut(values: list[int]) -> tuple[int, list[int]]:
    first = values[0]
    packed: list[int] = []
    deltas = [curr - prev for prev, curr in zip(values, values[1:])]
    for i in range(0, len(deltas), 2):
        lo = deltas[i]
        hi = deltas[i + 1] if i + 1 < len(deltas) else 0
        if lo < 0 or lo > 15 or hi < 0 or hi > 15:
            raise SystemExit(
                "calibration LUT cannot be packed into the current nibble-delta format; "
                f"deltas at positions {i + 1} and {i + 2} must stay within 0..15"
            )
        packed.append(lo | (hi << 4))
    return first, packed


def format_c_array(data: list[int]) -> str:
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i:i + 16]
        lines.append("    " + ", ".join(f"0x{value:02x}" for value in chunk) + ",")
    return "\n".join(lines)


def escape_c_string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Generate the build-time OLED LUT header from oled_gamma_calibration.txt when available."
    )
    parser.add_argument("--output", required=True, help="Path to the generated C header")
    parser.add_argument(
        "--calibration",
        default="oled_gamma_calibration.txt",
        help="Path to the calibration report to consume when it exists",
    )
    args = parser.parse_args()

    calibration_path = Path(args.calibration)
    if calibration_path.is_file():
        lut, symbol_name = load_lut(calibration_path)
        first_value, packed = pack_lut(lut)
        from_calibration = 1
        source_path = str(calibration_path)
        source_symbol = symbol_name
    else:
        lut = unpack_default_lut()
        first_value, packed = pack_lut(lut)
        from_calibration = 0
        source_path = "repository default"
        source_symbol = "default packed LUT"

    header = f'''#ifndef OLED_BUILD_LUT_H\n#define OLED_BUILD_LUT_H\n\n#include <stdint.h>\n\n#define OLED_GRAY_LUT_FROM_CALIBRATION {from_calibration}\n#define OLED_GRAY_LUT_SOURCE_PATH "{escape_c_string(source_path)}"\n#define OLED_GRAY_LUT_SOURCE_SYMBOL "{escape_c_string(source_symbol)}"\n#define OLED_GRAY_LUT_FIRST_VALUE {first_value}\n\nstatic const uint8_t oled_gray_lut_packed[] = {{\n{format_c_array(packed)}\n}};\n\n#endif\n'''

    Path(args.output).write_text(header, encoding="utf-8")


if __name__ == "__main__":
    main()
