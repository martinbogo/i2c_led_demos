# Bad Apple C Implementation for Waveshare Display

## Overview

This is a C implementation of the Bad Apple video player, expanding the original `badapple.c` (which targets SSD1306 I2C displays) to work with the Waveshare 1.28" SPI-based circular OLED display.

## Compilation

```bash
make badapple_waveshare
```

This compiles `badapple_waveshare.c` with the GC9A01A display driver and HAL layer.

## Usage

```bash
sudo ./badapple_waveshare <video_file> [fps]
```

### Arguments:
- `video_file`: Path to 1-bit monochrome video file (7200 bytes per frame for 240×240 pixels)
- `fps`: Frames per second (default: 30, range: 1-60)

### Examples:

```bash
# Play video at default 30 FPS
sudo ./badapple_waveshare bad_apple.bin

# Play at custom speed
sudo ./badapple_waveshare bad_apple.bin 24
```

## Video Format

Expected input format:
- Resolution: 240×240 pixels (circular display area)
- Color depth: 1-bit monochrome (black=0, white=1)
- Frame size: 7200 bytes per frame (240×240÷8)
- Byte order: MSB first (bit 7 = leftmost pixel)
- Multiple frames concatenated sequentially

## Technical Details

### Architecture

```
Video File
    ↓
[Frame Buffer] 7200 bytes/frame (monochrome)
    ↓
[Conversion] Monochrome → RGB565 (240×240×2 = 115,200 bytes)
    ↓
[Display Driver] GC9A01A SPI interface
    ↓
[Hardware] Waveshare 1.28" OLED
```

### Conversion Process

The `display_mono_frame()` function:
1. Allocates RGB565 frame buffer (115.2 KB)
2. Iterates through each bit in the monochrome data
3. Sets pixel to white (0xFFFF) or black (0x0000)
4. Sends complete frame via SPI to display controller
5. Frees temporary buffer

### Frame Rate Control

Uses `nanosleep()` for precise timing:
- Measures actual frame display time
- Sleeps for remaining time to hit target FPS
- Prevents frame dropping and stuttering

## Performance

- **Compilation size**: 72 KB executable
- **Memory usage**: ~115 KB per frame (temporary RGB565 buffer)
- **SPI speed**: 10 MHz
- **Frame transfer time**: ~12 ms (115K bytes @ 10MHz)
- **Typical FPS**: 24-30 FPS sustainable

## Differences from badapple.c (Original)

| Aspect | Original badapple.c | New badapple_waveshare.c |
|--------|-------------------|------------------------|
| Display | SSD1306 128×64 I2C | GC9A01A 240×240 SPI |
| Resolution | 128×64 pixels | 240×240 pixels |
| Video format | 1-bit @128×64 | 1-bit @240×240 |
| Interface | I2C | SPI |
| Frame rate | 30 FPS | 24-60 FPS (configurable) |
| Color support | Monochrome only | Monochrome only (can extend to RGB565) |

## Extending to Color

To add full RGB565 color support:

1. Modify video format to 24-bit RGB (115K bytes/frame)
2. Change frame reading in `play_video()`:
   ```c
   // Read RGB565 data directly
   fread(frame_buffer, sizeof(uint16_t), FRAME_WIDTH * FRAME_HEIGHT, fp);
   ```
3. Pass directly to display:
   ```c
   lcd_gc9a01a_push_frame_buffer((uint16_t *)frame_buffer);
   ```

## Troubleshooting

### Compilation Errors

If you get GPIO/SPI errors, ensure:
- `libgpiod-dev` is installed (if using libgpiod backend)
- `/dev/spidev0.0` exists, or `/dev/spidev10.0` exists for the automatic fallback path
- `/boot/firmware/config.txt` enables `dtparam=spi=on` and `dtparam=i2c_arm=on`
- GPIO pins `8`, `18`, `25`, and `27` are accessible for the LCD wiring in `gpio_config.h`
- Running with sudo/root privileges

Current Pi demo header wiring for the display path:

| Function | BCM GPIO | Physical Pin |
|----------|----------|--------------|
| SPI MOSI | GPIO10 | Pin 19 |
| SPI MISO | GPIO9 | Pin 21 |
| SPI SCLK | GPIO11 | Pin 23 |
| CS | GPIO8 | Pin 24 |
| DC | GPIO25 | Pin 22 |
| LCD reset | GPIO27 | Pin 13 |
| Backlight | GPIO18 | Pin 12 |

### Runtime Issues

**No display output**: See `POWER_TROUBLESHOOTING.md`
- Verify display VCC/GND power connections
- Check GPIO18 backlight: `gpio_test` tool in `lcd_test.py`
- Test SPI bus: `gpio_test` includes SPI verification

**Video plays but no update**: 
- Check frame rate: reduce FPS with command-line argument
- Verify video file format (must be exactly 7200 bytes/frame)
- Check SPI speed in `gpio_config.h`

## Related Files

- `badapple_waveshare.py`: Python implementation (tested, production-ready)
- `lcd_gc9a01.h/c`: Display driver with full command set
- `hal_gpio_spi.h/c`: Hardware abstraction layer
- `gpio_config.h`: Centralized GPIO/SPI configuration
- `lcd_test.py`: Python test suite for diagnostics

## References

- Original bad apple video: https://www.youtube.com/watch?v=I41VoH7mnRI
- Video creator: Anira (2009)
- Music: ZUN (Touhou Project)
- GC9A01A Datasheet: Tested with Waveshare 1.28" display
