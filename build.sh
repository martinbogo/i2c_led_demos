#!/bin/bash
set -e

echo "[Docker] Initializing cross-compilation image: raspi5-cross-cc..."
docker build -t raspi5-cross-cc .

echo "[Docker] Injecting codebase and executing cross-compiler..."
# Spin up the compiled container, hot-loading the host volume and triggering the aarch64 root Makefile
docker run --rm -v "$(pwd):/app" raspi5-cross-cc

echo ""
echo "[Success] Source files have been securely cross-compiled for Raspberry Pi 5 (Linux aarch64)!"
echo "          You can now SCP the binaries directly to the board and run them natively."
