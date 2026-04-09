/*
 * Interactive OLED grayscale calibration utility for the split yellow/blue SSD1306.
 *
 * Purpose:
 * - let a human operator tune gamma, gain, bias, and 16 anchor points in real time
 * - preview the current temporal PDM grayscale rendering directly on the OLED
 * - emit a report with both anchor points and an interpolated 256-entry LUT
 *
 * Controls:
 *   q        quit and save report
 *   p        save report immediately
 *   ?        print controls to the terminal
 *   n / b    next / previous wizard step
 *   1 / 2    edit A or B for side-by-side comparison
 *   c        copy the current curve into the other slot and switch to it
 *   a / d    select previous / next anchor
 *   j / k    decrease / increase selected anchor by 1
 *   J / K    decrease / increase selected anchor by 4
 *   [ / ]    decrease / increase gamma by 0.05 and rebuild anchors
 *   - / =    decrease / increase gain by 0.05 and rebuild anchors
 *   , / .    decrease / increase bias by 1 code value and rebuild anchors
 *   r        rebuild anchors from current gamma / gain / bias
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
    const char *what_to_look_for;
    const char *adjust_first;
    const char *why;
    const char *ab_test;
    const char *pattern;
    int suggested_anchor;
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
    calibration_variant_t variants[AB_SLOT_COUNT];
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
        "Look for a smooth top gradient and a 16-anchor strip that never goes backward.",
        "Do not chase perfection yet. Press c to fork the current curve, make one small change, then compare A on the left against B on the right.",
        "Novices usually judge two nearby choices better than they judge a long chain of edits from memory.",
        "Keep the side that looks smoother overall with 1 or 2, then move on to the next step with n.",
        "Pattern: full ramp on top, local anchor neighborhood below.",
        8,
    },
    {
        "BLACK FLOOR",
        "Patch 0 should stay truly black, while the first few dark patches should be barely but clearly visible.",
        "Start with bias or gain. Use gamma only if the entire dark end is too flat or too steep. Use anchor 2 only for a local fix.",
        "Bias shifts the whole curve, gain stretches it, gamma bends it, and anchors fix a small local mistake without moving everything else.",
        "Copy A to B, make one small change in B, and keep the side that reveals 1 through 4 better without making patch 0 glow.",
        "Patches: 0 1 2 3 4 6 8 12",
        1,
    },
    {
        "SHADOWS",
        "The dark patches should separate cleanly, but the ramp should still climb gently out of black.",
        "Work on anchors 2 through 4 first. Touch gamma again only if the entire shadow region bends the wrong way.",
        "This part decides whether dark scenes have detail or collapse into a murky block.",
        "Keep the side where the dark boxes separate more evenly, not the side with the biggest single jump.",
        "Patches: 0 4 8 12 16 24 32 40",
        2,
    },
    {
        "MIDTONES",
        "The middle patches should brighten at a steady pace with no obvious plateaus or sudden jumps.",
        "Use anchors 6 through 9 for local shaping. Change gamma only if the whole middle looks too dark or too bright.",
        "Midtones dominate most real content, so getting them even usually makes the whole display feel more natural.",
        "Pick the side whose middle boxes look most evenly spaced, even if the difference is subtle.",
        "Patches: 48 64 80 96 112 128 144 160",
        6,
    },
    {
        "HIGHLIGHTS",
        "Bright patches should stay distinct as long as possible before they merge into full white.",
        "Use anchors 11 through 14 for local shape. Reduce gain if everything is crushed into the brightest few steps.",
        "Highlight separation keeps bright UI elements and reflections from turning into one flat block.",
        "Keep the side where the last few bright patches stay separable longer, as long as 255 still reaches full white.",
        "Patches: 176 192 208 224 236 244 250 255",
        12,
    },
    {
        "STABILITY",
        "Look for sparkle, shimmer, or flicker in the checker blocks while the two compared levels alternate in space and time.",
        "If a block looks noisy, smooth the nearby anchors instead of forcing the levels farther apart.",
        "Temporal grayscale can look unstable when neighboring output codes land on awkward pulse patterns.",
        "Keep the side that looks calmer and more solid, even if it gives up a tiny amount of contrast.",
        "Checker tests: 4/8, 12/16, 224/232, 244/252",
        12,
    },
    {
        "REVIEW",
        "Check the whole ramp one last time and make sure the 16 anchor bars still rise monotonically from left to right.",
        "Stay with the better A or B result, then scan for any obvious step that still stands out too much.",
        "A final whole-curve review catches edits that improved one region but hurt another.",
        "Choose the side that looks best overall, not the side that wins only one tiny region.",
        "Pattern: full ramp on top, local anchor neighborhood below.",
        8,
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

static void sync_active_variant(calibration_t *cal) {
    variant_from_current(cal, &cal->variants[cal->active_variant]);
}

static void load_variant(calibration_t *cal, int variant) {
    int clamped = clampi(variant, 0, AB_SLOT_COUNT - 1);

    sync_active_variant(cal);
    cal->active_variant = clamped;
    current_from_variant(cal, &cal->variants[clamped]);
}

static void print_variant_selection(const calibration_t *cal) {
    fprintf(stderr,
            "Editing slot %c. Left preview is A, right preview is B.\n",
            variant_name(cal->active_variant));
}

static void fork_to_other_variant(calibration_t *cal) {
    int source = cal->active_variant;
    int target = source ^ 1;

    sync_active_variant(cal);
    cal->variants[target] = cal->variants[source];
    cal->active_variant = target;
    current_from_variant(cal, &cal->variants[target]);
    fprintf(stderr,
            "Copied slot %c into slot %c and switched to %c. Make one small change, then compare left A against right B.\n",
            variant_name(source), variant_name(target), variant_name(target));
}

static void print_wizard_step(const calibration_t *cal) {
    const wizard_step_t *step = &wizard_steps[cal->wizard_step];

    fprintf(stderr,
            "\nWizard step %d/%d: %s\n"
            "  What to judge: %s\n"
            "  Adjust first: %s\n"
            "  Why: %s\n"
            "  A/B test: %s\n"
            "  %s\n"
            "  Slot controls: 1 edit A, 2 edit B, c forks the current curve into the other slot.\n"
            "  Current slot: %c\n\n",
            cal->wizard_step + 1, WIZARD_STEP_COUNT,
            step->title,
            step->what_to_look_for,
            step->adjust_first,
            step->why,
            step->ab_test,
            step->pattern,
            variant_name(cal->active_variant));
}

static void set_wizard_step(calibration_t *cal, int step) {
    int clamped = clampi(step, 0, WIZARD_STEP_COUNT - 1);

    if (cal->wizard_step == clamped) {
        print_wizard_step(cal);
        return;
    }

    cal->wizard_step = clamped;
    if (wizard_steps[clamped].suggested_anchor >= 0)
        cal->selected = clampi(wizard_steps[clamped].suggested_anchor, 0, CAL_ANCHORS - 1);
    print_wizard_step(cal);
}

static void change_wizard_step(calibration_t *cal, int delta) {
    set_wizard_step(cal, cal->wizard_step + delta);
}

static void print_controls(void) {
    fprintf(stderr,
            "\nOLED grayscale calibration controls\n"
            "  q        quit and save report\n"
            "  p        save report immediately\n"
            "  ?        show this help\n"
            "  n / b    next / previous wizard step\n"
            "  1 / 2    edit slot A or B\n"
            "  c        copy the current slot into the other one and switch to it\n"
            "  a / d    select previous / next anchor\n"
            "  j / k    decrease / increase selected anchor by 1\n"
            "  J / K    decrease / increase selected anchor by 4\n"
            "  [ / ]    decrease / increase gamma by 0.05 and rebuild\n"
            "  - / =    decrease / increase gain by 0.05 and rebuild\n"
            "  , / .    decrease / increase bias by 1 and rebuild\n"
            "  r        rebuild anchors from current gamma / gain / bias\n\n"
            "Novice workflow:\n"
            "  1. Pick a wizard step with n or b.\n"
            "  2. Press c to clone the current result into the other slot.\n"
            "  3. Make one small change in the new slot.\n"
            "  4. Compare A on the left against B on the right.\n"
            "  5. Keep editing the better side with 1 or 2.\n\n");
}

static void print_status(const calibration_t *cal) {
    const wizard_step_t *step = &wizard_steps[cal->wizard_step];

    fprintf(stderr,
            "\rW %d/%d %-11s S %c  G %.2f  C %.2f  B %+03d  A %02d/%02d = %03u        ",
            cal->wizard_step + 1, WIZARD_STEP_COUNT, step->title, variant_name(cal->active_variant),
            cal->gamma, cal->gain, cal->bias,
            cal->selected + 1, CAL_ANCHORS, cal->anchors[cal->selected]);
    fflush(stderr);
}

static void rebuild_lut(calibration_t *cal) {
    for (int x = 0; x < 256; x++) {
        float pos = (float)x * (float)(CAL_ANCHORS - 1) / 255.0f;
        int idx = (int)floorf(pos);
        float t = pos - (float)idx;
        if (idx >= CAL_ANCHORS - 1) {
            cal->lut[x] = cal->anchors[CAL_ANCHORS - 1];
        } else {
            float v = cal->anchors[idx] + (cal->anchors[idx + 1] - cal->anchors[idx]) * t;
            cal->lut[x] = (uint8_t)clampi((int)lroundf(v), 0, 255);
        }
    }
    for (int x = 1; x < 256; x++) {
        if (cal->lut[x] < cal->lut[x - 1]) cal->lut[x] = cal->lut[x - 1];
    }
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

static void adjust_anchor(calibration_t *cal, int delta) {
    int idx = cal->selected;
    int lo = idx == 0 ? 0 : cal->anchors[idx - 1];
    int hi = idx == CAL_ANCHORS - 1 ? 255 : cal->anchors[idx + 1];
    int v = cal->anchors[idx] + delta;
    cal->anchors[idx] = (uint8_t)clampi(v, lo, hi);
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
    fprintf(stderr, "\nSaved %s from slot %c\n", REPORT_FILE, variant_name(cal->active_variant));
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
    int prev = variant->anchors[selected > 0 ? selected - 1 : 0];
    int curr = variant->anchors[selected];
    int next = variant->anchors[selected < CAL_ANCHORS - 1 ? selected + 1 : CAL_ANCHORS - 1];
    int span = right - left + 1;
    int split1 = left + span / 4 - 1;
    int split2 = left + span / 2 - 1;
    int split3 = left + (span * 3) / 4 - 1;

    draw_blue_level_rect(left, y0, split1, y1, prev, phase);
    draw_blue_level_rect(split1 + 1, y0, split2, y1, curr, phase);
    draw_blue_level_rect(split2 + 1, y0, split3, y1, next, phase);

    for (int y = y0; y <= y1; y++) {
        for (int x = split3 + 1; x <= right; x++) {
            int checker = (((x >> 2) ^ (y >> 2)) & 1) ? curr : next;
            if (pdm_on_level(x, y, checker, phase)) bpx(x, y);
        }
    }

    bline(split1, y0, split1, y1);
    bline(split2, y0, split2, y1);
    bline(split3, y0, split3, y1);
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
                                  left, right, 28, 47, phase, 1, frame_counter);
            break;
        case WIZARD_STEP_SHADOWS:
            draw_source_patch_row(variant, wizard_shadow_values,
                                  (int)(sizeof(wizard_shadow_values) / sizeof(wizard_shadow_values[0])),
                                  left, right, 28, 47, phase, 2, frame_counter);
            break;
        case WIZARD_STEP_MIDTONES:
            draw_source_patch_row(variant, wizard_midtone_values,
                                  (int)(sizeof(wizard_midtone_values) / sizeof(wizard_midtone_values[0])),
                                  left, right, 28, 47, phase, 3, frame_counter);
            break;
        case WIZARD_STEP_HIGHLIGHTS:
            draw_source_patch_row(variant, wizard_highlight_values,
                                  (int)(sizeof(wizard_highlight_values) / sizeof(wizard_highlight_values[0])),
                                  left, right, 28, 47, phase, 6, frame_counter);
            break;
        case WIZARD_STEP_STABILITY:
            draw_source_checker_rect(variant, left, 28, left + 15, 47, 4, 8, phase, frame_counter);
            draw_source_checker_rect(variant, left + 16, 28, left + 31, 47, 12, 16, phase, frame_counter);
            draw_source_checker_rect(variant, left + 32, 28, left + 47, 47, 224, 232, phase, frame_counter);
            draw_source_checker_rect(variant, left + 48, 28, right, 47, 244, 252, phase, frame_counter);
            break;
        case WIZARD_STEP_OVERVIEW:
        case WIZARD_STEP_REVIEW:
        default:
            draw_anchor_preview(variant, selected, left, right, 28, 47, phase);
            break;
    }
}

static void draw_wizard_preview(const calibration_t *cal, unsigned phase, unsigned frame_counter) {
    draw_wizard_preview_half(&cal->variants[0], cal->selected, (wizard_step_id_t)cal->wizard_step,
                             0, 63, phase, frame_counter);
    draw_wizard_preview_half(&cal->variants[1], cal->selected, (wizard_step_id_t)cal->wizard_step,
                             64, 127, phase, frame_counter);
    bline(63, 28, 63, 47);
    if (cal->active_variant == 0)
        draw_blue_rect_outline(0, 28, 63, 47);
    else
        draw_blue_rect_outline(64, 28, 127, 47);
}

static void draw_calibration_view(const calibration_t *cal, unsigned phase, unsigned frame_counter) {
    char line0[32];
    char line1[32];
    const wizard_step_t *step = &wizard_steps[cal->wizard_step];
    char active = variant_name(cal->active_variant);
    char other = variant_name(cal->active_variant ^ 1);

    snprintf(line0, sizeof(line0), "%d/%d %s", cal->wizard_step + 1, WIZARD_STEP_COUNT, step->title);
    snprintf(line1, sizeof(line1), "%c* %c G%.2f C%.2f", active, other, cal->gamma, cal->gain);
    draw_str(0, 0, line0);
    draw_str(0, 8, line1);

    for (int y = 0; y < 12; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int src = (x * 255) / (WIDTH - 1);
            int level = cal->lut[src];
            if (pdm_on_level(x, y, level, phase)) bpx(x, y);
        }
    }

    for (int i = 0; i < CAL_ANCHORS; i++) {
        int x0 = i * 8;
        int x1 = x0 + 7;
        draw_blue_level_rect(x0, 12, x1, 27, cal->anchors[i], phase);
    }

    int x0 = cal->selected * 8;
    int x1 = x0 + 7;
    bline(x0, 11, x0, 28);
    bline(x1, 11, x1, 28);
    bline(x0, 11, x1, 11);
    bline(x0, 28, x1, 28);

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
            load_variant(cal, 0);
            print_variant_selection(cal);
            break;
        case '2':
            load_variant(cal, 1);
            print_variant_selection(cal);
            break;
        case 'c':
        case 'C':
            fork_to_other_variant(cal);
            break;
        case 'a':
            cal->selected = clampi(cal->selected - 1, 0, CAL_ANCHORS - 1);
            break;
        case 'd':
            cal->selected = clampi(cal->selected + 1, 0, CAL_ANCHORS - 1);
            break;
        case 'j':
            adjust_anchor(cal, -1);
            break;
        case 'k':
            adjust_anchor(cal, +1);
            break;
        case 'J':
            adjust_anchor(cal, -4);
            break;
        case 'K':
            adjust_anchor(cal, +4);
            break;
        case '[':
            cal->gamma = clampf_local(cal->gamma - 0.05f, 0.40f, 3.00f);
            rebuild_anchors_from_curve(cal);
            break;
        case ']':
            cal->gamma = clampf_local(cal->gamma + 0.05f, 0.40f, 3.00f);
            rebuild_anchors_from_curve(cal);
            break;
        case '-':
            cal->gain = clampf_local(cal->gain - 0.05f, 0.20f, 2.50f);
            rebuild_anchors_from_curve(cal);
            break;
        case '=':
        case '+':
            cal->gain = clampf_local(cal->gain + 0.05f, 0.20f, 2.50f);
            rebuild_anchors_from_curve(cal);
            break;
        case ',':
            cal->bias = clampi(cal->bias - 1, -64, 64);
            rebuild_anchors_from_curve(cal);
            break;
        case '.':
            cal->bias = clampi(cal->bias + 1, -64, 64);
            rebuild_anchors_from_curve(cal);
            break;
        case 'r':
        case 'R':
            rebuild_anchors_from_curve(cal);
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

    sync_active_variant(cal);
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
    rebuild_anchors_from_curve(&cal);
    variant_from_current(&cal, &cal.variants[0]);
    cal.variants[1] = cal.variants[0];

    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    if (enter_raw_terminal() != 0) {
        perror("raw terminal");
        return 1;
    }

    print_controls();
    print_wizard_step(&cal);
    print_variant_selection(&cal);
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
