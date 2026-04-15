# Uno Q Star Trek smartwatch port

This app ports `st_smartwatch.c` to the Arduino Uno Q by rendering frames on the Linux side and presenting them on the MCU-owned OLED.

## Files

- `host/st_smartwatch_stream.c` - Linux-side smartwatch renderer and serial frame streamer.
- `sketch/sketch.ino` - STM32 OLED framebuffer sink.
- `python/main.py` - minimal App Lab placeholder.

## Build on the Uno Q Linux side

Compile the host renderer on the board:

- `gcc -O2 -o st_smartwatch_stream host/st_smartwatch_stream.c -lm`

## Run on the Uno Q

1. Upload the sketch.
2. Stop the standard router so `/dev/ttyHS1` is free.
3. Assert the MCU-ready GPIO state.
4. Run the host renderer.

Typical sequence on the board:

- `arduino-cli compile -b arduino:zephyr:unoq -e sketch`
- `arduino-cli upload -b arduino:zephyr:unoq -p <board-ip> --upload-field password=<password> sketch`
- `gcc -O2 -o st_smartwatch_stream host/st_smartwatch_stream.c -lm`
- `sudo systemctl stop arduino-router`
- `gpioset -c /dev/gpiochip1 -t0 37=0`
- `gpioset -c /dev/gpiochip1 -t0 70=1`
- `./st_smartwatch_stream`

When finished, restore the router if you want normal App Lab bridge behavior back:

- `sudo systemctl start arduino-router`

## Notes

- The host renderer targets `15 FPS` on the internal link.
- Timekeeping and animation remain on the Linux side, so the Uno Q displays the same scene logic as the original desktop or Pi-oriented demo.
- The sketch is a full-screen OLED framebuffer sink that accepts `1024`-byte frames over the internal serial link.
