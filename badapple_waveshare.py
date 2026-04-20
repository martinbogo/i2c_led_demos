#!/usr/bin/env python3
"""
Bad Apple video player for Waveshare 1.28" display
Plays 1-bit monochrome video on GC9A01A display
"""

import gpiod
from gpiod.line import Direction, Value
import spidev
import time
import sys
import os
import struct

# GPIO Configuration (BCM pins on gpiochip0)
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

LCD_WIDTH = 240
LCD_HEIGHT = 240

class DisplayDriver:
    """GC9A01A Display Driver"""
    
    def __init__(self, verbose=False, spi_speed_hz=SPI_SPEED, backlight_active_high=True):
        self.verbose = verbose
        self.spi_speed_hz = spi_speed_hz
        self.backlight_active_high = backlight_active_high
        
        # Initialize SPI
        self.spi = spidev.SpiDev()
        try:
            self.spi.open(SPI_BUS, SPI_DEVICE)
            self.spi.max_speed_hz = self.spi_speed_hz
            self.spi.mode = 0
            self._log(f"[SPI] Opened /dev/spidev{SPI_BUS}.{SPI_DEVICE} at {self.spi_speed_hz/1e6:.1f}MHz")
        except Exception as e:
            self._log(f"[ERROR] Failed to open SPI: {e}", force=True)
            raise
        
        # Initialize GPIO
        self._log("[GPIO] Initializing GPIO control...")
        self.chip = gpiod.Chip("/dev/gpiochip0")
        
        gpio_config = {
            LCD_CS_GPIO: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE),
            LCD_DC_GPIO: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE),
            LCD_RST_GPIO: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE),
            LCD_BL_GPIO: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE),
        }
        
        self.lines = self.chip.request_lines(gpio_config, consumer="BadApple")
        self._log(f"[GPIO] GPIO lines {LCD_CS_GPIO},{LCD_DC_GPIO},{LCD_RST_GPIO},{LCD_BL_GPIO} requested")
    
    def _log(self, msg, force=False):
        if self.verbose or force:
            print(msg)
    
    def _gpio_set(self, pin, value):
        """Set GPIO pin value"""
        val = Value.ACTIVE if value else Value.INACTIVE
        self.lines.set_value(pin, val)

    def _set_backlight(self, enabled):
        """Set backlight considering active-high/active-low polarity."""
        level = enabled if self.backlight_active_high else (not enabled)
        self._gpio_set(LCD_BL_GPIO, 1 if level else 0)
    
    def _write_command(self, cmd):
        """Write command byte (DC=0)"""
        self._gpio_set(LCD_DC_GPIO, 0)
        self._gpio_set(LCD_CS_GPIO, 0)
        self.spi.writebytes([cmd])
        self._gpio_set(LCD_CS_GPIO, 1)
        time.sleep(0.001)
    
    def _write_data_bytes(self, data):
        """Write data bytes (DC=1)"""
        self._gpio_set(LCD_DC_GPIO, 1)
        self._gpio_set(LCD_CS_GPIO, 0)
        
        # Send data in chunks to avoid kernel buffer limits
        max_chunk = 4000
        if isinstance(data, (list, bytes)):
            for i in range(0, len(data), max_chunk):
                chunk = data[i:i+max_chunk]
                self.spi.writebytes(chunk)
        else:
            self.spi.writebytes([data])
        
        self._gpio_set(LCD_CS_GPIO, 1)
    
    def _write_cmd_data(self, cmd, data=None):
        """Write command followed by optional data"""
        self._write_command(cmd)
        if data:
            self._write_data_bytes(data)

    def _run_waveshare_init_sequence(self):
        """Apply a fuller GC9A01A init sequence used by Waveshare-style demos."""
        init_cmds = [
            (0xEF, None),
            (0xEB, [0x14]),
            (0xFE, None),
            (0xEF, None),
            (0xEB, [0x14]),
            (0x84, [0x40]),
            (0x85, [0xFF]),
            (0x86, [0xFF]),
            (0x87, [0xFF]),
            (0x88, [0x0A]),
            (0x89, [0x21]),
            (0x8A, [0x00]),
            (0x8B, [0x80]),
            (0x8C, [0x01]),
            (0x8D, [0x01]),
            (0x8E, [0xFF]),
            (0x8F, [0xFF]),
            (0xB6, [0x00, 0x00]),
            (GC9A01A_MADCTL, [0x08]),
            (GC9A01A_COLMOD, [0x05]),
            (0x90, [0x08, 0x08, 0x08, 0x08]),
            (0xBD, [0x06]),
            (0xBC, [0x00]),
            (0xFF, [0x60, 0x01, 0x04]),
            (0xC3, [0x13]),
            (0xC4, [0x13]),
            (0xC9, [0x22]),
            (0xBE, [0x11]),
            (0xE1, [0x10, 0x0E]),
            (0xDF, [0x21, 0x0C, 0x02]),
            (0xF0, [0x45, 0x09, 0x08, 0x08, 0x26, 0x2A]),
            (0xF1, [0x43, 0x70, 0x72, 0x36, 0x37, 0x6F]),
            (0xF2, [0x45, 0x09, 0x08, 0x08, 0x26, 0x2A]),
            (0xF3, [0x43, 0x70, 0x72, 0x36, 0x37, 0x6F]),
            (0xED, [0x1B, 0x0B]),
            (0xAE, [0x77]),
            (0xCD, [0x63]),
            (0x70, [0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03]),
            (0xE8, [0x34]),
            (0x62, [0x18, 0x0D, 0x71, 0xED, 0x70, 0x70, 0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70]),
            (0x63, [0x18, 0x11, 0x71, 0xF1, 0x70, 0x70, 0x18, 0x13, 0x71, 0xF3, 0x70, 0x70]),
            (0x64, [0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07]),
            (0x66, [0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00]),
            (0x67, [0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98]),
            (0x74, [0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00]),
            (0x98, [0x3E, 0x07]),
        ]

        for cmd, data in init_cmds:
            self._write_cmd_data(cmd, data)
    
    def init(self):
        """Initialize the display"""
        self._log("[LCD] Initializing display...")
        
        # Set GPIO defaults
        self._gpio_set(LCD_CS_GPIO, 1)
        self._gpio_set(LCD_DC_GPIO, 0)
        self._set_backlight(False)
        
        # Hardware reset
        self._log("[LCD] Hardware reset...")
        self._gpio_set(LCD_RST_GPIO, 0)
        time.sleep(0.02)
        self._gpio_set(LCD_RST_GPIO, 1)
        time.sleep(0.12)
        
        # Software reset
        self._write_command(GC9A01A_SWRESET)
        time.sleep(0.15)
        
        # Sleep out
        self._write_command(GC9A01A_SLPOUT)
        time.sleep(0.12)
        
        # Apply fuller panel setup for Waveshare GC9A01 module variants
        self._run_waveshare_init_sequence()
        
        # Display on
        self._write_command(GC9A01A_DISPON)
        time.sleep(0.02)
        
        # Backlight on
        self._set_backlight(True)
        time.sleep(0.1)
        
        self._log("[LCD] Display initialized")
    
    def set_address_window(self, x1, y1, x2, y2):
        """Set address window"""
        caset_data = [(x1 >> 8) & 0xFF, x1 & 0xFF, (x2 >> 8) & 0xFF, x2 & 0xFF]
        self._write_cmd_data(GC9A01A_CASET, caset_data)
        
        raset_data = [(y1 >> 8) & 0xFF, y1 & 0xFF, (y2 >> 8) & 0xFF, y2 & 0xFF]
        self._write_cmd_data(GC9A01A_RASET, raset_data)
    
    def draw_frame(self, frame_data):
        """Draw a frame from monochrome (1-bit) or RGB565 data
        
        Args:
            frame_data: bytes/bytearray of pixel data
                       - If monochrome (1-bit): 7200 bytes (240*240/8)
                       - If RGB565: 115200 bytes (240*240*2)
        """
        if len(frame_data) == 7200:
            # Monochrome 1-bit: convert to RGB565
            self._draw_monochrome_frame(frame_data)
        elif len(frame_data) == 115200:
            # Already RGB565
            self._draw_rgb565_frame(frame_data)
        else:
            raise ValueError(f"Unsupported frame size: {len(frame_data)} bytes")
    
    def _draw_monochrome_frame(self, mono_data):
        """Draw monochrome 1-bit frame as white pixels on black"""
        self.set_address_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1)
        self._write_command(GC9A01A_RAMWR)
        
        rgb565_data = []
        for byte_idx, byte_val in enumerate(mono_data):
            for bit in range(8):
                # Each bit represents one pixel
                is_set = (byte_val >> (7 - bit)) & 1
                # Convert to RGB565: white (0xFFFF) or black (0x0000)
                color = 0xFFFF if is_set else 0x0000
                rgb565_data.append((color >> 8) & 0xFF)
                rgb565_data.append(color & 0xFF)
        
        self._write_data_bytes(rgb565_data)
    
    def _draw_rgb565_frame(self, rgb565_data):
        """Draw RGB565 frame"""
        self.set_address_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1)
        self._write_command(GC9A01A_RAMWR)
        self._write_data_bytes(rgb565_data)
    
    def cleanup(self):
        """Cleanup resources"""
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


class BadApplePlayer:
    """Bad Apple video player for display"""
    
    def __init__(self, video_file=None, fps=30, spi_speed_hz=SPI_SPEED, backlight_active_high=True):
        self.display = DisplayDriver(
            verbose=True,
            spi_speed_hz=spi_speed_hz,
            backlight_active_high=backlight_active_high,
        )
        self.video_file = video_file
        self.fps = fps
        self.frame_delay = 1.0 / fps
    
    def init(self):
        """Initialize display"""
        self.display.init()
    
    def play(self):
        """Play video from file"""
        if not self.video_file:
            print("Error: No video file specified")
            return False
        
        if not os.path.exists(self.video_file):
            print(f"Error: Video file not found: {self.video_file}")
            return False
        
        print(f"Playing video: {self.video_file}")
        print(f"FPS: {self.fps}, Frame delay: {self.frame_delay:.3f}s")
        
        try:
            with open(self.video_file, 'rb') as f:
                frame_num = 0
                while True:
                    # Read frame (240*240 pixels / 8 = 7200 bytes for 1-bit mono)
                    frame_data = f.read(7200)
                    if not frame_data:
                        break
                    
                    if len(frame_data) < 7200:
                        # Pad last frame if needed
                        frame_data += b'\x00' * (7200 - len(frame_data))
                    
                    start_time = time.time()
                    self.display.draw_frame(frame_data)
                    elapsed = time.time() - start_time
                    
                    frame_num += 1
                    if frame_num % 30 == 0:
                        print(f"  Frame {frame_num} (timing: {elapsed*1000:.1f}ms)")
                    
                    # Frame rate control
                    sleep_time = self.frame_delay - elapsed
                    if sleep_time > 0:
                        time.sleep(sleep_time)
            
            print(f"✓ Playback complete: {frame_num} frames played")
            return True
            
        except KeyboardInterrupt:
            print("\n✗ Playback interrupted by user")
            return False
        except Exception as e:
            print(f"✗ Playback error: {e}")
            import traceback
            traceback.print_exc()
            return False
    
    def cleanup(self):
        """Cleanup"""
        self.display.cleanup()


def main():
    import argparse
    
    parser = argparse.ArgumentParser(description="Bad Apple player for Waveshare display")
    parser.add_argument("video", nargs="?", help="Video file to play (1-bit monochrome, 7200 bytes per frame)")
    parser.add_argument("--fps", type=int, default=30, help="Frames per second (default: 30)")
    parser.add_argument("--test", action="store_true", help="Run display test instead of playing video")
    parser.add_argument("--spi-hz", type=int, default=SPI_SPEED, help="SPI clock in Hz (default: 10000000)")
    parser.add_argument("--bl-active-low", action="store_true", help="Use active-low backlight polarity")
    
    args = parser.parse_args()
    
    player = None
    try:
        if args.test:
            # Test mode
            print("Running display test...")
            player = BadApplePlayer(
                spi_speed_hz=args.spi_hz,
                backlight_active_high=not args.bl_active_low,
            )
            player.init()
            
            print("\n=== Display Test ===")
            
            # Test 1: Color fills
            print("Test 1: Color fills")
            colors = [0x0000, 0xF800, 0x07E0, 0x001F, 0xFFFF]  # BLACK, RED, GREEN, BLUE, WHITE
            for i, color in enumerate(colors):
                print(f"  Color {i+1}/5...", end="", flush=True)
                color_hi = (color >> 8) & 0xFF
                color_lo = color & 0xFF
                frame = bytearray([color_hi, color_lo] * (LCD_WIDTH * LCD_HEIGHT))
                player.display.draw_frame(frame)
                time.sleep(1)
                print(" OK")
            
            print("✓ Test complete")
            
        else:
            # Play video
            if not args.video:
                parser.print_help()
                print("\nExample: python3 badapple_waveshare.py badapple.bin --fps 30")
                return 1
            
            player = BadApplePlayer(
                args.video,
                fps=args.fps,
                spi_speed_hz=args.spi_hz,
                backlight_active_high=not args.bl_active_low,
            )
            player.init()
            player.play()
        
        return 0
        
    except KeyboardInterrupt:
        print("\nInterrupted")
        return 130
    except Exception as e:
        print(f"Error: {e}")
        import traceback
        traceback.print_exc()
        return 1
    finally:
        if player:
            player.cleanup()


if __name__ == "__main__":
    sys.exit(main())
