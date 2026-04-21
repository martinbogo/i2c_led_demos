#!/usr/bin/env python3
"""
Waveshare 1.28" LCD GC9A01A Test using gpiod
Tests display communication and basic drawing
"""

import time
import sys

try:
    import gpiod
    from gpiod.line import Direction, Value
except ImportError:
    print("Error: gpiod Python module not found")
    print("Install with: sudo apt install python3-libgpiod")
    sys.exit(1)

try:
    import spidev
except ImportError:
    print("Error: spidev Python module not found")
    sys.exit(1)

# GPIO Configuration (BCM pins)
LCD_CS_GPIO = 8
LCD_DC_GPIO = 25
LCD_RST_GPIO = 27
LCD_BL_GPIO = 18

# SPI Configuration
SPI_BUS = 10
SPI_DEVICE = 0
SPI_SPEED = 10000000  # 10 MHz

# GC9A01A Commands
GC9A01A_SWRESET = 0x01
GC9A01A_SLPOUT = 0x11
GC9A01A_INVOFF = 0x20
GC9A01A_DISPON = 0x29
GC9A01A_CASET = 0x2A
GC9A01A_RASET = 0x2B
GC9A01A_RAMWR = 0x2C
GC9A01A_MADCTL = 0x36
GC9A01A_COLMOD = 0x3A

# Colors (RGB565)
COLOR_BLACK = 0x0000
COLOR_WHITE = 0xFFFF
COLOR_RED = 0xF800
COLOR_GREEN = 0x07E0
COLOR_BLUE = 0x001F

LCD_WIDTH = 240
LCD_HEIGHT = 240

class LCD_GC9A01A:
    """GC9A01A Display Driver with gpiod"""
    
    def __init__(self):
        # Initialize SPI
        self.spi = spidev.SpiDev()
        try:
            self.spi.open(SPI_BUS, SPI_DEVICE)
            self.spi.max_speed_hz = SPI_SPEED
            self.spi.mode = 0
            print(f"[SPI] Opened /dev/spidev{SPI_BUS}.{SPI_DEVICE} at {SPI_SPEED/1e6:.1f}MHz")
        except Exception as e:
            print(f"[SPI] Error opening SPI device: {e}")
            raise
        
        # Initialize GPIO with gpiod
        print("[GPIO] Initializing GPIO control...")
        self.chip = gpiod.Chip("/dev/gpiochip0")
        
        # Request GPIO lines as outputs
        gpio_config = {
            LCD_CS_GPIO: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE),
            LCD_DC_GPIO: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE),
            LCD_RST_GPIO: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE),
            LCD_BL_GPIO: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE),
        }
        
        self.lines = self.chip.request_lines(gpio_config, consumer="LCD")
        print(f"[GPIO] GPIO lines {LCD_CS_GPIO},{LCD_DC_GPIO},{LCD_RST_GPIO},{LCD_BL_GPIO} requested")
    
    def gpio_set(self, pin, value):
        """Set GPIO pin value (0=low, 1=high)"""
        val = Value.ACTIVE if value else Value.INACTIVE
        self.lines.set_value(pin, val)
    
    def write_command(self, cmd):
        """Write command byte (DC=0)"""
        self.gpio_set(LCD_DC_GPIO, 0)
        self.gpio_set(LCD_CS_GPIO, 0)
        self.spi.writebytes([cmd])
        self.gpio_set(LCD_CS_GPIO, 1)
        time.sleep(0.001)
    
    def write_data_bytes(self, data):
        """Write data bytes (DC=1)"""
        self.gpio_set(LCD_DC_GPIO, 1)
        self.gpio_set(LCD_CS_GPIO, 0)
        
        # Send data in chunks to avoid hitting kernel buffer limits
        max_chunk = 4000  # Keep under 4096 byte limit
        if isinstance(data, (list, bytes)):
            for i in range(0, len(data), max_chunk):
                chunk = data[i:i+max_chunk]
                self.spi.writebytes(chunk)
        else:
            self.spi.writebytes([data])
        
        self.gpio_set(LCD_CS_GPIO, 1)
    
    def write_cmd_data(self, cmd, data=None):
        """Write command followed by optional data"""
        self.write_command(cmd)
        if data:
            self.write_data_bytes(data)
    
    def init(self):
        """Initialize the display"""
        print("[LCD] Initializing display...")
        
        # Set all GPIO outputs to inactive
        self.gpio_set(LCD_CS_GPIO, 1)   # CS inactive
        self.gpio_set(LCD_DC_GPIO, 0)   # Command mode
        self.gpio_set(LCD_BL_GPIO, 0)   # Backlight off
        
        # Hardware reset
        print("[LCD] Performing hardware reset...")
        self.gpio_set(LCD_RST_GPIO, 0)
        time.sleep(0.02)
        self.gpio_set(LCD_RST_GPIO, 1)
        time.sleep(0.12)
        
        # Software reset
        print("[LCD] Sending software reset...")
        self.write_command(GC9A01A_SWRESET)
        time.sleep(0.15)
        
        # Sleep out
        print("[LCD] Sleep out...")
        self.write_command(GC9A01A_SLPOUT)
        time.sleep(0.01)
        
        # Color mode - RGB565 (0x05)
        print("[LCD] Setting color mode to RGB565...")
        self.write_cmd_data(GC9A01A_COLMOD, [0x05])
        
        # Memory data access control
        print("[LCD] Setting memory access control...")
        self.write_cmd_data(GC9A01A_MADCTL, [0x00])
        
        # Inversion off
        self.write_command(GC9A01A_INVOFF)
        
        # Display on
        print("[LCD] Turning display on...")
        self.write_command(GC9A01A_DISPON)
        time.sleep(0.01)
        
        # Backlight on
        print("[LCD] Turning backlight on...")
        self.gpio_set(LCD_BL_GPIO, 1)
        time.sleep(0.1)
        
        print("[LCD] Display initialized successfully!")
    
    def set_address_window(self, x1, y1, x2, y2):
        """Set the address window for drawing"""
        # CASET (Column Address Set)
        caset_data = [(x1 >> 8) & 0xFF, x1 & 0xFF, 
                      (x2 >> 8) & 0xFF, x2 & 0xFF]
        self.write_cmd_data(GC9A01A_CASET, caset_data)
        
        # RASET (Row Address Set)
        raset_data = [(y1 >> 8) & 0xFF, y1 & 0xFF, 
                      (y2 >> 8) & 0xFF, y2 & 0xFF]
        self.write_cmd_data(GC9A01A_RASET, raset_data)
    
    def fill_screen(self, color):
        """Fill entire screen with color"""
        self.set_address_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1)
        self.write_command(GC9A01A_RAMWR)
        
        # Create pixel data (RGB565)
        pixels = []
        for _ in range(LCD_WIDTH * LCD_HEIGHT):
            pixels.append((color >> 8) & 0xFF)
            pixels.append(color & 0xFF)
        
        self.write_data_bytes(pixels)
    
    def fill_rect(self, x1, y1, x2, y2, color):
        """Fill rectangle with color"""
        if x1 > x2:
            x1, x2 = x2, x1
        if y1 > y2:
            y1, y2 = y2, y1
        
        width = x2 - x1 + 1
        height = y2 - y1 + 1
        
        self.set_address_window(x1, y1, x2, y2)
        self.write_command(GC9A01A_RAMWR)
        
        pixels = []
        for _ in range(width * height):
            pixels.append((color >> 8) & 0xFF)
            pixels.append(color & 0xFF)
        
        self.write_data_bytes(pixels)
    
    def cleanup(self):
        """Close resources"""
        try:
            if self.lines:
                self.lines.release()
        except:
            pass
        try:
            if self.chip:
                self.chip.close()
        except:
            pass
        try:
            self.spi.close()
        except:
            pass

def main():
    lcd = None
    
    try:
        # Initialize display
        lcd = LCD_GC9A01A()
        lcd.init()
        
        print("\n=== Display Test Sequence ===\n")
        
        # Test 1: Colors
        print("Test 1: Filling screen with colors")
        for color, name in [(COLOR_BLACK, "Black"),
                           (COLOR_RED, "Red"),
                           (COLOR_GREEN, "Green"),
                           (COLOR_BLUE, "Blue"),
                           (COLOR_WHITE, "White")]:
            print(f"  - {name}...", end="", flush=True)
            lcd.fill_screen(color)
            time.sleep(1)
            print(" OK")
        
        # Test 2: Rectangles
        print("\nTest 2: Drawing rectangles")
        lcd.fill_screen(COLOR_BLACK)
        time.sleep(0.5)
        
        print("  - Red rectangle...", end="", flush=True)
        lcd.fill_rect(20, 20, 100, 100, COLOR_RED)
        time.sleep(1)
        print(" OK")
        
        print("  - Green rectangle...", end="", flush=True)
        lcd.fill_rect(120, 120, 220, 220, COLOR_GREEN)
        time.sleep(1)
        print(" OK")
        
        # Test 3: Backlight control
        print("\nTest 3: Backlight control")
        lcd.fill_screen(COLOR_WHITE)
        
        print("  - Backlight off...", end="", flush=True)
        lcd.gpio_set(LCD_BL_GPIO, 0)
        time.sleep(1)
        print(" OK")
        
        print("  - Backlight on...", end="", flush=True)
        lcd.gpio_set(LCD_BL_GPIO, 1)
        time.sleep(1)
        print(" OK")
        
        print("\n✓ All tests completed successfully!")
        print("Display is functional and ready for video streaming.\n")
        
    except KeyboardInterrupt:
        print("\n\nInterrupted by user")
    except Exception as e:
        print(f"\n\n✗ Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)
    finally:
        if lcd:
            lcd.cleanup()

if __name__ == "__main__":
    main()
