#!/bin/bash
set -e

PLATFORM=${1:-all}

echo "Building for platform: $PLATFORM"

# Build Linux cross-compiled binaries using Docker
if [ "$PLATFORM" = "pi" ] || [ "$PLATFORM" = "unoq" ] || [ "$PLATFORM" = "all" ]; then
    echo "[Docker] Initializing cross-compilation image: raspi5-cross-cc..."
    docker build -t raspi5-cross-cc .

    if [ "$PLATFORM" = "pi" ]; then
        echo "[Docker] Cross-compiling Raspberry Pi target payloads..."
        docker run --rm -v "$(pwd):/app" raspi5-cross-cc make pi
        echo "[Success] Raspberry Pi source files have been cross-compiled for Linux aarch64!"
    elif [ "$PLATFORM" = "unoq" ]; then
        echo "[Docker] Cross-compiling Arduino Uno Q host payloads..."
        docker run --rm -v "$(pwd):/app" raspi5-cross-cc make unoq
        echo "[Success] Arduino Uno Q host files have been cross-compiled for Linux aarch64!"
    else
        echo "[Docker] Cross-compiling all payloads..."
        docker run --rm -v "$(pwd):/app" raspi5-cross-cc make all
        echo "[Success] All source files have been securely cross-compiled for Linux aarch64!"
    fi
fi

# Build Arduino sketches natively using arduino-cli
if [ "$PLATFORM" = "unoq" ] || [ "$PLATFORM" = "all" ]; then
    if command -v arduino-cli &> /dev/null; then
        echo "[Host] arduino-cli found, checking/installing Arduino Uno Q sketch dependencies..."
        arduino-cli lib install "Arduino_RouterBridge@0.4.1" "Arduino_RPClite@0.2.1" "MsgPack@0.4.2" \
                                "DebugLog@0.8.4" "ArxContainer@0.7.0" "ArxTypeTraits@0.3.2"

        for app in unoq_badapple unoq_st_dashboard unoq_st_smartwatch; do
            if [ -d "$app/sketch" ]; then
                echo "[Host] Compiling $app sketch..."
                (cd "$app/sketch" && arduino-cli compile --profile default)
            fi
        done
        echo "[Success] Arduino Uno Q sketches compiled successfully!"
    else
        echo "[Warning] arduino-cli is not installed natively on host. Skipping sketch compilation."
        echo "          Install arduino-cli to build the MCU firmwares for the Uno Q apps."
    fi
fi

echo ""
echo "[Success] Build process complete."

