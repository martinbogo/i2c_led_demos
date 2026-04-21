# Uno Q Bad Apple port

This folder contains a working port path for the `badapple` demo on the Arduino Uno Q.

## Architecture

The OLED is connected to the MCU-owned Arduino header I2C bus on `Wire`, not to any Linux-visible I2C controller. Because of that, the display must be driven by the STM32 sketch.

The video asset is too large to embed in the Uno Q sketch flash budget exposed by the Arduino app partition, so the movie frames stay on the Linux side and are streamed to the MCU over the internal `Serial1` link on `/dev/ttyHS1`.

This is intentionally **not** using the standard Arduino Router bridge for frame transport. The default bridge runs over the same internal serial link at `115200`, which is too slow for `768` bytes per frame at `30 FPS`. The custom player uses a faster direct UART rate of `460800`.

## Files

- `sketch/sketch.ino` - STM32 firmware that initializes the SSD1306, draws the static `BAD APPLE!` title, and receives packed `128x48` video frames over `Serial1`.
- `host/badapple_stream.py` - Linux-side streamer that reads `bad_apple.bin.gz`, handshakes with the sketch, and streams frames over `/dev/ttyHS1`.
- `python/main.py` - minimal placeholder App Lab Python app so `arduino-app-cli app restart` can compile and upload the sketch.

## Build

You can build the sketch portion on your host machine utilizing the repository's root build script:
- `cd ../ && ./build.sh unoq`

## Deploy to the Uno Q

Copy this folder to the board, for example into `/home/arduino/ArduinoApps/unoq-badapple`, then upload it with:

- `arduino-app-cli app restart /home/arduino/ArduinoApps/unoq-badapple`

## Run playback on the Uno Q

1. Stop the standard router so `/dev/ttyHS1` is free.
2. Run the Linux streamer on the board.
3. Start the router again when finished if you want App Lab bridge features back.

Typical sequence on the Uno Q:

- `sudo systemctl stop arduino-router`
- `python3 /home/arduino/ArduinoApps/unoq-badapple/host/badapple_stream.py /home/arduino/bad_apple.bin.gz`
- `sudo systemctl start arduino-router`

## Notes

- The sketch writes only the lower `48` video rows each frame. The top `16` rows are drawn once for the static title.
- The streamer waits for an ACK from the MCU after each frame. This keeps the sender paced to the real display throughput.
- The streamer also sends each frame in paced chunks instead of one large burst. This avoids overrunning the MCU UART receive buffer on the internal link.
- The streamer can be tested without the full movie using `--limit-frames N`.
