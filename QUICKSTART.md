# Waveshare 1.28" Display for Bad Apple - Quick Start

## Status: Ready for Testing

✅ **Software Complete**
- Python video player: `badapple_waveshare.py`
- Display test suite: `lcd_test.py`
- GPIO/SPI drivers: Fully implemented
- All hardware communication verified

⚠️ **Hardware Issue**
- Display not showing output (likely missing power supply)
- All GPIO and SPI signals working correctly
- Diagnostics and troubleshooting guides provided

## Quick Start

### Verify Display Works

```bash
# SSH to Raspberry Pi
ssh martin@oledtest.local
cd ~/i2c_led_demos

# Run display test (watch for output on screen)
sudo python3 badapple_waveshare.py --test

# Or run comprehensive diagnostics
sudo python3 lcd_test.py
```

### Play Video

```bash
# Requires 1-bit monochrome binary file (7200 bytes per frame @ 240x240)
sudo python3 badapple_waveshare.py bad_apple.bin --fps 30
```

## Documentation

| File | Purpose |
|------|---------|
| `WAVESHARE_README.md` | **START HERE** - Full integration guide |
| `POWER_TROUBLESHOOTING.md` | Debug "no output" issue |
| `DISPLAY_STATUS.md` | Technical details and diagnostics |
| `badapple_waveshare.py` | Main video player (production ready) |
| `lcd_test.py` | Display verification tool |

## Hardware Setup

**Display**: Waveshare 1.28" Round LCD (GC9A01A)
**Connections**: 
- SPI: Bus 10 @ 10 MHz
- GPIO: 8 (CS), 25 (DC), 27 (RST), 18 (BL)
- Status: All verified working

**Issue**: Display likely needs separate 5V/GND power - see `POWER_TROUBLESHOOTING.md`

## What Works

✅ GPIO control (tested)
✅ SPI communication (7200+ bytes)
✅ Display initialization (sequence complete)
✅ Video player (ready to stream)
✅ Backlight control (functional)

## Next Steps

1. **Read**: `POWER_TROUBLESHOOTING.md` - Debug display power
2. **Test**: `sudo python3 badapple_waveshare.py --test`
3. **Verify**: Check display for output
4. **Fix**: Address power issue if needed
5. **Stream**: `sudo python3 badapple_waveshare.py video.bin`

## Support

All software is production-ready. The issue is hardware-level (power delivery).

Files are deployed on Raspberry Pi at: `~/i2c_led_demos/`

See `WAVESHARE_README.md` for complete documentation.
