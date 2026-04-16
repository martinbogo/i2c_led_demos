# Uno Q Star Trek smartwatch port

This app now uses the standard Arduino App Lab router path.

The OLED remains MCU-owned on `Wire`, but the rendering is now done locally on the STM32 sketch. The Python side only sends compact watch state updates over Router Bridge, so `arduino-router` stays enabled and App Lab can run the app normally.

## Files

- `python/main.py` - App Lab Python loop that sends compact watch state updates with `Bridge.notify(...)`.
- `sketch/sketch.ino` - STM32 OLED renderer plus Bridge handlers.
- `host/st_smartwatch_stream.c` - earlier direct-streaming prototype kept for reference.

## Build and run

Run the app through Arduino App Lab, or compile and upload the sketch profile with Arduino CLI.

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

After the sketch is installed, launch the App Lab app in the normal way. There is no need to stop `arduino-router`, claim `/dev/ttyHS1`, or toggle the router GPIO lines.

## Runtime model

- Python sends `day_seconds`, `steps`, and `battery` every `0.5s`.
- The sketch derives the active LCARS scene locally and animates the ECG and diagnostics views on the MCU.
- The OLED is updated directly from the sketch over `Wire`.

## Notes

- The current App Lab-native implementation keeps the LCARS scene layout while replacing the old full-frame streaming transport with compact Bridge state updates.
- The legacy direct-stream host renderer remains in the folder as a reference implementation of the first porting approach.
