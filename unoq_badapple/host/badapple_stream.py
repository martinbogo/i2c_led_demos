#!/usr/bin/env python3
# Author  : Martin Bogomolni
# Date    : 2026-04-21
# License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
#
# badapple_stream.py - Linux-side streamer for Bad Apple on Arduino Uno Q
# Reads bad_apple.bin.gz and streams raw pixel frames over internal UART (ttyHS1)
# 
# Run:      sudo systemctl stop arduino-router
#           python3 badapple_stream.py /path/to/bad_apple.bin.gz
#           sudo systemctl start arduino-router
#
import argparse
import gzip
import os
import select
import sys
import termios
import time
from pathlib import Path

FRAME_BYTES = 768
MAGIC = b"BAPP"
DEFAULT_BAUD = 460800


class RawSerial:
    def __init__(self, path: str, baud: int):
        self.path = path
        self.baud = baud
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_SYNC)
        self._configure(baud)
        self._rx_buffer = bytearray()

    def _configure(self, baud: int) -> None:
        baud_attr = getattr(termios, f"B{baud}", None)
        if baud_attr is None:
            raise RuntimeError(f"Unsupported baud rate: {baud}")

        attrs = termios.tcgetattr(self.fd)
        attrs[0] = 0
        attrs[1] = 0
        attrs[2] &= ~(termios.PARENB | termios.CSTOPB | termios.CSIZE)
        if hasattr(termios, "CRTSCTS"):
            attrs[2] &= ~termios.CRTSCTS
        attrs[2] |= termios.CLOCAL | termios.CREAD | termios.CS8
        attrs[3] = 0
        attrs[4] = baud_attr
        attrs[5] = baud_attr
        attrs[6][termios.VMIN] = 0
        attrs[6][termios.VTIME] = 0
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)
        termios.tcflush(self.fd, termios.TCIOFLUSH)

    def close(self) -> None:
        os.close(self.fd)

    def write(self, payload: bytes) -> None:
        view = memoryview(payload)
        while view:
            written = os.write(self.fd, view)
            view = view[written:]

    def write_paced(self, payload: bytes, chunk_size: int, chunk_delay_s: float) -> None:
        if chunk_size <= 0:
            raise RuntimeError(f"chunk_size must be positive, got {chunk_size}")
        view = memoryview(payload)
        while view:
            chunk = view[:chunk_size]
            self.write(chunk)
            view = view[chunk_size:]
            if view and chunk_delay_s > 0:
                time.sleep(chunk_delay_s)

    def _fill(self, timeout: float) -> bool:
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            return False
        chunk = os.read(self.fd, 4096)
        if not chunk:
            return False
        self._rx_buffer.extend(chunk)
        return True

    def readline(self, timeout: float) -> str | None:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            newline_index = self._rx_buffer.find(b"\n")
            if newline_index >= 0:
                line = self._rx_buffer[:newline_index + 1]
                del self._rx_buffer[:newline_index + 1]
                return line.decode("utf-8", errors="replace").strip()
            self._fill(max(0.0, deadline - time.monotonic()))
        return None

    def wait_for_prefix(self, prefix: str, timeout: float) -> str:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            line = self.readline(max(0.0, deadline - time.monotonic()))
            if not line:
                continue
            print(line)
            if line.startswith(prefix):
                return line
            if line.startswith("ERR"):
                raise RuntimeError(line)
        raise RuntimeError(f"Timed out waiting for {prefix!r}")


def wait_for_ready(port: RawSerial, attempts: int, timeout: float) -> None:
    for attempt in range(1, attempts + 1):
        print(f"Handshake attempt {attempt}/{attempts}")
        port.write(MAGIC + b"Q")
        try:
            port.wait_for_prefix("READY", timeout)
            return
        except RuntimeError as exc:
            if "Timed out" not in str(exc):
                raise
    raise RuntimeError("Did not receive READY from the MCU sketch")


def stream_frames(
    port: RawSerial,
    asset_path: Path,
    fps: float,
    limit_frames: int | None,
    frame_chunk_bytes: int,
    frame_chunk_delay_ms: float,
    command_settle_ms: float,
) -> None:
    frame_interval = 1.0 / fps
    sent_frames = 0
    skipped_frames = 0
    source_frames = 0
    deadline = time.monotonic()
    start = deadline

    with gzip.open(asset_path, "rb") as payload:
        while True:
            frame = payload.read(FRAME_BYTES)
            if len(frame) < FRAME_BYTES:
                break

            source_frames += 1
            now = time.monotonic()

            while now - deadline > frame_interval:
                skipped_frames += 1
                deadline += frame_interval
                frame = payload.read(FRAME_BYTES)
                if len(frame) < FRAME_BYTES:
                    frame = b""
                    break
                source_frames += 1
                now = time.monotonic()

            if len(frame) < FRAME_BYTES:
                break

            if now < deadline:
                time.sleep(deadline - now)

            port.write(MAGIC + b"F")
            if command_settle_ms > 0:
                time.sleep(command_settle_ms / 1000.0)
            port.write_paced(frame, frame_chunk_bytes, frame_chunk_delay_ms / 1000.0)
            ack = port.wait_for_prefix("ACK", 5.0)
            sent_frames += 1
            deadline += frame_interval

            if sent_frames % 30 == 0:
                elapsed = max(0.001, time.monotonic() - start)
                print(
                    f"status sent={sent_frames} source={source_frames} skipped={skipped_frames} "
                    f"fps={sent_frames / elapsed:.2f} last={ack}"
                )

            if limit_frames is not None and sent_frames >= limit_frames:
                break

    port.write(MAGIC + b"E")
    try:
        done = port.wait_for_prefix("DONE", 2.0)
        print(done)
    except RuntimeError as exc:
        print(f"Warning: {exc}", file=sys.stderr)

    elapsed = max(0.001, time.monotonic() - start)
    print(
        f"Finished streaming. sent={sent_frames} source={source_frames} "
        f"skipped={skipped_frames} avg_fps={sent_frames / elapsed:.2f}"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Stream bad_apple.bin.gz to the Uno Q MCU over /dev/ttyHS1")
    parser.add_argument("asset", type=Path, help="Path to bad_apple.bin.gz")
    parser.add_argument("--device", default="/dev/ttyHS1", help="Internal serial device on the Uno Q Linux side")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="Serial baud rate")
    parser.add_argument("--fps", type=float, default=30.0, help="Target playback rate")
    parser.add_argument("--limit-frames", type=int, default=None, help="Send only the first N displayed frames")
    parser.add_argument("--handshake-attempts", type=int, default=10, help="How many READY probes to try before failing")
    parser.add_argument("--handshake-timeout", type=float, default=1.0, help="Seconds to wait for each READY reply")
    parser.add_argument("--frame-chunk-bytes", type=int, default=64, help="How many frame bytes to write per paced chunk")
    parser.add_argument("--frame-chunk-delay-ms", type=float, default=1.0, help="Delay between frame chunks in milliseconds")
    parser.add_argument("--command-settle-ms", type=float, default=2.0, help="Delay after sending the frame command before frame payload bytes start")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.asset.is_file():
        print(f"Asset not found: {args.asset}", file=sys.stderr)
        return 1

    port = RawSerial(args.device, args.baud)
    try:
        wait_for_ready(port, args.handshake_attempts, args.handshake_timeout)
        stream_frames(
            port,
            args.asset,
            args.fps,
            args.limit_frames,
            args.frame_chunk_bytes,
            args.frame_chunk_delay_ms,
            args.command_settle_ms,
        )
    finally:
        port.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
