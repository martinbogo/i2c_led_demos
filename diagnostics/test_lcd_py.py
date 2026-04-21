#!/usr/bin/env python3
"""
Test script for Waveshare 1.28" OLED display on Raspberry Pi
Uses RPi.GPIO for GPIO control and spidev for SPI
"""

import RPi.GPIO as GPIO
import spidev
import time
import sys

# GPIO Pin Definitions (BCM)
LCD_CS_GPIO = 8
LCD_DC_GPIO = 25
LCD_RST_GPIO = 27
LCD_BL_GPIO = 18

# SPI Configuration
SPI_SPEED = 10000000  # 10 MHz
SPI_DEVICE = (0, 0)   # /dev/spidev0.0

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

# Color definitions (RGB565)
COLOR_BLACK = 0x0000
COLOR_WHITE = 0xFFFF
COLOR_RED = 0xF800
COLOR_GREEN = 0x07E0
COLOR_BLUE = 0x001F
COLOR_CYAN = 0x07FF
COLOR_YELLOW = 0xFFE0
COLOR_GRAY = 0x8410

LCD_WIDTH = 240
LCD_HEIGHT = 240

class WaveshareLCD:
    def __init__(self):
        self.spi = None
        self.setup_gpio()
        self.setup_spi()
        self.init_display()
    
    def setup_gpio(self):
        """Initialize GPIO pins"""
        GPIO.setmode(GPIO.BCM)
        GPIO.setwarnings(False)
        
        # Set all pins as outputs
        for pin in [LCD_CS_GPIO, LCD_DC_GPIO, LCD_RST_GPIO, LCD_BL_GPIO]:
            GPIO.setup(pin, GPIO.OUT)
        
        # Initialize to inactive states
        GPIO.output(LCD_CS_GPIO, GPIO.HIGH)   # CS inactive
        GPIO.output(LCD_DC_GPIO, GPIO.LOW)    # DC = command
        GPIO.output(LCD_BL_GPIO, GPIO.LOW)    # BL off
        
        print("[GPIO] GPIO pins initialized")
    
    def setup_spi(self):
        """Initialize SPI interface"""
        try:
            self.spi = spidev.SpiDev()
            self.spi.open(0, 0)
            self.spi.max_speed_hz = SPI_SPEED
            self.spi.mode = 0
            print(f"[SPI] SPI initialized at {SPI_SPEED/1e6:.1f}MHz")
        except Exception as e:
            print(f"[SPI] Error: {e}")
            sys.exit(1)
    
    def write_command(self, cmd):
        """Write a command byte"""
        GPIO.output(LCD_DC_GPIO, GPIO.LOW)   # DC low = command
        GPIO.output(LCD_CS_GPIO, GPIO.LOW)   # CS low = select
        self.spi.writebytes([cmd])
        GPIO.output(LCD_CS_GPIO, GPIO.HIGH)  # CS high = deselect
    
    def write_data(self, data):
        """Write data bytes"""
        GPIO.output(LCD_DC_GPIO, GPIO.HIGH)  # DC high = data
        GPIO.output(LCD_CS_GPIO, GPIO.LOW)   # CS low = select
        if isinstance(data, int):
            self.spi.writebytes([data])
        else:
            self.spi.writebytes(data)
        GPIO.output(LCD_CS_GPIO, GPIO.HIGH)  # CS high = deselect
    
    def write_cmd_with_data(self, cmd, data=None):
        """Write command followed by data"""
        self.write_command(cmd)
        if data:
            self.write_data(data)
    
    def init_display(self):
        """Initialize the display"""
        print("[LCD] Starting initialization...")
        
        # Hardware reset
        GPIO.output(LCD_RST_GPIO, GPIO.LOW)
        time.sleep(0.02)
        GPIO.output(LCD_RST_GPIO, GPIO.HIGH)
        time.sleep(0.12)
        print("[LCD] Hardware reset completed")
        
        # Software reset
        self.write_command(GC9A01A_SWRESET)
        time.sleep(0.15)
        print("[LCD] Software reset sent")
        
        # Sleep out
        self.write_command(GC9A01A_SLPOUT)
        time.sleep(0.01)
        print("[LCD] Sleep out sent")
        
        # Color mode - RGB565
        self.write_cmd_with_data(GC9A01A_COLMOD, [0x05])
        print("[LCD] Color mode set to RGB565")
        
        # Memory data access control
        self.write_cmd_with_data(GC9A01A_MADCTL, [0x00])
        print("[LCD] Memory access control set")
        
        # Inversion off
        self.write_command(GC9A01A_INVOFF)
        
        # Display on
        self.write_command(GC9A01A_DISPON)
        print("[LCD] Display turned on")
        
        # Backlight on
        GPIO.output(LCD_BL_GPIO, GPIO.HIGH)
        print("[LCD] Backlight turned on")
        print("[LCD] Initialization complete!")
    
    def set_address_window(self, x1, y1, x2, y2):
        """Set the address window for drawing"""
        # Column address set
        caset_data = [(x1 >> 8) & 0xFF, x1 & 0xFF, (x2 >> 8) & 0xFF, x2 & 0xFF]
        self.write_cmd_with_data(GC9A01A_CASET, caset_data)
        
        # Row address set
        raset_data = [(y1 >> 8) & 0xFF, y1 & 0xFF, (y2 >> 8) & 0xFF, y2 & 0xFF]
        self.write_cmd_with_data(GC9A01A_RASET, raset_data)
    
    def fill_screen(self, color):
        """Fill the entire screen with a color"""
        self.fill_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, color)
    
    def fill_rect(self, x1, y1, x2, y2, color):
        """Fill a rectangle with a color"""
        if x1 > x2:
            x1, x2 = x2, x1
        if y1 > y2:
            y1, y2 = y2, y1
        
        pixels = (x2 - x1 + 1) * (y2 - y1 + 1)
        
        # Create pixel data
        pixel_data = []
        for _ in range(pixels):
            pixel_data.append((color >> 8) & 0xFF)
            pixel_data.append(color & 0xFF)
        
        self.set_address_window(x1, y1, x2, y2)
        self.write_command(GC9A01A_RAMWR)
        self.write_data(pixel_data)
    
    def draw_gradient(self):
        """Draw a color gradient"""
        print("[LCD] Drawing gradient...")
        for x in range(0, LCD_WIDTH, 5):
            r = int((x * 255) / LCD_WIDTH)
            g = int(((LCD_WIDTH - x) * 255) / LCD_WIDTH)
            b = 128
            color = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
            
            # Draw vertical line
            self.set_address_window(x, 0, x + 4, LCD_HEIGHT - 1)
            self.write_command(GC9A01A_RAMWR)
            
            pixels = []
            for _ in range(LCD_HEIGHT * 5):
                pixels.append((color >> 8) & 0xFF)
                pixels.append(color & 0xFF)
            
            self.write_data(pixels)
    
    def cleanup(self):
        """Cleanup GPIO and SPI"""
        if self.spi:
            self.spi.close()
        GPIO.cleanup()
        print("[Cleanup] GPIO and SPI cleaned up")

def main():
    try:
        lcd = WaveshareLCD()
        
        print("\n=== Test Sequence ===\n")
        
        # Test 1: Fill colors
        print("Test 1: Fill screen colors")
        for color, name in [(COLOR_BLACK, "Black"), (COLOR_RED, "Red"), 
                            (COLOR_GREEN, "Green"), (COLOR_BLUE, "Blue"),
                            (COLOR_WHITE, "White")]:
            print(f"  - {name}...")
            lcd.fill_screen(color)
            time.sleep(1)
        
        # Test 2: Draw gradient
        print("\nTest 2: Draw gradient")
        lcd.fill_screen(COLOR_BLACK)
        lcd.draw_gradient()
        time.sleep(2)
        
        # Test 3: Fill rectangles
        print("\nTest 3: Draw rectangles")
        lcd.fill_screen(COLOR_BLACK)
        lcd.fill_rect(10, 10, 100, 100, COLOR_RED)
        time.sleep(1)
        lcd.fill_rect(120, 20, 220, 120, COLOR_GREEN)
        time.sleep(1)
        
        print("\nAll tests completed successfully!")
        print("Display is ready for video streaming.\n")
        
    except KeyboardInterrupt:
        print("\nInterrupted by user")
    except Exception as e:
        print(f"\nError: {e}")
        import traceback
        traceback.print_exc()
    finally:
        lcd.cleanup()

if __name__ == "__main__":
    main()
