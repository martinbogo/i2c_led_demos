/*
 * gpio_config.h - GPIO pin configuration for Waveshare 1.28" OLED display
 * Raspberry Pi BCM pinout for GC9A01A LCD + CST816S touch controller
 */

#ifndef GPIO_CONFIG_H
#define GPIO_CONFIG_H

/* SPI Interface (LCD Display) */
#define SPI_MISO_GPIO    9   /* GPIO9 - MISO */
#define SPI_MOSI_GPIO    10  /* GPIO10 - MOSI */
#define SPI_SCLK_GPIO    11  /* GPIO11 - SCLK */

/* LCD Control Pins */
#define LCD_CS_GPIO      8   /* GPIO8 - Chip Select (active low) */
#define LCD_DC_GPIO      25  /* GPIO25 - Data/Command (0=cmd, 1=data) */
#define LCD_RST_GPIO     27  /* GPIO27 - Reset (active low) */
#define LCD_BL_GPIO      18  /* GPIO18 - Backlight PWM */

/* Touch Controller Pins (I2C) */
#define TOUCH_SDA_GPIO   2   /* GPIO2 - I2C SDA */
#define TOUCH_SCL_GPIO   3   /* GPIO3 - I2C SCL */
#define TOUCH_INT_GPIO   4   /* GPIO4 - Touch Interrupt */
#define TOUCH_RST_GPIO   17  /* GPIO17 - Touch Reset (active low) */

/* I2C Configuration */
#define TOUCH_I2C_ADDR   0x15  /* CST816S 7-bit I2C address */
#define TOUCH_I2C_BUS    "/dev/i2c-1"

/* SPI Configuration */
#define SPI_DEVICE       "/dev/spidev10.0"  /* SPI device on Raspberry Pi */
#define SPI_SPEED_HZ     10000000           /* 10 MHz */
#define SPI_MODE         0                  /* CPOL=0, CPHA=0 */
#define SPI_BITS_PER_WORD 8

/* Display Dimensions */
#define LCD_WIDTH        240
#define LCD_HEIGHT       240
#define LCD_BIT_DEPTH    16  /* RGB565 */

#endif /* GPIO_CONFIG_H */
