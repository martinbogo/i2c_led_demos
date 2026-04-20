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

# GPIO Configuration (BCM pins on gpiochip0)
LCD_CS_GPIO = 8
LCD_DC_GPIO = 25
LCD_RST_GPIO = 27
LCD_BL_GPIO = 18

# SPI Configuration
SPI_BUS = 0
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
    
    def __init__(self, verbose=False, spi_bus=SPI_BUS, spi_device=SPI_DEVICE, spi_speed_hz=SPI_SPEED, backlight_active_high=True, use_gpio_cs=False):
        self.verbose = verbose
        self.spi_bus = spi_bus
        self.spi_device = spi_device
        self.spi_speed_hz = spi_speed_hz
        self.backlight_active_high = backlight_active_high
        self.use_gpio_cs = use_gpio_cs
        
        # Initialize SPI
        self.spi = spidev.SpiDev()
        try:
            self.spi.open(self.spi_bus, self.spi_device)
            self.spi.max_speed_hz = self.spi_speed_hz
            self.spi.mode = 0
            self._log(f"[SPI] Opened /dev/spidev{self.spi_bus}.{self.spi_device} at {self.spi_speed_hz/1e6:.1f}MHz")
        except Exception as e:
            self._log(f"[ERROR] Failed to open SPI: {e}", force=True)
            raise
        
        # Initialize GPIO
        self._log("[GPIO] Initializing GPIO control...")
        self.chip = gpiod.Chip("/dev/gpiochip0")
        
        gpio_config = {
            LCD_DC_GPIO: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE),
            LCD_RST_GPIO: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE),
            LCD_BL_GPIO: gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE),
        }
        if self.use_gpio_cs:
            gpio_config[LCD_CS_GPIO] = gpiod.LineSettings(direction=Direction.OUTPUT, output_value=Value.INACTIVE)
        
        self.lines = self.chip.request_lines(gpio_config, consumer="BadApple")
        if self.use_gpio_cs:
            self._log(f"[GPIO] GPIO lines {LCD_CS_GPIO},{LCD_DC_GPIO},{LCD_RST_GPIO},{LCD_BL_GPIO} requested")
        else:
            self._log(f"[GPIO] GPIO lines {LCD_DC_GPIO},{LCD_RST_GPIO},{LCD_BL_GPIO} requested (SPI HW CS)")
    
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
        if self.use_gpio_cs:
            self._gpio_set(LCD_CS_GPIO, 0)
        self.spi.writebytes([cmd])
        if self.use_gpio_cs:
            self._gpio_set(LCD_CS_GPIO, 1)
        time.sleep(0.001)
    
    def _write_data_bytes(self, data):
        """Write data bytes (DC=1)"""
        self._gpio_set(LCD_DC_GPIO, 1)
        if self.use_gpio_cs:
            self._gpio_set(LCD_CS_GPIO, 0)
        
        # Send data in chunks to avoid kernel buffer limits
        max_chunk = 4000
        if isinstance(data, (list, bytes, bytearray)):
            for i in range(0, len(data), max_chunk):
                chunk = data[i:i+max_chunk]
                self.spi.writebytes(chunk)
        else:
            self.spi.writebytes([data])
        
        if self.use_gpio_cs:
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
        if self.use_gpio_cs:
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
    
    def __init__(self, video_file=None, fps=30, spi_bus=SPI_BUS, spi_device=SPI_DEVICE, spi_speed_hz=SPI_SPEED, backlight_active_high=True, use_gpio_cs=False):
        self.display = DisplayDriver(
            verbose=True,
            spi_bus=spi_bus,
            spi_device=spi_device,
            spi_speed_hz=spi_speed_hz,
            backlight_active_high=backlight_active_high,
            use_gpio_cs=use_gpio_cs,
        )
        self.video_file = video_file
        self.fps = fps
        self.frame_delay = 1.0 / fps
        self.frame_format = None
        self.frame_size = 0

        # Mapping parameters for legacy 128x48 SSD1306-style frames
        self.src_w = 128
        self.src_h = 48
        self.viewport_size = 220  # centered square area inside round panel
        self.dst_w = 0
        self.dst_h = 0
        self.dst_x0 = 0
        self.dst_y0 = 0
    
    def init(self):
        """Initialize display"""
        self.display.init()

    def _detect_frame_format(self):
        """Detect frame format from file size."""
        file_size = os.path.getsize(self.video_file)

        if file_size % 7200 == 0:
            self.frame_format = "mono240"
            self.frame_size = 7200
            return

        if file_size % 768 == 0:
            self.frame_format = "ssd1306_128x48"
            self.frame_size = 768

            # Fit 128x48 content into centered square viewport for round display
            scale = min(self.viewport_size / self.src_w, self.viewport_size / self.src_h)
            self.dst_w = max(1, int(self.src_w * scale))
            self.dst_h = max(1, int(self.src_h * scale))
            self.dst_x0 = (LCD_WIDTH - self.dst_w) // 2
            self.dst_y0 = (LCD_HEIGHT - self.dst_h) // 2
            return

        raise ValueError(
            f"Unsupported video format: {file_size} bytes (expected multiple of 7200 or 768)"
        )

    def _map_ssd1306_128x48_to_rgb565(self, frame_data):
        """Map 128x48 SSD1306 page-packed mono frame into 240x240 RGB565 frame."""
        # On this panel setup, 0xFFFF appears dark and 0x0000 appears bright.
        # Use a dark default fill so the area outside the rectangular video window is black.
        off_hi, off_lo = 0xFF, 0xFF  # dark background
        on_hi, on_lo = 0x00, 0x00    # bright foreground
        rgb = bytearray([off_hi, off_lo] * (LCD_WIDTH * LCD_HEIGHT))

        # Scale + center map
        for y_out in range(self.dst_h):
            y_src = (y_out * self.src_h) // self.dst_h
            page = y_src >> 3
            bit_mask = 1 << (y_src & 7)
            y_panel = self.dst_y0 + y_out
            row_base = y_panel * LCD_WIDTH

            for x_out in range(self.dst_w):
                x_src = (x_out * self.src_w) // self.dst_w
                src_byte = frame_data[x_src + page * self.src_w]
                if src_byte & bit_mask:
                    x_panel = self.dst_x0 + x_out
                    idx = (row_base + x_panel) * 2
                    rgb[idx] = on_hi
                    rgb[idx + 1] = on_lo

        return rgb
    
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

        self._detect_frame_format()
        print(f"Detected format: {self.frame_format} ({self.frame_size} bytes/frame)")
        if self.frame_format == "ssd1306_128x48":
            print(
                f"Mapping 128x48 -> {self.dst_w}x{self.dst_h} @ ({self.dst_x0},{self.dst_y0}) on 240x240"
            )
        
        try:
            with open(self.video_file, 'rb') as f:
                frame_num = 0
                while True:
                    frame_data = f.read(self.frame_size)
                    if not frame_data:
                        break
                    
                    if len(frame_data) < self.frame_size:
                        # Pad last frame if needed
                        frame_data += b'\x00' * (self.frame_size - len(frame_data))
                    
                    start_time = time.time()

                    if self.frame_format == "mono240":
                        self.display.draw_frame(frame_data)
                    else:
                        mapped = self._map_ssd1306_128x48_to_rgb565(frame_data)
                        self.display.draw_frame(mapped)

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
    parser.add_argument("video", nargs="?", help="Video file to play (auto-detects 240x240 mono or 128x48 SSD1306 packed)")
    parser.add_argument("--fps", type=int, default=30, help="Frames per second (default: 30)")
    parser.add_argument("--test", action="store_true", help="Run display test instead of playing video")
    parser.add_argument("--spi-bus", type=int, default=SPI_BUS, help="SPI bus number (default: 0)")
    parser.add_argument("--spi-device", type=int, default=SPI_DEVICE, help="SPI device number (default: 0)")
    parser.add_argument("--spi-hz", type=int, default=SPI_SPEED, help="SPI clock in Hz (default: 10000000)")
    parser.add_argument("--bl-active-low", action="store_true", help="Use active-low backlight polarity")
    parser.add_argument("--gpio-cs", action="store_true", help="Drive CS with GPIO instead of SPI hardware CS")
    
    args = parser.parse_args()
    
    player = None
    try:
        if args.test:
            # Test mode
            print("Running display test...")
            player = BadApplePlayer(
                spi_bus=args.spi_bus,
                spi_device=args.spi_device,
                spi_speed_hz=args.spi_hz,
                backlight_active_high=not args.bl_active_low,
                use_gpio_cs=args.gpio_cs,
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
                spi_bus=args.spi_bus,
                spi_device=args.spi_device,
                spi_speed_hz=args.spi_hz,
                backlight_active_high=not args.bl_active_low,
                use_gpio_cs=args.gpio_cs,
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
