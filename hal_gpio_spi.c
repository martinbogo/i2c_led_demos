/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-21
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * hal_gpio_spi.c - GPIO/SPI/I2C hardware abstraction layer.
 *
 */
/*
 * hal_gpio_spi.c - Hardware Abstraction Layer Implementation
 * Direct GPIO access via libgpiod and SPI access via /dev/spidev
 * Note: This is a Raspberry Pi specific implementation (Linux only)
 */

#include "hal_gpio_spi.h"
#include "gpio_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <errno.h>

/* Include Linux-specific headers only on Linux */
#ifdef __linux__
#include <gpiod.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <linux/i2c-dev.h>
#else
/* Stub implementations for non-Linux platforms (for compilation) */
struct spi_ioc_transfer {
    unsigned long tx_buf;
    unsigned long rx_buf;
    unsigned int len;
    unsigned short delay_usecs;
    unsigned int speed_hz;
    unsigned char bits_per_word;
};
#define SPI_IOC_MESSAGE(n) 0
#define SPI_IOC_WR_MODE 0
#define SPI_IOC_WR_BITS_PER_WORD 0
#define SPI_IOC_WR_MAX_SPEED_HZ 0
#define I2C_SLAVE 0
#endif

static int spi_fd = -1;
static int i2c_fd = -1;
static uint32_t spi_speed_hz_configured = 0;

static int open_spi_device_with_fallback(const char *device) {
    spi_fd = open(device, O_RDWR);
    if (spi_fd >= 0) {
        return 0;
    }

    if (strcmp(device, SPI_DEVICE) == 0 && strcmp(SPI_DEVICE, SPI_FALLBACK_DEVICE) != 0) {
        fprintf(stderr, "Failed to open %s, retrying with fallback %s\n", device, SPI_FALLBACK_DEVICE);
        spi_fd = open(SPI_FALLBACK_DEVICE, O_RDWR);
        if (spi_fd >= 0) {
            return 0;
        }
    }

    return -1;
}

enum {
    SPI_TRANSFER_CHUNK_SIZE = 4096
};

#ifdef __linux__
static struct gpiod_chip *gpio_chip = NULL;
static struct gpiod_line_request *gpio_requests[64] = {0};
static gpio_mode_t gpio_modes[64] = {0};

static int validate_gpio_pin(uint32_t pin) {
    return pin < 64U ? 0 : -1;
}

static int gpio_is_hw_spi_cs(uint32_t pin) {
    return pin == LCD_CS_GPIO;
}

#endif

#ifdef __linux__
static int ensure_gpio_chip(void) {
    if (gpio_chip != NULL) {
        return 0;
    }

    gpio_chip = gpiod_chip_open("/dev/gpiochip0");
    if (gpio_chip == NULL) {
        perror("Failed to open /dev/gpiochip0");
        return -1;
    }

    return 0;
}

static void release_gpio_request(uint32_t pin) {
    if (pin >= 64U) {
        return;
    }

    if (gpio_requests[pin] != NULL) {
        gpiod_line_request_release(gpio_requests[pin]);
        gpio_requests[pin] = NULL;
    }
}

static int request_gpio_line(uint32_t pin, gpio_mode_t mode, enum gpiod_line_value initial_value) {
    struct gpiod_line_settings *settings = NULL;
    struct gpiod_line_config *line_config = NULL;
    struct gpiod_request_config *request_config = NULL;
    struct gpiod_line_request *request = NULL;
    unsigned int offset = pin;
    int ret = -1;

    if (ensure_gpio_chip() < 0) {
        return -1;
    }

    release_gpio_request(pin);

    settings = gpiod_line_settings_new();
    line_config = gpiod_line_config_new();
    request_config = gpiod_request_config_new();
    if (settings == NULL || line_config == NULL || request_config == NULL) {
        fprintf(stderr, "Failed to allocate libgpiod request objects for GPIO%u\n", pin);
        goto cleanup;
    }

    if (mode == GPIO_MODE_IN) {
        if (gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT) < 0) {
            fprintf(stderr, "Failed to set GPIO%u direction to input\n", pin);
            goto cleanup;
        }
    } else {
        if (gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT) < 0) {
            fprintf(stderr, "Failed to set GPIO%u direction to output\n", pin);
            goto cleanup;
        }
        if (gpiod_line_settings_set_output_value(settings, initial_value) < 0) {
            fprintf(stderr, "Failed to set GPIO%u initial output value\n", pin);
            goto cleanup;
        }
    }

    if (gpiod_line_config_add_line_settings(line_config, &offset, 1, settings) < 0) {
        fprintf(stderr, "Failed to attach line settings for GPIO%u\n", pin);
        goto cleanup;
    }

    gpiod_request_config_set_consumer(request_config, "i2c_led_demos");

    request = gpiod_chip_request_lines(gpio_chip, request_config, line_config);
    if (request == NULL) {
        perror("Failed to request GPIO line");
        fprintf(stderr, "GPIO request failed for GPIO%u\n", pin);
        goto cleanup;
    }

    gpio_requests[pin] = request;
    gpio_modes[pin] = mode;
    ret = 0;

cleanup:
    if (ret < 0 && request != NULL) {
        gpiod_line_request_release(request);
    }
    if (request_config != NULL) {
        gpiod_request_config_free(request_config);
    }
    if (line_config != NULL) {
        gpiod_line_config_free(line_config);
    }
    if (settings != NULL) {
        gpiod_line_settings_free(settings);
    }

    return ret;
}
#endif

/* ============================================================================
 * GPIO Operations
 * ============================================================================ */

int hal_gpio_init(void) {
#ifdef __linux__
    if (ensure_gpio_chip() < 0) {
        return -1;
    }
#endif
    return 0;
}

int hal_gpio_cleanup(void) {
#ifdef __linux__
    for (uint32_t pin = 0; pin < 64U; ++pin) {
        release_gpio_request(pin);
    }

    if (gpio_chip != NULL) {
        gpiod_chip_close(gpio_chip);
        gpio_chip = NULL;
    }
#endif

    return 0;
}

int hal_gpio_set_mode(uint32_t pin, gpio_mode_t mode) {
#ifdef __linux__
    if (validate_gpio_pin(pin) < 0) {
        return -1;
    }

    if (gpio_is_hw_spi_cs(pin)) {
        return 0;
    }

    if (mode != GPIO_MODE_IN && mode != GPIO_MODE_OUT) {
        fprintf(stderr, "Unsupported GPIO mode %d for GPIO%u\n", mode, pin);
        return -1;
    }

    return request_gpio_line(pin, mode, GPIOD_LINE_VALUE_INACTIVE);
#else
    (void)pin;
    (void)mode;
    return 0;
#endif
}

int hal_gpio_write(uint32_t pin, gpio_level_t level) {
#ifdef __linux__
    if (validate_gpio_pin(pin) < 0) {
        return -1;
    }

    if (gpio_is_hw_spi_cs(pin)) {
        return 0;
    }

    if (gpio_requests[pin] == NULL || gpio_modes[pin] != GPIO_MODE_OUT) {
        if (request_gpio_line(pin, GPIO_MODE_OUT,
                              level == GPIO_HIGH ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE) < 0) {
            fprintf(stderr, "GPIO write failed: cannot request GPIO%u for output\n", pin);
            return -1;
        }
    }

    if (gpiod_line_request_set_value(gpio_requests[pin], pin,
            level == GPIO_HIGH ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE) < 0) {
        perror("GPIO write failed");
        fprintf(stderr, "GPIO write failed for GPIO%u\n", pin);
        return -1;
    }

    return 0;
#else
    (void)pin;
    (void)level;
    return 0;
#endif
}

gpio_level_t hal_gpio_read(uint32_t pin) {
#ifdef __linux__
    enum gpiod_line_value value;

    if (validate_gpio_pin(pin) < 0) {
        return GPIO_LOW;
    }

    if (gpio_is_hw_spi_cs(pin)) {
        return GPIO_LOW;
    }

    if (gpio_requests[pin] == NULL || gpio_modes[pin] != GPIO_MODE_IN) {
        if (request_gpio_line(pin, GPIO_MODE_IN, GPIOD_LINE_VALUE_INACTIVE) < 0) {
            return GPIO_LOW;
        }
    }

    value = gpiod_line_request_get_value(gpio_requests[pin], pin);
    return value == GPIOD_LINE_VALUE_ACTIVE ? GPIO_HIGH : GPIO_LOW;
#else
    (void)pin;
    return GPIO_LOW;
#endif
}

int hal_gpio_set_pwm(uint32_t pin, uint32_t frequency, uint32_t duty_cycle) {
    /* PWM support via /sys/class/pwm if needed */
    /* For now, simple on/off control */
    return hal_gpio_write(pin, (duty_cycle > 50) ? GPIO_HIGH : GPIO_LOW);
}

/* ============================================================================
 * SPI Operations
 * ============================================================================ */

int hal_spi_init(const char *device, uint32_t speed_hz, uint8_t mode) {
    if (open_spi_device_with_fallback(device) < 0) {
        perror("Failed to open SPI device");
        return -1;
    }
    
    uint8_t spi_mode = mode;
    uint8_t bits_per_word = 8;
    uint32_t spi_speed = speed_hz;
    
    if (ioctl(spi_fd, SPI_IOC_WR_MODE, &spi_mode) < 0) {
        perror("Failed to set SPI mode");
        close(spi_fd);
        spi_fd = -1;
        return -1;
    }
    
    if (ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits_per_word) < 0) {
        perror("Failed to set bits per word");
        close(spi_fd);
        spi_fd = -1;
        return -1;
    }
    
    if (ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ, &spi_speed) < 0) {
        perror("Failed to set SPI speed");
        close(spi_fd);
        spi_fd = -1;
        return -1;
    }

    spi_speed_hz_configured = speed_hz;
    
    return 0;
}

int hal_spi_close(void) {
    if (spi_fd >= 0) {
        close(spi_fd);
        spi_fd = -1;
    }
    spi_speed_hz_configured = 0;
    return 0;
}

int hal_spi_write(const uint8_t *data, size_t len) {
    size_t offset = 0;

    if (spi_fd < 0 || data == NULL) return -1;

    while (offset < len) {
        const size_t chunk_len = (len - offset) > SPI_TRANSFER_CHUNK_SIZE ? SPI_TRANSFER_CHUNK_SIZE : (len - offset);
        struct spi_ioc_transfer tr = {
            .tx_buf = (unsigned long)(data + offset),
            .rx_buf = 0,
            .len = chunk_len,
            .delay_usecs = 0,
            .speed_hz = spi_speed_hz_configured,
            .bits_per_word = 8,
        };

        if (ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr) < 0) {
            perror("SPI write failed");
            return -1;
        }

        offset += chunk_len;
    }

    return 0;
}

int hal_spi_read(uint8_t *data, size_t len) {
    if (spi_fd < 0) return -1;
    
    struct spi_ioc_transfer tr = {
        .tx_buf = 0,
        .rx_buf = (unsigned long)data,
        .len = len,
        .delay_usecs = 0,
        .speed_hz = 0,
        .bits_per_word = 8,
    };
    
    return ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
}

int hal_spi_transfer(const uint8_t *tx_data, uint8_t *rx_data, size_t len) {
    if (spi_fd < 0) return -1;
    
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)tx_data,
        .rx_buf = (unsigned long)rx_data,
        .len = len,
        .delay_usecs = 0,
        .speed_hz = 0,
        .bits_per_word = 8,
    };
    
    return ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
}

/* ============================================================================
 * I2C Operations
 * ============================================================================ */

int hal_i2c_init(const char *device) {
    i2c_fd = open(device, O_RDWR);
    if (i2c_fd < 0) {
        perror("Failed to open I2C device");
        return -1;
    }
    return 0;
}

int hal_i2c_close(void) {
    if (i2c_fd >= 0) {
        close(i2c_fd);
        i2c_fd = -1;
    }
    return 0;
}

int hal_i2c_write_byte(uint8_t addr, uint8_t reg, uint8_t value) {
    if (i2c_fd < 0) return -1;
    
    if (ioctl(i2c_fd, I2C_SLAVE, addr) < 0) {
        perror("Failed to set I2C slave address");
        return -1;
    }
    
    uint8_t buf[2] = {reg, value};
    return write(i2c_fd, buf, 2) == 2 ? 0 : -1;
}

int hal_i2c_write_bytes(uint8_t addr, uint8_t reg, const uint8_t *data, size_t len) {
    if (i2c_fd < 0) return -1;
    
    if (ioctl(i2c_fd, I2C_SLAVE, addr) < 0) {
        return -1;
    }
    
    uint8_t *buf = malloc(len + 1);
    if (!buf) return -1;
    
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    
    int ret = write(i2c_fd, buf, len + 1) == (int)(len + 1) ? 0 : -1;
    free(buf);
    return ret;
}

int hal_i2c_read_byte(uint8_t addr, uint8_t reg) {
    if (i2c_fd < 0) return -1;
    
    if (ioctl(i2c_fd, I2C_SLAVE, addr) < 0) {
        return -1;
    }
    
    if (write(i2c_fd, &reg, 1) != 1) {
        return -1;
    }
    
    uint8_t val;
    return read(i2c_fd, &val, 1) == 1 ? val : -1;
}

int hal_i2c_read_bytes(uint8_t addr, uint8_t reg, uint8_t *data, size_t len) {
    if (i2c_fd < 0) return -1;
    
    if (ioctl(i2c_fd, I2C_SLAVE, addr) < 0) {
        return -1;
    }
    
    if (write(i2c_fd, &reg, 1) != 1) {
        return -1;
    }
    
    return read(i2c_fd, data, len) == (int)len ? 0 : -1;
}

/* ============================================================================
 * Delay Operations
 * ============================================================================ */

void hal_delay_ms(uint32_t ms) {
    usleep(ms * 1000);
}

void hal_delay_us(uint32_t us) {
    usleep(us);
}
