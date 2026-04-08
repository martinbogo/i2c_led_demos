/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-06
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * horizon.c - Glass-cockpit PFD on SSD1306 128x64 yellow/blue OLED
 *
 * Yellow zone (top 16px): heading compass ribbon, time, FPS
 * Blue zone  (bottom 48px):
 *   Left:   airspeed tape
 *   Center: attitude indicator with pitch ladder, bank arc, slip ball
 *   Right:  altitude tape + vertical speed indicator
 *
 * Simulates gentle flight manoeuvres with smooth noise.
 *
 * Compile:  gcc -O2 -o horizon horizon.c -lm
 * Run:      sudo ./horizon
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

#define WIDTH       128
#define HEIGHT      64
#define PAGES       (HEIGHT / 8)
#define ADDR        0x3C
#define TARGET_FPS  30

/* Layout geometry */
#define YELLOW_H    16

/* Tape widths */
#define TAPE_W      18    /* width of speed/alt tapes */
#define VSI_W       6     /* vertical speed indicator bar width */

/* Attitude indicator: centered between tapes */
#define AI_LEFT     (TAPE_W + 1)
#define AI_RIGHT    (WIDTH - TAPE_W - VSI_W - 2)
#define AI_W        (AI_RIGHT - AI_LEFT)
#define AI_CX       (AI_LEFT + AI_W / 2)
#define AI_Y        YELLOW_H
#define AI_H        48
#define AI_CY       (AI_Y + AI_H / 2)
#define AI_R        (AI_W / 2 - 1)

/* Slip ball */
#define SLIP_Y      (HEIGHT - 5)
#define SLIP_TUBE_W 20

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int i2c_fd;
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

/* ------------------------------------------------------------------ */
/*  I2C / SSD1306                                                      */
/* ------------------------------------------------------------------ */

static void init_display(void) {
    uint8_t cmds[] = {
        0x00, 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
    };
    write(i2c_fd, cmds, sizeof(cmds));
}

static void flush(void) {
    uint8_t ac[] = {0x00, 0x21, 0, WIDTH-1, 0x22, 0, PAGES-1};
    write(i2c_fd, ac, sizeof(ac));
    for (int i = 0; i < WIDTH * PAGES; i += 32) {
        uint8_t buf[33];
        buf[0] = 0x40;
        int len = WIDTH * PAGES - i;
        if (len > 32) len = 32;
        memcpy(buf + 1, fb + i, len);
        write(i2c_fd, buf, len + 1);
    }
}

static void px(int x, int y) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
        fb[x + (y / 8) * WIDTH] |= 1 << (y & 7);
}

static void clear_px(int x, int y) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
        fb[x + (y / 8) * WIDTH] &= ~(1 << (y & 7));
}

/* ------------------------------------------------------------------ */
/*  5x7 font                                                           */
/* ------------------------------------------------------------------ */

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
            if (bits & (1 << row)) px(x + col, y + row);
    }
}

static void __attribute__((unused)) draw_str(int x, int y, const char *s) {
    while (*s) { draw_char(x, y, *s++); x += 6; }
}

/* Clipped draw_char: only pixels within [cx0..cx1] horizontally */
static void draw_char_clip(int x, int y, char c, int cx0, int cx1) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        int sx = x + col;
        if (sx < cx0 || sx > cx1) continue;
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++)
            if (bits & (1 << row)) px(sx, y + row);
    }
}

static void draw_str_clip(int x, int y, const char *s, int cx0, int cx1) {
    while (*s) { draw_char_clip(x, y, *s++, cx0, cx1); x += 6; }
}

/* ------------------------------------------------------------------ */
/*  Drawing primitives                                                 */
/* ------------------------------------------------------------------ */

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

static void draw_hline(int x0, int x1, int y) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++) px(x, y);
}

static void draw_vline(int x, int y0, int y1) {
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) px(x, y);
}

static void __attribute__((unused)) draw_rect(int x0, int y0, int x1, int y1) {
    draw_hline(x0, x1, y0);
    draw_hline(x0, x1, y1);
    draw_vline(x0, y0, y1);
    draw_vline(x1, y0, y1);
}

/* ------------------------------------------------------------------ */
/*  Smooth noise for flight simulation                                 */
/* ------------------------------------------------------------------ */

static float smooth_val(float t, float s1, float s2, float s3, float s4) {
    return sinf(t * s1) * 0.4f
         + sinf(t * s2 + 1.7f) * 0.3f
         + sinf(t * s3 + 3.1f) * 0.2f
         + cosf(t * s4 + 0.5f) * 0.1f;
}

/* ------------------------------------------------------------------ */
/*  Inverted drawing helpers (black on filled yellow)                   */
/* ------------------------------------------------------------------ */

static void fill_yellow(void) {
    /* Fill entire yellow zone solid (all pixels on = bright yellow bar) */
    for (int y = 0; y < YELLOW_H; y++)
        for (int x = 0; x < WIDTH; x++)
            px(x, y);
}

/* Draw character as black cutout on filled background */
static void draw_char_inv(int x, int y, char c) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++)
            if (bits & (1 << row)) clear_px(x + col, y + row);
    }
}

static void draw_str_inv(int x, int y, const char *s) {
    while (*s) { draw_char_inv(x, y, *s++); x += 6; }
}

/* Clear a rectangular area within the yellow zone (for compass ribbon) */
static void clear_rect(int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            clear_px(x, y);
}

/* ------------------------------------------------------------------ */
/*  Yellow status bar: heading, compass ribbon, speed, alt, time       */
/* ------------------------------------------------------------------ */

static void draw_yellow_bar(float heading, float airspeed, float altitude) {
    fill_yellow();

    char buf[24];
    int hdg_int = ((int)(heading + 0.5f)) % 360;

    /* Row 1: SPD | heading digits | ALT */
    snprintf(buf, sizeof(buf), "%3dkt", (int)(airspeed + 0.5f));
    draw_str_inv(0, 1, buf);

    /* Heading number centered */
    snprintf(buf, sizeof(buf), "%03d", hdg_int);
    draw_str_inv(WIDTH / 2 - 8, 1, buf);

    /* Altitude top-right */
    snprintf(buf, sizeof(buf), "%5d", (int)(altitude + 0.5f));
    draw_str_inv(WIDTH - 30, 1, buf);

    /* Row 2: heading compass ribbon (cut out of filled bar) */
    int ribbon_y0 = 9;
    int ribbon_y1 = YELLOW_H - 1;

    /* Clear ribbon strip so ticks appear as lit pixels on dark */
    clear_rect(0, ribbon_y0, WIDTH - 1, ribbon_y1);

    /* Center pointer triangle (filled/lit) */
    int rcx = WIDTH / 2;
    px(rcx, ribbon_y0);
    px(rcx - 1, ribbon_y0 + 1);
    px(rcx + 1, ribbon_y0 + 1);

    /* Scrolling compass ticks */
    float ppd = 1.5f;
    for (int deg = -90; deg <= 90; deg++) {
        float hdg = heading + (float)deg;
        if (hdg < 0)    hdg += 360;
        if (hdg >= 360) hdg -= 360;

        int sx = rcx + (int)((float)deg * ppd);
        if (sx < 0 || sx >= WIDTH) continue;

        int hdg_i = ((int)(hdg + 0.5f)) % 360;
        if (hdg_i < 0) hdg_i += 360;

        if (hdg_i % 30 == 0) {
            /* Major tick */
            draw_vline(sx, ribbon_y0 + 2, ribbon_y1);
            /* Cardinal labels */
            const char *lbl = NULL;
            switch (hdg_i) {
                case 0:   lbl = "N"; break;
                case 90:  lbl = "E"; break;
                case 180: lbl = "S"; break;
                case 270: lbl = "W"; break;
            }
            if (lbl)
                draw_char(sx - 2, ribbon_y0 + 2, lbl[0]);
        } else if (hdg_i % 10 == 0) {
            /* Minor tick */
            draw_vline(sx, ribbon_y0 + 4, ribbon_y1);
        }
    }

}

/* ------------------------------------------------------------------ */
/*  Airspeed tape (left side of blue zone)                             */
/* ------------------------------------------------------------------ */

static void draw_speed_tape(float speed) {
    int x0 = 0;
    int x1 = TAPE_W - 1;
    int y0 = YELLOW_H;
    int y1 = HEIGHT - 1;
    int cy = (y0 + y1) / 2;

    /* Border */
    draw_vline(x1, y0, y1);

    /* Scrolling tape: 1 pixel per knot */
    float ppk = 1.0f;
    for (int kts = -60; kts <= 60; kts++) {
        float spd = speed + (float)kts;
        if (spd < 0) continue;
        int sy = cy - (int)((float)kts * ppk);
        if (sy < y0 || sy > y1) continue;

        int spd_i = (int)(spd + 0.5f);

        if (spd_i % 20 == 0) {
            /* Major tick + number */
            draw_hline(x1 - 4, x1, sy);
            char buf[6];
            snprintf(buf, sizeof(buf), "%3d", spd_i);
            draw_str_clip(x0, sy - 3, buf, x0, x1 - 5);
        } else if (spd_i % 10 == 0) {
            /* Medium tick */
            draw_hline(x1 - 3, x1, sy);
        } else if (spd_i % 5 == 0) {
            /* Minor tick */
            draw_hline(x1 - 1, x1, sy);
        }
    }

    /* Current value pointer: filled arrow pointing right */
    px(x1 - 1, cy);
    draw_hline(x1 - 3, x1 - 1, cy - 1);
    draw_hline(x1 - 3, x1 - 1, cy + 1);
    px(x1 - 4, cy - 2);
    px(x1 - 4, cy + 2);

    /* Readout box */
    {
        char buf[6];
        snprintf(buf, sizeof(buf), "%3d", (int)(speed + 0.5f));
        /* Black out background area */
        for (int yy = cy - 4; yy <= cy + 4; yy++)
            for (int xx = x0; xx <= x1 - 5; xx++)
                clear_px(xx, yy);
        draw_str_clip(x0, cy - 3, buf, x0, x1 - 5);
    }
}

/* ------------------------------------------------------------------ */
/*  Altitude tape (right side of blue zone)                            */
/* ------------------------------------------------------------------ */

static void draw_alt_tape(float alt) {
    int x0 = WIDTH - TAPE_W - VSI_W;
    int x1 = WIDTH - VSI_W - 1;
    int y0 = YELLOW_H;
    int y1 = HEIGHT - 1;
    int cy = (y0 + y1) / 2;

    /* Border */
    draw_vline(x0, y0, y1);

    /* Scrolling tape: 1 pixel per 50 feet */
    float ppf = 1.0f / 50.0f;
    for (int ft = -3000; ft <= 3000; ft += 10) {
        float a = alt + (float)ft;
        if (a < 0) continue;
        int sy = cy - (int)((float)ft * ppf);
        if (sy < y0 || sy > y1) continue;

        int a_i = (int)(a + 0.5f);
        /* Round to nearest 10 */
        a_i = (a_i / 10) * 10;

        if (a_i % 500 == 0) {
            /* Major tick + number */
            draw_hline(x0, x0 + 4, sy);
            char buf[12];
            if (a_i >= 10000)
                snprintf(buf, sizeof(buf), "%d", a_i / 1000);
            else
                snprintf(buf, sizeof(buf), "%d", a_i);
            draw_str_clip(x0 + 5, sy - 3, buf, x0 + 1, x1);
        } else if (a_i % 100 == 0) {
            /* Minor tick */
            draw_hline(x0, x0 + 2, sy);
        }
    }

    /* Current value pointer: filled arrow pointing left */
    px(x0 + 1, cy);
    draw_hline(x0 + 1, x0 + 3, cy - 1);
    draw_hline(x0 + 1, x0 + 3, cy + 1);
    px(x0 + 4, cy - 2);
    px(x0 + 4, cy + 2);

    /* Readout box */
    {
        char buf[12];
        snprintf(buf, sizeof(buf), "%5d", (int)(alt + 0.5f));
        for (int yy = cy - 4; yy <= cy + 4; yy++)
            for (int xx = x0 + 5; xx <= x1; xx++)
                clear_px(xx, yy);
        draw_str_clip(x0 + 5, cy - 3, buf, x0 + 5, x1);
    }
}

/* ------------------------------------------------------------------ */
/*  Vertical speed indicator (far right strip)                         */
/* ------------------------------------------------------------------ */

static void draw_vsi(float vs) {
    int x0 = WIDTH - VSI_W;
    int x1 = WIDTH - 1;
    int y0 = YELLOW_H;
    int y1 = HEIGHT - 1;
    int cy = (y0 + y1) / 2;

    draw_vline(x0, y0, y1);
    draw_hline(x0, x1, cy);  /* zero line */

    /* Scale: full range = +/- 2000 fpm, half height = 24px */
    float ppp = 24.0f / 2000.0f;
    int bar_h = (int)(fabsf(vs) * ppp);
    if (bar_h > 23) bar_h = 23;

    if (vs > 0) {
        /* Climbing: bar goes up from center */
        for (int y = cy - bar_h; y < cy; y++)
            for (int x = x0 + 1; x <= x1; x++)
                px(x, y);
    } else {
        /* Descending: bar goes down from center */
        for (int y = cy + 1; y <= cy + bar_h; y++)
            for (int x = x0 + 1; x <= x1; x++)
                px(x, y);
    }

    /* Tick marks at 1000 fpm */
    int y_1k = cy - (int)(1000.0f * ppp);
    draw_hline(x0, x0 + 2, y_1k);
    int y_m1k = cy + (int)(1000.0f * ppp);
    draw_hline(x0, x0 + 2, y_m1k);
}

/* ------------------------------------------------------------------ */
/*  Attitude indicator (center of blue zone)                           */
/* ------------------------------------------------------------------ */

static void draw_attitude(float pitch_deg, float bank_deg, float slip) {
    float bank_rad = bank_deg * (float)M_PI / 180.0f;
    float cb = cosf(bank_rad);
    float sb = sinf(bank_rad);
    float ppd = 0.8f;
    float pitch_offset = pitch_deg * ppd;

    /* Sky/ground fill within bezel */
    for (int sy = AI_Y; sy < AI_Y + AI_H; sy++) {
        for (int sx = AI_LEFT; sx <= AI_RIGHT; sx++) {
            float dx = (float)(sx - AI_CX);
            float dy = (float)(sy - AI_CY);
            if (dx * dx + dy * dy > AI_R * AI_R) continue;

            float ry = -dx * sb + dy * cb;
            float horizon_y = -pitch_offset;

            if (ry > horizon_y) {
                int depth = (int)(ry - horizon_y);
                if ((sx + sy) & 1) px(sx, sy);
                if (depth > 8) px(sx, sy);
            }
        }
    }

    /* Horizon line */
    for (int i = -AI_R; i <= AI_R; i++) {
        float fx = AI_CX + (float)i * cb + pitch_offset * sb;
        float fy = AI_CY + (float)i * sb - pitch_offset * cb;
        int ix = (int)(fx + 0.5f);
        int iy = (int)(fy + 0.5f);
        int ddx = ix - AI_CX, ddy = iy - AI_CY;
        if (ddx * ddx + ddy * ddy <= AI_R * AI_R)
            px(ix, iy);
    }

    /* Pitch ladder: every 10 degrees */
    for (int pdeg = -30; pdeg <= 30; pdeg += 10) {
        if (pdeg == 0) continue;
        float py_off = (pitch_deg - (float)pdeg) * ppd;
        int half_w = (abs(pdeg) >= 20) ? 6 : 9;

        for (int i = -half_w; i <= half_w; i++) {
            if (abs(i) < 2) continue;
            float fx = AI_CX + (float)i * cb + py_off * sb;
            float fy = AI_CY + (float)i * sb - py_off * cb;
            int ix = (int)(fx + 0.5f);
            int iy = (int)(fy + 0.5f);
            int ddx = ix - AI_CX, ddy = iy - AI_CY;
            if (ddx * ddx + ddy * ddy <= (AI_R - 2) * (AI_R - 2))
                px(ix, iy);
        }

        /* End ticks */
        int tick_dir = (pdeg > 0) ? 1 : -1;
        for (int side = -1; side <= 1; side += 2) {
            float ex = AI_CX + (float)(side * half_w) * cb + py_off * sb;
            float ey = AI_CY + (float)(side * half_w) * sb - py_off * cb;
            for (int t = 0; t < 3; t++) {
                float tx = ex - (float)(t * tick_dir) * sb;
                float ty = ey + (float)(t * tick_dir) * cb;
                int ix = (int)(tx + 0.5f);
                int iy = (int)(ty + 0.5f);
                int ddx = ix - AI_CX, ddy = iy - AI_CY;
                if (ddx * ddx + ddy * ddy <= (AI_R - 2) * (AI_R - 2))
                    px(ix, iy);
            }
        }
    }

    /* Bank arc and ticks at top */
    {
        /* Fixed index triangle */
        px(AI_CX, AI_CY - AI_R + 1);
        px(AI_CX - 1, AI_CY - AI_R + 3);
        px(AI_CX + 1, AI_CY - AI_R + 3);
        px(AI_CX, AI_CY - AI_R + 2);

        /* Bank tick marks */
        float ticks[] = {-60,-45,-30,-20,-10, 0, 10, 20, 30, 45, 60};
        for (int i = 0; i < 11; i++) {
            float a = (ticks[i] - 90.0f) * (float)M_PI / 180.0f;
            int len = (ticks[i] == 0 || fabsf(ticks[i]) == 30.0f ||
                       fabsf(ticks[i]) == 60.0f) ? 3 : 2;
            float r1 = AI_R - 1;
            float r2 = AI_R - 1 - len;
            draw_line(AI_CX + (int)(r1 * cosf(a)),
                      AI_CY + (int)(r1 * sinf(a)),
                      AI_CX + (int)(r2 * cosf(a)),
                      AI_CY + (int)(r2 * sinf(a)));
        }

        /* Bank pointer (rotates with bank) */
        float a = (-bank_deg - 90.0f) * (float)M_PI / 180.0f;
        float r1 = AI_R - 1;
        float r2 = AI_R - 5;
        int bx1 = AI_CX + (int)(r1 * cosf(a));
        int by1 = AI_CY + (int)(r1 * sinf(a));
        int bx2 = AI_CX + (int)(r2 * cosf(a));
        int by2 = AI_CY + (int)(r2 * sinf(a));
        draw_line(bx1, by1, bx2, by2);
    }

    /* Clip to circular bezel */
    for (int y = AI_Y; y < AI_Y + AI_H; y++) {
        for (int x = AI_LEFT; x <= AI_RIGHT; x++) {
            int ddx = x - AI_CX, ddy = y - AI_CY;
            if (ddx * ddx + ddy * ddy > AI_R * AI_R)
                clear_px(x, y);
        }
    }

    /* Bezel circle */
    {
        int r = AI_R;
        int x = r, y = 0, err = 1 - r;
        while (x >= y) {
            px(AI_CX+x, AI_CY+y); px(AI_CX+y, AI_CY+x);
            px(AI_CX-y, AI_CY+x); px(AI_CX-x, AI_CY+y);
            px(AI_CX-x, AI_CY-y); px(AI_CX-y, AI_CY-x);
            px(AI_CX+y, AI_CY-x); px(AI_CX+x, AI_CY-y);
            y++;
            if (err < 0) err += 2*y+1;
            else { x--; err += 2*(y-x)+1; }
        }
    }

    /* Fixed aircraft symbol */
    px(AI_CX, AI_CY);
    px(AI_CX-1, AI_CY); px(AI_CX+1, AI_CY);
    px(AI_CX, AI_CY-1); px(AI_CX, AI_CY+1);

    /* Wings */
    draw_hline(AI_CX - 3, AI_CX - 9, AI_CY);
    draw_vline(AI_CX - 9, AI_CY, AI_CY + 2);
    draw_hline(AI_CX + 3, AI_CX + 9, AI_CY);
    draw_vline(AI_CX + 9, AI_CY, AI_CY + 2);

    /* Slip/skid ball at bottom of bezel */
    {
        int tube_cx = AI_CX;
        int tube_y = AI_CY + AI_R - 4;
        /* Tube outline */
        draw_hline(tube_cx - SLIP_TUBE_W/2, tube_cx + SLIP_TUBE_W/2, tube_y - 2);
        draw_hline(tube_cx - SLIP_TUBE_W/2, tube_cx + SLIP_TUBE_W/2, tube_y + 2);
        /* Center marks */
        draw_vline(tube_cx - 3, tube_y - 2, tube_y + 2);
        draw_vline(tube_cx + 3, tube_y - 2, tube_y + 2);

        /* Ball position: slip in degrees, scale to pixels */
        int ball_x = tube_cx + (int)(slip * 1.5f);
        if (ball_x < tube_cx - SLIP_TUBE_W/2 + 2)
            ball_x = tube_cx - SLIP_TUBE_W/2 + 2;
        if (ball_x > tube_cx + SLIP_TUBE_W/2 - 2)
            ball_x = tube_cx + SLIP_TUBE_W/2 - 2;

        /* Filled ball (3px wide) */
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++)
                px(ball_x + dx, tube_y + dy);
    }
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

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
    i2c_fd = open("/dev/i2c-1", O_RDWR);
    if (i2c_fd < 0) { perror("open i2c"); return 1; }
    if (ioctl(i2c_fd, I2C_SLAVE, ADDR) < 0) { perror("ioctl"); return 1; }

    init_display();
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    long frame_ns = 1000000000L / TARGET_FPS;
    struct timespec next, t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &next);

    float fps = 0;
    float heading = 270.0f;
    float altitude = 4500.0f;
    float airspeed = 120.0f;
    float vs = 0;
    float t = 0;

    while (running) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        float dt = 1.0f / TARGET_FPS;
        t += dt;

        /* Simulate flight: smooth organic manoeuvres */
        float pitch = 8.0f  * smooth_val(t, 0.23f, 0.41f, 0.67f, 0.13f)
                    + 4.0f  * smooth_val(t, 0.87f, 1.13f, 0.31f, 0.53f);
        float bank  = 25.0f * smooth_val(t, 0.19f, 0.37f, 0.71f, 0.11f)
                    + 10.0f * smooth_val(t, 0.53f, 0.91f, 0.29f, 0.77f);

        /* Slip: slight uncoordinated flight feel */
        float slip = 3.0f * smooth_val(t, 0.61f, 1.07f, 0.43f, 0.83f);

        /* Heading changes with bank (coordinated turn) */
        heading += bank * 0.003f * 60.0f * dt;
        if (heading < 0)    heading += 360.0f;
        if (heading >= 360) heading -= 360.0f;

        /* Vertical speed from pitch */
        float target_vs = pitch * 80.0f;  /* fpm per degree */
        vs += (target_vs - vs) * 0.02f;

        /* Altitude integrates vertical speed */
        altitude += vs * dt / 60.0f;
        if (altitude < 0) altitude = 0;

        /* Airspeed wanders */
        airspeed = 120.0f + 20.0f * smooth_val(t, 0.17f, 0.29f, 0.53f, 0.07f)
                          + 8.0f  * smooth_val(t, 0.71f, 1.03f, 0.37f, 0.61f);
        if (airspeed < 60) airspeed = 60;

        /* -- Render -- */
        memset(fb, 0, sizeof(fb));

        /* Yellow zone: inverted status bar */
        draw_yellow_bar(heading, airspeed, altitude);

        /* Blue zone instruments */
        draw_speed_tape(airspeed);
        draw_alt_tape(altitude);
        draw_vsi(vs);
        draw_attitude(pitch, bank, slip);

        flush();

        /* FPS */
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L
                      + (t1.tv_nsec - t0.tv_nsec);
        if (elapsed > 0)
            fps = fps * 0.9f + 0.1f * (1000000000.0f / (float)elapsed);

        next.tv_nsec += frame_ns;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }

    memset(fb, 0, sizeof(fb));
    flush();
    uint8_t off[2] = {0x00, 0xAE};
    write(i2c_fd, off, 2);
    close(i2c_fd);
    return 0;
}
