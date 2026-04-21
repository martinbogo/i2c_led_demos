/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-21
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * st_dashboard_stream.c - Legacy direct-stream host renderer for ST Dashboard on Uno Q
 *
 * Build:     ./build.sh unoq
 */
/*
 * Uno Q host-side port of st_dashboard.c
 * Renders frames on the Linux side and streams them to the MCU OLED sink.
 */

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/statvfs.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define WIDTH 128
#define HEIGHT 64
#define PAGES (HEIGHT / 8)
#define TARGET_FPS 12.0f
#define SCENE_COUNT 6
#define SCENE_SECONDS 7
#define MAX_CORES 8
#define HISTORY_LEN 64
#define ALERT_HOLD_SEC 180.0f
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

#define ALERT_UV        (1u << 0)
#define ALERT_THROTTLE  (1u << 1)
#define ALERT_TEMP      (1u << 2)
#define ALERT_NET       (1u << 3)
#define ALERT_DISK      (1u << 4)
#define ALERT_MEM       (1u << 5)
#define ALERT_SERVICE   (1u << 6)
#define ALERT_FAN       (1u << 7)

typedef struct {
    unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
} cpu_snap_t;

typedef struct {
    int temp_c;
    float temp_headroom_c;
    int fan_value;
    int fan_is_rpm;
    float arm_freq_mhz;
    float arm_max_mhz;
    float core_freq_mhz[MAX_CORES];
    int core_count;
    float total_cpu_pct;
    float per_core_pct[MAX_CORES];
    float iowait_pct;
    float load1;
    float load5;
    float load15;
    unsigned long long mem_total_kb;
    unsigned long long mem_avail_kb;
    unsigned long long mem_cached_kb;
    unsigned long long swap_total_kb;
    unsigned long long swap_free_kb;
    unsigned long long zram_used_kb;
    float mem_psi_avg10;
    unsigned long long disk_total_b;
    unsigned long long disk_used_b;
    unsigned long long disk_read_rate_bps;
    unsigned long long disk_write_rate_bps;
    char root_dev[32];
    char root_tag[12];
    int nvme_temp_c;
    unsigned long long net_rx_rate_bps;
    unsigned long long net_tx_rate_bps;
    char eth_if[16];
    char eth_ip[16];
    int eth_up;
    int eth_speed_mbps;
    char wlan_if[16];
    char wlan_ip[16];
    int wlan_up;
    int wifi_rssi_dbm;
    char gateway_ip[16];
    float gateway_latency_ms;
    int failed_units;
    char governor[24];
    char arch[16];
    char kernel[16];
    long uptime;
    int procs;
    unsigned int throttled_mask;
    unsigned int alert_current;
    unsigned int alert_recent;
} telemetry_t;

static int serial_fd = -1;
static uint8_t fb[FRAME_BYTES];
static volatile int running = 1;
static telemetry_t g_telemetry;
static float g_temp_hist[HISTORY_LEN];
static float g_cpu_hist[HISTORY_LEN];
static float g_mem_hist[HISTORY_LEN];
static float g_io_hist[HISTORY_LEN];
static float g_net_hist[HISTORY_LEN];
static float g_alert_seen[8] = { -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f };

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

static void outline_rect(int x0, int y0, int x1, int y1) {
    fill_rect(x0, y0, x1, y0);
    fill_rect(x0, y1, x1, y1);
    fill_rect(x0, y0, x0, y1);
    fill_rect(x1, y0, x1, y1);
}

static void clear_rect(int x0, int y0, int x1, int y1) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            clear_px(x, y);
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

static float clampf_local(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void history_push(float *hist, float val) {
    memmove(hist, hist + 1, (HISTORY_LEN - 1) * sizeof(float));
    hist[HISTORY_LEN - 1] = val;
}

static float rate_to_pct(unsigned long long rate, unsigned long long full_scale) {
    if (full_scale == 0) return 0.0f;
    return clampf_local((100.0f * (float)rate) / (float)full_scale, 0.0f, 100.0f);
}

static void trim_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r')) s[--n] = '\0';
}

static int read_first_line(const char *path, char *buf, size_t size) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(buf, (int)size, f) != NULL;
    fclose(f);
    if (!ok) return 0;
    trim_newline(buf);
    return 1;
}

static int read_u64_file(const char *path, unsigned long long *out) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fscanf(f, "%llu", out) == 1;
    fclose(f);
    return ok;
}

static void format_compact_bytes(unsigned long long b, char *out, size_t out_sz) {
    if (b >= (1ULL << 30)) snprintf(out, out_sz, "%.1fG", (double)b / (double)(1ULL << 30));
    else if (b >= (1ULL << 20)) snprintf(out, out_sz, "%.1fM", (double)b / (double)(1ULL << 20));
    else if (b >= (1ULL << 10)) snprintf(out, out_sz, "%lluK", b >> 10);
    else snprintf(out, out_sz, "%lluB", b);
}

static void format_compact_kib(unsigned long long kb, char *out, size_t out_sz) {
    format_compact_bytes(kb * 1024ULL, out, out_sz);
}

static void draw_hbar(int x, int y, int w, int h, float ratio) {
    outline_rect(x, y, x + w - 1, y + h - 1);
    if (w <= 2 || h <= 2) return;
    int fill_w = (int)lroundf(clampf_local(ratio, 0.0f, 1.0f) * (float)(w - 2));
    if (fill_w > 0)
        fill_rect(x + 1, y + 1, x + fill_w, y + h - 2);
}

static void draw_vbar(int x, int y, int w, int h, float ratio) {
    outline_rect(x, y, x + w - 1, y + h - 1);
    if (w <= 2 || h <= 2) return;
    int fill_h = (int)lroundf(clampf_local(ratio, 0.0f, 1.0f) * (float)(h - 2));
    if (fill_h > 0)
        fill_rect(x + 1, y + h - 1 - fill_h, x + w - 2, y + h - 2);
}

static void draw_sparkline(int x, int y, int w, int h, const float *hist, float max_v) {
    outline_rect(x, y, x + w - 1, y + h - 1);
    if (w <= 2 || h <= 2 || max_v <= 0.0f) return;

    int inner_w = w - 2;
    int start = HISTORY_LEN - inner_w;
    if (start < 0) start = 0;

    for (int i = 0; i < inner_w; i++) {
        int idx = start + i;
        if (idx >= HISTORY_LEN) break;
        float v = clampf_local(hist[idx], 0.0f, max_v);
        int bar_h = (int)lroundf((v / max_v) * (float)(h - 2));
        for (int dy = 0; dy < bar_h; dy++)
            px(x + 1 + i, y + h - 2 - dy);
    }
}

static void draw_alert_pips(int x, int y, unsigned int current, unsigned int recent) {
    static const unsigned int bits[] = {
        ALERT_UV, ALERT_THROTTLE, ALERT_TEMP, ALERT_NET,
        ALERT_DISK, ALERT_MEM, ALERT_SERVICE, ALERT_FAN
    };

    for (int i = 0; i < 8; i++) {
        int px0 = x + i * 5;
        if (recent & bits[i]) outline_rect(px0, y, px0 + 3, y + 3);
        if (current & bits[i]) fill_rect(px0 + 1, y + 1, px0 + 2, y + 2);
    }
}

static void build_alert_summary(unsigned int recent, char *out, size_t out_sz) {
    static const struct { unsigned int bit; const char *tag; } tags[] = {
        { ALERT_UV, "UV" }, { ALERT_THROTTLE, "THR" }, { ALERT_TEMP, "TMP" },
        { ALERT_NET, "NET" }, { ALERT_DISK, "DSK" }, { ALERT_MEM, "MEM" },
        { ALERT_SERVICE, "SVC" }, { ALERT_FAN, "FAN" }
    };

    out[0] = '\0';
    for (int i = 0; i < 8; i++) {
        if (recent & tags[i].bit) {
            if (out[0] != '\0') strncat(out, " ", out_sz - strlen(out) - 1);
            strncat(out, tags[i].tag, out_sz - strlen(out) - 1);
        }
    }
    if (out[0] == '\0') snprintf(out, out_sz, "NOMINAL");
}

static int parse_cpu_fields(const char *line, cpu_snap_t *s) {
    return sscanf(line, "%*s %llu %llu %llu %llu %llu %llu %llu %llu",
                  &s->user, &s->nice, &s->sys, &s->idle,
                  &s->iowait, &s->irq, &s->softirq, &s->steal) == 8;
}

static void read_cpu_snaps(cpu_snap_t *total, cpu_snap_t cores[MAX_CORES], int *core_count) {
    FILE *f = fopen("/proc/stat", "r");
    char line[256];
    *core_count = 0;
    memset(total, 0, sizeof(*total));
    memset(cores, 0, sizeof(cpu_snap_t) * MAX_CORES);
    if (!f) return;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "cpu ", 4) == 0) {
            parse_cpu_fields(line, total);
        } else if (strncmp(line, "cpu", 3) == 0 && isdigit((unsigned char)line[3])) {
            int idx = atoi(line + 3);
            if (idx >= 0 && idx < MAX_CORES && parse_cpu_fields(line, &cores[idx]) && idx + 1 > *core_count)
                *core_count = idx + 1;
        }
    }
    fclose(f);
}

static float calc_cpu_pct(const cpu_snap_t *a, const cpu_snap_t *b) {
    unsigned long long idle_a = a->idle + a->iowait;
    unsigned long long idle_b = b->idle + b->iowait;
    unsigned long long total_a = a->user + a->nice + a->sys + a->idle +
                                 a->iowait + a->irq + a->softirq + a->steal;
    unsigned long long total_b = b->user + b->nice + b->sys + b->idle +
                                 b->iowait + b->irq + b->softirq + b->steal;
    unsigned long long total_d = total_b - total_a;
    unsigned long long idle_d = idle_b - idle_a;
    if (total_d == 0) return 0.0f;
    return 100.0f * (float)(total_d - idle_d) / (float)total_d;
}

static float calc_iowait_pct(const cpu_snap_t *a, const cpu_snap_t *b) {
    unsigned long long total_a = a->user + a->nice + a->sys + a->idle +
                                 a->iowait + a->irq + a->softirq + a->steal;
    unsigned long long total_b = b->user + b->nice + b->sys + b->idle +
                                 b->iowait + b->irq + b->softirq + b->steal;
    unsigned long long total_d = total_b - total_a;
    if (total_d == 0) return 0.0f;
    return 100.0f * (float)(b->iowait - a->iowait) / (float)total_d;
}

static void read_loadavg(float *l1, float *l5, float *l15) {
    FILE *f = fopen("/proc/loadavg", "r");
    *l1 = *l5 = *l15 = 0.0f;
    if (!f) return;
    if (fscanf(f, "%f %f %f", l1, l5, l15) != 3) {
        *l1 = *l5 = *l15 = 0.0f;
    }
    fclose(f);
}

static void read_meminfo(telemetry_t *t) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    t->mem_total_kb = t->mem_avail_kb = t->mem_cached_kb = 0;
    t->swap_total_kb = t->swap_free_kb = 0;
    if (!f) return;

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %llu kB", &t->mem_total_kb) == 1) continue;
        if (sscanf(line, "MemAvailable: %llu kB", &t->mem_avail_kb) == 1) continue;
        if (sscanf(line, "Cached: %llu kB", &t->mem_cached_kb) == 1) continue;
        if (sscanf(line, "SwapTotal: %llu kB", &t->swap_total_kb) == 1) continue;
        if (sscanf(line, "SwapFree: %llu kB", &t->swap_free_kb) == 1) continue;
    }
    fclose(f);
}

static float read_psi_avg10(const char *path) {
    FILE *f = fopen(path, "r");
    char line[256];
    float avg10 = 0.0f;
    if (!f) return 0.0f;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "some ", 5) == 0) {
            char *p = strstr(line, "avg10=");
            if (p) sscanf(p + 6, "%f", &avg10);
            break;
        }
    }
    fclose(f);
    return avg10;
}

static unsigned long long read_zram_used_kb(void) {
    unsigned long long total_kb = 0;
    char path[64];
    for (int i = 0; i < 8; i++) {
        snprintf(path, sizeof(path), "/sys/block/zram%d/mm_stat", i);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        unsigned long long orig = 0, compr = 0, used = 0;
        if (fscanf(f, "%llu %llu %llu", &orig, &compr, &used) == 3)
            total_kb += used / 1024ULL;
        fclose(f);
    }
    return total_kb;
}

static float read_cpu_freq_mhz(int cpu) {
    char path[128];
    unsigned long long freq = 0;
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
    if (read_u64_file(path, &freq)) return (float)freq / 1000.0f;
    snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_cur_freq", cpu);
    if (read_u64_file(path, &freq)) return (float)freq / 1000.0f;
    return 0.0f;
}

static float read_cpu_max_mhz(void) {
    unsigned long long freq = 0;
    if (read_u64_file("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", &freq))
        return (float)freq / 1000.0f;
    if (read_u64_file("/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq", &freq))
        return (float)freq / 1000.0f;
    return 0.0f;
}

static void read_governor(char *out, size_t out_sz) {
    if (!read_first_line("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor", out, out_sz))
        snprintf(out, out_sz, "N/A");
}

static void root_base_device(const char *device, char *out, size_t out_sz) {
    const char *base = strrchr(device, '/');
    base = base ? base + 1 : device;
    size_t copy_len = strlen(base);
    if (copy_len >= out_sz) copy_len = out_sz - 1;
    memcpy(out, base, copy_len);
    out[copy_len] = '\0';

    size_t len = strlen(out);
    if (strncmp(out, "nvme", 4) == 0 || strncmp(out, "mmcblk", 6) == 0) {
        char *p = strrchr(out, 'p');
        if (p && p[1] && isdigit((unsigned char)p[1])) *p = '\0';
    } else {
        while (len > 0 && isdigit((unsigned char)out[len - 1])) out[--len] = '\0';
    }
}

static void detect_root_device(char *root_dev, size_t dev_sz, char *root_tag, size_t tag_sz) {
    FILE *f = fopen("/proc/mounts", "r");
    char device[128], mountpt[128], fs[64];
    snprintf(root_dev, dev_sz, "unknown");
    snprintf(root_tag, tag_sz, "ROOT");
    if (!f) return;

    while (fscanf(f, "%127s %127s %63s %*s %*d %*d", device, mountpt, fs) == 3) {
        if (strcmp(mountpt, "/") == 0) {
            root_base_device(device, root_dev, dev_sz);
            if (strstr(root_dev, "nvme")) snprintf(root_tag, tag_sz, "NVME");
            else if (strstr(root_dev, "mmcblk")) snprintf(root_tag, tag_sz, "SD");
            else if (strncmp(root_dev, "sd", 2) == 0) snprintf(root_tag, tag_sz, "USB");
            else if (strncmp(root_dev, "dm-", 3) == 0) snprintf(root_tag, tag_sz, "LVM");
            break;
        }
    }
    fclose(f);
}

static int read_nvme_temp_c(void) {
    char path[128], name[64];
    unsigned long long temp = 0;
    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", i);
        if (!read_first_line(path, name, sizeof(name))) continue;
        if (!strstr(name, "nvme")) continue;
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/temp1_input", i);
        if (read_u64_file(path, &temp)) return (int)(temp / 1000ULL);
    }
    return -1;
}

static int read_iface_carrier(const char *ifname) {
    char path[128];
    unsigned long long carrier = 0;
    if (!ifname[0]) return 0;
    snprintf(path, sizeof(path), "/sys/class/net/%s/carrier", ifname);
    if (read_u64_file(path, &carrier)) return carrier ? 1 : 0;
    return 0;
}

static int read_eth_speed_mbps(const char *ifname) {
    char path[128];
    unsigned long long speed = 0;
    if (!ifname[0]) return -1;
    snprintf(path, sizeof(path), "/sys/class/net/%s/speed", ifname);
    if (read_u64_file(path, &speed)) return (int)speed;
    return -1;
}

static int read_wifi_rssi_dbm(const char *ifname) {
    FILE *f = fopen("/proc/net/wireless", "r");
    char line[256], name[32];
    float status = 0.0f, quality = 0.0f, level = 0.0f;
    if (!f || !ifname[0]) return -1000;

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, " %31[^:]: %f %f %f", name, &status, &quality, &level) == 4) {
            if (strcmp(name, ifname) == 0) {
                fclose(f);
                return (int)lroundf(level);
            }
        }
    }
    fclose(f);
    return -1000;
}

static void get_default_gateway_ip(char *out, size_t out_sz) {
    FILE *f = fopen("/proc/net/route", "r");
    char line[256], iface[32];
    unsigned long dest = 0, gate = 0, flags = 0;
    out[0] = '\0';
    if (!f) return;

    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return;
    }
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%31s %lx %lx %lx", iface, &dest, &gate, &flags) == 4) {
            if (dest == 0 && (flags & 0x2)) {
                snprintf(out, out_sz, "%u.%u.%u.%u",
                         (unsigned int)(gate & 0xFF),
                         (unsigned int)((gate >> 8) & 0xFF),
                         (unsigned int)((gate >> 16) & 0xFF),
                         (unsigned int)((gate >> 24) & 0xFF));
                break;
            }
        }
    }
    fclose(f);
}

static float ping_host_ms(const char *host) {
    if (!host[0]) return -1.0f;
    char cmd[128], line[256];
    snprintf(cmd, sizeof(cmd), "ping -n -c 1 -W 1 %s 2>/dev/null", host);
    FILE *p = popen(cmd, "r");
    if (!p) return -1.0f;
    float ms = -1.0f;
    while (fgets(line, sizeof(line), p)) {
        char *t = strstr(line, "time=");
        if (t && sscanf(t + 5, "%f", &ms) == 1) break;
    }
    pclose(p);
    return ms;
}

static int count_failed_units(void) {
    FILE *p = popen("systemctl --failed --no-legend --plain 2>/dev/null", "r");
    char line[256];
    int count = 0;
    if (!p) return -1;
    while (fgets(line, sizeof(line), p)) count++;
    pclose(p);
    return count;
}

static unsigned int read_throttled_mask(void) {
    return 0;
}

static int read_best_temp_c(void) {
    int best = 0;
    char path[128];
    for (int i = 0; i < 64; i++) {
        unsigned long long temp = 0;
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", i);
        if (!read_u64_file(path, &temp)) continue;
        if (temp >= 1000ULL && temp <= 200000ULL) {
            int c = (int)(temp / 1000ULL);
            if (c > best) best = c;
        }
    }
    return best;
}

static int get_fan(void) {
    int val = -1;
    char path[128];
    FILE *f;

    for (int i = 0; i < 32; i++) {
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/fan1_input", i);
        f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%d", &val) == 1) {
                fclose(f);
                return val;
            }
            fclose(f);
        }
    }

    for (int i = 0; i < 16; i++) {
        snprintf(path, sizeof(path), "/sys/class/thermal/cooling_device%d/type", i);
        f = fopen(path, "r");
        if (f) {
            char type[64] = {0};
            if (fscanf(f, "%63s", type) != 1) type[0] = '\0';
            fclose(f);
            if (strstr(type, "fan") || strstr(type, "pwm")) {
                snprintf(path, sizeof(path), "/sys/class/thermal/cooling_device%d/cur_state", i);
                f = fopen(path, "r");
                if (f) {
                    if (fscanf(f, "%d", &val) == 1) {
                        fclose(f);
                        return val;
                    }
                    fclose(f);
                }
            }
        }
    }
    return -1;
}

static void get_net(unsigned long long *rx, unsigned long long *tx) {
    *rx = 0;
    *tx = 0;
    FILE *f = fopen("/proc/net/dev", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (!strstr(line, "lo:") && strchr(line, ':')) {
            char *p = strchr(line, ':') + 1;
            unsigned long long r, t;
            if (sscanf(p, "%llu %*d %*d %*d %*d %*d %*d %*d %llu", &r, &t) == 2) {
                *rx += r;
                *tx += t;
            }
        }
    }
    fclose(f);
}

static void get_disk_io_for_root(const char *root_dev, unsigned long long *rd, unsigned long long *wr) {
    *rd = 0;
    *wr = 0;
    if (!root_dev[0] || strcmp(root_dev, "unknown") == 0) return;

    char path[128];
    snprintf(path, sizeof(path), "/sys/block/%s/stat", root_dev);
    FILE *f = fopen(path, "r");
    if (!f) return;

    unsigned long long vals[11] = {0};
    int n = fscanf(f, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &vals[0], &vals[1], &vals[2], &vals[3], &vals[4], &vals[5],
                   &vals[6], &vals[7], &vals[8], &vals[9], &vals[10]);
    fclose(f);
    if (n >= 7) {
        *rd = vals[2] * 512ULL;
        *wr = vals[6] * 512ULL;
    }
}

static void get_ips(char *eth_if, char *eth_ip, char *wlan_if, char *wlan_ip) {
    struct ifaddrs *ifap, *ifa;
    struct sockaddr_in *sa;

    strcpy(eth_if, "eth0");
    strcpy(eth_ip, "OFFLINE");
    strcpy(wlan_if, "wlan0");
    strcpy(wlan_ip, "OFFLINE");

    if (getifaddrs(&ifap) == -1) return;
    for (ifa = ifap; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            sa = (struct sockaddr_in *)ifa->ifa_addr;
            if (strncmp(ifa->ifa_name, "eth", 3) == 0 || strncmp(ifa->ifa_name, "en", 2) == 0) {
                strncpy(eth_if, ifa->ifa_name, 15);
                eth_if[15] = '\0';
                strncpy(eth_ip, inet_ntoa(sa->sin_addr), 15);
                eth_ip[15] = '\0';
            } else if (strncmp(ifa->ifa_name, "wlan", 4) == 0 || strncmp(ifa->ifa_name, "wl", 2) == 0) {
                strncpy(wlan_if, ifa->ifa_name, 15);
                wlan_if[15] = '\0';
                strncpy(wlan_ip, inet_ntoa(sa->sin_addr), 15);
                wlan_ip[15] = '\0';
            }
        }
    }
    freeifaddrs(ifap);
}

static void touch_alert_mask(float sim_t, unsigned int mask) {
    static const unsigned int bits[8] = {
        ALERT_UV, ALERT_THROTTLE, ALERT_TEMP, ALERT_NET,
        ALERT_DISK, ALERT_MEM, ALERT_SERVICE, ALERT_FAN
    };
    for (int i = 0; i < 8; i++) {
        if (mask & bits[i]) g_alert_seen[i] = sim_t;
    }
}

static unsigned int recent_alert_mask(float sim_t) {
    static const unsigned int bits[8] = {
        ALERT_UV, ALERT_THROTTLE, ALERT_TEMP, ALERT_NET,
        ALERT_DISK, ALERT_MEM, ALERT_SERVICE, ALERT_FAN
    };
    unsigned int mask = 0;
    for (int i = 0; i < 8; i++) {
        if (g_alert_seen[i] >= 0.0f && sim_t - g_alert_seen[i] <= ALERT_HOLD_SEC) mask |= bits[i];
    }
    return mask;
}

static void refresh_alert_state(float sim_t) {
    unsigned int current = 0;
    float mem_used_pct = 0.0f;
    if (g_telemetry.mem_total_kb > 0) {
        mem_used_pct = 100.0f * (float)(g_telemetry.mem_total_kb - g_telemetry.mem_avail_kb) /
                       (float)g_telemetry.mem_total_kb;
    }

    if (g_telemetry.throttled_mask & 0x1u) current |= ALERT_UV;
    if (g_telemetry.throttled_mask & ((1u << 1) | (1u << 2) | (1u << 3))) current |= ALERT_THROTTLE;
    if (g_telemetry.temp_c >= 80 || g_telemetry.temp_headroom_c <= 5.0f) current |= ALERT_TEMP;
    if (!g_telemetry.eth_up && !g_telemetry.wlan_up) current |= ALERT_NET;
    if (g_telemetry.disk_total_b > 0 && (double)g_telemetry.disk_used_b / (double)g_telemetry.disk_total_b > 0.90)
        current |= ALERT_DISK;
    if (mem_used_pct > 92.0f || g_telemetry.mem_psi_avg10 > 0.50f) current |= ALERT_MEM;
    if (g_telemetry.failed_units > 0) current |= ALERT_SERVICE;
    if (g_telemetry.fan_value < 0 && g_telemetry.temp_c >= 65) current |= ALERT_FAN;

    touch_alert_mask(sim_t, current);
    g_telemetry.alert_current = current;
    g_telemetry.alert_recent = recent_alert_mask(sim_t);
}

static void update_telemetry(float sim_t) {
    static float last_fast = -1000.0f;
    static float last_slow = -1000.0f;
    static cpu_snap_t prev_total = {0};
    static cpu_snap_t prev_cores[MAX_CORES] = {{0}};
    static int have_prev_cpu = 0;
    static unsigned long long prev_rx = 0, prev_tx = 0;
    static unsigned long long prev_rd = 0, prev_wr = 0;
    static float prev_net_t = 0.0f;
    static float prev_disk_t = 0.0f;

    if (sim_t - last_fast < 0.95f && last_fast > -999.0f) return;

    struct sysinfo info;
    memset(&info, 0, sizeof(info));
    sysinfo(&info);
    g_telemetry.uptime = info.uptime;
    g_telemetry.procs = info.procs;

    struct utsname uts;
    if (uname(&uts) == 0) {
        strncpy(g_telemetry.arch, uts.machine, sizeof(g_telemetry.arch) - 1);
        g_telemetry.arch[sizeof(g_telemetry.arch) - 1] = '\0';
        strncpy(g_telemetry.kernel, uts.release, sizeof(g_telemetry.kernel) - 1);
        g_telemetry.kernel[sizeof(g_telemetry.kernel) - 1] = '\0';
    }

    g_telemetry.temp_c = read_best_temp_c();
    g_telemetry.temp_headroom_c = 85.0f - (float)g_telemetry.temp_c;
    g_telemetry.fan_value = get_fan();
    g_telemetry.fan_is_rpm = g_telemetry.fan_value > 255;

    read_loadavg(&g_telemetry.load1, &g_telemetry.load5, &g_telemetry.load15);
    read_meminfo(&g_telemetry);
    g_telemetry.mem_psi_avg10 = read_psi_avg10("/proc/pressure/memory");
    g_telemetry.zram_used_kb = read_zram_used_kb();

    cpu_snap_t total;
    cpu_snap_t cores[MAX_CORES];
    int core_count = 0;
    read_cpu_snaps(&total, cores, &core_count);
    g_telemetry.core_count = core_count;
    g_telemetry.total_cpu_pct = 0.0f;
    g_telemetry.iowait_pct = 0.0f;
    memset(g_telemetry.per_core_pct, 0, sizeof(g_telemetry.per_core_pct));

    if (have_prev_cpu) {
        g_telemetry.total_cpu_pct = calc_cpu_pct(&prev_total, &total);
        g_telemetry.iowait_pct = calc_iowait_pct(&prev_total, &total);
        for (int i = 0; i < core_count; i++)
            g_telemetry.per_core_pct[i] = calc_cpu_pct(&prev_cores[i], &cores[i]);
    }
    prev_total = total;
    memcpy(prev_cores, cores, sizeof(prev_cores));
    have_prev_cpu = 1;

    for (int i = 0; i < core_count; i++) g_telemetry.core_freq_mhz[i] = read_cpu_freq_mhz(i);
    g_telemetry.arm_freq_mhz = read_cpu_freq_mhz(0);

    unsigned long long rx = 0, tx = 0;
    get_net(&rx, &tx);
    if (prev_net_t > 0.0f) {
        float dt = sim_t - prev_net_t;
        if (dt > 0.0f) {
            g_telemetry.net_rx_rate_bps = (unsigned long long)((double)(rx - prev_rx) / dt);
            g_telemetry.net_tx_rate_bps = (unsigned long long)((double)(tx - prev_tx) / dt);
        }
    }
    prev_rx = rx;
    prev_tx = tx;
    prev_net_t = sim_t;

    unsigned long long rd = 0, wr = 0;
    get_disk_io_for_root(g_telemetry.root_dev, &rd, &wr);
    if (prev_disk_t > 0.0f) {
        float dt = sim_t - prev_disk_t;
        if (dt > 0.0f) {
            g_telemetry.disk_read_rate_bps = (unsigned long long)((double)(rd - prev_rd) / dt);
            g_telemetry.disk_write_rate_bps = (unsigned long long)((double)(wr - prev_wr) / dt);
        }
    }
    prev_rd = rd;
    prev_wr = wr;
    prev_disk_t = sim_t;

    struct statvfs sfs;
    if (statvfs("/", &sfs) == 0) {
        g_telemetry.disk_total_b = (unsigned long long)sfs.f_blocks * sfs.f_frsize;
        g_telemetry.disk_used_b = g_telemetry.disk_total_b - ((unsigned long long)sfs.f_bfree * sfs.f_frsize);
    }

    get_ips(g_telemetry.eth_if, g_telemetry.eth_ip, g_telemetry.wlan_if, g_telemetry.wlan_ip);
    g_telemetry.eth_up = read_iface_carrier(g_telemetry.eth_if) || strcmp(g_telemetry.eth_ip, "OFFLINE") != 0;
    g_telemetry.wlan_up = read_iface_carrier(g_telemetry.wlan_if) || strcmp(g_telemetry.wlan_ip, "OFFLINE") != 0;

    if (sim_t - last_slow >= 4.5f || last_slow < -999.0f) {
        g_telemetry.arm_max_mhz = read_cpu_max_mhz();
        read_governor(g_telemetry.governor, sizeof(g_telemetry.governor));
        detect_root_device(g_telemetry.root_dev, sizeof(g_telemetry.root_dev),
                           g_telemetry.root_tag, sizeof(g_telemetry.root_tag));
        g_telemetry.nvme_temp_c = read_nvme_temp_c();
        g_telemetry.throttled_mask = read_throttled_mask();
        g_telemetry.eth_speed_mbps = read_eth_speed_mbps(g_telemetry.eth_if);
        g_telemetry.wifi_rssi_dbm = read_wifi_rssi_dbm(g_telemetry.wlan_if);
        get_default_gateway_ip(g_telemetry.gateway_ip, sizeof(g_telemetry.gateway_ip));
        g_telemetry.gateway_latency_ms = ping_host_ms(g_telemetry.gateway_ip);
        g_telemetry.failed_units = count_failed_units();
        last_slow = sim_t;
    }

    refresh_alert_state(sim_t);

    history_push(g_temp_hist, (float)g_telemetry.temp_c);
    history_push(g_cpu_hist, g_telemetry.total_cpu_pct);
    history_push(g_mem_hist,
                 g_telemetry.mem_total_kb > 0 ?
                 100.0f * (float)(g_telemetry.mem_total_kb - g_telemetry.mem_avail_kb) /
                 (float)g_telemetry.mem_total_kb : 0.0f);
    history_push(g_io_hist, rate_to_pct(g_telemetry.disk_read_rate_bps + g_telemetry.disk_write_rate_bps,
                                        100ULL * 1024ULL * 1024ULL));
    history_push(g_net_hist, rate_to_pct(g_telemetry.net_rx_rate_bps + g_telemetry.net_tx_rate_bps,
                                         125ULL * 1024ULL * 1024ULL));

    last_fast = sim_t;
}

static void draw_lcars_frame(float sim_t, int scene, const char *title) {
    fill_rect(0, 0, WIDTH - 12, 0);
    fill_rect(0, 14, WIDTH - 12, 14);
    fill_rect(0, 0, 0, 14);

    for (float a = (float)(-M_PI / 2.0); a <= (float)(M_PI / 2.0); a += 0.05f) {
        px(WIDTH - 12 + (int)(7.0f * cosf(a)), 7 + (int)(7.0f * sinf(a)));
    }

    char buf[24];
    snprintf(buf, sizeof(buf), "%s", title);
    int tw = (int)strlen(buf) * 6;

    static const int title_offsets[] = { -3, -2, 2, 3 };
    static int title_jitter = -2;
    static int last_scene = -1;
    if (scene != last_scene) {
        int next_jitter = title_jitter;
        while (next_jitter == title_jitter)
            next_jitter = title_offsets[rand() % 4];
        title_jitter = next_jitter;
        last_scene = scene;
    }

    int str_x = WIDTH - 20 - tw + title_jitter;
    if (str_x < 50) str_x = 50;
    int str_y = 5;
    draw_alert_pips(4, 5, g_telemetry.alert_current, g_telemetry.alert_recent);
    draw_str(str_x, str_y, buf);

    const int box_left = 0;
    const int box_right = 14;
    const int top_box_top = 17;
    const int top_box_bottom = 30;
    const int top_box_lip = 22;
    const int curve_start_y = 14;

    fill_rect(box_left, top_box_top, top_box_lip, top_box_top);
    fill_rect(box_left, top_box_bottom, box_right, top_box_bottom);
    fill_rect(box_left, top_box_top, box_left, top_box_bottom);

    for (int y = curve_start_y; y <= top_box_bottom; y++) {
        float t = (float)(y - curve_start_y) / (float)(top_box_bottom - curve_start_y);
        int x = box_right + (int)lroundf((top_box_lip - box_right) * cosf(t * (float)(M_PI / 2.0f)));
        px(x, y);
    }

    outline_rect(box_left, 33, box_right, 46);
    outline_rect(box_left, 49, box_right, 62);

    int cpu_h = (int)lroundf(clampf_local(g_telemetry.total_cpu_pct, 0.0f, 100.0f) * 11.0f / 100.0f);
    if (cpu_h > 11) cpu_h = 11;
    if (cpu_h < 0) cpu_h = 0;
    for (int y = 29; y >= 29 - cpu_h; y--) fill_rect(2, y, 12, y);

    float mem_used_pct = 0.0f;
    if (g_telemetry.mem_total_kb > 0) {
        mem_used_pct = 100.0f * (float)(g_telemetry.mem_total_kb - g_telemetry.mem_avail_kb) /
                       (float)g_telemetry.mem_total_kb;
    }
    int ram_h = (int)lroundf(clampf_local(mem_used_pct, 0.0f, 100.0f) * 11.0f / 100.0f);
    if (ram_h > 11) ram_h = 11;
    if (ram_h < 0) ram_h = 0;
    for (int y = 45; y >= 45 - ram_h; y--) fill_rect(2, y, 12, y);

    float act_pct = g_io_hist[HISTORY_LEN - 1];
    if (g_net_hist[HISTORY_LEN - 1] > act_pct) act_pct = g_net_hist[HISTORY_LEN - 1];
    if (g_telemetry.alert_current) act_pct = 100.0f;
    int act_h = (int)lroundf(clampf_local(act_pct, 0.0f, 100.0f) * 11.0f / 100.0f);
    if (act_h > 11) act_h = 11;
    if (act_h < 0) act_h = 0;
    for (int y = 61; y >= 61 - act_h; y--) fill_rect(2, y, 12, y);

    if (((int)(sim_t * 6.0f)) % 2 == 0) px(7, 31);
}

static void draw_scene_thermal_power(float sim_t, int x, int y) {
    char buf[64];
    const int stats_x = 78;

    snprintf(buf, sizeof(buf), "%dC", g_telemetry.temp_c);
    draw_large_str(x, y, buf, 2);
    draw_hbar(x, y + 16, 34, 6, g_telemetry.temp_c / 85.0f);
    snprintf(buf, sizeof(buf), "HD %dC", (int)lroundf(g_telemetry.temp_headroom_c));
    draw_str(x, y + 24, buf);

    if (g_telemetry.fan_value < 0) snprintf(buf, sizeof(buf), "FAN OFF");
    else if (g_telemetry.fan_is_rpm) snprintf(buf, sizeof(buf), "RPM %d", g_telemetry.fan_value);
    else snprintf(buf, sizeof(buf), "FAN %d", g_telemetry.fan_value);
    draw_str(x, y + 32, buf);

    if (g_telemetry.alert_current & ALERT_UV) snprintf(buf, sizeof(buf), "UV ALERT");
    else if (g_telemetry.alert_current & ALERT_THROTTLE) snprintf(buf, sizeof(buf), "THROTL");
    else snprintf(buf, sizeof(buf), "PWR OK");
    draw_str(x, y + 40, buf);

    snprintf(buf, sizeof(buf), "A%.0fM", g_telemetry.arm_freq_mhz);
    draw_str(stats_x, y, buf);
    snprintf(buf, sizeof(buf), "M%.0fM", g_telemetry.arm_max_mhz);
    draw_str(stats_x, y + 8, buf);
    snprintf(buf, sizeof(buf), "G %.6s", g_telemetry.governor);
    draw_str(stats_x, y + 16, buf);

    draw_sparkline(stats_x, y + 24, 28, 16, g_temp_hist, 85.0f);
    if (((int)(sim_t * 12.0f)) % 2 == 0) px(stats_x + 24, y + 42);
}

static void draw_scene_cpu_topology(int x, int y) {
    char buf[32];
    const int stats_x = 78;
    int bar_y = y + 10;
    int bar_h = 20;
    int bar_x0 = x + 1;

    for (int i = 0; i < g_telemetry.core_count && i < MAX_CORES; i++) {
        int bx = bar_x0 + i * 11;
        draw_char(bx + 1, y, (char)('0' + (i % 10)));
        draw_vbar(bx, bar_y, 7, bar_h, g_telemetry.per_core_pct[i] / 100.0f);
        if (g_telemetry.arm_max_mhz > 0.0f && g_telemetry.core_freq_mhz[i] > 0.0f) {
            float fr = clampf_local(g_telemetry.core_freq_mhz[i] / g_telemetry.arm_max_mhz, 0.0f, 1.0f);
            int tick_y = bar_y + 1 + (int)lroundf((1.0f - fr) * (float)(bar_h - 3));
            fill_rect(bx + 1, tick_y, bx + 5, tick_y);
        }
    }

    snprintf(buf, sizeof(buf), "A%.0f", g_telemetry.arm_freq_mhz);
    draw_str(stats_x, y, buf);
    snprintf(buf, sizeof(buf), "L%.1f", g_telemetry.load1);
    draw_str(stats_x, y + 8, buf);
    snprintf(buf, sizeof(buf), "I%.0f%%", g_telemetry.iowait_pct);
    draw_str(stats_x, y + 16, buf);
    snprintf(buf, sizeof(buf), "P%d", g_telemetry.procs);
    draw_str(stats_x, y + 24, buf);

    draw_sparkline(x, y + 34, 78, 12, g_cpu_hist, 100.0f);
}

static void draw_scene_memory_pressure(int x, int y) {
    char buf[32], a_buf[16], u_buf[16], c_buf[16], s_buf[16], z_buf[16];
    const int stats_x = 78;
    unsigned long long used_kb = g_telemetry.mem_total_kb - g_telemetry.mem_avail_kb;
    unsigned long long swap_used_kb = g_telemetry.swap_total_kb - g_telemetry.swap_free_kb;

    format_compact_kib(g_telemetry.mem_avail_kb, a_buf, sizeof(a_buf));
    format_compact_kib(used_kb, u_buf, sizeof(u_buf));
    format_compact_kib(g_telemetry.mem_cached_kb, c_buf, sizeof(c_buf));
    format_compact_kib(swap_used_kb, s_buf, sizeof(s_buf));
    format_compact_kib(g_telemetry.zram_used_kb, z_buf, sizeof(z_buf));

    draw_str(x, y, "AVAIL");
    draw_large_str(x, y + 8, a_buf, 2);
    draw_hbar(x, y + 26, 34, 6,
              g_telemetry.mem_total_kb > 0 ? (float)used_kb / (float)g_telemetry.mem_total_kb : 0.0f);
    snprintf(buf, sizeof(buf), "PSI %.2f", g_telemetry.mem_psi_avg10);
    draw_str(x, y + 36, buf);

    snprintf(buf, sizeof(buf), "U %s", u_buf);
    draw_str(stats_x, y, buf);
    snprintf(buf, sizeof(buf), "C %s", c_buf);
    draw_str(stats_x, y + 8, buf);
    snprintf(buf, sizeof(buf), "S %s", s_buf);
    draw_str(stats_x, y + 16, buf);
    snprintf(buf, sizeof(buf), "Z %s", z_buf);
    draw_str(stats_x, y + 24, buf);

    draw_sparkline(stats_x, y + 32, 28, 14, g_mem_hist, 100.0f);
}

static void draw_scene_storage_pcie(int x, int y) {
    char buf[48], used_buf[16], total_buf[16], rd_buf[16], wr_buf[16], bw_buf[16];
    const int stats_x = 78;
    unsigned long long total_bw_bps = g_telemetry.disk_read_rate_bps + g_telemetry.disk_write_rate_bps;

    draw_large_str(x, y, g_telemetry.root_tag[0] ? g_telemetry.root_tag : "ROOT", 2);
    draw_hbar(x, y + 16, 50, 6,
              g_telemetry.disk_total_b > 0 ? (float)g_telemetry.disk_used_b / (float)g_telemetry.disk_total_b : 0.0f);

    format_compact_bytes(g_telemetry.disk_used_b, used_buf, sizeof(used_buf));
    format_compact_bytes(g_telemetry.disk_total_b, total_buf, sizeof(total_buf));
    format_compact_bytes(g_telemetry.disk_read_rate_bps, rd_buf, sizeof(rd_buf));
    format_compact_bytes(g_telemetry.disk_write_rate_bps, wr_buf, sizeof(wr_buf));
    format_compact_bytes(total_bw_bps, bw_buf, sizeof(bw_buf));

    snprintf(buf, sizeof(buf), "U %s/%s", used_buf, total_buf);
    draw_str(x, y + 24, buf);
    snprintf(buf, sizeof(buf), "R %s", rd_buf);
    draw_str(x, y + 32, buf);
    snprintf(buf, sizeof(buf), "W %s", wr_buf);
    draw_str(x, y + 40, buf);
    snprintf(buf, sizeof(buf), "IOW %.0f%%", g_telemetry.iowait_pct);
    draw_str(stats_x, y, buf);
    if (g_telemetry.nvme_temp_c >= 0) snprintf(buf, sizeof(buf), "NV %dC", g_telemetry.nvme_temp_c);
    else snprintf(buf, sizeof(buf), "NV --");
    draw_str(stats_x, y + 8, buf);
    snprintf(buf, sizeof(buf), "DV %.6s", g_telemetry.root_dev);
    draw_str(stats_x, y + 16, buf);
    snprintf(buf, sizeof(buf), "BW %s", bw_buf);
    draw_str(stats_x, y + 24, buf);

    draw_sparkline(stats_x, y + 32, 28, 10, g_io_hist, 100.0f);
}

static void draw_scene_network_link(float sim_t, int x, int y) {
    char buf[48], rx_buf[16], tx_buf[16];
    (void)sim_t;

    format_compact_bytes(g_telemetry.net_rx_rate_bps, rx_buf, sizeof(rx_buf));
    format_compact_bytes(g_telemetry.net_tx_rate_bps, tx_buf, sizeof(tx_buf));

    snprintf(buf, sizeof(buf), "E:%s", strcmp(g_telemetry.eth_ip, "OFFLINE") ? g_telemetry.eth_ip : "--");
    draw_str(x, y, buf);
    snprintf(buf, sizeof(buf), "W:%s", strcmp(g_telemetry.wlan_ip, "OFFLINE") ? g_telemetry.wlan_ip : "--");
    draw_str(x, y + 8, buf);

    if (g_telemetry.eth_speed_mbps >= 0 && g_telemetry.gateway_latency_ms >= 0.0f)
        snprintf(buf, sizeof(buf), "GW %.1f LK %d", g_telemetry.gateway_latency_ms, g_telemetry.eth_speed_mbps);
    else if (g_telemetry.eth_speed_mbps >= 0)
        snprintf(buf, sizeof(buf), "GW -- LK %d", g_telemetry.eth_speed_mbps);
    else
        snprintf(buf, sizeof(buf), "GW -- LK --");
    draw_str(x, y + 16, buf);

    if (g_telemetry.wifi_rssi_dbm > -999)
        snprintf(buf, sizeof(buf), "RS %d RX %s", g_telemetry.wifi_rssi_dbm, rx_buf);
    else
        snprintf(buf, sizeof(buf), "RS -- RX %s", rx_buf);
    draw_str(x, y + 24, buf);
    snprintf(buf, sizeof(buf), "TX %s", tx_buf);
    draw_str(x, y + 32, buf);
    draw_sparkline(x, y + 40, 78, 12, g_net_hist, 100.0f);
}

static void draw_scene_system_alerts(int x, int y) {
    char buf[64], alerts[48], alerts_short[7];
    const int stats_x = 78;

    snprintf(buf, sizeof(buf), "UP %ldh%02ld", g_telemetry.uptime / 3600, (g_telemetry.uptime % 3600) / 60);
    draw_str(x, y, buf);
    snprintf(buf, sizeof(buf), "P %d", g_telemetry.procs);
    draw_str(x, y + 8, buf);
    snprintf(buf, sizeof(buf), "K %.8s", g_telemetry.kernel);
    draw_str(x, y + 16, buf);
    snprintf(buf, sizeof(buf), "A %.6s", g_telemetry.arch);
    draw_str(x, y + 24, buf);
    if (g_telemetry.failed_units >= 0) snprintf(buf, sizeof(buf), "F %d", g_telemetry.failed_units);
    else snprintf(buf, sizeof(buf), "F --");
    draw_str(x, y + 32, buf);

    build_alert_summary(g_telemetry.alert_recent, alerts, sizeof(alerts));
    snprintf(alerts_short, sizeof(alerts_short), "%.6s", alerts);
    snprintf(buf, sizeof(buf), "R:%s", alerts_short);
    draw_str(stats_x, y, buf);
    draw_alert_pips(stats_x, y + 10, g_telemetry.alert_current, g_telemetry.alert_recent);
    snprintf(buf, sizeof(buf), "G %.5s", g_telemetry.governor);
    draw_str(stats_x, y + 24, buf);
    snprintf(buf, sizeof(buf), "%s", g_telemetry.alert_current ? "ALERT" : "NOM");
    draw_str(stats_x, y + 32, buf);
}

static void draw_stats_scenes(float sim_t, int scene) {
    int play_x = 26;
    int play_y = 18;
    switch (scene) {
        case 0: draw_scene_thermal_power(sim_t, play_x, play_y); break;
        case 1: draw_scene_cpu_topology(play_x, play_y); break;
        case 2: draw_scene_memory_pressure(play_x, play_y); break;
        case 3: draw_scene_storage_pcie(play_x, play_y); break;
        case 4: draw_scene_network_link(sim_t, play_x, play_y); break;
        case 5: draw_scene_system_alerts(play_x, play_y); break;
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
    struct timespec report_prev;
    clock_gettime(CLOCK_MONOTONIC, &report_prev);

    float sim_t = 0.0f;
    unsigned long frame_count = 0;

    while (running) {
        struct timespec frame_start;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);

        float dt = (float)(frame_start.tv_sec - prev.tv_sec) +
                   (float)(frame_start.tv_nsec - prev.tv_nsec) / 1e9f;
        if (dt > 1.0f) dt = 1.0f;
        prev = frame_start;
        sim_t += dt;
        update_telemetry(sim_t);

        time_t now = time(NULL);
        int scene = (int)((now / SCENE_SECONDS) % SCENE_COUNT);
        const char *titles[SCENE_COUNT] = {
            "THERM/PWR", "CPU TOPO", "MEM/PRESS",
            "PCIe/STOR", "NET/LINK", "SYS/ALERT"
        };

        memset(fb, 0, sizeof(fb));
        draw_lcars_frame(sim_t, scene, titles[scene]);
        draw_stats_scenes(sim_t, scene);

        if (!send_frame_and_wait_ack()) {
            fprintf(stderr, "Frame send failed\n");
            break;
        }
        frame_count++;

        if ((frame_count % 48u) == 0u) {
            struct timespec report_now;
            clock_gettime(CLOCK_MONOTONIC, &report_now);
            double elapsed = (double)(report_now.tv_sec - report_prev.tv_sec) +
                             (double)(report_now.tv_nsec - report_prev.tv_nsec) / 1e9;
            if (elapsed > 0.0) {
                printf("status sent=%lu fps=%.2f temp=%d cpu=%.1f mem=%.1f\n",
                       frame_count,
                       48.0 / elapsed,
                       g_telemetry.temp_c,
                       g_telemetry.total_cpu_pct,
                       g_mem_hist[HISTORY_LEN - 1]);
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
