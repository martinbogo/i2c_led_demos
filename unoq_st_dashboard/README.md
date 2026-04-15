# Uno Q Star Trek dashboard port

This app ports `st_dashboard.c` to the Arduino Uno Q by splitting responsibilities across both processors:

- Linux side: gather telemetry, render the LCARS dashboard, and stream full `128x64` OLED frames.
- MCU side: own the SSD1306 on `Wire` and present each streamed framebuffer.

## Files

- `host/st_dashboard_stream.c` - Linux-side dashboard renderer and serial frame streamer.
- `sketch/sketch.ino` - STM32 OLED framebuffer sink.
- `python/main.py` - minimal App Lab placeholder.

## Build on the Uno Q Linux side

Compile the host renderer on the board:

- `gcc -O2 -o st_dashboard_stream host/st_dashboard_stream.c -lm`

## Run on the Uno Q

1. Upload the sketch.
2. Stop the standard router so `/dev/ttyHS1` is free.
3. Assert the MCU-ready GPIO state.
4. Run the host renderer.

Typical sequence on the board:

- `arduino-cli compile -b arduino:zephyr:unoq -e sketch`
- `arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> --upload-field password=<password> sketch`
- `gcc -O2 -o st_dashboard_stream host/st_dashboard_stream.c -lm`
- `sudo systemctl stop arduino-router`
- `gpioset -c /dev/gpiochip1 -t0 37=0`
- `gpioset -c /dev/gpiochip1 -t0 70=1`
- `./st_dashboard_stream`

When finished, restore the router if you want normal App Lab bridge behavior back:

- `sudo systemctl start arduino-router`

## Notes

- The host renderer targets `12 FPS` on the internal link.
- Telemetry is adapted for generic Linux paths on the Uno Q and does not depend on Raspberry Pi-only commands.
- The sketch accepts full `1024`-byte framebuffers using the same proven serial transport style as the Uno Q `badapple` port.
