/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-21
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * st_smartwatch_stream.c - Legacy direct-stream host renderer for ST Smartwatch on Uno Q
 *
 * Build:     ./build.sh unoq
 */
/*
 * Uno Q host-side port of st_smartwatch.c
 * Renders frames on the Linux side and streams them to the MCU OLED sink.
 */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define WIDTH 128
#define HEIGHT 64
#define PAGES (HEIGHT / 8)
#define TARGET_FPS 15.0f
#define FRAME_BYTES (WIDTH * PAGES)
#define SERIAL_DEVICE "/dev/ttyHS1"
#ifdef B460800
#define SERIAL_BAUD B460800
#else
#define SERIAL_BAUD B115200
#endif
#define MAGIC "OLED"
#define HANDSHAKE_ATTEMPTS 10
#define HANDSHAKE_TIMEOUT_SEC 1.0
#define ACK_TIMEOUT_SEC 3.0
#define FRAME_CHUNK_BYTES 128
#define FRAME_CHUNK_DELAY_US 500
#define COMMAND_SETTLE_US 1000

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int serial_fd = -1;
static uint8_t fb[FRAME_BYTES];
static volatile int running = 1;

static void stop(int sig) { (void)sig; running = 0; }

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [-d] [-h|-?]\n"
            "  -d      run as a daemon\n"
            "  -h, -?  show this help message\n",
            argv0);
}

static int serial_open_raw(const char *path) {
    int fd = open(path, O_RDWR | O_NOCTTY | O_SYNC);
    if (fd < 0) return -1;

    struct termios tty;
    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return -1;
    }

    cfmakeraw(&tty);
    tty.c_cflag |= CLOCAL | CREAD;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;
    cfsetispeed(&tty, SERIAL_BAUD);
    cfsetospeed(&tty, SERIAL_BAUD);

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return -1;
    }
    tcflush(fd, TCIOFLUSH);
    return fd;
}

static int serial_write_all(const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t written = write(serial_fd, p, len);
        if (written < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        p += (size_t)written;
        len -= (size_t)written;
    }
    return 1;
}

static int serial_write_paced(const uint8_t *buf, size_t len) {
    while (len > 0) {
        size_t chunk = len > FRAME_CHUNK_BYTES ? FRAME_CHUNK_BYTES : len;
        if (!serial_write_all(buf, chunk)) return 0;
        buf += chunk;
        len -= chunk;
        if (len > 0) usleep(FRAME_CHUNK_DELAY_US);
    }
    return 1;
}

static int serial_readline(double timeout_sec, char *line, size_t line_sz) {
    static char rxbuf[2048];
    static size_t rxlen = 0;
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        for (size_t i = 0; i < rxlen; i++) {
            if (rxbuf[i] == '\n') {
                size_t n = i;
                if (n >= line_sz) n = line_sz - 1;
                memcpy(line, rxbuf, n);
                line[n] = '\0';
                memmove(rxbuf, rxbuf + i + 1, rxlen - (i + 1));
                rxlen -= (i + 1);
                while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = '\0';
                return 1;
            }
        }

        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - start.tv_sec) +
                         (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        double remaining = timeout_sec - elapsed;
        if (remaining <= 0.0) return 0;

        struct timeval tv;
        tv.tv_sec = (time_t)remaining;
        tv.tv_usec = (suseconds_t)((remaining - (double)tv.tv_sec) * 1e6);
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(serial_fd, &rfds);
        int rc = select(serial_fd + 1, &rfds, NULL, NULL, &tv);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (rc == 0) return 0;

        ssize_t got = read(serial_fd, rxbuf + rxlen, sizeof(rxbuf) - rxlen);
        if (got < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        if (got == 0) continue;
        rxlen += (size_t)got;
        if (rxlen == sizeof(rxbuf)) rxlen = 0;
    }
}

static int wait_for_prefix(const char *prefix, double timeout_sec, char *out, size_t out_sz) {
    char line[256];
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);

    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        double elapsed = (double)(now.tv_sec - start.tv_sec) +
                         (double)(now.tv_nsec - start.tv_nsec) / 1e9;
        double remaining = timeout_sec - elapsed;
        if (remaining <= 0.0) break;
        if (!serial_readline(remaining, line, sizeof(line))) break;
        printf("%s\n", line);
        fflush(stdout);
        if (strncmp(line, prefix, strlen(prefix)) == 0) {
            snprintf(out, out_sz, "%s", line);
            return 1;
        }
        if (strncmp(line, "ERR", 3) == 0) {
            snprintf(out, out_sz, "%s", line);
            return 0;
        }
    }
    snprintf(out, out_sz, "timeout waiting for %s", prefix);
    return 0;
}

static int wait_for_ready(void) {
    char line[256];
    for (int attempt = 1; attempt <= HANDSHAKE_ATTEMPTS; attempt++) {
        printf("Handshake attempt %d/%d\n", attempt, HANDSHAKE_ATTEMPTS);
        fflush(stdout);
        if (!serial_write_all(MAGIC "Q", 5)) return 0;
        if (wait_for_prefix("READY", HANDSHAKE_TIMEOUT_SEC, line, sizeof(line))) return 1;
    }
    return 0;
}

static int send_frame_and_wait_ack(void) {
    char line[256];
    if (!serial_write_all(MAGIC "F", 5)) return 0;
    usleep(COMMAND_SETTLE_US);
    if (!serial_write_paced(fb, sizeof(fb))) return 0;
    return wait_for_prefix("ACK", ACK_TIMEOUT_SEC, line, sizeof(line));
}

static void send_end(void) {
    char line[256];
    if (!serial_write_all(MAGIC "E", 5)) return;
    wait_for_prefix("DONE", 1.0, line, sizeof(line));
}

static void px(int x, int y) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
        fb[x + (y / 8) * WIDTH] |= 1u << (y & 7);
}

static void clear_px(int x, int y) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
        fb[x + (y / 8) * WIDTH] &= (uint8_t)~(1u << (y & 7));
}

static void fill_rect(int x0, int y0, int x1, int y1) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            px(x, y);
}

static void clear_rect(int x0, int y0, int x1, int y1) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            clear_px(x, y);
}

static void fill_circle(int cx, int cy, int r) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r) px(cx + x, cy + y);
}

static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00},{0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},{0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},{0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},{0x00,0x41,0x22,0x1C,0x00},
    {0x08,0x2A,0x1C,0x2A,0x08},{0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00},{0x00,0x56,0x36,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41},{0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00},{0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},{0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36},{0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},{0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x01,0x01},{0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F},{0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},{0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},{0x7F,0x02,0x04,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F},{0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},{0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},{0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},{0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},{0x7F,0x20,0x18,0x20,0x7F},
    {0x63,0x14,0x08,0x14,0x63},{0x03,0x04,0x78,0x04,0x03},
    {0x61,0x51,0x49,0x45,0x43},{0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},{0x41,0x41,0x7F,0x00,0x00},
    {0x04,0x02,0x01,0x02,0x04},{0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},{0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},{0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},{0x38,0x54,0x54,0x54,0x18},
    {0x08,0x7E,0x09,0x01,0x02},{0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78},{0x00,0x44,0x7D,0x40,0x00},
    {0x20,0x40,0x44,0x3D,0x00},{0x00,0x7F,0x10,0x28,0x44},
    {0x00,0x41,0x7F,0x40,0x00},{0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},{0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},{0x08,0x14,0x14,0x18,0x7C},
    {0x7C,0x08,0x04,0x04,0x08},{0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},{0x3C,0x40,0x40,0x20,0x7C},
    {0x1C,0x20,0x40,0x20,0x1C},{0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},{0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},{0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},{0x00,0x41,0x36,0x08,0x00},
    {0x08,0x08,0x2A,0x1C,0x08},
};

static void draw_char(int x, int y, char c) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++)
            if (bits & (1u << row)) px(x + col, y + row);
    }
}

static void draw_str(int x, int y, const char *s) {
    while (*s) { draw_char(x, y, *s++); x += 6; }
}

static void draw_str_inv(int x, int y, const char *s) {
    while (*s) {
        char c = *s++;
        if (c < 32 || c > 126) c = '?';
        const uint8_t *g = font5x7[c - 32];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < 7; row++) {
                if (bits & (1u << row)) clear_px(x + col, y + row);
                else px(x + col, y + row);
            }
        }
        for (int row = 0; row < 7; row++) px(x + 5, y + row);
        x += 6;
    }
}

static void draw_large_char(int x, int y, char c, int scale) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1u << row)) {
                fill_rect(x + col * scale, y + row * scale,
                          x + col * scale + scale - 1, y + row * scale + scale - 1);
            }
        }
    }
}

static void draw_large_str(int x, int y, const char *s, int scale) {
    while (*s) { draw_large_char(x, y, *s++, scale); x += (5 + 1) * scale; }
}

static void draw_lcars_frame(float sim_t, int scene) {
    (void)sim_t;

    fill_rect(0, 0, WIDTH - 6, 14);
    for (int r = 0; r <= 7; r++) {
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                if (dx * dx + dy * dy <= r * r) px(WIDTH - 7 + dx, 7 + dy);
            }
        }
    }
    clear_rect(0, 0, WIDTH - 1, 1);
    clear_rect(0, 15, WIDTH - 1, 15);

    char buf[24];
    snprintf(buf, sizeof(buf), "LCARS 47-%02d", scene);
    int tw = 11 * 6;
    int str_x = WIDTH - 8 - tw;
    int str_y = 5;
    fill_rect(str_x, str_y, str_x + tw, str_y + 7);
    draw_str_inv(str_x, str_y, buf);

    fill_rect(0, 17, 16, HEIGHT - 1);
    clear_rect(17, 17, 30, 30);
    for (int y = -8; y <= 8; y++) {
        for (int x = -8; x <= 8; x++) {
            if (x * x + y * y > 64 && x < 0 && y < 0) px(25 + x, 25 + y);
        }
    }

    clear_rect(0, 30, 16, 32);
    clear_rect(0, 48, 16, 50);

    char id2[5];
    snprintf(id2, sizeof(id2), "%02d", 24 + scene);
    fill_rect(0, 38, 16, 45);
    draw_str_inv(2, 38, id2);
}

static void draw_scenes(const struct tm *tm, int steps, int batt, float sim_t, float ecg_phase, int scene) {
    int PLAY_X = 24;
    int PLAY_Y = 18;
    char buf[32];

    switch (scene) {
        case 0:
            snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
            draw_large_str(PLAY_X, PLAY_Y + 6, buf, 3);

            snprintf(buf, sizeof(buf), "%02d", tm->tm_sec);
            draw_large_str(WIDTH - 24, PLAY_Y + 6, buf, 1);

            draw_str(PLAY_X, PLAY_Y + 34, "MAIN TIME");
            fill_rect(PLAY_X, PLAY_Y + 42, PLAY_X + (batt * 70) / 100, PLAY_Y + 44);
            break;

        case 1: {
            draw_str(PLAY_X, PLAY_Y, "BIO-MONITOR");
            int bpm = 72 + (int)(sinf(sim_t) * 5.0f);
            snprintf(buf, sizeof(buf), "%d", bpm);
            draw_large_str(PLAY_X, PLAY_Y + 12, buf, 2);
            draw_str(PLAY_X + 28, PLAY_Y + 20, "BPM");

            for (int x = 0; x < 60; x++) {
                float ph = (float)x * 0.1f - ecg_phase * 10.0f;
                int cy = PLAY_Y + 14 + (int)(expf(-powf((fmodf(fabsf(ph), 10.0f) - 5.0f) * 2.0f, 2.0f)) * -14.0f);
                px(PLAY_X + 44 + x, cy);
                px(PLAY_X + 44 + x, cy + 1);
            }

            draw_str(PLAY_X, PLAY_Y + 36, "VITALS: NOMINAL");
            break;
        }

        case 2: {
            draw_str(PLAY_X, PLAY_Y, "ENGINEERING");
            draw_str(PLAY_X, PLAY_Y + 12, "STP");
            int step_w = (steps * 60) / 10000;
            if (step_w > 60) step_w = 60;
            fill_rect(PLAY_X + 25, PLAY_Y + 12, PLAY_X + 25 + step_w, PLAY_Y + 18);
            for (int x = 0; x <= 60; x += 8)
                clear_rect(PLAY_X + 25 + x, PLAY_Y + 12, PLAY_X + 25 + x, PLAY_Y + 18);

            draw_str(PLAY_X, PLAY_Y + 24, "PWR");
            int b_cells = (batt * 10) / 100;
            for (int i = 0; i < 10; i++) {
                if (i < b_cells) fill_rect(PLAY_X + 25 + i * 6, PLAY_Y + 24, PLAY_X + 25 + i * 6 + 4, PLAY_Y + 30);
                else {
                    px(PLAY_X + 25 + i * 6, PLAY_Y + 30);
                    px(PLAY_X + 25 + i * 6 + 4, PLAY_Y + 30);
                }
            }

            snprintf(buf, sizeof(buf), "T %02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
            draw_str(PLAY_X, PLAY_Y + 36, buf);
            break;
        }

        case 3: {
            draw_str(PLAY_X, PLAY_Y, "DIAGNOSTIC");
            for (int r = 0; r < 4; r++) {
                snprintf(buf, sizeof(buf), "0x%04X %04X", rand() % 0xFFFF, rand() % 0xFFFF);
                draw_str(PLAY_X, PLAY_Y + 10 + r * 8, buf);
            }

            float ang = sim_t * 5.0f;
            int r_cx = WIDTH - 16;
            int r_cy = PLAY_Y + 20;
            fill_circle(r_cx, r_cy, 1);
            px(r_cx + (int)(12 * cosf(ang)), r_cy + (int)(12 * sinf(ang)));
            px(r_cx + (int)(10 * cosf(ang - 0.2f)), r_cy + (int)(10 * sinf(ang - 0.2f)));
            px(r_cx + (int)(8 * cosf(ang - 0.4f)), r_cy + (int)(8 * sinf(ang - 0.4f)));
            break;
        }
    }
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

    serial_fd = serial_open_raw(SERIAL_DEVICE);
    if (serial_fd < 0) {
        perror("open serial");
        return 1;
    }

    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    srand((unsigned)time(NULL));

    if (!wait_for_ready()) {
        fprintf(stderr, "Failed to receive READY from Uno Q OLED sink\n");
        close(serial_fd);
        return 1;
    }

    struct timespec prev;
    clock_gettime(CLOCK_MONOTONIC, &prev);

    float sim_t = 0.0f;
    float ecg_phase = 0.0f;
    int base_steps = 5500 + (rand() % 1000);
    unsigned long frame_count = 0;
    struct timespec report_prev;
    clock_gettime(CLOCK_MONOTONIC, &report_prev);

    while (running) {
        struct timespec frame_start;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);

        float dt = (float)(frame_start.tv_sec - prev.tv_sec) +
                   (float)(frame_start.tv_nsec - prev.tv_nsec) / 1e9f;
        if (dt > 1.0f) dt = 1.0f;
        prev = frame_start;
        sim_t += dt;

        time_t now = time(NULL);
        struct tm tm_now;
        localtime_r(&now, &tm_now);

        float bpm = 72.0f;
        int steps = base_steps + (int)(sim_t * 2.8f);
        if (steps > 14000) steps = 14000;
        int batt = 87 - (int)(sim_t * 0.005f);
        if (batt < 5) batt = 5;

        ecg_phase += dt / (60.0f / bpm);
        if (ecg_phase >= 1.0f) ecg_phase -= 1.0f;

        memset(fb, 0, sizeof(fb));
        int scene = (int)((now / 15) % 4);
        draw_lcars_frame(sim_t, scene);
        draw_scenes(&tm_now, steps, batt, sim_t, ecg_phase, scene);

        if (!send_frame_and_wait_ack()) {
            fprintf(stderr, "Frame send failed\n");
            break;
        }
        frame_count++;

        if ((frame_count % 60u) == 0u) {
            struct timespec report_now;
            clock_gettime(CLOCK_MONOTONIC, &report_now);
            double elapsed = (double)(report_now.tv_sec - report_prev.tv_sec) +
                             (double)(report_now.tv_nsec - report_prev.tv_nsec) / 1e9;
            if (elapsed > 0.0) {
                printf("status sent=%lu fps=%.2f\n", frame_count, 60.0 / elapsed);
                fflush(stdout);
            }
            report_prev = report_now;
        }

        struct timespec frame_end;
        clock_gettime(CLOCK_MONOTONIC, &frame_end);
        double frame_elapsed = (double)(frame_end.tv_sec - frame_start.tv_sec) +
                               (double)(frame_end.tv_nsec - frame_start.tv_nsec) / 1e9;
        double sleep_s = (1.0 / TARGET_FPS) - frame_elapsed;
        if (sleep_s > 0.0) {
            struct timespec req;
            req.tv_sec = (time_t)sleep_s;
            req.tv_nsec = (long)((sleep_s - (double)req.tv_sec) * 1e9);
            nanosleep(&req, NULL);
        }
    }

    memset(fb, 0, sizeof(fb));
    send_frame_and_wait_ack();
    send_end();
    close(serial_fd);
    return 0;
}
