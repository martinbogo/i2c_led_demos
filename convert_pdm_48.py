#!/usr/bin/env python3
import argparse
from functools import lru_cache
from pathlib import Path
import random
import re
import struct
import sys

MAGIC = 0x314D4450  # 'PDM1' little-endian
ENCODING_RAW = 0
ENCODING_PHASE_XOR = 1
DITHER_ORDERED = "ordered"
DITHER_BLUE_NOISE = "blue-noise"
BLUE_NOISE_TILE = 8
BLUE_NOISE_CANDIDATES = 64
BAYER_8X8 = (
    (0, 48, 12, 60, 3, 51, 15, 63),
    (32, 16, 44, 28, 35, 19, 47, 31),
    (8, 56, 4, 52, 11, 59, 7, 55),
    (40, 24, 36, 20, 43, 27, 39, 23),
    (2, 50, 14, 62, 1, 49, 13, 61),
    (34, 18, 46, 30, 33, 17, 45, 29),
    (10, 58, 6, 54, 9, 57, 5, 53),
    (42, 26, 38, 22, 41, 25, 37, 21),
)

TEMPORAL_ORDERS = {
    1: (0,),
    2: (0, 1),
    3: (0, 2, 1),
    4: (0, 2, 1, 3),
    5: (0, 3, 1, 4, 2),
    6: (0, 3, 1, 4, 2, 5),
    7: (0, 4, 2, 6, 1, 5, 3),
    8: (0, 4, 2, 6, 1, 5, 3, 7),
}

LUT_NAMES = ("lut256", "oled_gray_lut")
DEFAULT_CALIBRATION_REPORT = Path(__file__).resolve().with_name("oled_gamma_calibration.txt")


def toroidal_distance_sq(a, b, width: int, height: int, depth: int) -> float:
    dx = abs(a[0] - b[0])
    dy = abs(a[1] - b[1])
    dz = abs(a[2] - b[2])
    dx = min(dx, width - dx) / width
    dy = min(dy, height - dy) / height
    dz = min(dz, depth - dz) / depth
    return dx * dx + dy * dy + dz * dz


@lru_cache(maxsize=None)
def blue_noise_thresholds(phases: int, tile_size: int = BLUE_NOISE_TILE):
    cells = [(x, y, phase)
             for phase in range(phases)
             for y in range(tile_size)
             for x in range(tile_size)]
    rng = random.Random(0x5EED1234 + phases * 97 + tile_size * 13)
    remaining = list(cells)
    progressive = [remaining.pop(rng.randrange(len(remaining)))]

    while remaining:
        sample_count = min(BLUE_NOISE_CANDIDATES, len(remaining))
        candidate_indices = rng.sample(range(len(remaining)), sample_count)
        best_idx = candidate_indices[0]
        best_score = -1.0
        for idx in candidate_indices:
            candidate = remaining[idx]
            score = min(
                toroidal_distance_sq(candidate, prior, tile_size, tile_size, phases)
                for prior in progressive
            )
            if score > best_score:
                best_idx = idx
                best_score = score

        progressive.append(remaining.pop(best_idx))

    thresholds = [[[0] * tile_size for _ in range(tile_size)] for _ in range(phases)]
    for rank, (x, y, phase) in enumerate(progressive):
        thresholds[phase][y][x] = rank

    return tuple(
        tuple(tuple(row) for row in phase_rows)
        for phase_rows in thresholds
    )


def pack_pdm_planes(frame: bytes, width: int, height: int, phases: int, dither: str):
    packed_bytes = width * (height // 8)
    planes = [bytearray(packed_bytes) for _ in range(phases)]
    spatial_levels = 64
    level_scale = phases * spatial_levels + 1
    temporal_order = TEMPORAL_ORDERS[phases] if dither == DITHER_ORDERED else None
    blue_noise = blue_noise_thresholds(phases) if dither == DITHER_BLUE_NOISE else None

    for y in range(height):
        row_off = y * width
        page_off = (y // 8) * width
        bit = 1 << (y & 7)
        for x in range(width):
            gray = frame[row_off + x]
            level = gray * level_scale // 256
            col = page_off + x
            if dither == DITHER_ORDERED:
                bayer = BAYER_8X8[y & 7][x & 7]
                phase_rotate = (x * 3 + y * 5) % phases
                for phase in range(phases):
                    temporal_rank = temporal_order[(phase + phase_rotate) % phases]
                    if level > temporal_rank * spatial_levels + bayer:
                        planes[phase][col] |= bit
            else:
                threshold_row = y % BLUE_NOISE_TILE
                threshold_col = x % BLUE_NOISE_TILE
                for phase in range(phases):
                    if level > blue_noise[phase][threshold_row][threshold_col]:
                        planes[phase][col] |= bit

    return planes


def read_exact(stream, size: int) -> bytes:
    buf = bytearray()
    while len(buf) < size:
        chunk = stream.read(size - len(buf))
        if not chunk:
            break
        buf.extend(chunk)
    return bytes(buf)


def phase_xor_encode(phase_payloads, frame_plane_bytes: int):
    encoded = []
    for payload in phase_payloads:
        blocks = [payload[i:i + frame_plane_bytes] for i in range(0, len(payload), frame_plane_bytes)]
        out = bytearray()
        prev = bytes(frame_plane_bytes)
        for block in blocks:
            out.extend(a ^ b for a, b in zip(block, prev))
            prev = block
        encoded.append(out)
    return encoded


def load_calibration_lut(path: str) -> bytes:
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()

    for name in LUT_NAMES:
        match = re.search(rf"\b{name}\b\s*=\s*\{{(.*?)\}}\s*;?", text, flags=re.S)
        if not match:
            continue
        values = [int(token) for token in re.findall(r"\d+", match.group(1))]
        if len(values) != 256:
            raise SystemExit(f"{path}: {name} must contain exactly 256 values")
        if any(value < 0 or value > 255 for value in values):
            raise SystemExit(f"{path}: {name} values must be in the range 0..255")
        return bytes(values)

    raise SystemExit(f"{path}: no supported LUT found; expected one of: {', '.join(LUT_NAMES)}")


def resolve_calibration_report(path_arg: str | None) -> Path | None:
    if path_arg:
        return Path(path_arg)
    if DEFAULT_CALIBRATION_REPORT.is_file():
        return DEFAULT_CALIBRATION_REPORT
    return None


def main():
    parser = argparse.ArgumentParser(
        description="Convert grayscale raw video into a hybrid spatiotemporal PDM OLED playback stream."
    )
    parser.add_argument("--width", type=int, default=128)
    parser.add_argument("--height", type=int, default=48)
    parser.add_argument("--phases", type=int, default=3)
    parser.add_argument("--subframe-hz", type=int, default=90)
    parser.add_argument("--dither", choices=(DITHER_ORDERED, DITHER_BLUE_NOISE), default=DITHER_BLUE_NOISE,
                        help="thresholding mode for grayscale-to-PDM conversion")
    parser.add_argument("--encoding", choices=("raw", "phase-xor"), default="phase-xor")
    parser.add_argument(
        "--calibration",
        help=(
            "path to an OLED calibration report containing a 256-entry LUT; "
            f"defaults to {DEFAULT_CALIBRATION_REPORT.name} when present"
        ),
    )
    args = parser.parse_args()

    if args.height % 8 != 0:
        raise SystemExit("height must be divisible by 8")
    if args.phases <= 0 or args.phases > 8:
        raise SystemExit("phases must be between 1 and 8")
    if args.subframe_hz <= 0:
        raise SystemExit("subframe-hz must be positive")

    frame_bytes = args.width * args.height
    frame_plane_bytes = args.width * (args.height // 8)
    out = sys.stdout.buffer
    frame_count = 0
    phase_payloads = [bytearray() for _ in range(args.phases)]
    calibration_report = resolve_calibration_report(args.calibration)
    calibration_lut = load_calibration_lut(str(calibration_report)) if calibration_report else None

    while True:
        frame = read_exact(sys.stdin.buffer, frame_bytes)
        if len(frame) < frame_bytes:
            break
        if calibration_lut is not None:
            frame = frame.translate(calibration_lut)
        for phase, plane in enumerate(pack_pdm_planes(frame, args.width, args.height, args.phases, args.dither)):
            phase_payloads[phase].extend(plane)
        frame_count += 1

    encoding = ENCODING_PHASE_XOR if args.encoding == "phase-xor" else ENCODING_RAW
    if encoding == ENCODING_PHASE_XOR:
        phase_payloads = phase_xor_encode(phase_payloads, frame_plane_bytes)

    out.write(struct.pack("<7I", MAGIC, args.width, args.height, args.phases,
                          frame_count, args.subframe_hz, encoding))
    for phase_payload in phase_payloads:
        out.write(phase_payload)


if __name__ == "__main__":
    main()
