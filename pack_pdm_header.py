#!/usr/bin/env python3
import argparse
from pathlib import Path
import sys
import zlib

try:
    import zopfli.zlib as zopfli_zlib
except ImportError:
    zopfli_zlib = None


def c_guard(name: str) -> str:
    return ''.join(ch if ch.isalnum() else '_' for ch in name.upper())


def format_c_array(data: bytes) -> str:
    lines = []
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in chunk) + ',')
    return '\n'.join(lines)


def main() -> None:
    parser = argparse.ArgumentParser(
        description='Read raw PDM bytes from stdin, deflate-compress them, and emit a C header.'
    )
    parser.add_argument('--output', required=True, help='Path to the generated header file')
    parser.add_argument('--array-name', default='woz_pdm_z', help='C symbol for the embedded deflate data')
    parser.add_argument('--z-len-name', default='woz_pdm_z_len', help='C symbol for compressed length')
    parser.add_argument('--raw-len-name', default='woz_pdm_raw_len', help='C symbol for uncompressed length')
    args = parser.parse_args()

    raw = sys.stdin.buffer.read()
    if not raw:
        raise SystemExit('no PDM data received on stdin')

    packed = zopfli_zlib.compress(raw) if zopfli_zlib else zlib.compress(raw, level=9)
    guard = c_guard(Path(args.output).name)

    header = f'''#ifndef {guard}\n#define {guard}\n\nstatic const unsigned char {args.array_name}[] = {{\n{format_c_array(packed)}\n}};\n\nstatic const unsigned int {args.z_len_name} = {len(packed)}u;\nstatic const unsigned int {args.raw_len_name} = {len(raw)}u;\n\n#endif\n'''

    Path(args.output).write_text(header)


if __name__ == '__main__':
    main()
