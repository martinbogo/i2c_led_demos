/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-06
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * cube.c - Rotating 3D wireframe cube on SSD1306 128x64 OLED via I2C (Pi 5)
 *
 * Compile:  gcc -o cube cube.c -lm
 * Run:      sudo ./cube
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
#define FPS        30

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
        0x00,
        0xAE,
        0xD5, 0x80,
        0xA8, 0x3F,
        0xD3, 0x00,
        0x40,
        0x8D, 0x14,
        0x20, 0x00,
        0xA1,
        0xC8,
        0xDA, 0x12,
        0x81, 0x6F,
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,
        0xA6,
        0xAF
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

    for (int i = 0; i < WIDTH * PAGES; i += 32) {
        uint8_t buf[33];
        buf[0] = 0x40;
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

/* Bresenham line drawing */
static void draw_line(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        set_pixel(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* Unit cube vertices centered at origin (-1 to +1) */
static const float verts[8][3] = {
    {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
    {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1}
};

/* 12 edges as pairs of vertex indices */
static const int edges[12][2] = {
    {0,1}, {1,2}, {2,3}, {3,0},   /* back face  */
    {4,5}, {5,6}, {6,7}, {7,4},   /* front face */
    {0,4}, {1,5}, {2,6}, {3,7}    /* connecting */
};

static void rotate(float in[3], float out[3], float ax, float ay, float az) {
    float cx = cosf(ax), sx = sinf(ax);
    float cy = cosf(ay), sy = sinf(ay);
    float cz = cosf(az), sz = sinf(az);

    /* Rotate around X */
    float y1 = in[1] * cx - in[2] * sx;
    float z1 = in[1] * sx + in[2] * cx;

    /* Rotate around Y */
    float x2 = in[0] * cy + z1 * sy;
    float z2 = -in[0] * sy + z1 * cy;

    /* Rotate around Z */
    out[0] = x2 * cz - y1 * sz;
    out[1] = x2 * sz + y1 * cz;
    out[2] = z2;
}

/* Perspective projection */
static void project(float v[3], int *sx, int *sy) {
    float dist = 4.0f;   /* camera distance */
    float scale = 28.0f; /* controls size on screen */
    float z = v[2] + dist;
    if (z < 0.1f) z = 0.1f;
    *sx = (int)(WIDTH  / 2.0f + scale * v[0] / z);
    *sy = (int)(HEIGHT / 2.0f + scale * v[1] / z);
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

    float ax = 0, ay = 0, az = 0;

    long frame_ns = 1000000000L / FPS;
    struct timespec next_frame;
    clock_gettime(CLOCK_MONOTONIC, &next_frame);

    while (running) {
        memset(fb, 0, sizeof(fb));

        /* Transform and project all 8 vertices */
        int screen[8][2];
        for (int i = 0; i < 8; i++) {
            float r[3];
            rotate((float *)verts[i], r, ax, ay, az);
            project(r, &screen[i][0], &screen[i][1]);
        }

        /* Draw 12 edges */
        for (int i = 0; i < 12; i++) {
            int a = edges[i][0], b = edges[i][1];
            draw_line(screen[a][0], screen[a][1],
                      screen[b][0], screen[b][1]);
        }

        flush();

        /* Rotate slowly on all three axes at different rates */
        ax += 0.03f;
        ay += 0.02f;
        az += 0.01f;

        next_frame.tv_nsec += frame_ns;
        if (next_frame.tv_nsec >= 1000000000L) {
            next_frame.tv_nsec -= 1000000000L;
            next_frame.tv_sec += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_frame, NULL);
    }

    memset(fb, 0, sizeof(fb));
    flush();
    cmd(0xAE);
    close(fd);
    return 0;
}
