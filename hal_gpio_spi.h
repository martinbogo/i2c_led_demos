/*
 * hal_gpio_spi.h - Hardware Abstraction Layer for GPIO and SPI
 * Supports both WiringPi and direct GPIO/SPI access on Raspberry Pi
 */

#ifndef HAL_GPIO_SPI_H
#define HAL_GPIO_SPI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GPIO_MODE_IN,
    GPIO_MODE_OUT,
    GPIO_MODE_ALT0,
    GPIO_MODE_ALT1,
    GPIO_MODE_ALT2,
    GPIO_MODE_ALT3,
    GPIO_MODE_ALT4,
    GPIO_MODE_ALT5
} gpio_mode_t;

typedef enum {
    GPIO_HIGH = 1,
    GPIO_LOW = 0
} gpio_level_t;

/* GPIO Operations */
int hal_gpio_init(void);
int hal_gpio_cleanup(void);
int hal_gpio_set_mode(uint32_t pin, gpio_mode_t mode);
int hal_gpio_write(uint32_t pin, gpio_level_t level);
gpio_level_t hal_gpio_read(uint32_t pin);
int hal_gpio_set_pwm(uint32_t pin, uint32_t frequency, uint32_t duty_cycle);

/* SPI Operations */
int hal_spi_init(const char *device, uint32_t speed_hz, uint8_t mode);
int hal_spi_close(void);
int hal_spi_write(const uint8_t *data, size_t len);
int hal_spi_read(uint8_t *data, size_t len);
int hal_spi_transfer(const uint8_t *tx_data, uint8_t *rx_data, size_t len);

/* I2C Operations */
int hal_i2c_init(const char *device);
int hal_i2c_close(void);
int hal_i2c_write_byte(uint8_t addr, uint8_t reg, uint8_t value);
int hal_i2c_write_bytes(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len);
int hal_i2c_read_byte(uint8_t addr, uint8_t reg);
int hal_i2c_read_bytes(uint8_t addr, uint8_t reg, uint8_t *data, size_t len);

/* Delay Operations */
void hal_delay_ms(uint32_t ms);
void hal_delay_us(uint32_t us);

#ifdef __cplusplus
}
#endif

#endif /* HAL_GPIO_SPI_H */
