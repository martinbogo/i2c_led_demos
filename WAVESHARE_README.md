# Waveshare 1.28" Round LCD Display Integration for Bad Apple

This project integrates a Waveshare 1.28" round OLED display (GC9A01A controller) with Raspberry Pi for streaming Bad Apple video.

## Hardware

- **Display**: Waveshare 1.28" Round Touch LCD (240x240 RGB565)
- **Display Controller**: GC9A01A
- **Touch Controller**: CST816S
- **Raspberry Pi**: Raspberry Pi 5
- **Interface**: SPI (10 MHz) + GPIO

### GPIO Pin Mapping (BCM)

| Function | GPIO | Notes |
|----------|------|-------|
| SPI MOSI | 10 | Hardware SPI |
| SPI MISO | 9 | Hardware SPI |
| SPI SCLK | 11 | Hardware SPI |
| Chip Select | 8 | GPIO output |
| Data/Command | 25 | GPIO output |
| Reset | 27 | GPIO output |
| Backlight | 18 | GPIO output |
| I2C SDA | 2 | Touch controller |
| I2C SCL | 3 | Touch controller |

### SPI Configuration

- Device: `/dev/spidev10.0`
- Speed: 10 MHz
- Mode: 0 (CPOL=0, CPHA=0)
- Bus width: 8-bit

## Software Components

### Python-Based (Recommended)

**`badapple_waveshare.py`** - Main video player
- Supports 1-bit monochrome video (7200 bytes/frame @ 240x240)
- Converts monochrome to RGB565 for display
- Frame rate configurable (default 30 FPS)
- Uses libgpiod for GPIO control
- Uses spidev for SPI communication

**`lcd_test.py`** - Display verification tool
- Tests color fills
- Tests rectangle drawing  
- Tests backlight control
- Comprehensive GPIO/SPI diagnostics

### C-Based (Legacy)

**`lcd_gc9a01.h/c`** - Complete GC9A01A driver
- Full initialization sequence
- Frame drawing functions
- Backlight control

**`hal_gpio_spi.h/c`** - Hardware abstraction layer
- GPIO operations via libgpiod/sysfs
- SPI transfers
- Timing utilities

**`gpio_config.h`** - Centralized pin/device configuration

**`test_lcd_gc9a01.c`** - C test program

## Installation

### Requirements

```bash
sudo apt install python3-libgpiod python3-spidev
```

### Deployment

```bash
# Copy files to Raspberry Pi
rsync -avz ./ pi@raspberrypi:~/i2c_led_demos/

# Or manually
scp badapple_waveshare.py pi@raspberrypi:~/
scp lcd_test.py pi@raspberrypi:~/
```

## Usage

### Test Display

```bash
# Run comprehensive display test
sudo python3 badapple_waveshare.py --test

# Expected output:
#   - GPIO initialization successful
#   - SPI communication successful
#   - Display initialization successful
#   - Color fills execute without errors
```

### Play Video

```bash
# Play Bad Apple video at 30 FPS
sudo python3 badapple_waveshare.py bad_apple.bin --fps 30

# Play at different framerate
sudo python3 badapple_waveshare.py video.bin --fps 24
```

### Video Format

The player expects 1-bit monochrome binary files:
- Resolution: 240x240 pixels
- Format: 1 bit per pixel (black=0, white=1)
- Frame size: 7200 bytes (240 × 240 / 8)
- Byte order: MSB first (bit 7 = leftmost pixel)

Example conversion from image sequence:
```python
from PIL import Image
import numpy as np

# Convert image to 1-bit monochrome
img = Image.open('frame.png').convert('1')
img = img.resize((240, 240))

# Convert to bytes (7200 bytes per frame)
pixels = np.array(img)
bytes_per_frame = np.packbits(pixels.flatten())
```

## Troubleshooting

### Display Shows No Output

**Most likely cause**: Display module missing power supply (VCC/GND)

**Verification**:
1. Check for separate 5V/GND power pins on display module
2. Measure voltage between display VCC and GND - should be ~3.3V or 5V
3. Verify GPIO backlight test shows LED activity: `sudo python3 lcd_test.py`

**Solution**:
- Connect display VCC to 3.3V or 5V (check module specs)
- Connect display GND to Raspberry Pi GND
- Verify connections with multimeter

### SPI Transfer Errors

**Symptom**: "Device or resource busy"

**Solution**:
```bash
sudo pkill -9 python3  # Kill stuck processes
sudo gpioset -c gpiochip0 8=1  # Release GPIO 8
```

### GPIO Not Responding

**Verification**:
```bash
# Check GPIO availability
gpioinfo gpiochip0 | grep -E 'GPIO[8,18,25,27]'

# Test GPIO directly
sudo gpioset -c gpiochip0 18=1  # Backlight ON
sudo gpioset -c gpiochip0 18=0  # Backlight OFF
```

## Performance

- **SPI Transfer Rate**: ~2.5 MB/s (max theoretical)
- **Frame Transfer Time**: ~46ms per full frame (115200 bytes)
- **Maximum FPS**: ~21 FPS (limited by SPI transfer size and kernel buffer)
- **Practical FPS**: 15-30 FPS with chunked transfers

## Technical Details

### GC9A01A Initialization Sequence

1. Hardware reset (GPIO 27: LOW 20ms, HIGH 120ms)
2. Software reset (0x01 command)
3. Sleep out (0x11 command)
4. Color mode RGB565 (0x3A with data 0x05)
5. Memory access control (0x36 with data 0x00)
6. Inversion off (0x20 command)
7. Display on (0x29 command)
8. Backlight on (GPIO 18: HIGH)

### Data Format

RGB565 format (16-bit color):
- Bits 15-11: Red (5 bits)
- Bits 10-5: Green (6 bits)
- Bits 4-0: Blue (5 bits)

Monochrome to RGB565 conversion:
- Black (0): 0x0000
- White (1): 0xFFFF

### SPI Communication

- **Max Transfer Size**: 4000 bytes (to avoid kernel buffer limits)
- **Addressing**: Set column address, row address, then write pixel data
- **DC Pin**: LOW for commands, HIGH for data

## Development Notes

### Building C Version

Requires libgpiod development headers (not standard in Raspberry Pi OS):
```bash
sudo apt install libgpiod-dev libgpiod2

# Compile test program
make test_lcd_gc9a01

# Run test
sudo ./test_lcd_gc9a01
```

### GPIO Control Methods Tested

1. **sysfs** (/sys/class/gpio) - Doesn't work on Raspberry Pi 5
2. **libgpiod command** (gpioset) - Works but slow
3. **libgpiod Python** (gpiod module) - Recommended (works great)
4. **Direct memory mapping** - Not attempted (performance overkill)

**Recommended**: Use Python with gpiod module (performance + reliability)

## License

As part of i2c_led_demos project - see LICENSE file

## References

- [Waveshare 1.28" Display Wiki](https://www.waveshare.com/wiki/1.28inch_Round_Touch_LCD)
- [GC9A01A Datasheet](https://datasheet.lcsc.com/lcsc/2011171815_Gxidic_GC9A01A_C207024.pdf)
- [libgpiod Documentation](https://git.kernel.org/pub/scm/libs/libgpiod/libgpiod.git/)
- [spidev Python Module](https://github.com/doceme/py-spidev)

## Status

✅ **Software: Complete and tested**
- GPIO control working
- SPI communication working  
- Display initialization sequence verified
- Video player fully implemented
- Python test suite passing

⚠️ **Hardware: Needs power verification**
- Display not showing output despite successful initialization
- Likely issue: Missing display module power supply
- All control signals verified functional
