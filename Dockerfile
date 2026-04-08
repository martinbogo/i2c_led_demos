# Cross-compilation environment for Raspberry Pi 5 (aarch64)
FROM ubuntu:22.04

# Prevent interactive prompts
ENV DEBIAN_FRONTEND=noninteractive

# Update apt and install cross-compilation toolchains natively targeting ARM64
RUN dpkg --add-architecture arm64 && apt-get update && apt-get install -y \
    build-essential \
    crossbuild-essential-arm64 \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    libc6-dev-arm64-cross \
    zlib1g-dev:arm64 \
    make \
 && rm -rf /var/lib/apt/lists/*

# Set default working directory where local volume mounts will attach
WORKDIR /app

# Standard entrypoint enforces Make compilation prioritizing the aarch64 gcc compiler
CMD ["make", "CC=aarch64-linux-gnu-gcc"]
