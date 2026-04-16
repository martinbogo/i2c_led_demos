# Uno Q Star Trek dashboard port

This app now uses the normal Arduino App Lab router path.

The dashboard rendering has moved onto the STM32 sketch, which owns the SSD1306 OLED on `Wire`. The Python side only gathers compact telemetry snapshots and sends them to the sketch over Router Bridge, so `arduino-router` remains enabled.

## Files

- `python/main.py` - App Lab Python telemetry collector and Bridge notifier.
- `sketch/sketch.ino` - STM32 OLED dashboard renderer plus staged Bridge handlers.
- `host/st_dashboard_stream.c` - earlier direct-streaming prototype kept for reference.

## Build and run

Run the app normally from Arduino App Lab, or build the sketch profile with Arduino CLI.

The sketch profile pins the Bridge dependency stack used for local CLI builds:

- `Arduino_RouterBridge (0.4.1)`
- `Arduino_RPClite (0.2.1)`
- `MsgPack (0.4.2)`
- `DebugLog (0.8.4)`
- `ArxContainer (0.7.0)`
- `ArxTypeTraits (0.3.2)`

Typical sketch build flow:

- `arduino-cli compile --profile default sketch`
- `arduino-cli upload -p <board-ip> --upload-field password=<password> sketch`

After the sketch is installed, start the app from Arduino App Lab. There is no need to stop `arduino-router`, claim `/dev/ttyHS1`, or toggle the router GPIO lines.

## Runtime model

- Python samples Linux telemetry roughly once per second.
- Each sample is sent as a small staged snapshot using `Bridge.notify(...)` calls.
- The sketch commits each snapshot, maintains local history buffers, rotates the LCARS scenes, and redraws the OLED locally.

## Notes

- The App Lab-native version keeps the router active and avoids the earlier full-frame streaming transport entirely.
- Some telemetry fields are intentionally reduced to compact integer snapshots so the MCU can render the scenes locally without depending on raw framebuffer transport.
- The legacy direct-stream host renderer remains in the folder as a reference implementation of the first porting approach.
