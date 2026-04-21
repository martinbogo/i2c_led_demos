/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-21
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * lcd_gc9a01.c - Low-level C driver for Waveshare 1.28" GC9A01 LCD
 * Uses hal_gpio_spi for display commands and video frame pushing.
 *
 */
/*
 * lcd_gc9a01.c - GC9A01A LCD Controller Driver Implementation
 * 240x240 RGB565 display via SPI
 */

#include "lcd_gc9a01.h"
#include "hal_gpio_spi.h"
#include "gpio_config.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static lcd_device_t lcd_dev = {
    .width = LCD_WIDTH,
    .height = LCD_HEIGHT,
    .rotation = GC9A01A_ROTATE_0,
    .bg_color = COLOR_BLACK
};

/* ============================================================================
 * Low-level SPI Commands
 * ============================================================================ */

static int lcd_write_command(uint8_t cmd) {
    hal_gpio_write(LCD_DC_GPIO, GPIO_LOW);  /* DC low = command */
    hal_gpio_write(LCD_CS_GPIO, GPIO_LOW);  /* CS low = select */
    int ret = hal_spi_write(&cmd, 1);
    hal_gpio_write(LCD_CS_GPIO, GPIO_HIGH); /* CS high = deselect */
    return ret;
}

static int lcd_write_data_byte(uint8_t data) {
    hal_gpio_write(LCD_DC_GPIO, GPIO_HIGH); /* DC high = data */
    hal_gpio_write(LCD_CS_GPIO, GPIO_LOW);  /* CS low = select */
    int ret = hal_spi_write(&data, 1);
    hal_gpio_write(LCD_CS_GPIO, GPIO_HIGH); /* CS high = deselect */
    return ret;
}

static int lcd_write_data(const uint8_t *data, size_t len) {
    hal_gpio_write(LCD_DC_GPIO, GPIO_HIGH); /* DC high = data */
    hal_gpio_write(LCD_CS_GPIO, GPIO_LOW);  /* CS low = select */
    int ret = hal_spi_write(data, len);
    hal_gpio_write(LCD_CS_GPIO, GPIO_HIGH); /* CS high = deselect */
    return ret;
}

static int lcd_write_cmd_with_data(uint8_t cmd, const uint8_t *data, size_t len) {
    lcd_write_command(cmd);
    if (len > 0) {
        lcd_write_data(data, len);
    }
    return 0;
}

/* ============================================================================
 * Display Initialization
 * ============================================================================ */

int lcd_gc9a01a_init(void) {
    /* Initialize HAL */
    if (hal_gpio_init() < 0) {
        fprintf(stderr, "Failed to initialize GPIO\n");
        return -1;
    }
    
    if (hal_spi_init(SPI_DEVICE, SPI_SPEED_HZ, SPI_MODE) < 0) {
        fprintf(stderr, "Failed to initialize SPI\n");
        return -1;
    }
    
    /* Configure GPIO pins */
    hal_gpio_set_mode(LCD_CS_GPIO, GPIO_MODE_OUT);
    hal_gpio_set_mode(LCD_DC_GPIO, GPIO_MODE_OUT);
    hal_gpio_set_mode(LCD_RST_GPIO, GPIO_MODE_OUT);
    hal_gpio_set_mode(LCD_BL_GPIO, GPIO_MODE_OUT);
    
    /* Initialize pin states */
    hal_gpio_write(LCD_CS_GPIO, GPIO_HIGH);  /* CS inactive */
    hal_gpio_write(LCD_DC_GPIO, GPIO_LOW);   /* DC = command by default */
    hal_gpio_write(LCD_BL_GPIO, GPIO_LOW);   /* Backlight off initially */
    
    /* Hardware reset */
    hal_gpio_write(LCD_RST_GPIO, GPIO_LOW);
    hal_delay_ms(20);
    hal_gpio_write(LCD_RST_GPIO, GPIO_HIGH);
    hal_delay_ms(120);
    
    /* GC9A01A Initialization Sequence */
    
    /* Software Reset */
    lcd_write_command(GC9A01A_SWRESET);
    hal_delay_ms(150);
    
    /* Sleep Out */
    lcd_write_command(GC9A01A_SLPOUT);
    hal_delay_ms(10);
    
    /* Color Mode - RGB565 */
    uint8_t colmod = GC9A01A_COLMOD_16BIT;
    lcd_write_cmd_with_data(GC9A01A_COLMOD, &colmod, 1);
    
    /* Memory Data Access Control - Standard orientation */
    uint8_t madctl = 0x00;  /* Row/Col normal order, RGB */
    lcd_write_cmd_with_data(GC9A01A_MADCTL, &madctl, 1);
    
    /* Display Inversion Off */
    lcd_write_command(GC9A01A_INVOFF);
    
    /* Display On */
    lcd_write_command(GC9A01A_DISPON);
    hal_delay_ms(10);
    
    /* Backlight On */
    hal_gpio_write(LCD_BL_GPIO, GPIO_HIGH);
    
    fprintf(stderr, "LCD GC9A01A initialized successfully\n");
    return 0;
}

int lcd_gc9a01a_deinit(void) {
    hal_gpio_write(LCD_BL_GPIO, GPIO_LOW);
    lcd_write_command(GC9A01A_DISPOFF);
    hal_delay_ms(10);
    lcd_write_command(GC9A01A_SLPIN);
    hal_delay_ms(120);
    
    hal_spi_close();
    hal_gpio_cleanup();
    
    return 0;
}

/* ============================================================================
 * Basic Control
 * ============================================================================ */

int lcd_gc9a01a_reset(void) {
    hal_gpio_write(LCD_RST_GPIO, GPIO_LOW);
    hal_delay_ms(10);
    hal_gpio_write(LCD_RST_GPIO, GPIO_HIGH);
    hal_delay_ms(120);
    return 0;
}

int lcd_gc9a01a_sleep_in(void) {
    return lcd_write_command(GC9A01A_SLPIN);
}

int lcd_gc9a01a_sleep_out(void) {
    int ret = lcd_write_command(GC9A01A_SLPOUT);
    hal_delay_ms(10);
    return ret;
}

int lcd_gc9a01a_display_on(void) {
    return lcd_write_command(GC9A01A_DISPON);
}

int lcd_gc9a01a_display_off(void) {
    return lcd_write_command(GC9A01A_DISPOFF);
}

int lcd_gc9a01a_set_rotation(gc9a01a_rotation_t rotation) {
    lcd_dev.rotation = rotation;
    return lcd_write_cmd_with_data(GC9A01A_MADCTL, (uint8_t *)&rotation, 1);
}

int lcd_gc9a01a_set_invert(uint8_t invert) {
    uint8_t cmd = invert ? GC9A01A_INVON : GC9A01A_INVOFF;
    return lcd_write_command(cmd);
}

/* ============================================================================
 * Address Window and Drawing
 * ============================================================================ */

int lcd_gc9a01a_set_address_window(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2) {
    /* Column Address Set */
    uint8_t caset_data[4] = {
        (x1 >> 8) & 0xFF, x1 & 0xFF,
        (x2 >> 8) & 0xFF, x2 & 0xFF
    };
    lcd_write_cmd_with_data(GC9A01A_CASET, caset_data, 4);
    
    /* Row Address Set */
    uint8_t raset_data[4] = {
        (y1 >> 8) & 0xFF, y1 & 0xFF,
        (y2 >> 8) & 0xFF, y2 & 0xFF
    };
    lcd_write_cmd_with_data(GC9A01A_RASET, raset_data, 4);
    
    return 0;
}

int lcd_gc9a01a_fill_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, rgb565_t color) {
    if (x1 > x2) {
        uint16_t t = x1; x1 = x2; x2 = t;
    }
    if (y1 > y2) {
        uint16_t t = y1; y1 = y2; y2 = t;
    }
    
    uint32_t pixels = (uint32_t)(x2 - x1 + 1) * (y2 - y1 + 1);
    uint8_t *buf = malloc(pixels * 2);
    if (!buf) return -1;
    
    /* Fill buffer with color */
    for (uint32_t i = 0; i < pixels; i++) {
        buf[i * 2] = (color >> 8) & 0xFF;
        buf[i * 2 + 1] = color & 0xFF;
    }
    
    lcd_gc9a01a_set_address_window(x1, y1, x2, y2);
    lcd_write_command(GC9A01A_RAMWR);
    lcd_write_data(buf, pixels * 2);
    
    free(buf);
    return 0;
}

int lcd_gc9a01a_fill_screen(rgb565_t color) {
    return lcd_gc9a01a_fill_rect(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1, color);
}

int lcd_gc9a01a_draw_pixel(uint16_t x, uint16_t y, rgb565_t color) {
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT) return -1;
    
    lcd_gc9a01a_set_address_window(x, y, x, y);
    lcd_write_command(GC9A01A_RAMWR);
    
    uint8_t pixel[2] = {(color >> 8) & 0xFF, color & 0xFF};
    lcd_write_data(pixel, 2);
    
    return 0;
}

int lcd_gc9a01a_draw_h_line(uint16_t x, uint16_t y, uint16_t length, rgb565_t color) {
    if (x + length > LCD_WIDTH) length = LCD_WIDTH - x;
    return lcd_gc9a01a_fill_rect(x, y, x + length - 1, y, color);
}

int lcd_gc9a01a_draw_v_line(uint16_t x, uint16_t y, uint16_t length, rgb565_t color) {
    if (y + length > LCD_HEIGHT) length = LCD_HEIGHT - y;
    return lcd_gc9a01a_fill_rect(x, y, x, y + length - 1, color);
}

int lcd_gc9a01a_draw_rect(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2, rgb565_t color) {
    lcd_gc9a01a_draw_h_line(x1, y1, x2 - x1 + 1, color);
    lcd_gc9a01a_draw_h_line(x1, y2, x2 - x1 + 1, color);
    lcd_gc9a01a_draw_v_line(x1, y1, y2 - y1 + 1, color);
    lcd_gc9a01a_draw_v_line(x2, y1, y2 - y1 + 1, color);
    return 0;
}

/* ============================================================================
 * Bulk Data Transfer
 * ============================================================================ */

int lcd_gc9a01a_write_ram(const uint8_t *data, size_t len) {
    lcd_write_command(GC9A01A_RAMWR);
    return lcd_write_data(data, len);
}

int lcd_gc9a01a_push_frame_buffer(const rgb565_t *fb) {
    if (!fb) return -1;
    
    /* Set full screen address window */
    lcd_gc9a01a_set_address_window(0, 0, LCD_WIDTH - 1, LCD_HEIGHT - 1);
    
    /* Write entire frame buffer */
    lcd_write_command(GC9A01A_RAMWR);
    
    /* Convert frame buffer to bytes for SPI */
    uint8_t *buf = malloc(LCD_FRAME_BUFFER_SIZE);
    if (!buf) return -1;
    
    for (size_t i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
        buf[i * 2] = (fb[i] >> 8) & 0xFF;
        buf[i * 2 + 1] = fb[i] & 0xFF;
    }
    
    int ret = lcd_write_data(buf, LCD_FRAME_BUFFER_SIZE);
    free(buf);
    
    return ret;
}

/* ============================================================================
 * Backlight Control
 * ============================================================================ */

int lcd_gc9a01a_set_backlight(uint8_t brightness) {
    /* Simple on/off for now; PWM could be added */
    if (brightness > 0) {
        hal_gpio_write(LCD_BL_GPIO, GPIO_HIGH);
    } else {
        hal_gpio_write(LCD_BL_GPIO, GPIO_LOW);
    }
    return 0;
}
