/*
 * lcd_gc9a01.h - GC9A01A LCD Controller Driver
 * 240x240 RGB565 1.28" OLED round display via SPI
 */

#ifndef LCD_GC9A01_H
#define LCD_GC9A01_H

#include <stdint.h>
#include <stddef.h>

/* GC9A01A Commands */
#define GC9A01A_SWRESET    0x01
#define GC9A01A_SLPIN      0x10
#define GC9A01A_SLPOUT     0x11
#define GC9A01A_PTLON      0x12
#define GC9A01A_PTLOFF     0x13
#define GC9A01A_INVOFF     0x20
#define GC9A01A_INVON      0x21
#define GC9A01A_DISPOFF    0x28
#define GC9A01A_DISPON     0x29
#define GC9A01A_CASET      0x2A
#define GC9A01A_RASET      0x2B
#define GC9A01A_RAMWR      0x2C
#define GC9A01A_RAMRD      0x2E
#define GC9A01A_MADCTL     0x36
#define GC9A01A_COLMOD     0x3A
#define GC9A01A_FRCTRL     0xB3

/* Color Depth */
#define GC9A01A_COLMOD_12BIT  0x03  /* 4096 colors */
#define GC9A01A_COLMOD_16BIT  0x05  /* 65K colors (RGB565) */
#define GC9A01A_COLMOD_18BIT  0x06  /* 262K colors */

/* Memory Data Access Control */
#define GC9A01A_MADCTL_MY   0x80   /* Row address order */
#define GC9A01A_MADCTL_MX   0x40   /* Column address order */
#define GC9A01A_MADCTL_MV   0x20   /* Row/Column exchange */
#define GC9A01A_MADCTL_ML   0x10   /* Vertical refresh */
#define GC9A01A_MADCTL_RGB  0x00   /* RGB color order */
#define GC9A01A_MADCTL_BGR  0x08   /* BGR color order */

/* Display Rotation */
typedef enum {
    GC9A01A_ROTATE_0   = 0x00,
    GC9A01A_ROTATE_90  = 0x60,
    GC9A01A_ROTATE_180 = 0xC0,
    GC9A01A_ROTATE_270 = 0xA0
} gc9a01a_rotation_t;

/* RGB565 Color Definition */
typedef uint16_t rgb565_t;

#define RGB565(r, g, b) \
    (((((r) >> 3) & 0x1F) << 11) | \
     ((((g) >> 2) & 0x3F) << 5) | \
     (((b) >> 3) & 0x1F))

/* Common Colors */
#define COLOR_BLACK     RGB565(0, 0, 0)
#define COLOR_WHITE     RGB565(255, 255, 255)
#define COLOR_RED       RGB565(255, 0, 0)
#define COLOR_GREEN     RGB565(0, 255, 0)
#define COLOR_BLUE      RGB565(0, 0, 255)
#define COLOR_CYAN      RGB565(0, 255, 255)
#define COLOR_MAGENTA   RGB565(255, 0, 255)
#define COLOR_YELLOW    RGB565(255, 255, 0)
#define COLOR_GRAY      RGB565(128, 128, 128)

/* Display Dimensions */
#define LCD_WIDTH   240
#define LCD_HEIGHT  240

/* Frame buffer size for RGB565 (240x240x2 bytes) */
#define LCD_FRAME_BUFFER_SIZE (LCD_WIDTH * LCD_HEIGHT * 2)

/* LCD Device Structure */
typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t rotation;
    rgb565_t bg_color;
} lcd_device_t;

/* Initialization */
int lcd_gc9a01a_init(void);
int lcd_gc9a01a_deinit(void);

/* Basic Control */
int lcd_gc9a01a_reset(void);
int lcd_gc9a01a_sleep_in(void);
int lcd_gc9a01a_sleep_out(void);
int lcd_gc9a01a_display_on(void);
int lcd_gc9a01a_display_off(void);
int lcd_gc9a01a_set_rotation(gc9a01a_rotation_t rotation);
int lcd_gc9a01a_set_invert(uint8_t invert);

/* Drawing Operations */
int lcd_gc9a01a_fill_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, rgb565_t color);
int lcd_gc9a01a_fill_screen(rgb565_t color);
int lcd_gc9a01a_draw_pixel(uint16_t x, uint16_t y, rgb565_t color);
int lcd_gc9a01a_draw_h_line(uint16_t x, uint16_t y, uint16_t length, rgb565_t color);
int lcd_gc9a01a_draw_v_line(uint16_t x, uint16_t y, uint16_t length, rgb565_t color);
int lcd_gc9a01a_draw_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, rgb565_t color);

/* Bulk Data Transfer */
int lcd_gc9a01a_write_ram(const uint8_t *data, size_t len);
int lcd_gc9a01a_set_address_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2);

/* Backlight Control */
int lcd_gc9a01a_set_backlight(uint8_t brightness);  /* 0-255 */

/* Frame Buffer */
int lcd_gc9a01a_push_frame_buffer(const rgb565_t *fb);

#endif /* LCD_GC9A01_H */
