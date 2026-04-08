/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-07
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * i2c_oled_demo.c - 10-scene OLED demoscene reel for the split yellow/blue SSD1306
 *
 * Features:
 * - Scenes 1-9 run for 30 seconds each and scene 10 runs for 60 seconds
 * - Top 16 physical pixels reserved for a yellow-on-black scrolling description banner
 * - Bottom 128x48 playfield dedicated to compact monochrome demo effects
 * - Compact procedural scenes with embedded hybrid spatiotemporal grayscale video playback
 * - Scene 10 uses a compiled-in `woz_pdm.h` asset, temporal FRC, and spatiotemporal blue-noise dithering
 *
 * Compile:  gcc -Os -s -ffunction-sections -fdata-sections -Wl,--gc-sections \
 *               -o i2c_oled_demo i2c_oled_demo.c -lm -lz
 * Run:      sudo ./i2c_oled_demo
 */

#include <fcntl.h>
#if defined(__has_include)
#if __has_include(<linux/i2c-dev.h>)
#include <linux/i2c-dev.h>
#endif
#endif
#ifndef I2C_SLAVE
#define I2C_SLAVE 0x0703
#endif
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#if defined(__has_include)
#if __has_include("woz_pdm.h")
#include "woz_pdm.h"
#define HAVE_WOZ_PDM_HEADER 1
#else
#define HAVE_WOZ_PDM_HEADER 0
#endif
#else
#define HAVE_WOZ_PDM_HEADER 0
#endif

#define WIDTH             128
#define HEIGHT            64
#define PAGES             (HEIGHT / 8)
#define ADDR              0x3C
#define DEMO_FPS          20
#define VIDEO_FPS_DEFAULT 90
#define YELLOW_H          16
#define BLUE_Y            16
#define BLUE_H            48
#define BLUE_START_PAGE   (BLUE_Y / 8)
#define SCENE_COUNT       10
#define SCENE_SECONDS     30.0f
#define SCENE10_SECONDS   60.0f
#define HEADER_SCROLL_PPS 18.0f
#define MAP_W             12
#define MAP_H             12
#define PROJ_SCALE        72.0f
#define PDM_BLUE_BYTES    (WIDTH * (BLUE_H / 8))
#define PDM_PHASES        4
#define PDM_PHASES_MAX    8
#define PDM_MAGIC         0x314D4450u
#define PDM_ENCODING_RAW  0u
#define PDM_ENCODING_PHASE_XOR 1u
#define I2C_CHUNK         255

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float x, y, z;
} vec3_t;

typedef struct {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t phase_count;
    uint32_t frame_count;
    uint32_t subframe_hz;
    uint32_t encoding;
} pdm_header_t;

static int i2c_fd;
static uint8_t fb[WIDTH * PAGES];
static volatile int running = 1;
static unsigned frame_counter = 0;
static uint32_t pdm_phase_count = PDM_PHASES;
static uint32_t pdm_frame_count;
static uint32_t pdm_subframe_hz = VIDEO_FPS_DEFAULT;
static uint32_t pdm_encoding = PDM_ENCODING_RAW;
static uint8_t *pdm_asset_raw;
static const uint8_t *pdm_frame_data;

static const char *scene_msgs[SCENE_COUNT] = {
    "01 METABALLS / THRESHOLD FLUID BLOBS / MONO FIELD",
    "02 DOOM STYLE GAME",
    "03 WIREFRAME TANK WARS",
    "04 VOXEL HILLS PAPER AIRPLANE FLYBY",
    "05 WIREFRAME SPACE BATTLE",
    "06 MANDELBROT ZOOMERD",
    "07 ASCII NYAN CAT",
    "08 MASKING AND KINETIC TYPOGRAPHY",
    "09 ANIMATED MOIRE",
    "10 WIZARD OF OZ VIDEO"
};

static const float scene_durations[SCENE_COUNT] = {
    SCENE_SECONDS, SCENE_SECONDS, SCENE_SECONDS, SCENE_SECONDS, SCENE_SECONDS,
    SCENE_SECONDS, SCENE_SECONDS, SCENE_SECONDS, SCENE_SECONDS, SCENE10_SECONDS
};

static const char doom_map[MAP_H][MAP_W + 1] = {
    "############",
    "#..........#",
    "#..##......#",
    "#..........#",
    "#.....##...#",
    "#..........#",
    "#..###.....#",
    "#..........#",
    "#......##..#",
    "#..........#",
    "#..........#",
    "############"
};

static const uint8_t dither4x4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5}
};

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

static void stop(int sig) { (void)sig; running = 0; }

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [-d] [-s 1-%d] [-h|-?]\n"
            "  -d      run as a daemon\n"
            "  -s N    start on scene N (1-%d)\n"
            "  -h, -?  show this help message\n",
            argv0, SCENE_COUNT, SCENE_COUNT);
}

static int parse_scene_number(const char *arg, int *scene_idx) {
    char *end = NULL;
    long value = strtol(arg, &end, 10);
    if (!arg[0] || (end && *end) || value < 1 || value > SCENE_COUNT) return 0;
    *scene_idx = (int)value - 1;
    return 1;
}

static float reel_duration_seconds(void) {
    float total = 0.0f;
    for (int i = 0; i < SCENE_COUNT; i++) total += scene_durations[i];
    return total;
}

static float scene_start_seconds(int scene_idx) {
    float offset = 0.0f;
    for (int i = 0; i < scene_idx && i < SCENE_COUNT; i++) offset += scene_durations[i];
    return offset;
}

static void resolve_scene_time(float reel_t, int *scene_idx, float *scene_t) {
    float loop = reel_duration_seconds();
    float t = fmodf(reel_t, loop);
    if (t < 0.0f) t += loop;

    for (int i = 0; i < SCENE_COUNT; i++) {
        if (t < scene_durations[i] || i == SCENE_COUNT - 1) {
            *scene_idx = i;
            *scene_t = t;
            return;
        }
        t -= scene_durations[i];
    }

    *scene_idx = SCENE_COUNT - 1;
    *scene_t = scene_durations[SCENE_COUNT - 1];
}

static int daemonize_process(void) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    if (setsid() < 0) return -1;

    pid = fork();
    if (pid < 0) return -1;
    if (pid > 0) _exit(0);

    if (chdir("/") != 0) return -1;

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    if (open("/dev/null", O_RDWR) < 0) return -1;
    if (dup(STDIN_FILENO) < 0) return -1;
    if (dup(STDIN_FILENO) < 0) return -1;
    return 0;
}

static void init_display(void) {
    uint8_t cmds[] = {
        0x00, 0xAE, 0xD5, 0xF0, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0x6F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
    };
    ssize_t written = write(i2c_fd, cmds, sizeof(cmds));
    (void)written;
}

static void flush_pages(int start_page, int end_page) {
    uint8_t ac[] = {0x00, 0x21, 0, WIDTH - 1, 0x22, (uint8_t)start_page, (uint8_t)end_page};
    ssize_t written = write(i2c_fd, ac, sizeof(ac));
    (void)written;
    int offset = start_page * WIDTH;
    int total = (end_page - start_page + 1) * WIDTH;
    for (int i = 0; i < total; i += I2C_CHUNK) {
        uint8_t buf[I2C_CHUNK + 1];
        buf[0] = 0x40;
        int len = total - i;
        if (len > I2C_CHUNK) len = I2C_CHUNK;
        memcpy(buf + 1, fb + offset + i, len);
        written = write(i2c_fd, buf, len + 1);
        (void)written;
    }
}

static void flush(void) {
    flush_pages(0, PAGES - 1);
}

static void sleep_until(const struct timespec *next) {
#if defined(__APPLE__) || !defined(TIMER_ABSTIME)
    struct timespec now, delta;
    clock_gettime(CLOCK_MONOTONIC, &now);
    delta.tv_sec = next->tv_sec - now.tv_sec;
    delta.tv_nsec = next->tv_nsec - now.tv_nsec;
    if (delta.tv_nsec < 0) {
        delta.tv_nsec += 1000000000L;
        delta.tv_sec -= 1;
    }
    if (delta.tv_sec > 0 || (delta.tv_sec == 0 && delta.tv_nsec > 0))
        nanosleep(&delta, NULL);
#else
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL);
#endif
}

static void pdm_release_asset(void) {
    free(pdm_asset_raw);
    pdm_asset_raw = NULL;
    pdm_frame_data = NULL;
    pdm_phase_count = PDM_PHASES;
    pdm_frame_count = 0;
    pdm_subframe_hz = VIDEO_FPS_DEFAULT;
    pdm_encoding = PDM_ENCODING_RAW;
}

static void pdm_decode_phase_xor(uint8_t *payload, uint32_t phase_count, uint32_t frame_count) {
    size_t phase_span = (size_t)frame_count * (size_t)PDM_BLUE_BYTES;
    for (uint32_t phase = 0; phase < phase_count; phase++) {
        uint8_t *phase_base = payload + (size_t)phase * phase_span;
        for (uint32_t frame = 1; frame < frame_count; frame++) {
            uint8_t *prev = phase_base + (size_t)(frame - 1) * (size_t)PDM_BLUE_BYTES;
            uint8_t *curr = phase_base + (size_t)frame * (size_t)PDM_BLUE_BYTES;
            for (size_t i = 0; i < (size_t)PDM_BLUE_BYTES; i++) curr[i] ^= prev[i];
        }
    }
}

#if HAVE_WOZ_PDM_HEADER
static int pdm_prepare_embedded_asset(void) {
    if (woz_pdm_z_len == 0 || woz_pdm_raw_len < sizeof(pdm_header_t)) return 0;

    pdm_asset_raw = malloc(woz_pdm_raw_len);
    if (!pdm_asset_raw) return 0;

    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    zs.next_in = (Bytef *)woz_pdm_z;
    zs.avail_in = woz_pdm_z_len;
    zs.next_out = pdm_asset_raw;
    zs.avail_out = woz_pdm_raw_len;

    if (inflateInit(&zs) != Z_OK) {
        pdm_release_asset();
        return 0;
    }

    int rc = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (rc != Z_STREAM_END || zs.total_out != woz_pdm_raw_len) {
        pdm_release_asset();
        return 0;
    }

    pdm_header_t header;
    memcpy(&header, pdm_asset_raw, sizeof(header));
    if (header.magic != PDM_MAGIC || header.width != WIDTH || header.height != BLUE_H ||
        header.phase_count == 0 || header.phase_count > PDM_PHASES_MAX || header.frame_count == 0 ||
        header.subframe_hz == 0 ||
        (header.encoding != PDM_ENCODING_RAW && header.encoding != PDM_ENCODING_PHASE_XOR)) {
        pdm_release_asset();
        return 0;
    }

    size_t expected = sizeof(pdm_header_t) +
                      (size_t)header.frame_count * (size_t)header.phase_count * (size_t)PDM_BLUE_BYTES;
    if (expected != woz_pdm_raw_len) {
        pdm_release_asset();
        return 0;
    }

    pdm_phase_count = header.phase_count;
    pdm_frame_count = header.frame_count;
    pdm_subframe_hz = header.subframe_hz;
    pdm_encoding = header.encoding;
    pdm_frame_data = pdm_asset_raw + sizeof(pdm_header_t);
    if (pdm_encoding == PDM_ENCODING_PHASE_XOR)
        pdm_decode_phase_xor(pdm_asset_raw + sizeof(pdm_header_t), pdm_phase_count, pdm_frame_count);
    return 1;
}

static void pdm_draw_video(float scene_t) {
    unsigned tick = (unsigned)floorf(scene_t * pdm_subframe_hz);
    unsigned phase = tick % pdm_phase_count;
    unsigned frame_idx = tick / pdm_phase_count;
    if (frame_idx >= pdm_frame_count) frame_idx = pdm_frame_count - 1;
    size_t offset = ((size_t)phase * (size_t)pdm_frame_count + (size_t)frame_idx) * (size_t)PDM_BLUE_BYTES;

    memcpy(fb + WIDTH * BLUE_START_PAGE, pdm_frame_data + offset, PDM_BLUE_BYTES);
}
#endif

static void px(int x, int y) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
        fb[x + (y / 8) * WIDTH] |= 1 << (y & 7);
}

static void bpx(int x, int y) {
    px(x, y + BLUE_Y);
}

static void fill_rect(int x0, int y0, int x1, int y1) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            px(x, y);
}

static void bfill_rect(int x0, int y0, int x1, int y1) {
    fill_rect(x0, y0 + BLUE_Y, x1, y1 + BLUE_Y);
}

static void draw_line(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        px(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void bline(int x0, int y0, int x1, int y1) {
    draw_line(x0, y0 + BLUE_Y, x1, y1 + BLUE_Y);
}

static void fill_circle(int cx, int cy, int r) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                px(cx + x, cy + y);
}

static void bfill_circle(int cx, int cy, int r) {
    fill_circle(cx, cy + BLUE_Y, r);
}

static float clampf_local(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float mixf_local(float a, float b, float t) {
    return a + (b - a) * t;
}

static float smoothstep_local(float t) {
    t = clampf_local(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static float bell_local(float t, float center, float width) {
    float x = (t - center) / width;
    return expf(-x * x);
}

static float window_local(float t, float start, float end, float fade) {
    float in = smoothstep_local((t - start) / fade);
    float out = smoothstep_local((t - end) / fade);
    return clampf_local(in - out, 0.0f, 1.0f);
}

static float fractf_local(float v) {
    return v - floorf(v);
}

static float angle_diff_local(float a, float b) {
    float d = a - b;
    while (d > (float)M_PI) d -= 2.0f * (float)M_PI;
    while (d < -(float)M_PI) d += 2.0f * (float)M_PI;
    return d;
}

static float hash01(int n) {
    float s = sinf((float)n * 12.9898f + 78.233f) * 43758.5453f;
    return fractf_local(s);
}

static int imod(int a, int b) {
    int r = a % b;
    return r < 0 ? r + b : r;
}

static void draw_char(int x, int y, char c) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++)
            if (bits & (1 << row))
                px(x + col, y + row);
    }
}

static void draw_str(int x, int y, const char *s) {
    while (*s) {
        draw_char(x, y, *s++);
        x += 6;
    }
}

static void shade_px(int x, int y, int level, unsigned phase) {
    if (level <= 0) return;
    if (level >= 4) {
        bpx(x, y);
        return;
    }
    if (dither4x4[(y + (int)phase) & 3][x & 3] < level * 4)
        bpx(x, y);
}

static int scaled_str_width(const char *s, int scale) {
    int len = (int)strlen(s);
    if (len <= 0 || scale < 1) return 0;
    return len * 6 * scale - scale;
}

static void draw_fx_str(float cx, float cy, const char *s, int scale, float angle,
                        float shear, float wave_amp, float wave_freq, float wave_phase,
                        float slice_px, int level, unsigned phase) {
    if (!s || !*s || scale < 1) return;

    int width = scaled_str_width(s, scale);
    int height = 7 * scale;
    float c = cosf(angle), sn = sinf(angle);

    for (int ci = 0; s[ci]; ci++) {
        char ch = s[ci];
        if (ch < 32 || ch > 126) ch = '?';
        const uint8_t *g = font5x7[ch - 32];

        for (int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < 7; row++) {
                if (!(bits & (1 << row))) continue;

                for (int yy = 0; yy < scale; yy++) {
                    for (int xx = 0; xx < scale; xx++) {
                        float lx = (float)(ci * 6 * scale + col * scale + xx) - width * 0.5f;
                        float ly = (float)(row * scale + yy) - height * 0.5f;
                        float tx = lx + shear * (ly / (float)scale);

                        if (slice_px != 0.0f) {
                            int band = (int)((ly + height * 0.5f) / (2.0f * scale));
                            tx += (band & 1) ? slice_px : -slice_px;
                        }

                        float ty = ly + wave_amp * sinf((lx / (float)scale) * wave_freq + wave_phase);
                        float rx = tx * c - ty * sn;
                        float ry = tx * sn + ty * c;
                        shade_px((int)lroundf(cx + rx), (int)lroundf(cy + ry), level, phase);
                    }
                }
            }
        }
    }
}

#if !HAVE_WOZ_PDM_HEADER
static int pdm_on(int x, int y, int level, unsigned phase) {
    if (level <= 0) return 0;
    if (level >= 63) return 1;
    int base = level >> 2;
    int rem = level & 3;
    int bayer = dither4x4[y & 3][x & 3];
    return bayer < base || (bayer == base && ((int)phase & 3) < rem);
}

static void pdm_px(int x, int y, int level, unsigned phase) {
    if (pdm_on(x, y, level, phase))
        bpx(x, y);
}
#endif

static vec3_t v3(float x, float y, float z) {
    vec3_t v = {x, y, z};
    return v;
}

static vec3_t v3_add(vec3_t a, vec3_t b) {
    return v3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static vec3_t v3_sub(vec3_t a, vec3_t b) {
    return v3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static vec3_t v3_scale(vec3_t a, float s) {
    return v3(a.x * s, a.y * s, a.z * s);
}

static vec3_t v3_mix(vec3_t a, vec3_t b, float t) {
    return v3(mixf_local(a.x, b.x, t),
              mixf_local(a.y, b.y, t),
              mixf_local(a.z, b.z, t));
}

static vec3_t rot_x(vec3_t p, float a) {
    float c = cosf(a), s = sinf(a);
    return v3(p.x, p.y * c - p.z * s, p.y * s + p.z * c);
}

static vec3_t rot_y(vec3_t p, float a) {
    float c = cosf(a), s = sinf(a);
    return v3(p.x * c - p.z * s, p.y, p.x * s + p.z * c);
}

static vec3_t rot_z(vec3_t p, float a) {
    float c = cosf(a), s = sinf(a);
    return v3(p.x * c - p.y * s, p.x * s + p.y * c, p.z);
}

static vec3_t view_point(vec3_t p, vec3_t cam, float yaw, float pitch) {
    p = v3_sub(p, cam);
    p = rot_y(p, -yaw);
    p = rot_x(p, pitch);
    return p;
}

static int project_view(vec3_t p, float scale, int *sx, int *sy) {
    if (p.z <= 0.15f) return 0;
    *sx = WIDTH / 2 + (int)lroundf(scale * p.x / p.z);
    *sy = BLUE_H / 2 - (int)lroundf(scale * p.y / p.z);
    return 1;
}

static int project_world(vec3_t p, vec3_t cam, float yaw, float pitch,
                         float scale, int *sx, int *sy) {
    return project_view(view_point(p, cam, yaw, pitch), scale, sx, sy);
}

static void draw_3d_line(vec3_t a, vec3_t b, vec3_t cam, float yaw, float pitch, float scale) {
    int x0, y0, x1, y1;
    if (!project_world(a, cam, yaw, pitch, scale, &x0, &y0)) return;
    if (!project_world(b, cam, yaw, pitch, scale, &x1, &y1)) return;
    bline(x0, y0, x1, y1);
}

static int hill_surface_visible(int sx, int sy, const int *ybuf) {
    if (sx < 0 || sx >= WIDTH || sy < 0 || sy >= BLUE_H) return 0;
    return sy >= ybuf[sx] - 1;
}

static void draw_hill_tree(vec3_t base, float height, float lean,
                           vec3_t cam, float yaw, float pitch, float scale,
                           const int *ybuf) {
    vec3_t top = v3(base.x + lean, base.y + height, base.z);
    int bx, by, tx, ty;

    if (!project_world(base, cam, yaw, pitch, scale, &bx, &by)) return;
    if (!project_world(top, cam, yaw, pitch, scale, &tx, &ty)) return;
    if (!hill_surface_visible(bx, by, ybuf)) return;

    int crown = abs(by - ty) / 2 + 1;
    if (crown < 1) crown = 1;
    if (crown > 5) crown = 5;

    int trunk_join_y = by - (by - ty) / 5;
    int mid_y = ty + (by - ty) / 2;

    bline(bx, by, tx, ty);
    bline(tx, ty, tx - crown, mid_y);
    bline(tx, ty, tx + crown, mid_y);
    bline(tx - crown, mid_y, bx, trunk_join_y);
    bline(tx + crown, mid_y, bx, trunk_join_y);

    if (crown >= 3) {
        int crown2 = crown - 1;
        int upper_y = ty + (mid_y - ty) / 2;
        bline(tx, upper_y, tx - crown2, upper_y + crown2);
        bline(tx, upper_y, tx + crown2, upper_y + crown2);
    }
}

static void draw_hill_rock(vec3_t base, float size, float skew,
                           vec3_t cam, float yaw, float pitch, float scale,
                           const int *ybuf) {
    vec3_t left = v3(base.x - size * 0.70f, base.y + size * 0.10f, base.z);
    vec3_t right = v3(base.x + size * 0.75f, base.y + size * 0.06f, base.z + skew * 0.20f);
    vec3_t peak = v3(base.x + skew * 0.25f, base.y + size, base.z);
    int bx, by, lx, ly, rx, ry, px, py;

    if (!project_world(base, cam, yaw, pitch, scale, &bx, &by)) return;
    if (!project_world(left, cam, yaw, pitch, scale, &lx, &ly)) return;
    if (!project_world(right, cam, yaw, pitch, scale, &rx, &ry)) return;
    if (!project_world(peak, cam, yaw, pitch, scale, &px, &py)) return;
    if (!hill_surface_visible(bx, by, ybuf)) return;

    bline(lx, ly, px, py);
    bline(px, py, rx, ry);
    bline(rx, ry, bx, by);
    bline(bx, by, lx, ly);
}

static int doom_solid(int x, int y) {
    if (x < 0 || y < 0 || x >= MAP_W || y >= MAP_H) return 1;
    return doom_map[y][x] != '.';
}

static float terrain_height(float x, float z) {
    return 4.2f * sinf(x * 0.13f)
         + 3.6f * cosf(z * 0.10f)
         + 2.2f * sinf((x + z) * 0.05f)
         + 1.7f * cosf((x - z) * 0.07f);
}

static float paper_plane_loop_u(float scene_t) {
    return clampf_local((scene_t - 19.5f) / 6.6f, 0.0f, 1.0f);
}

static vec3_t paper_plane_path(float scene_t) {
    float loop_u = paper_plane_loop_u(scene_t);
    float loop_a = loop_u * 2.0f * (float)M_PI;
    float loop_r = 2.35f;
    float x = 5.8f * sinf(scene_t * 0.21f + 0.2f)
            + 2.4f * sinf(scene_t * 0.63f - 0.4f)
            + 1.8f * bell_local(scene_t, 6.0f, 2.0f)
            - 2.6f * bell_local(scene_t, 12.0f, 2.4f)
            + 2.2f * bell_local(scene_t, 17.0f, 1.8f)
            - 1.5f * bell_local(scene_t, 26.8f, 1.6f);
    float z = 16.0f + scene_t * 2.55f
            + 0.9f * sinf(scene_t * 0.22f)
            + loop_r * sinf(loop_a);
    float altitude = 6.6f
                   + 0.45f * sinf(scene_t * 1.05f)
                   + 0.30f * cosf(scene_t * 0.48f)
                   - 2.2f * bell_local(scene_t, 6.5f, 1.7f)
                   + 3.1f * bell_local(scene_t, 11.4f, 2.3f)
                   - 1.6f * bell_local(scene_t, 16.3f, 1.4f)
                   + 2.4f * bell_local(scene_t, 18.8f, 1.7f)
                   + loop_r * (1.0f - cosf(loop_a))
                   + 0.9f * bell_local(scene_t, 27.4f, 1.5f);
    float y = terrain_height(x, z) + altitude;
    return v3(x, y, z);
}

static void paper_plane_attitude(float scene_t, float *yaw, float *pitch, float *roll) {
    const float dt = 0.12f;
    vec3_t prev = paper_plane_path(scene_t - dt);
    vec3_t curr = paper_plane_path(scene_t);
    vec3_t next = paper_plane_path(scene_t + dt);
    vec3_t vel = v3_sub(next, prev);
    vec3_t prev_vel = v3_sub(curr, prev);
    vec3_t next_vel = v3_sub(next, curr);
    float horiz = sqrtf(vel.x * vel.x + vel.z * vel.z);
    float yaw_prev = atan2f(prev_vel.x, prev_vel.z);
    float yaw_next = atan2f(next_vel.x, next_vel.z);
    float yaw_rate = angle_diff_local(yaw_next, yaw_prev) / (2.0f * dt);
    float loop_bank_relax = sinf(paper_plane_loop_u(scene_t) * (float)M_PI);

    if (horiz < 0.001f) horiz = 0.001f;

    *yaw = atan2f(vel.x, vel.z);
    *pitch = atan2f(vel.y, horiz);
    *roll = clampf_local(-yaw_rate * 0.85f, -0.95f, 0.95f) * (1.0f - 0.75f * loop_bank_relax)
          + 0.10f * sinf(scene_t * 1.7f) * (1.0f - loop_bank_relax);
}

static vec3_t tank_tf(vec3_t p, vec3_t pos, float yaw) {
    return v3_add(rot_y(p, yaw), pos);
}

static vec3_t tank_path(int tank_idx, float scene_t) {
    float drive = scene_t * 0.86f;

    if (tank_idx == 0) {
        return v3(8.8f * sinf(drive * 1.18f) + 2.4f * sinf(drive * 2.40f),
                  0.0f,
                  16.0f + 6.4f * cosf(drive * 0.92f + 0.4f) + 1.4f * cosf(drive * 1.90f));
    }

    vec3_t anchor = tank_path(0, scene_t);
    return v3(anchor.x + 4.8f * sinf(drive * 1.42f + 1.1f) + 1.4f * cosf(drive * 2.10f),
              0.0f,
              anchor.z + 8.0f + 2.2f * cosf(drive * 1.66f + 0.2f));
}

static float tank_path_yaw(int tank_idx, float scene_t) {
    vec3_t p0 = tank_path(tank_idx, scene_t);
    vec3_t p1 = tank_path(tank_idx, scene_t + 0.12f);
    return atan2f(p1.x - p0.x, p1.z - p0.z);
}

static void draw_wire_burst(vec3_t pos, float burst_t,
                            vec3_t cam, float yaw, float pitch, float scale) {
    static const vec3_t rays[] = {
        { 1.0f, 0.3f,  0.0f}, {-1.0f, 0.4f,  0.1f},
        { 0.0f, 1.1f,  0.0f}, { 0.2f, 0.2f,  1.0f},
        {-0.2f, 0.3f, -1.0f}, { 0.8f, 0.6f,  0.7f},
        {-0.7f, 0.5f,  0.8f}, { 0.7f, 0.5f, -0.8f},
        {-0.8f, 0.4f, -0.7f}
    };
    vec3_t center = v3_add(pos, v3(0.0f, 1.0f + burst_t * 0.8f, 0.0f));
    float reach = 0.8f + burst_t * 4.8f;
    float ring_r = 0.6f + burst_t * 2.2f;

    for (unsigned i = 0; i < sizeof(rays) / sizeof(rays[0]); i++)
        draw_3d_line(center, v3_add(center, v3_scale(rays[i], reach)), cam, yaw, pitch, scale);

    for (int i = 0; i < 6; i++) {
        float a0 = (float)i * 2.0f * (float)M_PI / 6.0f;
        float a1 = (float)(i + 1) * 2.0f * (float)M_PI / 6.0f;
        vec3_t p0 = v3_add(center, v3(cosf(a0) * ring_r, sinf(a0 * 2.0f) * 0.3f, sinf(a0) * ring_r));
        vec3_t p1 = v3_add(center, v3(cosf(a1) * ring_r, sinf(a1 * 2.0f) * 0.3f, sinf(a1) * ring_r));
        draw_3d_line(p0, p1, cam, yaw, pitch, scale);
    }
}

static void draw_wire_tank(vec3_t pos, float body_yaw, float turret_yaw,
                           vec3_t cam, float yaw, float pitch, float scale) {
    static const vec3_t body[] = {
        {-1.8f, 0.1f, -2.4f}, { 1.8f, 0.1f, -2.4f},
        { 1.8f, 0.1f,  2.4f}, {-1.8f, 0.1f,  2.4f},
        {-1.8f, 1.0f, -2.4f}, { 1.8f, 1.0f, -2.4f},
        { 1.8f, 1.0f,  2.4f}, {-1.8f, 1.0f,  2.4f}
    };
    static const int edges[][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
    };
    static const vec3_t turret[] = {
        {-0.9f, 1.0f, -0.9f}, { 0.9f, 1.0f, -0.9f},
        { 0.9f, 1.55f, 0.8f}, {-0.9f, 1.55f, 0.8f}
    };

    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        draw_3d_line(tank_tf(body[edges[i][0]], pos, body_yaw),
                     tank_tf(body[edges[i][1]], pos, body_yaw),
                     cam, yaw, pitch, scale);
    }

    draw_3d_line(tank_tf(turret[0], pos, turret_yaw), tank_tf(turret[1], pos, turret_yaw), cam, yaw, pitch, scale);
    draw_3d_line(tank_tf(turret[1], pos, turret_yaw), tank_tf(turret[2], pos, turret_yaw), cam, yaw, pitch, scale);
    draw_3d_line(tank_tf(turret[2], pos, turret_yaw), tank_tf(turret[3], pos, turret_yaw), cam, yaw, pitch, scale);
    draw_3d_line(tank_tf(turret[3], pos, turret_yaw), tank_tf(turret[0], pos, turret_yaw), cam, yaw, pitch, scale);
    draw_3d_line(tank_tf(v3(0.0f, 1.3f, 0.8f), pos, turret_yaw),
                 tank_tf(v3(0.0f, 1.3f, 3.8f), pos, turret_yaw),
                 cam, yaw, pitch, scale);
}

static vec3_t ship_tf(vec3_t p, vec3_t pos, float yaw, float pitch, float roll, float scale) {
    p = v3_scale(p, scale);
    p = rot_z(p, roll);
    p = rot_x(p, pitch);
    p = rot_y(p, yaw);
    return v3_add(p, pos);
}

static void draw_wire_ship(vec3_t pos, float yaw_l, float pitch_l, float roll_l, float scale_l,
                           vec3_t cam, float yaw, float pitch, float scale) {
    vec3_t nose = ship_tf(v3( 0.0f,  0.0f,  2.6f), pos, yaw_l, pitch_l, roll_l, scale_l);
    vec3_t left = ship_tf(v3(-2.2f,  0.0f,  0.2f), pos, yaw_l, pitch_l, roll_l, scale_l);
    vec3_t right= ship_tf(v3( 2.2f,  0.0f,  0.2f), pos, yaw_l, pitch_l, roll_l, scale_l);
    vec3_t tail = ship_tf(v3( 0.0f,  0.0f, -2.0f), pos, yaw_l, pitch_l, roll_l, scale_l);
    vec3_t top  = ship_tf(v3( 0.0f,  0.8f, -0.1f), pos, yaw_l, pitch_l, roll_l, scale_l);
    vec3_t bot  = ship_tf(v3( 0.0f, -0.4f, -0.1f), pos, yaw_l, pitch_l, roll_l, scale_l);

    draw_3d_line(nose, left,  cam, yaw, pitch, scale);
    draw_3d_line(nose, right, cam, yaw, pitch, scale);
    draw_3d_line(left, tail,  cam, yaw, pitch, scale);
    draw_3d_line(right, tail, cam, yaw, pitch, scale);
    draw_3d_line(left, top,   cam, yaw, pitch, scale);
    draw_3d_line(right, top,  cam, yaw, pitch, scale);
    draw_3d_line(top, tail,   cam, yaw, pitch, scale);
    draw_3d_line(bot, tail,   cam, yaw, pitch, scale);
}

static void draw_wire_paper_plane(vec3_t pos, float yaw_l, float pitch_l, float roll_l, float scale_l,
                                  vec3_t cam, float yaw, float pitch, float scale) {
    static const vec3_t verts[] = {
        { 0.000f, -0.536f, -1.900f},
        { 0.103f,  0.076f, -1.900f},
        { 0.000f,  0.076f,  3.791f},
        { 1.642f,  0.076f, -1.900f},
        { 0.205f,  0.000f, -1.900f},
        {-0.103f,  0.076f, -1.900f},
        {-1.642f,  0.076f, -1.900f},
        {-0.205f,  0.000f, -1.900f}
    };
    static const int edges[][2] = {
        {0,1}, {1,2}, {0,2},
        {1,3}, {2,3},
        {3,4}, {2,4},
        {5,0}, {2,5},
        {6,5}, {2,6},
        {2,7}, {6,7}
    };

    for (unsigned i = 0; i < sizeof(edges) / sizeof(edges[0]); i++) {
        draw_3d_line(ship_tf(verts[edges[i][0]], pos, yaw_l, pitch_l, roll_l, scale_l),
                     ship_tf(verts[edges[i][1]], pos, yaw_l, pitch_l, roll_l, scale_l),
                     cam, yaw, pitch, scale);
    }
}

static void draw_scroller(const char *msg, float scene_t, int scene_idx) {
    int msg_w = (int)strlen(msg) * 6;
    int span = msg_w + WIDTH + 24;
    int x = WIDTH - ((int)(scene_t * HEADER_SCROLL_PPS) % span);

    draw_str(x, 4, msg);
    draw_str(x + msg_w + 24, 4, msg);

    for (int i = 0; i < SCENE_COUNT; i++) {
        int px0 = 2 + i * 6;
        if (i == scene_idx) fill_rect(px0, 14, px0 + 3, 15);
        else px(px0, 15);
    }
}

static void draw_scene_metaballs(float scene_t, unsigned phase) {
    float bx[4] = {
        32.0f + 20.0f * sinf(scene_t * 0.80f),
        64.0f + 26.0f * cosf(scene_t * 0.63f),
        95.0f + 18.0f * sinf(scene_t * 1.17f),
        63.0f + 30.0f * cosf(scene_t * 0.31f)
    };
    float by[4] = {
        20.0f + 11.0f * cosf(scene_t * 1.10f),
        14.0f + 16.0f * sinf(scene_t * 0.73f),
        26.0f + 14.0f * cosf(scene_t * 0.87f),
        24.0f + 10.0f * sinf(scene_t * 1.43f)
    };
    float rs[4] = { 190.0f, 170.0f, 150.0f, 120.0f };

    for (int y = 0; y < BLUE_H; y++) {
        for (int x = 0; x < WIDTH; x++) {
            float field = 0.0f;
            for (int i = 0; i < 4; i++) {
                float dx = (float)x - bx[i];
                float dy = (float)y - by[i];
                field += rs[i] / (dx * dx + dy * dy + 10.0f);
            }
            float shade = clampf_local((field - 0.85f) * 1.85f, 0.0f, 1.0f);
            int level = (int)lroundf(shade * 15.0f);
            if (level > dither4x4[y & 3][x & 3]) bpx(x, y);
            if (field > 0.74f && field < 0.82f && ((x + y + (int)phase) & 3) == 0) bpx(x, y);
        }
    }

    for (int i = 0; i < 4; i++)
        bfill_circle((int)lroundf(bx[i]), (int)lroundf(by[i]), 1);
}

static void draw_scene_doom(float scene_t, unsigned phase) {
    float cam_x = 5.8f + 2.6f * sinf(scene_t * 0.22f) + 0.7f * cosf(scene_t * 0.53f);
    float cam_y = 5.8f + 2.1f * cosf(scene_t * 0.17f);
    float cam_a = scene_t * 0.22f + 0.65f * sinf(scene_t * 0.11f);
    float bob = 1.1f * sinf(scene_t * 2.3f);

    for (int col = 0; col < 64; col++) {
        float ray_a = cam_a + (((float)col / 63.0f) - 0.5f) * 1.05f;
        float dist = 0.12f;
        float hx = cam_x, hy = cam_y;
        for (int step = 0; step < 256; step++) {
            hx = cam_x + cosf(ray_a) * dist;
            hy = cam_y + sinf(ray_a) * dist;
            if (doom_solid((int)hx, (int)hy)) break;
            dist += 0.05f;
        }

        float corrected = dist * cosf(ray_a - cam_a);
        if (corrected < 0.12f) corrected = 0.12f;
        int wall_h = (int)lroundf(38.0f / corrected);
        if (wall_h > BLUE_H) wall_h = BLUE_H;
        int top = BLUE_H / 2 - wall_h / 2 + (int)lroundf(bob);
        int bottom = top + wall_h;

        float edge_x = fabsf(fractf_local(hx) - 0.5f);
        float edge_y = fabsf(fractf_local(hy) - 0.5f);
        int level = corrected < 2.0f ? 4 : corrected < 4.0f ? 3 : corrected < 6.0f ? 2 : 1;
        if (edge_x > edge_y && level > 1) level--;

        for (int yy = 0; yy < top; yy++) {
            if (((yy + col + (int)(scene_t * 4.0f)) & 15) == 0) {
                bpx(col * 2, yy);
                bpx(col * 2 + 1, yy);
            }
        }

        for (int yy = top; yy <= bottom; yy++) {
            if (yy >= 0 && yy < BLUE_H) {
                shade_px(col * 2, yy, level, phase);
                shade_px(col * 2 + 1, yy, level, phase);
            }
        }

        for (int yy = bottom < 0 ? 0 : bottom; yy < BLUE_H; yy++) {
            int dist_band = yy - BLUE_H / 2;
            int floor_level = dist_band > 15 ? 3 : dist_band > 7 ? 2 : 1;
            if ((imod(yy + col + (int)phase, floor_level == 3 ? 2 : floor_level == 2 ? 3 : 4)) == 0) {
                bpx(col * 2, yy);
                bpx(col * 2 + 1, yy);
            }
        }
    }

    bline(62, 23, 66, 23);
    bline(64, 21, 64, 25);

    int gun_y = 40 + (int)lroundf(sinf(scene_t * 3.0f) * 1.5f);
    bline(55, BLUE_H - 1, 61, gun_y + 1);
    bline(72, BLUE_H - 1, 66, gun_y + 1);
    bfill_rect(61, gun_y, 66, BLUE_H - 1);
}

static void draw_scene_tank_wars(float scene_t) {
    float duel_t = fmodf(scene_t, 8.0f);
    vec3_t tank_a = tank_path(0, scene_t);
    vec3_t tank_b = tank_path(1, scene_t);
    float a_yaw = tank_path_yaw(0, scene_t);
    float b_yaw = tank_path_yaw(1, scene_t);
    float a_turret_yaw = atan2f(tank_b.x - tank_a.x, tank_b.z - tank_a.z)
                       + 0.05f * sinf(scene_t * 2.6f);
    float b_turret_yaw = atan2f(tank_a.x - tank_b.x, tank_a.z - tank_b.z)
                       + 0.05f * cosf(scene_t * 2.3f);
    float recoil = 0.0f;
    if (duel_t >= 1.10f && duel_t < 1.35f)
        recoil = 1.0f - (duel_t - 1.10f) / 0.25f;

    vec3_t cam = tank_tf(v3(0.0f, 1.45f - recoil * 0.05f, -0.35f - recoil * 0.40f),
                         tank_a, a_turret_yaw);
    vec3_t aim = v3_add(tank_b, v3(0.0f, 1.3f, 0.0f));
    vec3_t aim_delta = v3_sub(aim, cam);
    float yaw = atan2f(aim.x - cam.x, aim.z - cam.z) + 0.03f * sinf(scene_t * 2.0f);
    float pitch = -atan2f(cam.y - aim.y,
                          sqrtf(aim_delta.x * aim_delta.x + aim_delta.z * aim_delta.z))
                + 0.02f * sinf(scene_t * 1.7f);
    float scene_scale = 92.0f;
    vec3_t muzzle_a = tank_tf(v3(0.0f, 1.3f, 3.8f), tank_a, a_turret_yaw);
    vec3_t muzzle_b = tank_tf(v3(0.0f, 1.3f, 3.8f), tank_b, b_turret_yaw);
    vec3_t miss_a = tank_tf(v3(1.6f, 0.4f, 5.5f), tank_a, a_yaw);
    int enemy_visible = duel_t < 2.0f || duel_t >= 3.4f;

    for (int z = 4; z <= 34; z += 4)
        draw_3d_line(v3(-20.0f, 0.0f, (float)z), v3(20.0f, 0.0f, (float)z), cam, yaw, pitch, scene_scale);
    for (int x = -20; x <= 20; x += 4)
        draw_3d_line(v3((float)x, 0.0f, 4.0f), v3((float)x, 0.0f, 34.0f), cam, yaw, pitch, scene_scale);

    if (enemy_visible)
        draw_wire_tank(tank_b, b_yaw, b_turret_yaw, cam, yaw, pitch, scene_scale);

    vec3_t shell;
    int sx, sy;

    if (duel_t >= 1.10f && duel_t < 2.0f) {
        float shot = (duel_t - 1.10f) / 0.90f;
        shell = v3_add(muzzle_a, v3_scale(v3_sub(v3_add(tank_b, v3(0.0f, 1.3f, 0.0f)), muzzle_a), shot));
        if (project_world(shell, cam, yaw, pitch, scene_scale, &sx, &sy)) {
            bfill_circle(sx, sy, 1);
            bline(sx - 2, sy, sx + 2, sy);
        }
    } else if (duel_t >= 2.0f && duel_t < 2.9f) {
        draw_wire_burst(tank_b, (duel_t - 2.0f) / 0.90f, cam, yaw, pitch, scene_scale);
        if (project_world(v3_add(tank_b, v3(0.0f, 1.1f, 0.0f)), cam, yaw, pitch, scene_scale, &sx, &sy)) {
            for (int i = 0; i < 6; i++) {
                float a = (float)i * (float)M_PI / 3.0f;
                bline(sx, sy, sx + (int)lroundf(cosf(a) * 4.0f), sy + (int)lroundf(sinf(a) * 4.0f));
            }
        }
    } else if (duel_t >= 2.9f && duel_t < 3.4f) {
        draw_wire_burst(tank_b, (3.4f - duel_t) / 1.0f, cam, yaw, pitch, scene_scale);
    }

    if (duel_t >= 4.2f && duel_t < 5.0f) {
        float shot = (duel_t - 4.2f) / 0.8f;
        shell = v3_add(muzzle_b, v3_scale(v3_sub(miss_a, muzzle_b), shot));
        if (project_world(shell, cam, yaw, pitch, scene_scale, &sx, &sy)) {
            bfill_circle(sx, sy, 1);
            bline(sx, sy - 2, sx, sy + 2);
        }
    } else if (duel_t >= 5.0f && duel_t < 5.5f) {
        if (project_world(miss_a, cam, yaw, pitch, scene_scale, &sx, &sy)) {
            for (int i = 0; i < 6; i++) {
                float a = (float)i * (float)M_PI / 3.0f;
                bline(sx, sy, sx + (int)lroundf(cosf(a) * 4.0f), sy + (int)lroundf(sinf(a) * 4.0f));
            }
        }
    }

    int gun_y = 34 + (int)lroundf(sinf(scene_t * 2.1f) * 1.0f);
    bline(56, BLUE_H - 1, 62, gun_y + 1);
    bline(72, BLUE_H - 1, 66, gun_y + 1);
    bfill_rect(62, gun_y, 66, BLUE_H - 1);
    bline(61, 23, 67, 23);
    bline(64, 20, 64, 26);
}

static void draw_scene_voxel_plane(float scene_t, unsigned phase) {
    int ybuf[WIDTH];
    vec3_t plane = paper_plane_path(scene_t);
    float plane_yaw, plane_pitch, plane_roll;
    paper_plane_attitude(scene_t, &plane_yaw, &plane_pitch, &plane_roll);
     float flyby_w = window_local(scene_t, 7.8f, 13.8f, 1.6f);
     float skim_w = window_local(scene_t, 25.0f, 29.2f, 1.0f);
    vec3_t chase_anchor = paper_plane_path(scene_t - 0.95f);
    float chase_yaw, chase_pitch, unused_roll;
    paper_plane_attitude(scene_t - 0.35f, &chase_yaw, &chase_pitch, &unused_roll);
    vec3_t lookahead = paper_plane_path(scene_t + 1.35f);
     vec3_t chase_cam = ship_tf(v3(2.5f * sinf(scene_t * 0.24f + 0.7f),
                                             2.2f + 0.35f * cosf(scene_t * 0.41f),
                                             -11.5f),
                                         chase_anchor, chase_yaw, chase_pitch * 0.35f, 0.0f, 1.0f);
     vec3_t chase_focus = v3_add(v3_mix(plane, lookahead, 0.55f), v3(0.0f, -1.0f, 0.0f));

     vec3_t fly_anchor = paper_plane_path(scene_t + 0.65f);
    float fly_yaw, fly_pitch, unused_fly_roll;
    paper_plane_attitude(scene_t + 0.65f, &fly_yaw, &fly_pitch, &unused_fly_roll);
     vec3_t fly_cam = ship_tf(v3(8.6f,
                                          1.4f + 0.30f * cosf(scene_t * 0.52f),
                                          -2.0f),
                                      fly_anchor, fly_yaw, fly_pitch * 0.12f, 0.0f, 1.0f);
     vec3_t fly_focus = v3_add(v3_mix(plane, paper_plane_path(scene_t + 0.35f), 0.20f),
                                        v3(0.0f, -0.9f, 0.0f));

     vec3_t skim_anchor = paper_plane_path(scene_t - 0.18f);
    float skim_yaw, skim_pitch, unused_skim_roll;
    paper_plane_attitude(scene_t + 0.10f, &skim_yaw, &skim_pitch, &unused_skim_roll);
     vec3_t skim_cam = ship_tf(v3(-4.5f,
                                            -0.25f + 0.15f * sinf(scene_t * 0.80f),
                                            -6.8f),
                                        skim_anchor, skim_yaw, skim_pitch * 0.16f, 0.0f, 1.0f);
     float skim_floor = terrain_height(skim_cam.x, skim_cam.z) + 0.95f;
     if (skim_cam.y < skim_floor) skim_cam.y = skim_floor;
     vec3_t skim_focus = v3_add(v3_mix(plane, paper_plane_path(scene_t + 1.65f), 0.68f),
                                         v3(0.0f, -1.8f, 0.0f));

     vec3_t cam = v3_mix(chase_cam, fly_cam, flyby_w);
     vec3_t focus = v3_mix(chase_focus, fly_focus, flyby_w);
     cam = v3_mix(cam, skim_cam, skim_w);
     focus = v3_mix(focus, skim_focus, skim_w);
    vec3_t focus_delta = v3_sub(focus, cam);
    float yaw = -atan2f(focus_delta.x, focus_delta.z);
    float pitch = atan2f(focus_delta.y,
                         sqrtf(focus_delta.x * focus_delta.x + focus_delta.z * focus_delta.z));
     float scene_scale = mixf_local(79.0f, 86.0f, clampf_local(0.35f * flyby_w + 0.80f * skim_w, 0.0f, 1.0f));
    float cam_h = cam.y;
    float horizon_y = 14.0f + pitch * 22.0f;

    for (int x = 0; x < WIDTH; x++) ybuf[x] = BLUE_H - 1;

    for (float depth = 58.0f; depth > 1.0f; depth -= 0.45f) {
        int level = depth < 7.0f ? 4 : depth < 15.0f ? 3 : depth < 28.0f ? 2 : 1;
        for (int x = 0; x < WIDTH; x++) {
            float ray = yaw + (((float)x - WIDTH * 0.5f) / 84.0f);
            float wx = cam.x + sinf(ray) * depth;
            float wz = cam.z + cosf(ray) * depth;
            float h = terrain_height(wx, wz);
            int sy = (int)lroundf(horizon_y + (cam_h - h) * 24.0f / depth);
            int tex_seed = imod((int)floorf(wx * 0.45f) + (int)floorf(wz * 0.35f), 6);
            int contour = fabsf(fractf_local((h + depth * 0.18f) * 0.35f) - 0.5f) < 0.10f;
            if (sy < 0) sy = 0;
            if (sy < ybuf[x]) {
                for (int y = sy; y <= ybuf[x]; y++) {
                    int tex_level = level;
                    if (((y + tex_seed) & 1) == 0 && tex_level > 1) tex_level--;
                    if (depth > 16.0f && imod(y + x + tex_seed, 3) == 0 && tex_level > 1) tex_level--;
                    if (contour && y <= sy + 1) tex_level = 4;
                    shade_px(x, y, tex_level, phase);
                }
                ybuf[x] = sy;
            }
        }
    }

    for (int x = 1; x < WIDTH; x++) {
        if (abs(ybuf[x] - ybuf[x - 1]) < 7)
            bline(x - 1, ybuf[x - 1], x, ybuf[x]);
    }

    {
        int z_near = (int)floorf((cam.z + 6.0f) / 4.2f);
        int z_far = (int)floorf((cam.z + 58.0f) / 4.2f);

        for (int iz = z_far; iz >= z_near; iz--) {
            float row_dist = (float)iz * 4.2f - cam.z;
            float near_t = 1.0f - clampf_local((row_dist - 8.0f) / 40.0f, 0.0f, 1.0f);
            int x_span = near_t > 0.70f ? 8 : near_t > 0.35f ? 7 : 6;
            int variants = near_t > 0.72f ? 3 : near_t > 0.34f ? 2 : 1;
            float presence_cut = mixf_local(0.90f, 0.44f, near_t);

            for (int ix = -x_span; ix <= x_span; ix++) {
                for (int variant = 0; variant < variants; variant++) {
                    int seed = ix * 92821 + iz * 68917 + variant * 1237;
                    float presence = hash01(seed + 5);
                    float kind = hash01(seed + 17);
                    float variant_center = variants > 1 ? (float)variant / (float)(variants - 1) - 0.5f : 0.0f;
                    float wz, wx, ground, scale_boost;
                    vec3_t base;

                    if (presence < presence_cut) continue;

                    wz = iz * 4.2f
                       + (hash01(seed + 31) - 0.5f) * 1.5f
                       + variant_center * (0.55f + 0.65f * near_t);
                    wx = ix * 4.8f
                       + (hash01(seed + 47) - 0.5f) * (2.6f + near_t * 1.3f)
                       + 1.0f * sinf((float)iz * 0.7f + (float)ix)
                       + variant_center * (0.90f + 0.80f * near_t);

                    if (fabsf(wx - plane.x) < 3.0f && fabsf(wz - plane.z) < 7.0f) continue;

                    ground = terrain_height(wx, wz);
                    base = v3(wx, ground + 0.05f, wz);
                    scale_boost = 0.90f + near_t * 0.55f;

                    if (kind > mixf_local(0.66f, 0.48f, near_t) && ground > -6.0f) {
                        float height = (0.75f + 1.45f * hash01(seed + 59)) * scale_boost;
                        float lean = (hash01(seed + 71) - 0.5f) * (0.25f + 0.20f * near_t);
                        draw_hill_tree(base, height, lean, cam, yaw, pitch, scene_scale, ybuf);
                    } else {
                        float size = (0.26f + 0.62f * hash01(seed + 83)) * scale_boost;
                        float skew = hash01(seed + 97) - 0.5f;
                        draw_hill_rock(base, size, skew, cam, yaw, pitch, scene_scale, ybuf);
                    }
                }
            }
        }
    }

    for (int i = 0; i < 12; i++) {
        int cell = (int)floorf(cam.z * 0.65f) + i;
        float wz = cell * 1.75f + hash01(cell * 19 + 3) * 1.1f;
        float wx = (hash01(cell * 29 + 11) - 0.5f) * 30.0f + 3.8f * sinf(cell * 0.31f);
        float base_h = terrain_height(wx, wz) + 0.05f;
        float rush_h = 0.45f + 1.10f * hash01(cell * 41 + 17);
        vec3_t base = v3(wx, base_h, wz);
        vec3_t tip = v3(wx + 0.15f * sinf((float)cell),
                        base_h + rush_h,
                        wz + 0.15f * cosf((float)cell));
        int sx0, sy0, sx1, sy1;

        if (!project_world(base, cam, yaw, pitch, scene_scale, &sx0, &sy0)) continue;
        if (!project_world(tip, cam, yaw, pitch, scene_scale, &sx1, &sy1)) continue;
        if (sx0 < 1 || sx0 >= WIDTH - 1 || sy0 < BLUE_H / 2 || sy0 >= BLUE_H) continue;
        if (fabsf(wx - plane.x) < 4.0f && fabsf(wz - plane.z) < 6.0f) continue;
        if (sy0 < ybuf[sx0] - 1) continue;

        bline(sx0, sy0, sx1, sy1);
        if (sy1 > 0 && sy1 < BLUE_H) bpx(sx1, sy1);
    }

    for (int i = 0; i < 6; i++) {
        int cx = imod((int)(scene_t * 6.0f) + i * 23, WIDTH + 18) - 9;
        int cy = 5 + (int)lroundf(sinf(scene_t * 0.35f + i * 0.9f) * 2.0f);
        bline(cx, cy, cx + 6, cy);
    }

    {
        float altitude = plane.y - terrain_height(plane.x, plane.z);
        float shadow_len = clampf_local(1.3f + altitude * 0.55f, 1.8f, 5.8f);
        float shadow_w = clampf_local(0.9f + altitude * 0.20f, 1.0f, 3.0f);
        vec3_t shadow = v3(plane.x, terrain_height(plane.x, plane.z) + 0.10f, plane.z);
        vec3_t shadow_nose = v3(shadow.x + sinf(plane_yaw) * shadow_len,
                                terrain_height(shadow.x + sinf(plane_yaw) * shadow_len,
                                               shadow.z + cosf(plane_yaw) * shadow_len) + 0.10f,
                                shadow.z + cosf(plane_yaw) * shadow_len);
        vec3_t shadow_tail = v3(shadow.x - sinf(plane_yaw) * shadow_len * 0.55f,
                                terrain_height(shadow.x - sinf(plane_yaw) * shadow_len * 0.55f,
                                               shadow.z - cosf(plane_yaw) * shadow_len * 0.55f) + 0.10f,
                                shadow.z - cosf(plane_yaw) * shadow_len * 0.55f);
        vec3_t shadow_left = v3(shadow.x - cosf(plane_yaw) * shadow_w,
                                terrain_height(shadow.x - cosf(plane_yaw) * shadow_w,
                                               shadow.z + sinf(plane_yaw) * shadow_w) + 0.10f,
                                shadow.z + sinf(plane_yaw) * shadow_w);
        vec3_t shadow_right = v3(shadow.x + cosf(plane_yaw) * shadow_w,
                                 terrain_height(shadow.x + cosf(plane_yaw) * shadow_w,
                                                shadow.z - sinf(plane_yaw) * shadow_w) + 0.10f,
                                 shadow.z - sinf(plane_yaw) * shadow_w);

        draw_3d_line(shadow_tail, shadow_nose, cam, yaw, pitch, scene_scale);
        draw_3d_line(shadow_left, shadow_right, cam, yaw, pitch, scene_scale);
    }

    draw_wire_paper_plane(plane, plane_yaw, plane_pitch, plane_roll, 1.45f,
                          cam, yaw, pitch, scene_scale);
}

static void draw_scene_space_battle(float scene_t) {
    float travel = scene_t * 3.4f;
    vec3_t cam = v3(2.0f * sinf(scene_t * 0.25f), 1.1f + 0.4f * sinf(scene_t * 0.4f), travel);
    vec3_t focus = v3(0.0f, 0.5f, travel + 16.0f);
    float yaw = atan2f(focus.x - cam.x, focus.z - cam.z);
    float pitch = 0.03f * sinf(scene_t * 0.5f);

    for (int i = 0; i < 96; i++) {
        float z = cam.z + 2.0f + fractf_local(hash01(i * 19 + 7) + scene_t * 0.3f) * 42.0f;
        float x = (hash01(i * 31 + 3) - 0.5f) * 34.0f;
        float y = (hash01(i * 47 + 11) - 0.5f) * 12.0f;
        int sx, sy;
        if (project_world(v3(x, y, z), cam, yaw, pitch, PROJ_SCALE, &sx, &sy)) bpx(sx, sy);
    }

    vec3_t ally = v3(-1.8f + 1.2f * sinf(scene_t * 0.9f), 0.3f * sinf(scene_t * 1.1f), travel + 8.0f);
    vec3_t enemy0 = v3(-5.0f + 2.0f * sinf(scene_t * 1.2f), 1.3f * sinf(scene_t * 1.4f), travel + 20.0f);
    vec3_t enemy1 = v3( 4.2f + 1.8f * cosf(scene_t * 1.0f),-0.9f + 1.1f * cosf(scene_t * 1.5f), travel + 24.0f);
    vec3_t enemy2 = v3( 0.0f + 4.6f * sinf(scene_t * 0.7f), 1.5f * sinf(scene_t * 0.8f), travel + 30.0f);

    draw_wire_ship(ally, 0.08f * sinf(scene_t * 1.7f), 0.05f * sinf(scene_t * 1.2f), 0.12f * sinf(scene_t * 2.0f),
                   0.95f, cam, yaw, pitch, PROJ_SCALE);
    draw_wire_ship(enemy0, (float)M_PI + 0.10f * cosf(scene_t * 1.3f), 0.03f * sinf(scene_t * 1.1f), -0.10f, 1.0f,
                   cam, yaw, pitch, PROJ_SCALE);
    draw_wire_ship(enemy1, (float)M_PI - 0.08f * sinf(scene_t * 1.5f),-0.06f * cosf(scene_t * 1.1f),  0.08f, 0.9f,
                   cam, yaw, pitch, PROJ_SCALE);
    draw_wire_ship(enemy2, (float)M_PI + 0.06f * cosf(scene_t * 0.9f), 0.04f * sinf(scene_t * 1.3f), -0.06f, 0.85f,
                   cam, yaw, pitch, PROJ_SCALE);

    float burst = fmodf(scene_t, 7.0f);
    vec3_t laser_a, laser_b;
    if (burst < 2.0f) {
        laser_a = v3_add(ally, v3(0.0f, 0.0f, 1.6f));
        laser_b = v3_add(enemy0, v3_scale(v3_sub(v3_add(enemy0, v3(0.0f, 0.0f, 0.0f)), laser_a), burst / 2.0f));
        draw_3d_line(laser_a, laser_b, cam, yaw, pitch, PROJ_SCALE);
    } else if (burst < 2.8f) {
        int sx, sy;
        if (project_world(enemy0, cam, yaw, pitch, PROJ_SCALE, &sx, &sy)) {
            for (int i = 0; i < 8; i++) {
                float a = (float)i * 2.0f * (float)M_PI / 8.0f;
                bline(sx, sy, sx + (int)lroundf(cosf(a) * 5.0f), sy + (int)lroundf(sinf(a) * 5.0f));
            }
        }
    } else if (burst < 5.2f) {
        laser_a = v3_add(enemy1, v3(0.0f, 0.0f, -1.4f));
        laser_b = v3_add(ally, v3_scale(v3_sub(ally, laser_a), (burst - 2.8f) / 2.4f));
        draw_3d_line(laser_a, laser_b, cam, yaw, pitch, PROJ_SCALE);
    } else if (burst < 6.0f) {
        int sx, sy;
        if (project_world(ally, cam, yaw, pitch, PROJ_SCALE, &sx, &sy)) {
            bline(sx - 4, sy, sx + 4, sy);
            bline(sx, sy - 4, sx, sy + 4);
        }
    }
}

static void draw_scene_mandelbrot(float scene_t) {
    float u = clampf_local(scene_t / SCENE_SECONDS, 0.0f, 1.0f);
    float s = smoothstep_local(u);
    double cr0 = -0.55, ci0 = 0.0;
    double cr1 = -0.743643887037151, ci1 = 0.131825904205330;
    double center_r = cr0 + (cr1 - cr0) * (double)s;
    double center_i = ci0 + (ci1 - ci0) * (double)s;
    double scale = 2.6 * exp(-6.2 * (double)s);
    int max_iter = 28 + (int)lroundf(28.0f * s);

    for (int y = 0; y < BLUE_H; y++) {
        for (int x = 0; x < WIDTH; x++) {
            double pr = center_r + (((double)x - WIDTH * 0.5) / (double)BLUE_H) * scale * 2.0;
            double pi = center_i + (((double)y - BLUE_H * 0.5) / (double)BLUE_H) * scale;
            double zr = 0.0, zi = 0.0;
            int i = 0;
            while (zr * zr + zi * zi < 4.0 && i < max_iter) {
                double nzr = zr * zr - zi * zi + pr;
                zi = 2.0 * zr * zi + pi;
                zr = nzr;
                i++;
            }

            if (i < max_iter) {
                float shade = 0.18f + 0.82f * ((float)i / (float)max_iter);
                if ((int)lroundf(shade * 15.0f) > dither4x4[y & 3][x & 3])
                    bpx(x, y);
            }
        }
    }
}

static void draw_scene_ascii_nyan(float scene_t) {
    static const char *cat_frames[2][4] = {
        {
            "_,------,",
            "_|  /\\_/\\",
            "~|_( ^ .^)",
            " \"\"  \"\" "
        },
        {
            "_,------,",
            "_|   /\\_/\\",
            "^|__( ^ .^)",
            "  \"\"  \"\""
        }
    };
    static const char trail_row0[] = "=+-";
    static const char trail_row1[] = "-~=";
    static const char trail_row2[] = "~=-";
    int frame = ((int)floorf(scene_t * 4.0f)) & 1;
    int cat_x = WIDTH - ((int)(scene_t * 10.0f) % 210);
    int cat_y = 8 + (int)lroundf(sinf(scene_t * 3.0f) * 2.0f);

    for (int i = 0; i < 22; i++) {
        int sx = WIDTH - imod((int)(scene_t * 13.0f) + i * 17, WIDTH + 18);
        int sy = 2 + imod(i * 7 + (i & 1 ? 3 : 0), 40);
        draw_char(sx, sy + BLUE_Y, (i % 3) == 0 ? '*' : (i % 3) == 1 ? '+' : '.');
    }

    for (int tx = 0; tx < cat_x - 6; tx += 6) {
        draw_char(tx + (((tx / 6) + frame) & 1 ? 0 : 3), BLUE_Y + cat_y + 4,  trail_row0[(tx / 6 + frame) % 3]);
        draw_char(tx + (((tx / 6) + frame) & 1 ? 3 : 0), BLUE_Y + cat_y + 12, trail_row1[(tx / 6 + frame) % 3]);
        draw_char(tx + (((tx / 6) + frame) & 1 ? 0 : 3), BLUE_Y + cat_y + 20, trail_row2[(tx / 6 + frame) % 3]);
    }

    draw_str(cat_x, BLUE_Y + cat_y + 0,  cat_frames[frame][0]);
    draw_str(cat_x, BLUE_Y + cat_y + 8,  cat_frames[frame][1]);
    draw_str(cat_x, BLUE_Y + cat_y + 16, cat_frames[frame][2]);
    draw_str(cat_x, BLUE_Y + cat_y + 24, cat_frames[frame][3]);
}

static void draw_scene_mask_type(float scene_t, unsigned phase) {
    float seg_span = SCENE_SECONDS / 4.0f;
    float seg_f = scene_t / seg_span;
    int seg = ((int)seg_f) & 3;
    float u = fractf_local(seg_f);
    float s = smoothstep_local(u);

    bline(6, 4, 20, 4);
    bline(6, 4, 6, 12);
    bline(121, 4, 107, 4);
    bline(121, 4, 121, 12);
    bline(6, 43, 20, 43);
    bline(6, 43, 6, 35);
    bline(121, 43, 107, 43);
    bline(121, 43, 121, 35);

    switch (seg) {
        case 0: {
            float split = mixf_local(16.0f, 0.0f, s);
            float jitter = u < 0.20f ? ((((int)(scene_t * 18.0f)) & 1) ? 2.0f : -2.0f) : 0.0f;

            draw_fx_str(64.0f + jitter, 20.0f, "CUT", 4, 0.0f, 0.0f,
                        0.0f, 0.0f, 0.0f, split, 4, phase);
            bline(26, 24, 102, 24);
            break;
        }

        case 1: {
            float push_s = smoothstep_local(clampf_local(u * 1.55f, 0.0f, 1.0f));
            int scale = push_s < 0.25f ? 2 : push_s < 0.70f ? 3 : 4;
            float push_x = mixf_local(36.0f, 64.0f, push_s);
            int rail = (int)lroundf(mixf_local(60.0f, 18.0f, push_s));

            draw_fx_str(push_x - 14.0f * (1.0f - push_s), 21.0f, "PUSH", scale,
                        0.05f * sinf(scene_t * 2.2f), 0.10f,
                        0.0f, 0.0f, 0.0f,
                        0.0f, 4, phase);
            if (push_s < 0.78f)
                draw_fx_str(push_x - 18.0f * (1.0f - push_s), 21.0f, "PUSH", scale > 1 ? scale - 1 : 1,
                            0.0f, 0.0f,
                            0.0f, 0.0f, 0.0f,
                            0.0f, 4, phase);
            bline(rail, 12, rail, 34);
            bline(WIDTH - rail, 12, WIDTH - rail, 34);
            break;
        }

        case 2: {
            float slide_s = smoothstep_local(clampf_local(u * 1.55f, 0.0f, 1.0f));
            float left_x = mixf_local(20.0f, 50.0f, slide_s);
            float right_x = mixf_local(108.0f, 78.0f, slide_s);
            float sway = 1.4f * sinf(scene_t * 5.0f);

            draw_fx_str(left_x, 17.0f + sway, "SLIDE", 3, 0.0f,
                        0.14f * sinf(scene_t * 2.0f), 0.0f, 0.0f, 0.0f,
                        0.0f, 4, phase);
            draw_fx_str(right_x, 29.0f - sway, "SLIDE", 2, 0.0f,
                        -0.10f * cosf(scene_t * 2.2f), 0.0f, 0.0f, 0.0f,
                        0.0f, 4, phase);
            break;
        }

        default: {
            float ease_x = mixf_local(34.0f, 96.0f, 0.5f + 0.5f * sinf(scene_t * 0.55f));
            float ease_y = 22.0f + 4.0f * sinf(scene_t * 0.9f);
            float angle = 0.12f * sinf(scene_t * 0.8f);

            draw_fx_str(ease_x, ease_y, "EASE", 3, angle,
                        0.0f,
                        1.6f, 0.18f, scene_t * 2.8f,
                        0.0f, 4, phase);
            bline(24, 34, 104, 34);
            break;
        }
    }
}

static void draw_scene_moire(float scene_t) {
    int mode = ((int)scene_t / 10) % 3;

    for (int y = 0; y < BLUE_H; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int on = 0;
            if (mode == 0) {
                float dx0 = (float)x - (64.0f + 14.0f * sinf(scene_t * 0.7f));
                float dy0 = (float)y - (24.0f + 9.0f * cosf(scene_t * 0.9f));
                float dx1 = (float)x - (64.0f - 14.0f * cosf(scene_t * 0.5f));
                float dy1 = (float)y - (24.0f - 8.0f * sinf(scene_t * 0.8f));
                int a = (((int)(sqrtf(dx0 * dx0 + dy0 * dy0) * 1.8f + scene_t * 8.0f)) & 7) < 1;
                int b = (((int)(sqrtf(dx1 * dx1 + dy1 * dy1) * 1.8f - scene_t * 6.0f)) & 7) < 1;
                on = a ^ b;
            } else if (mode == 1) {
                float a0 = scene_t * 0.33f;
                float a1 = -scene_t * 0.27f;
                float vx0 = ((float)x - 64.0f) * cosf(a0) + ((float)y - 24.0f) * sinf(a0);
                float vx1 = ((float)x - 64.0f) * cosf(a1) - ((float)y - 24.0f) * sinf(a1);
                int g0 = (((int)fabsf(vx0 * 1.6f)) & 7) < 1;
                int g1 = (((int)fabsf(vx1 * 1.6f)) & 7) < 1;
                on = g0 ^ g1;
            } else {
                float p = sinf((float)x * 0.22f + scene_t * 2.0f)
                        + cosf((float)y * 0.31f - scene_t * 1.4f)
                        + sinf(((float)x + (float)y) * 0.13f + scene_t);
                on = fabsf(p) < 0.16f || fabsf(p - 1.0f) < 0.13f;
            }
            if (on) bpx(x, y);
        }
    }
}

#if !HAVE_WOZ_PDM_HEADER
static void draw_scene_pdm_cinema(float scene_t, unsigned phase) {
    float moon_x = 96.0f + 8.0f * sinf(scene_t * 0.07f);
    float moon_y = 9.0f + 2.0f * cosf(scene_t * 0.09f);
    float road_bend = 12.0f * sinf(scene_t * 0.22f);
    int car_x = WIDTH - imod((int)(scene_t * 7.0f), WIDTH + 30) + 10;

    for (int y = 0; y < BLUE_H; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int level = 0;

            if (y < 24) {
                level = 10 + (24 - y) / 2;
                level += (int)lroundf((sinf((float)x * 0.11f + scene_t * 0.5f)
                                     + cosf((float)y * 0.38f - scene_t * 0.7f)) * 3.0f + 3.0f);
            }

            float mdx = (float)x - moon_x;
            float mdy = (float)y - moon_y;
            float md = sqrtf(mdx * mdx + mdy * mdy);
            if (md < 9.0f) {
                int moon = (int)lroundf(56.0f - md * 4.0f);
                if (moon > level) level = moon;
            }

            int hill0 = 22 + (int)lroundf(sinf((float)x * 0.08f + scene_t * 0.15f) * 2.0f
                                         + cosf((float)x * 0.03f - scene_t * 0.10f) * 2.0f);
            int hill1 = 28 + (int)lroundf(sinf((float)x * 0.05f - scene_t * 0.09f) * 3.0f);
            if (y > hill1) level = 5;
            else if (y > hill0) level = 9;

            if (y > 26) {
                float span = (float)(y - 26);
                float center = 64.0f + road_bend * (span / 22.0f);
                float half = 4.0f + span * 1.35f;
                float dx = fabsf((float)x - center);
                if (dx < half) {
                    int road = 8 + (int)lroundf((span / 22.0f) * 18.0f);
                    if (road > level) level = road;
                    if (dx < 1.5f + span * 0.03f && imod(y + (int)(scene_t * 8.0f), 4) < 2) level = 48;
                }
            }

            if (y < 18) {
                float cloud = sinf((float)x * 0.07f + scene_t * 0.6f)
                            + sinf((float)x * 0.15f - (float)y * 0.33f - scene_t * 0.2f);
                if (cloud > 1.0f) level += 6;
            }

            if (x > car_x && x < car_x + 10 && y > 30 && y < 36) level = 0;
            if (x == car_x - 1 && y == 33) level = 52;

            level += (int)(hash01(x + y * 131 + (int)(scene_t * 11.0f)) * 5.0f) - 2;
            level = (int)clampf_local((float)level, 0.0f, 63.0f);
            pdm_px(x, y, level, phase);
        }
    }
}

static void draw_scene_pdm_video(float scene_t, unsigned phase) {
    draw_scene_pdm_cinema(scene_t, phase);
}
#else
static void draw_scene_pdm_video(float scene_t, unsigned phase) {
    (void)phase;
    pdm_draw_video(scene_t);
}
#endif

static void draw_scene(int scene_idx, float scene_t, unsigned phase) {
    switch (scene_idx) {
        case 0: draw_scene_metaballs(scene_t, phase); break;
        case 1: draw_scene_doom(scene_t, phase); break;
        case 2: draw_scene_tank_wars(scene_t); break;
        case 3: draw_scene_voxel_plane(scene_t, phase); break;
        case 4: draw_scene_space_battle(scene_t); break;
        case 5: draw_scene_mandelbrot(scene_t); break;
        case 6: draw_scene_ascii_nyan(scene_t); break;
        case 7: draw_scene_mask_type(scene_t, phase); break;
        case 8: draw_scene_moire(scene_t); break;
        case 9: draw_scene_pdm_video(scene_t, phase); break;
    }
}

int main(int argc, char *argv[]) {
    int daemonize = 0;
    int start_scene_idx = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemonize = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-?") == 0) {
            usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (++i >= argc || !parse_scene_number(argv[i], &start_scene_idx)) {
                usage(argv[0]);
                return 1;
            }
        } else {
            usage(argv[0]);
            return 1;
        }
    }

#if HAVE_WOZ_PDM_HEADER
    if (!pdm_prepare_embedded_asset()) {
        fputs("invalid embedded woz_pdm.h asset\n", stderr);
        return 1;
    }
#endif

    if (daemonize) {
        if (daemonize_process() == -1) {
            perror("daemonize_process");
            return 1;
        }
    }

    i2c_fd = open("/dev/i2c-1", O_RDWR);
    if (i2c_fd < 0) {
        perror("open i2c");
        return 1;
    }
    if (ioctl(i2c_fd, I2C_SLAVE, ADDR) < 0) {
        perror("ioctl");
        close(i2c_fd);
        return 1;
    }

    init_display();
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    struct timespec prev;
    clock_gettime(CLOCK_MONOTONIC, &prev);

    float sim_t = scene_start_seconds(start_scene_idx);
    int last_scene_idx = -1;
    int last_header_step = -1;
    unsigned active_fps = DEMO_FPS;

    while (running) {
        struct timespec curr;
        clock_gettime(CLOCK_MONOTONIC, &curr);
        float dt = (curr.tv_sec - prev.tv_sec) + (curr.tv_nsec - prev.tv_nsec) / 1e9f;
        if (dt > 1.0f) dt = 1.0f;
        prev = curr;
        sim_t += dt;

        int scene_idx;
        float scene_t;
        resolve_scene_time(sim_t, &scene_idx, &scene_t);
        int header_step = (int)(scene_t * HEADER_SCROLL_PPS);
        int fast_video = scene_idx == 9 && HAVE_WOZ_PDM_HEADER;
        int full_refresh = !fast_video || scene_idx != last_scene_idx || header_step != last_header_step;

        if (full_refresh) {
            memset(fb, 0, sizeof(fb));
            draw_scroller(scene_msgs[scene_idx], scene_t, scene_idx);
            draw_scene(scene_idx, scene_t, frame_counter);
            flush();
        } else {
            memset(fb + WIDTH * BLUE_START_PAGE, 0, PDM_BLUE_BYTES);
            draw_scene(scene_idx, scene_t, frame_counter);
            flush_pages(BLUE_START_PAGE, PAGES - 1);
        }

        last_scene_idx = scene_idx;
        last_header_step = header_step;

        frame_counter++;
        unsigned target_fps = fast_video ? pdm_subframe_hz : DEMO_FPS;
        long frame_ns = 1000000000L / (long)target_fps;
        if (target_fps != active_fps) {
            active_fps = target_fps;
            clock_gettime(CLOCK_MONOTONIC, &next);
        }
        next.tv_nsec += frame_ns;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec += 1;
        }
        sleep_until(&next);
    }

    memset(fb, 0, sizeof(fb));
    flush();
    uint8_t off[2] = {0x00, 0xAE};
    ssize_t written = write(i2c_fd, off, 2);
    (void)written;
    pdm_release_asset();
    close(i2c_fd);
    return 0;
}
