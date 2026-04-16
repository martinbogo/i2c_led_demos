# i2c_led_demos

This repository is a collection of small C demos for SSD1306 OLED displays over I2C.

I made these mostly for fun, curiosity, and education. It is a place to experiment with tiny graphics, animation, rendering tricks, and display ideas on simple hardware. It is not meant to be a polished framework or reusable library.

## What is in this repo

Most of the top-level `.c` files are standalone programs. Each one builds into a binary with the same name.

Included demos:

- `badapple.c` - plays compressed video frames in the 128x48 blue area
- `anim_v_animator.c` - a storyboard-driven Animator vs. Animation recreation for the 128x64 OLED
- `bounce.c` - a simple bouncing ball demo
- `cube.c` - a rotating wireframe cube
- `horizon.c` - a basic artificial horizon style display
- `i2c_oled_demo.c` - the larger 10-scene demo reel
- `showreel.c` - another multi-scene reel for the split yellow/blue display
- `smartwatch.c` - a particle-based watch face
- `st_dashboard.c` - a Star Trek inspired dashboard
- `st_smartwatch.c` - a Star Trek inspired watch face
- `sysmon.c` - simple system monitor graphs
- `water.c` - a water and ripple effect demo

There are also a few helper files:

- `Makefile` - builds the top-level demos
- `build.sh` - cross-builds the repo with Docker for Raspberry Pi 5
- `Dockerfile` - container used by `build.sh`
- `convert.py` and `convert_48.py` - helper scripts for preparing video assets

There are also Uno Q app folders for the dual-processor board ports:

- `unoq_badapple` - MCU OLED sink plus Linux-side video streamer for the Bad Apple port
- `unoq_st_dashboard` - App Lab-native LCARS dashboard with MCU-side OLED rendering and Bridge-fed telemetry snapshots
- `unoq_st_smartwatch` - App Lab-native LCARS smartwatch with MCU-side OLED rendering and Bridge-fed state updates

## Display assumptions

The demos target a 128x64 SSD1306 OLED connected over I2C at address `0x3C`.

Several of the programs assume the common split-color layout:

- top 16 pixels in yellow
- bottom 48 pixels in blue

## Build

### Build directly on a Raspberry Pi

Install the zlib development package, then build everything with `make`:

```bash
sudo apt-get update
sudo apt-get install -y zlib1g-dev
make clean
make
```

### Cross-build with Docker

If you are building from macOS or another machine with Docker installed:

```bash
./build.sh
```

That uses the included `Dockerfile` to build the binaries for Raspberry Pi 5 Linux aarch64.

## Run

Each demo builds to its own executable. Run the one you want as root so it can talk to `/dev/i2c-1`.

Examples:

```bash
sudo ./anim_v_animator
sudo ./water
sudo ./st_dashboard
sudo ./showreel
sudo ./i2c_oled_demo -s 10
```

Most demos support `-d` to run as a daemon.

`i2c_oled_demo` also supports `-s N` to start from a specific scene, where `N` is `1` through `10`.

## Notes

- This repo is a set of experiments, not a stable API.
- The demos are intentionally small and direct.
- Some of the video-oriented pieces use the conversion helpers in this repo to prepare assets before building.
- Only convert or embed video that you have permission to use.

## License

This project is released under **CC BY-NC 4.0**.

In plain terms: you are welcome to study it, share it, and learn from it, but not use it commercially.

See `LICENSE` for the full license text.
