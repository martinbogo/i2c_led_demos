/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-21
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * hal_gpio_spi.c - Software bitbanged SPI using Linux sysfs GPIO
 * Provide bit-banged SPI primitives for driving GC9A01 LCD.
 *
 */
/*
 * hal_gpio_spi.c - Hardware Abstraction Layer Implementation
 * Direct GPIO and SPI access via /sys/class/gpio and /dev/spidev
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
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>
#include <linux/i2c-dev.h>
#else
/* Stub implementations for non-Linux platforms (for compilation) */
typedef int spi_ioc_transfer;
#define SPI_IOC_MESSAGE(n) 0
#define I2C_SLAVE 0
#endif

static int spi_fd = -1;
static int i2c_fd = -1;

/* ============================================================================
 * GPIO Operations using libgpiod (gpioset command)
 * ============================================================================ */

int hal_gpio_init(void) {
    /* Initialize GPIO subsystem by pre-exporting needed pins */
    uint32_t pins[] = {LCD_CS_GPIO, LCD_DC_GPIO, LCD_RST_GPIO, LCD_BL_GPIO};
    
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        char path[64];
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u", pins[i]);
        
        /* Check if already exported */
        if (access(path, F_OK) == 0) {
            continue;  /* Already exported */
        }
        
        /* Export the GPIO */
        int fd = open("/sys/class/gpio/export", O_WRONLY);
        if (fd >= 0) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%u", pins[i]);
            write(fd, buf, strlen(buf));
            close(fd);
            usleep(100000);  /* Wait for GPIO to be created */
        }
    }
    
    return 0;
}

int hal_gpio_cleanup(void) {
    /* Cleanup GPIO subsystem */
    return 0;
}

int hal_gpio_set_mode(uint32_t pin, gpio_mode_t mode) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/direction", pin);
    
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }
    
    const char *direction = (mode == GPIO_MODE_IN) ? "in" : "out";
    ssize_t ret = write(fd, direction, strlen(direction));
    close(fd);
    
    return (ret > 0) ? 0 : -1;
}

int hal_gpio_write(uint32_t pin, gpio_level_t level) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/value", pin);
    
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "GPIO write failed: cannot open %s\n", path);
        return -1;
    }
    
    const char *value = (level == GPIO_HIGH) ? "1" : "0";
    ssize_t ret = write(fd, value, 1);
    close(fd);
    
    if (ret <= 0) {
        fprintf(stderr, "GPIO write failed: cannot write to %s\n", path);
    }
    
    return (ret > 0) ? 0 : -1;
}

gpio_level_t hal_gpio_read(uint32_t pin) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%u/value", pin);
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) return GPIO_LOW;
    
    char val;
    ssize_t n = read(fd, &val, 1);
    close(fd);
    
    return (n > 0 && val == '1') ? GPIO_HIGH : GPIO_LOW;
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
    spi_fd = open(device, O_RDWR);
    if (spi_fd < 0) {
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
    
    return 0;
}

int hal_spi_close(void) {
    if (spi_fd >= 0) {
        close(spi_fd);
        spi_fd = -1;
    }
    return 0;
}

int hal_spi_write(const uint8_t *data, size_t len) {
    if (spi_fd < 0) return -1;
    
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)data,
        .rx_buf = 0,
        .len = len,
        .delay_usecs = 0,
        .speed_hz = 0,
        .bits_per_word = 8,
    };
    
    return ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
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
