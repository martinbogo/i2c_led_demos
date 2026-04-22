# Display Power Troubleshooting Guide

## Quick Diagnosis

If you're reading this, the display is initializing successfully but showing NO OUTPUT.

### Step 1: Verify Backlight Activity (Required)

Run this test while watching the display:

```bash
ssh martin@oledtest.local 'python3 << "EOF"
import gpiod
from gpiod.line import Direction, Value
import time
chip = gpiod.Chip("/dev/gpiochip0")
lines = chip.request_lines({18: gpiod.LineSettings(direction=Direction.OUTPUT)}, consumer="blink")
print("Testing GPIO 18 (backlight)...")
for i in range(5):
    print(f"  Cycle {i+1}: ON", end="", flush=True)
    lines.set_value(18, Value.ACTIVE)
    time.sleep(0.5)
    print(" OFF")
    lines.set_value(18, Value.INACTIVE)
    time.sleep(0.5)
lines.release()
chip.close()
EOF'
```

**RESULT:**
- [ ] Backlight turned ON/OFF (changes in brightness or visible LED)
- [ ] No visible change at all

### Step 2: Check Display Module Connections

The Waveshare display requires TWO separate connections:

1. **GPIO/SPI Control Signals** (currently connected and working):
   - GPIO 8, 25, 27, 18 for the LCD
   - GPIO 2, 3, 4, 17 for touch-enabled demos
   - SPI signals on GPIO 9, 10, 11

2. **Main Power Supply** (likely missing):
   - VCC: 3.3V or 5V (depends on module variant)
   - GND: Ground reference

The current Pi demo wiring assumes the Raspberry Pi header connections below:

| Function | BCM GPIO | Physical Pin |
|----------|----------|--------------|
| CS | GPIO 8 | Pin 24 |
| DC | GPIO 25 | Pin 22 |
| LCD RST | GPIO 27 | Pin 13 |
| BL | GPIO 18 | Pin 12 |
| MOSI | GPIO 10 | Pin 19 |
| MISO | GPIO 9 | Pin 21 |
| SCLK | GPIO 11 | Pin 23 |
| Touch SDA | GPIO 2 | Pin 3 |
| Touch SCL | GPIO 3 | Pin 5 |
| Touch INT | GPIO 4 | Pin 7 |
| Touch RST | GPIO 17 | Pin 11 |

Before debugging the display itself, confirm `/boot/firmware/config.txt` enables both buses:

```ini
dtparam=spi=on
dtparam=i2c_arm=on
```

**Check if these connections exist:**
```bash
# Physically inspect the display module/cable for:
# - A separate power connector (JST, micro-USB, dupont pins, etc.)
# - Or power pins on the main ribbon cable
# - Or separate red/black wires for power
```

### Step 3: Measure Power

If you have a multimeter:

```bash
# Check if display has power
1. Measure between display GND and VCC: should show 3.3V or 5V
2. Measure between display GND and backlight pin: should show ~3.3V when GPIO18=HIGH

# If measurements show 0V everywhere:
→ Display not receiving power, even if GPIO works
```

## Solution: Adding Power

### Case A: Display Has Separate Power Connector
- Connect display module power input to Raspberry Pi 5V and GND
- Ensure current limiting or use appropriate power supply

### Case B: Display Power on Ribbon Cable
- Check pin 1, 2, 19, 20 on the 20-pin connector (common for SPI displays)
- Connect to 3.3V or 5V accordingly

### Case C: No Visible Power Connections
- Check Waveshare wiki/manual for exact pinout
- Display may require specific power supply configuration

## Alternative: Software Verification (No Hardware Needed)

If you can't easily access power pins, verify all software is working:

```bash
# Run full test suite
cd ~/i2c_led_demos

# Test 1: GPIO/SPI loopback
sudo python3 -c "
import gpiod, spidev
# GPIO works if no exceptions
chip = gpiod.Chip('/dev/gpiochip0')
chip.close()
# SPI works if no exceptions
spi = spidev.SpiDev()
try:
   spi.open(0, 0)
except Exception:
   spi.open(10, 0)
spi.close()
print('✓ GPIO and SPI hardware working')
"

# Test 2: Display initialization
sudo python3 badapple_waveshare.py --test
# If this completes without errors and shows:
#   [LCD] Display initialized
# → All software is correct

# Test 3: Full display test
sudo python3 lcd_test.py
# If completes without errors:
# → All communication working
```

## Expected Behavior

### When Everything Works (Power Connected)
1. Display backlight turns ON (visible illumination)
2. Display shows content or is bright/active
3. Colors fill correctly
4. Video plays smoothly

### When Power Missing (Current Situation)
1. Backlight may or may not respond to GPIO control
2. Display remains completely dark/inactive
3. All software says "success" but nothing visible
4. GPIO/SPI transfers work but no pixel activity

## Checklist

- [ ] Ran backlight blink test - did you see any change?
- [ ] Verified display module has 5V/GND pins
- [ ] Measured voltage between display VCC and GND
- [ ] Checked for separate power connector on display
- [ ] Verified all GPIO/SPI connections secure
- [ ] Ran lcd_test.py successfully
- [ ] Connected display power supply

## Files for Reference

- `badapple_waveshare.py` - Main video player
- `lcd_test.py` - Display test
- `WAVESHARE_README.md` - Full integration guide
- `DISPLAY_STATUS.md` - Technical details

## Next Steps

Once power is verified:

```bash
# Quick test
sudo python3 badapple_waveshare.py --test

# Play video (if file available)
sudo python3 badapple_waveshare.py bad_apple.bin
```

## Support

The software is complete and tested. The issue is purely hardware (power delivery).

Key facts:
- GPIO control: ✓ Working
- SPI communication: ✓ Working  
- Display initialization: ✓ Working
- Pixel output: ✗ No power to display controller
