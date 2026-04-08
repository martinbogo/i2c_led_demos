/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-06
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * bounce.c - Bouncing ball on SSD1306 128x64 OLED via I2C (Pi 5)
 *
 * Compile:  gcc -o bounce bounce.c -lm
 * Run:      sudo ./bounce
 *
 * Pins: 1 (3.3V), 3 (SDA), 5 (SCL), 9 (GND)
 */

#include <fcntl.h>
#include <linux/i2c-dev.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define WIDTH      128
#define HEIGHT     64
#define PAGES      (HEIGHT / 8)
#define ADDR       0x3C
#define BALL_R     4
#define FPS        60

static int fd;
static uint8_t fb[WIDTH * PAGES];
static volatile int running = 1;

static void stop(int sig) { (void)sig; running = 0; }

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [-d] [-h|-?]\n"
            "  -d      run as a daemon\n"
            "  -h, -?  show this help message\n",
            argv0);
}

static void cmd(uint8_t c) {
    uint8_t buf[2] = {0x00, c};
    if (write(fd, buf, 2) < 0) {
        perror("write cmd");
        running = 0;
    }
}

static void init_display(void) {
    uint8_t init_cmds[] = {
        0x00, /* Control byte indicating command payload */
        0xAE,          /* display off */
        0xD5, 0x80,
        0xA8, 0x3F,
        0xD3, 0x00,
        0x40,
        0x8D, 0x14, /* charge pump on */
        0x20, 0x00, /* horizontal addressing */
        0xA1,            /* segment remap */
        0xC8,            /* COM scan direction */
        0xDA, 0x12,
        0x81, 0xCF,
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,
        0xA6,            /* normal display */
        0xAF             /* display on */
    };
    if (write(fd, init_cmds, sizeof(init_cmds)) < 0) {
        perror("write init");
        running = 0;
    }
}

static void flush(void) {
    uint8_t addr_cmds[] = {
        0x00,
        0x21, 0, WIDTH - 1,
        0x22, 0, PAGES - 1
    };
    if (write(fd, addr_cmds, sizeof(addr_cmds)) < 0) {
        perror("write addr");
        running = 0;
    }

    /* send framebuffer in 32-byte chunks (I2C limit) */
    for (int i = 0; i < WIDTH * PAGES; i += 32) {
        uint8_t buf[33];
        buf[0] = 0x40; /* data */
        int len = WIDTH * PAGES - i;
        if (len > 32) len = 32;
        memcpy(buf + 1, fb + i, len);
        if (write(fd, buf, len + 1) < 0) {
            perror("write fb");
            running = 0;
            break;
        }
    }
}

static void set_pixel(int x, int y) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
        fb[x + (y / 8) * WIDTH] |= 1 << (y & 7);
}

static void draw_ball(int cx, int cy, int r) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r)
                set_pixel(cx + dx, cy + dy);
}

int main(int argc, char *argv[]) {
    int daemonize = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemonize = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-?") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }
    if (daemonize) {
        if (daemon(0, 0) == -1) {
            perror("daemon");
            return 1;
        }
    }
    fd = open("/dev/i2c-1", O_RDWR);
    if (fd < 0) { perror("open i2c"); return 1; }
    if (ioctl(fd, I2C_SLAVE, ADDR) < 0) { perror("ioctl"); return 1; }

    init_display();
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    float x = WIDTH / 2.0f, y = HEIGHT / 2.0f;
    float vx = 1.8f, vy = 1.2f;

    long frame_ns = 1000000000L / FPS;
    struct timespec next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    while (running) {
        x += vx;
        y += vy;

        if (x - BALL_R < 0)         { x = 2 * BALL_R - x;               vx = -vx; }
        if (x + BALL_R >= WIDTH)    { x = 2 * (WIDTH - 1 - BALL_R) - x; vx = -vx; }
        if (y - BALL_R < 0)         { y = 2 * BALL_R - y;               vy = -vy; }
        if (y + BALL_R >= HEIGHT)   { y = 2 * (HEIGHT - 1 - BALL_R) - y; vy = -vy; }

        memset(fb, 0, sizeof(fb));
        draw_ball((int)x, (int)y, BALL_R);
        flush();

        next_frame.tv_nsec += frame_ns;
        if (next_frame.tv_nsec >= 1000000000L) {
            next_frame.tv_nsec -= 1000000000L;
            next_frame.tv_sec += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);
    }

    /* clear screen before exit */
    memset(fb, 0, sizeof(fb));
    flush();
    cmd(0xAE);
    close(fd);
    return 0;
}
