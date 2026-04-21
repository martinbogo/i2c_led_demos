# Uno Q Star Trek smartwatch port

This app now uses the standard Arduino App Lab router path.

The OLED remains MCU-owned on `Wire`, but the rendering is now done locally on the STM32 sketch. The Python side only sends compact watch state updates over Router Bridge, so `arduino-router` stays enabled and App Lab can run the app normally.

## Files

- `python/main.py` - App Lab Python provider that exposes watch state over Bridge RPC (`watch_sync`).
- `sketch/sketch.ino` - STM32 OLED renderer and Bridge state puller.
- `host/st_smartwatch_stream.c` - earlier direct-streaming prototype kept for reference.

## Build and run

Run the app through Arduino App Lab, or natively build the sketch profile on your host using the repository's build system:

- `cd ../ && ./build.sh unoq`

This automatically checks for `arduino-cli` and installs the required Bridge dependency stack used for local CLI builds:

- `Arduino_RouterBridge (0.4.1)`
- `Arduino_RPClite (0.2.1)`
- `MsgPack (0.4.2)`
- `DebugLog (0.8.4)`
- `ArxContainer (0.7.0)`
- `ArxTypeTraits (0.3.2)`

Once built, you can upload the sketch profile using the standard deployment process:

- `arduino-cli upload -p <board-ip> --upload-field password=<password> sketch`

After the sketch is installed, launch the App Lab app in the normal way. There is no need to stop `arduino-router`, claim `/dev/ttyHS1`, or toggle the router GPIO lines.

## Runtime model

- The sketch calls `Bridge.call("watch_sync")` approximately every `0.5s`.
- Python's `watch_sync()` RPC returns status integer (1 = success, 0 = failure).
- Inside `watch_sync()`, Python sends watch state via `Bridge.notify("watch_state", day_seconds, steps, battery)`.
- The MCU notify handler receives state, derives the active LCARS scene locally, and animates the ECG and diagnostics views.
- The OLED is updated directly from the sketch over `Wire`.

## Notes

- The current App Lab-native implementation keeps the LCARS scene layout while replacing the old full-frame streaming transport with compact Bridge state updates.
- The legacy direct-stream host renderer remains in the folder as a reference implementation of the first porting approach.
