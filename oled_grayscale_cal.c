/*
 * Interactive OLED grayscale calibration utility for the split yellow/blue SSD1306.
 *
 * Purpose:
 * - let a human operator tune gamma, gain, bias, and 16 anchor points in real time
 * - preview the current temporal PDM grayscale rendering directly on the OLED
 * - emit a report with both anchor points and an interpolated 256-entry LUT
 *
 * Controls:
 *   1        choose A, the left option
 *   2        choose B, the right option
 *   n        accept the current result and move to the next step
 *   b        go back to the previous step
 *   p        save report immediately
 *   q        quit and save report
 *   ?        print controls to the terminal
 */

#include <errno.h>
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
#include <sys/select.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define WIDTH             128
#define HEIGHT            64
#define PAGES             (HEIGHT / 8)
#define ADDR              0x3C
#define YELLOW_H          16
#define BLUE_Y            16
#define BLUE_H            48
#define BLUE_START_PAGE   (BLUE_Y / 8)
#define TARGET_FPS        90
#define PDM_PHASES        3
#define I2C_CHUNK         255
#define CAL_ANCHORS       16
#define AB_SLOT_COUNT     2
#define WIZARD_STEP_COUNT 7
#define RAMP_Y0           0
#define RAMP_Y1           10
#define PREVIEW_Y0        14
#define PREVIEW_Y1        47
#define REPORT_FILE       "oled_gamma_calibration.txt"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef enum {
    WIZARD_STEP_OVERVIEW,
    WIZARD_STEP_BLACK,
    WIZARD_STEP_SHADOWS,
    WIZARD_STEP_MIDTONES,
    WIZARD_STEP_HIGHLIGHTS,
    WIZARD_STEP_STABILITY,
    WIZARD_STEP_REVIEW,
} wizard_step_id_t;

typedef struct {
    const char *title;
    const char *look_at;
    const char *better_means;
    const char *when_done;
} wizard_step_t;

typedef struct {
    float gamma;
    float gain;
    int bias;
    uint8_t anchors[CAL_ANCHORS];
    uint8_t lut[256];
} calibration_variant_t;

typedef struct {
    float gamma;
    float gain;
    int bias;
    int selected;
    int wizard_step;
    int active_variant;
    int question_round;
    calibration_variant_t variants[AB_SLOT_COUNT];
    calibration_variant_t step_history[WIZARD_STEP_COUNT];
    uint8_t anchors[CAL_ANCHORS];
    uint8_t lut[256];
} calibration_t;

static const uint8_t bayer8x8[8][8] = {
    { 0, 48, 12, 60,  3, 51, 15, 63 },
    {32, 16, 44, 28, 35, 19, 47, 31 },
    { 8, 56,  4, 52, 11, 59,  7, 55 },
    {40, 24, 36, 20, 43, 27, 39, 23 },
    { 2, 50, 14, 62,  1, 49, 13, 61 },
    {34, 18, 46, 30, 33, 17, 45, 29 },
    {10, 58,  6, 54,  9, 57,  5, 53 },
    {42, 26, 38, 22, 41, 25, 37, 21 },
};

static const uint8_t temporal_order[PDM_PHASES] = { 0, 2, 1 };

static const wizard_step_t wizard_steps[WIZARD_STEP_COUNT] = {
    {
        "OVERVIEW",
        "the two full ramps and the two rows of bars.",
        "Pick the side where the ramp rises more evenly from black to white, and where no bar in the row below suddenly looks darker than the bar before it.",
        "Press n when both sides look close and the whole curve looks smooth enough.",
    },
    {
        "BLACK FLOOR",
        "the darkest boxes: 0 1 2 3 4 6 8 12.",
        "Pick the side where box 0 stays black and the next few boxes appear one by one without a sudden jump.",
        "Press n when black looks clean and the first dark boxes are easy enough to tell apart.",
    },
    {
        "SHADOWS",
        "the dark boxes: 0 4 8 12 16 24 32 40.",
        "Pick the side where the dark boxes separate more evenly and no single box jumps much brighter than the one before it.",
        "Press n when the dark end rises gently out of black and the boxes look evenly spaced.",
    },
    {
        "MIDTONES",
        "the middle boxes: 48 64 80 96 112 128 144 160.",
        "Pick the side where the middle boxes brighten at a steadier pace, with no flat section and no sudden leap.",
        "Press n when the middle of the curve looks even and natural.",
    },
    {
        "HIGHLIGHTS",
        "the brightest boxes: 176 192 208 224 236 244 250 255.",
        "Pick the side where the bright boxes stay separate longer, but the last box still reaches full white.",
        "Press n when the top end is bright enough and the last few boxes do not merge too early.",
    },
    {
        "STABILITY",
        "the checker pairs 4/8, 12/16, 224/232, and 244/252.",
        "Pick the side that looks steadier and less sparkly, even if it gives up a small amount of contrast.",
        "Press n when the checker blocks look calm enough.",
    },
    {
        "REVIEW",
        "the two full ramps and the two rows of bars again.",
        "Pick the side that looks better overall from black to white, without one area standing out as obviously wrong.",
        "Press p to save when the full curve looks right. Press b if you need to revisit an earlier step.",
    },
};

static const uint8_t wizard_black_values[] = { 0, 1, 2, 3, 4, 6, 8, 12 };
static const uint8_t wizard_shadow_values[] = { 0, 4, 8, 12, 16, 24, 32, 40 };
static const uint8_t wizard_midtone_values[] = { 48, 64, 80, 96, 112, 128, 144, 160 };
static const uint8_t wizard_highlight_values[] = { 176, 192, 208, 224, 236, 244, 250, 255 };

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

static int i2c_fd = -1;
static uint8_t fb[WIDTH * PAGES];
static volatile int running = 1;
static struct termios saved_termios;
static int termios_valid = 0;
static int saved_flags = -1;

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static char variant_name(int variant) {
    return (char)('A' + clampi(variant, 0, AB_SLOT_COUNT - 1));
}

static float clampf_local(float v, float lo, float hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void stop(int sig) {
    (void)sig;
    running = 0;
}

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [-h|-?]\n"
            "Interactive grayscale calibration wizard for the split yellow/blue SSD1306.\n"
            "Writes %s on exit and shows runtime controls in the terminal.\n"
            "  -h, -?  show this help message\n",
            argv0, REPORT_FILE);
}

static void restore_terminal(void) {
    if (termios_valid) tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
    if (saved_flags != -1) fcntl(STDIN_FILENO, F_SETFL, saved_flags);
}

static int enter_raw_terminal(void) {
    if (!isatty(STDIN_FILENO)) return -1;
    if (tcgetattr(STDIN_FILENO, &saved_termios) != 0) return -1;
    termios_valid = 1;
    saved_flags = fcntl(STDIN_FILENO, F_GETFL, 0);

    struct termios raw = saved_termios;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return -1;
    if (saved_flags != -1) fcntl(STDIN_FILENO, F_SETFL, saved_flags | O_NONBLOCK);
    atexit(restore_terminal);
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

static void flush_region(int start_page, int end_page) {
    uint8_t ac[] = {0x00, 0x21, 0, WIDTH - 1, 0x22, (uint8_t)start_page, (uint8_t)end_page};
    ssize_t written = write(i2c_fd, ac, sizeof(ac));
    (void)written;
    for (int i = start_page * WIDTH; i < (end_page + 1) * WIDTH; i += I2C_CHUNK) {
        uint8_t buf[I2C_CHUNK + 1];
        buf[0] = 0x40;
        int len = (end_page + 1) * WIDTH - i;
        if (len > I2C_CHUNK) len = I2C_CHUNK;
        memcpy(buf + 1, fb + i, len);
        written = write(i2c_fd, buf, len + 1);
        (void)written;
    }
}

static void flush_full(void) {
    flush_region(0, PAGES - 1);
}

static void flush_blue(void) {
    flush_region(BLUE_START_PAGE, PAGES - 1);
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

static void px(int x, int y) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
        fb[x + (y / 8) * WIDTH] |= (uint8_t)(1u << (y & 7));
}

static void bpx(int x, int y) {
    px(x, y + BLUE_Y);
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

static void draw_char(int x, int y, char c) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++)
            if (bits & (1u << row))
                px(x + col, y + row);
    }
}

static void draw_str(int x, int y, const char *s) {
    while (*s) {
        draw_char(x, y, *s++);
        x += 6;
    }
}

static void variant_from_current(const calibration_t *cal, calibration_variant_t *variant) {
    variant->gamma = cal->gamma;
    variant->gain = cal->gain;
    variant->bias = cal->bias;
    memcpy(variant->anchors, cal->anchors, sizeof(variant->anchors));
    memcpy(variant->lut, cal->lut, sizeof(variant->lut));
}

static void current_from_variant(calibration_t *cal, const calibration_variant_t *variant) {
    cal->gamma = variant->gamma;
    cal->gain = variant->gain;
    cal->bias = variant->bias;
    memcpy(cal->anchors, variant->anchors, sizeof(cal->anchors));
    memcpy(cal->lut, variant->lut, sizeof(cal->lut));
}

static void rebuild_variant_lut(calibration_variant_t *variant) {
    for (int x = 0; x < 256; x++) {
        float pos = (float)x * (float)(CAL_ANCHORS - 1) / 255.0f;
        int idx = (int)floorf(pos);
        float t = pos - (float)idx;
        if (idx >= CAL_ANCHORS - 1) {
            variant->lut[x] = variant->anchors[CAL_ANCHORS - 1];
        } else {
            float v = variant->anchors[idx] + (variant->anchors[idx + 1] - variant->anchors[idx]) * t;
            variant->lut[x] = (uint8_t)clampi((int)lroundf(v), 0, 255);
        }
    }
    for (int x = 1; x < 256; x++) {
        if (variant->lut[x] < variant->lut[x - 1]) variant->lut[x] = variant->lut[x - 1];
    }
}

static void normalize_variant(calibration_variant_t *variant) {
    variant->anchors[0] = 0;
    for (int i = 1; i < CAL_ANCHORS - 1; i++) {
        variant->anchors[i] = (uint8_t)clampi(variant->anchors[i], 0, 255);
        if (variant->anchors[i] < variant->anchors[i - 1])
            variant->anchors[i] = variant->anchors[i - 1];
    }
    variant->anchors[CAL_ANCHORS - 1] = 255;
    rebuild_variant_lut(variant);
}

static void apply_region_delta(calibration_variant_t *variant, float center, float width, int delta) {
    uint8_t original[CAL_ANCHORS];
    memcpy(original, variant->anchors, sizeof(original));

    for (int i = 1; i < CAL_ANCHORS - 1; i++) {
        float x = (float)i / (float)(CAL_ANCHORS - 1);
        float dist = fabsf(x - center) / width;
        float weight = dist >= 1.0f ? 0.0f : (1.0f - dist);
        int v = (int)lroundf((float)original[i] + (float)delta * weight);
        variant->anchors[i] = (uint8_t)clampi(v, 0, 255);
    }

    normalize_variant(variant);
}

static void smooth_region(calibration_variant_t *variant, float center, float width, float amount) {
    uint8_t original[CAL_ANCHORS];
    memcpy(original, variant->anchors, sizeof(original));

    for (int i = 1; i < CAL_ANCHORS - 1; i++) {
        float x = (float)i / (float)(CAL_ANCHORS - 1);
        float dist = fabsf(x - center) / width;
        float weight = dist >= 1.0f ? 0.0f : (1.0f - dist);
        float average = (original[i - 1] + original[i] + original[i + 1]) / 3.0f;
        float v = original[i] + (average - original[i]) * amount * weight;
        variant->anchors[i] = (uint8_t)clampi((int)lroundf(v), 0, 255);
    }

    normalize_variant(variant);
}

static void sharpen_region(calibration_variant_t *variant, float center, float width, float amount) {
    uint8_t original[CAL_ANCHORS];
    memcpy(original, variant->anchors, sizeof(original));

    for (int i = 1; i < CAL_ANCHORS - 1; i++) {
        float x = (float)i / (float)(CAL_ANCHORS - 1);
        float dist = fabsf(x - center) / width;
        float weight = dist >= 1.0f ? 0.0f : (1.0f - dist);
        float average = (original[i - 1] + original[i] + original[i + 1]) / 3.0f;
        float v = original[i] + (original[i] - average) * amount * weight;
        variant->anchors[i] = (uint8_t)clampi((int)lroundf(v), 0, 255);
    }

    normalize_variant(variant);
}

static int step_delta(wizard_step_id_t step, int round) {
    static const int global_steps[] = { 18, 12, 8, 5, 3 };
    static const int local_steps[] = { 14, 9, 6, 4, 2 };
    static const int black_steps[] = { 10, 7, 5, 3, 2 };
    int idx = clampi(round, 0, 4);

    switch (step) {
        case WIZARD_STEP_BLACK:
            return black_steps[idx];
        case WIZARD_STEP_SHADOWS:
        case WIZARD_STEP_MIDTONES:
        case WIZARD_STEP_HIGHLIGHTS:
            return local_steps[idx];
        case WIZARD_STEP_OVERVIEW:
        case WIZARD_STEP_REVIEW:
        default:
            return global_steps[idx];
    }
}

static float stability_amount(int round) {
    static const float amounts[] = { 0.75f, 0.60f, 0.45f, 0.35f, 0.25f };
    return amounts[clampi(round, 0, 4)];
}

static void generate_candidates(calibration_t *cal) {
    calibration_variant_t base;
    int delta = step_delta((wizard_step_id_t)cal->wizard_step, cal->question_round);

    variant_from_current(cal, &base);
    cal->variants[0] = base;
    cal->variants[1] = base;

    switch ((wizard_step_id_t)cal->wizard_step) {
        case WIZARD_STEP_BLACK:
            apply_region_delta(&cal->variants[0], 0.08f, 0.22f, -delta);
            apply_region_delta(&cal->variants[1], 0.08f, 0.22f, +delta);
            break;
        case WIZARD_STEP_SHADOWS:
            apply_region_delta(&cal->variants[0], 0.20f, 0.24f, -delta);
            apply_region_delta(&cal->variants[1], 0.20f, 0.24f, +delta);
            break;
        case WIZARD_STEP_MIDTONES:
            apply_region_delta(&cal->variants[0], 0.50f, 0.28f, -delta);
            apply_region_delta(&cal->variants[1], 0.50f, 0.28f, +delta);
            break;
        case WIZARD_STEP_HIGHLIGHTS:
            apply_region_delta(&cal->variants[0], 0.82f, 0.22f, -delta);
            apply_region_delta(&cal->variants[1], 0.82f, 0.22f, +delta);
            break;
        case WIZARD_STEP_STABILITY: {
            float amount = stability_amount(cal->question_round);
            smooth_region(&cal->variants[0], 0.20f, 0.24f, amount);
            smooth_region(&cal->variants[0], 0.82f, 0.22f, amount);
            sharpen_region(&cal->variants[1], 0.20f, 0.24f, amount * 0.45f);
            sharpen_region(&cal->variants[1], 0.82f, 0.22f, amount * 0.45f);
            break;
        }
        case WIZARD_STEP_REVIEW:
            apply_region_delta(&cal->variants[0], 0.50f, 0.60f, -delta / 2);
            apply_region_delta(&cal->variants[1], 0.50f, 0.60f, +delta / 2);
            break;
        case WIZARD_STEP_OVERVIEW:
        default:
            apply_region_delta(&cal->variants[0], 0.50f, 0.60f, -delta);
            apply_region_delta(&cal->variants[1], 0.50f, 0.60f, +delta);
            break;
    }
}

static void print_choice_feedback(int choice) {
    fprintf(stderr,
            "\nYou picked %c. I made the next comparison for you.\n",
            variant_name(choice));
}

static void print_wizard_step(const calibration_t *cal);

static void choose_candidate(calibration_t *cal, int choice) {
    current_from_variant(cal, &cal->variants[choice]);
    cal->active_variant = choice;
    variant_from_current(cal, &cal->step_history[cal->wizard_step]);
    cal->question_round++;
    print_choice_feedback(choice);
    generate_candidates(cal);
    print_wizard_step(cal);
}

static void print_wizard_step(const calibration_t *cal) {
    const wizard_step_t *step = &wizard_steps[cal->wizard_step];

    fprintf(stderr,
            "\nStep %d/%d: %s\n"
            "  Look at %s\n"
            "  Press 1 if A is better. Press 2 if B is better.\n"
            "  Better means: %s\n"
            "  After you choose, I will make the next comparison for you.\n"
            "  %s\n"
            "  Other keys: n next step, b previous step, p save, q quit, ? help.\n\n",
            cal->wizard_step + 1, WIZARD_STEP_COUNT,
            step->title,
            step->look_at,
            step->better_means,
            step->when_done);
}

static void change_wizard_step(calibration_t *cal, int delta) {
    int new_step = clampi(cal->wizard_step + delta, 0, WIZARD_STEP_COUNT - 1);

    if (new_step == cal->wizard_step) {
        if (delta > 0 && cal->wizard_step == WIZARD_STEP_COUNT - 1)
            fprintf(stderr, "\nThis is the last step. Press p to save or q to save and quit.\n");
        print_wizard_step(cal);
        return;
    }

    if (delta > 0)
        variant_from_current(cal, &cal->step_history[new_step]);

    cal->wizard_step = new_step;
    current_from_variant(cal, &cal->step_history[new_step]);
    cal->question_round = 0;
    generate_candidates(cal);
    print_wizard_step(cal);
}

static void print_controls(void) {
    fprintf(stderr,
            "\nOLED grayscale calibration\n"
            "  1        choose A, the left option\n"
            "  2        choose B, the right option\n"
            "  n        keep the current result and move to the next step\n"
            "  b        go back to the previous step\n"
            "  p        save report immediately\n"
            "  q        quit and save report\n"
            "  ?        show this help\n\n"
            "How to use it:\n"
            "  1. Look at A on the left and B on the right.\n"
            "  2. Press 1 if A is better. Press 2 if B is better.\n"
            "  3. I will make the next comparison for you.\n"
            "  4. Press n when the current step looks good enough.\n\n");
}

static void print_status(const calibration_t *cal) {
    const wizard_step_t *step = &wizard_steps[cal->wizard_step];

    fprintf(stderr,
            "\rStep %d/%d %-11s  1=A  2=B  n=next  b=back  p=save  q=quit        ",
            cal->wizard_step + 1, WIZARD_STEP_COUNT, step->title);
    fflush(stderr);
}

static void rebuild_lut(calibration_t *cal) {
    calibration_variant_t variant;

    variant_from_current(cal, &variant);
    rebuild_variant_lut(&variant);
    memcpy(cal->lut, variant.lut, sizeof(cal->lut));
}

static void rebuild_anchors_from_curve(calibration_t *cal) {
    for (int i = 0; i < CAL_ANCHORS; i++) {
        float x = (float)i / (float)(CAL_ANCHORS - 1);
        float y = powf(clampf_local(x, 0.0f, 1.0f), cal->gamma);
        int v = (int)lroundf(y * cal->gain * 255.0f) + cal->bias;
        cal->anchors[i] = (uint8_t)clampi(v, 0, 255);
    }
    cal->anchors[0] = 0;
    for (int i = 1; i < CAL_ANCHORS; i++) {
        if (cal->anchors[i] < cal->anchors[i - 1]) cal->anchors[i] = cal->anchors[i - 1];
    }
    cal->anchors[CAL_ANCHORS - 1] = 255;
    rebuild_lut(cal);
}

static int save_report(const calibration_t *cal) {
    FILE *fp = fopen(REPORT_FILE, "w");
    if (!fp) return -1;

    fprintf(fp, "# OLED grayscale calibration report\n");
    fprintf(fp, "gamma=%.2f\n", cal->gamma);
    fprintf(fp, "gain=%.2f\n", cal->gain);
    fprintf(fp, "bias=%d\n", cal->bias);
    fprintf(fp, "anchor_count=%d\n\n", CAL_ANCHORS);

    fprintf(fp, "anchors16 = {");
    for (int i = 0; i < CAL_ANCHORS; i++) {
        fprintf(fp, "%s%u", i ? ", " : "", cal->anchors[i]);
    }
    fprintf(fp, "};\n\n");

    fprintf(fp, "lut256 = {\n");
    for (int i = 0; i < 256; i++) {
        if ((i % 16) == 0) fprintf(fp, "    ");
        fprintf(fp, "%3u", cal->lut[i]);
        if (i != 255) fprintf(fp, ", ");
        if ((i % 16) == 15) fprintf(fp, "\n");
    }
    fprintf(fp, "};\n\n");

    fprintf(fp, "static const unsigned char oled_gray_anchors[%d] = {", CAL_ANCHORS);
    for (int i = 0; i < CAL_ANCHORS; i++) fprintf(fp, "%s%u", i ? ", " : "", cal->anchors[i]);
    fprintf(fp, "};\n\n");

    fprintf(fp, "static const unsigned char oled_gray_lut[256] = {\n");
    for (int i = 0; i < 256; i++) {
        if ((i % 16) == 0) fprintf(fp, "    ");
        fprintf(fp, "%3u", cal->lut[i]);
        if (i != 255) fprintf(fp, ", ");
        if ((i % 16) == 15) fprintf(fp, "\n");
    }
    fprintf(fp, "};\n");

    fclose(fp);
    fprintf(stderr, "\nSaved %s\n", REPORT_FILE);
    print_status(cal);
    return 0;
}

static int pdm_on_level(int x, int y, int level, unsigned phase) {
    int scaled = level * (PDM_PHASES * 64 + 1) / 256;
    int bayer = bayer8x8[y & 7][x & 7];
    unsigned rotate = (unsigned)((x * 3 + y * 5) % PDM_PHASES);
    unsigned rank = temporal_order[(phase + rotate) % PDM_PHASES];
    return scaled > (int)(rank * 64u) + bayer;
}

static void draw_blue_rect_outline(int x0, int y0, int x1, int y1) {
    bline(x0, y0, x1, y0);
    bline(x0, y1, x1, y1);
    bline(x0, y0, x0, y1);
    bline(x1, y0, x1, y1);
}

static void draw_blue_level_rect(int x0, int y0, int x1, int y1, int level, unsigned phase) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (pdm_on_level(x, y, level, phase)) bpx(x, y);
        }
    }
}

static void draw_half_ramp(const calibration_variant_t *variant,
                           int left,
                           int right,
                           int y0,
                           int y1,
                           unsigned phase) {
    for (int y = y0; y <= y1; y++) {
        for (int x = left; x <= right; x++) {
            int src = ((x - left) * 255) / ((right - left) ? (right - left) : 1);
            int level = variant->lut[src];
            if (pdm_on_level(x, y, level, phase)) bpx(x, y);
        }
    }
}

static void draw_source_patch_row(const calibration_variant_t *variant,
                                  const uint8_t *source_values,
                                  int count,
                                  int left,
                                  int right,
                                  int y0,
                                  int y1,
                                  unsigned phase,
                                  int highlighted_patch,
                                  unsigned frame_counter) {
    int span = right - left + 1;

    for (int i = 0; i < count; i++) {
        int x0 = left + (i * span) / count;
        int x1 = left + ((i + 1) * span) / count - 1;
        int level = variant->lut[source_values[i]];

        draw_blue_level_rect(x0, y0, x1, y1, level, phase);
        if (i > 0)
            bline(x0, y0, x0, y1);
        if (i == highlighted_patch)
            draw_blue_rect_outline(x0, y0, x1, y1);
    }
}

static void draw_source_checker_rect(const calibration_variant_t *variant,
                                     int x0,
                                     int y0,
                                     int x1,
                                     int y1,
                                     uint8_t source_a,
                                     uint8_t source_b,
                                     unsigned phase,
                                     unsigned frame_counter) {
    int level_a = variant->lut[source_a];
    int level_b = variant->lut[source_b];

    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            int tile = (((x - x0) >> 2) ^ ((y - y0) >> 2)) & 1;
            int level = tile ? level_a : level_b;
            if (pdm_on_level(x, y, level, phase)) bpx(x, y);
        }
    }

    draw_blue_rect_outline(x0, y0, x1, y1);
}

static void draw_anchor_preview(const calibration_variant_t *variant,
                                int selected,
                                int left,
                                int right,
                                int y0,
                                int y1,
                                unsigned phase) {
    (void)selected;
    int span = right - left + 1;

    for (int i = 0; i < CAL_ANCHORS; i++) {
        int x0 = left + (i * span) / CAL_ANCHORS;
        int x1 = left + ((i + 1) * span) / CAL_ANCHORS - 1;
        draw_blue_level_rect(x0, y0, x1, y1, variant->anchors[i], phase);
        if (i > 0)
            bline(x0, y0, x0, y1);
    }
}

static void draw_wizard_preview_half(const calibration_variant_t *variant,
                                     int selected,
                                     wizard_step_id_t step,
                                     int left,
                                     int right,
                                     unsigned phase,
                                     unsigned frame_counter) {
    switch (step) {
        case WIZARD_STEP_BLACK:
            draw_source_patch_row(variant, wizard_black_values,
                                  (int)(sizeof(wizard_black_values) / sizeof(wizard_black_values[0])),
                                  left, right, PREVIEW_Y0, PREVIEW_Y1, phase, 1, frame_counter);
            break;
        case WIZARD_STEP_SHADOWS:
            draw_source_patch_row(variant, wizard_shadow_values,
                                  (int)(sizeof(wizard_shadow_values) / sizeof(wizard_shadow_values[0])),
                                  left, right, PREVIEW_Y0, PREVIEW_Y1, phase, 2, frame_counter);
            break;
        case WIZARD_STEP_MIDTONES:
            draw_source_patch_row(variant, wizard_midtone_values,
                                  (int)(sizeof(wizard_midtone_values) / sizeof(wizard_midtone_values[0])),
                                  left, right, PREVIEW_Y0, PREVIEW_Y1, phase, 3, frame_counter);
            break;
        case WIZARD_STEP_HIGHLIGHTS:
            draw_source_patch_row(variant, wizard_highlight_values,
                                  (int)(sizeof(wizard_highlight_values) / sizeof(wizard_highlight_values[0])),
                                  left, right, PREVIEW_Y0, PREVIEW_Y1, phase, 6, frame_counter);
            break;
        case WIZARD_STEP_STABILITY:
            draw_source_checker_rect(variant, left, PREVIEW_Y0, left + 15, PREVIEW_Y1, 4, 8, phase, frame_counter);
            draw_source_checker_rect(variant, left + 16, PREVIEW_Y0, left + 31, PREVIEW_Y1, 12, 16, phase, frame_counter);
            draw_source_checker_rect(variant, left + 32, PREVIEW_Y0, left + 47, PREVIEW_Y1, 224, 232, phase, frame_counter);
            draw_source_checker_rect(variant, left + 48, PREVIEW_Y0, right, PREVIEW_Y1, 244, 252, phase, frame_counter);
            break;
        case WIZARD_STEP_OVERVIEW:
        case WIZARD_STEP_REVIEW:
        default:
            draw_anchor_preview(variant, selected, left, right, PREVIEW_Y0, PREVIEW_Y1, phase);
            break;
    }
}

static void draw_wizard_preview(const calibration_t *cal, unsigned phase, unsigned frame_counter) {
    draw_wizard_preview_half(&cal->variants[0], cal->selected, (wizard_step_id_t)cal->wizard_step,
                             0, 63, phase, frame_counter);
    draw_wizard_preview_half(&cal->variants[1], cal->selected, (wizard_step_id_t)cal->wizard_step,
                             64, 127, phase, frame_counter);
    bline(63, PREVIEW_Y0, 63, PREVIEW_Y1);
}

static void draw_calibration_view(const calibration_t *cal, unsigned phase, unsigned frame_counter) {
    char line0[32];
    char line1[32];
    const wizard_step_t *step = &wizard_steps[cal->wizard_step];

    snprintf(line0, sizeof(line0), "%d/%d %s", cal->wizard_step + 1, WIZARD_STEP_COUNT, step->title);
    snprintf(line1, sizeof(line1), "1=A 2=B N=OK");
    draw_str(0, 0, line0);
    draw_str(0, 8, line1);

    draw_half_ramp(&cal->variants[0], 0, 63, RAMP_Y0, RAMP_Y1, phase);
    draw_half_ramp(&cal->variants[1], 64, 127, RAMP_Y0, RAMP_Y1, phase);
    bline(63, RAMP_Y0, 63, RAMP_Y1);

    draw_wizard_preview(cal, phase, frame_counter);
}

static void handle_key(calibration_t *cal, int ch) {
    switch (ch) {
        case 'b':
        case 'B':
            change_wizard_step(cal, -1);
            break;
        case 'n':
        case 'N':
            change_wizard_step(cal, +1);
            break;
        case '1':
            choose_candidate(cal, 0);
            break;
        case '2':
            choose_candidate(cal, 1);
            break;
        case 'p':
        case 'P':
            save_report(cal);
            break;
        case '?':
            print_controls();
            print_wizard_step(cal);
            break;
        case 'q':
        case 'Q':
            running = 0;
            break;
        default:
            break;
    }
}

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-?") == 0) {
            usage(argv[0]);
            return 0;
        }
        usage(argv[0]);
        return 1;
    }

    calibration_t cal;
    memset(&cal, 0, sizeof(cal));
    cal.gamma = 1.60f;
    cal.gain = 1.00f;
    cal.bias = 0;
    cal.selected = 8;
    cal.wizard_step = WIZARD_STEP_OVERVIEW;
    cal.active_variant = 0;
    cal.question_round = 0;
    rebuild_anchors_from_curve(&cal);
    for (int i = 0; i < WIZARD_STEP_COUNT; i++)
        variant_from_current(&cal, &cal.step_history[i]);
    generate_candidates(&cal);

    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    if (enter_raw_terminal() != 0) {
        perror("raw terminal");
        return 1;
    }

    print_controls();
    print_wizard_step(&cal);
    print_status(&cal);

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

    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    long frame_ns = 1000000000L / TARGET_FPS;
    unsigned frame_counter = 0;
    int header_dirty = 1;

    while (running) {
        for (;;) {
            unsigned char ch;
            ssize_t n = read(STDIN_FILENO, &ch, 1);
            if (n == 1) {
                handle_key(&cal, ch);
                header_dirty = 1;
                print_status(&cal);
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n <= 0) break;
        }

        memset(fb, 0, sizeof(fb));
        draw_calibration_view(&cal, frame_counter % PDM_PHASES, frame_counter);
        if (header_dirty) {
            flush_full();
            header_dirty = 0;
        } else {
            flush_blue();
        }

        frame_counter++;
        next.tv_nsec += frame_ns;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec += 1;
        }
        sleep_until(&next);
    }

    save_report(&cal);
    memset(fb, 0, sizeof(fb));
    flush_full();
    uint8_t off[2] = {0x00, 0xAE};
    ssize_t written = write(i2c_fd, off, 2);
    (void)written;
    close(i2c_fd);
    fprintf(stderr, "\nExited. Report written to %s\n", REPORT_FILE);
    return 0;
}
