/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-06
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * water.c - Demoscene water simulation on SSD1306 128x64 yellow/blue OLED
 *
 * Yellow zone (top 16px): FPS, time, frame counter
 * Blue zone  (bottom 48px): ripple-tank fluid sim with spray particles
 *
 * Compile:  gcc -O2 -o water water.c -lm
 * Run:      sudo ./water
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
#define TARGET_FPS 60

#define YELLOW_H   16
#define BLUE_Y     16
#define BLUE_H     48

/* Ripple-tank physics */
#define WCOLS      WIDTH
#define DAMP       0.990f
#define SPREAD     0.48f
#define GRAVITY    0.06f
#define PASSES     8

/* Spray particles */
#define MAX_DROPS  80
#define AIR_DRAG   0.015f

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
        0x81, 0x6F, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6, 0xAF
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

/* ------------------------------------------------------------------ */
/*  5x7 font (same compact table)                                     */
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

static void draw_str(int x, int y, const char *s) {
    while (*s) { draw_char(x, y, *s++); x += 6; }
}

/* ------------------------------------------------------------------ */
/*  Water simulation — double-buffered ripple tank                     */
/* ------------------------------------------------------------------ */

static float wh_cur[WCOLS];   /* current height map */
static float wh_prv[WCOLS];   /* previous height map */
static float wv[WCOLS];       /* velocity per column (for spray detection) */

static void water_init(void) {
    for (int i = 0; i < WCOLS; i++) {
        wh_cur[i] = BLUE_H / 2.0f;
        wh_prv[i] = BLUE_H / 2.0f;
        wv[i] = 0;
    }
}

static void water_step(float dt) {
    float wh_new[WCOLS];
    (void)dt;  /* physics is frame-rate-independent via ripple-tank formulation */

    /*
     * Ripple-tank propagation:
     *   new[i] = (left + right) * spread_factor - prev[i]
     * With damping applied to suppress energy over time.
     *
     * Reflective boundaries: mirror the neighbor at edges so waves
     * bounce back instead of being absorbed.
     */
    for (int i = 0; i < WCOLS; i++) {
        float left  = (i > 0)          ? wh_cur[i-1] : wh_cur[1];
        float right = (i < WCOLS - 1)  ? wh_cur[i+1] : wh_cur[WCOLS-2];

        wh_new[i] = (left + right) * SPREAD
                   + wh_cur[i] * (2.0f - 2.0f * SPREAD)
                   - wh_prv[i];
        wh_new[i] *= DAMP;
    }

    /* Multi-pass spreading for smoother wave transport.
     * Alternating left-to-right and right-to-left sweeps
     * prevent directional bias. */
    float ldeltas[WCOLS];
    float rdeltas[WCOLS];
    for (int pass = 0; pass < PASSES; pass++) {
        /* Left-to-right */
        memset(ldeltas, 0, sizeof(ldeltas));
        for (int i = 0; i < WCOLS - 1; i++) {
            float d = SPREAD * 0.25f * (wh_new[i] - wh_new[i+1]);
            ldeltas[i]   -= d;
            ldeltas[i+1] += d;
        }
        for (int i = 0; i < WCOLS; i++) wh_new[i] += ldeltas[i];

        /* Right-to-left */
        memset(rdeltas, 0, sizeof(rdeltas));
        for (int i = WCOLS - 1; i > 0; i--) {
            float d = SPREAD * 0.25f * (wh_new[i] - wh_new[i-1]);
            rdeltas[i]   -= d;
            rdeltas[i-1] += d;
        }
        for (int i = 0; i < WCOLS; i++) wh_new[i] += rdeltas[i];
    }

    /* Compute velocity (used for spray spawning and foam),
     * and clamp heights */
    for (int i = 0; i < WCOLS; i++) {
        wv[i] = (wh_new[i] - wh_prv[i]) * 0.5f;

        if (wh_new[i] < 1.0f)           { wh_new[i] = 1.0f;        wv[i] *= -0.3f; }
        if (wh_new[i] > BLUE_H - 1.0f)  { wh_new[i] = BLUE_H - 1; wv[i] *= -0.3f; }
    }

    /* Swap buffers */
    memcpy(wh_prv, wh_cur, sizeof(wh_cur));
    memcpy(wh_cur, wh_new, sizeof(wh_new));
}

static void water_disturb(int col, float force) {
    if (col < 0) col = 0;
    if (col >= WCOLS) col = WCOLS - 1;
    /* Gaussian splash — affects current buffer directly */
    for (int i = -8; i <= 8; i++) {
        int c = col + i;
        if (c >= 0 && c < WCOLS) {
            float f = force * expf(-(float)(i*i) / 10.0f);
            wh_cur[c] += f;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Spray / droplet particles                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    float x, y, vx, vy;
    int   alive;
    int   big;      /* 1 = 2px drop, 0 = 1px drop */
} drop_t;

static drop_t drops[MAX_DROPS];

static void spawn_drop(float x, float y, float vx, float vy) {
    for (int i = 0; i < MAX_DROPS; i++) {
        if (!drops[i].alive) {
            int big = (fabsf(vy) > 1.5f) ? 1 : 0;
            drops[i] = (drop_t){x, y, vx, vy, 1, big};
            return;
        }
    }
}

static void update_drops(float dt) {
    float s = 60.0f * dt;
    for (int i = 0; i < MAX_DROPS; i++) {
        if (!drops[i].alive) continue;

        /* Air drag (proportional to v²) */
        float speed = sqrtf(drops[i].vx * drops[i].vx +
                            drops[i].vy * drops[i].vy);
        float drag = AIR_DRAG * speed;
        if (speed > 0.001f) {
            drops[i].vx -= drag * (drops[i].vx / speed) * s;
            drops[i].vy -= drag * (drops[i].vy / speed) * s;
        }

        /* Gravity */
        drops[i].vy += GRAVITY * s;

        /* Integrate position */
        drops[i].x += drops[i].vx * s;
        drops[i].y += drops[i].vy * s;

        int col = (int)drops[i].x;
        if (col < 0 || col >= WCOLS) { drops[i].alive = 0; continue; }

        /* Droplet re-enters water surface — create secondary micro-ripple */
        float surface_y = (float)(HEIGHT - 1) - wh_cur[col];
        if (drops[i].y >= surface_y && drops[i].vy > 0) {
            float impact = drops[i].vy * 0.12f;
            wh_cur[col] += impact;
            /* Small splash ring */
            if (col > 0)          wh_cur[col-1] += impact * 0.4f;
            if (col < WCOLS - 1)  wh_cur[col+1] += impact * 0.4f;
            if (col > 1)          wh_cur[col-2] += impact * 0.15f;
            if (col < WCOLS - 2)  wh_cur[col+2] += impact * 0.15f;
            drops[i].alive = 0;
            continue;
        }
        /* Off screen */
        if (drops[i].y < 0 || drops[i].y >= HEIGHT)
            drops[i].alive = 0;
    }
}

static void draw_drops(void) {
    for (int i = 0; i < MAX_DROPS; i++) {
        if (!drops[i].alive) continue;
        int dx = (int)drops[i].x;
        int dy = (int)drops[i].y;
        px(dx, dy);
        if (drops[i].big) {
            /* 2×2 drop for bigger splash particles */
            px(dx + 1, dy);
            px(dx, dy + 1);
            px(dx + 1, dy + 1);
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Dithered water rendering with highlights & caustics               */
/* ------------------------------------------------------------------ */

static const uint8_t dither4x4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5}
};

static void draw_water(int frame) {
    for (int col = 0; col < WCOLS; col++) {
        int h = (int)(wh_cur[col] + 0.5f);
        if (h < 0) h = 0;
        if (h > BLUE_H) h = BLUE_H;

        int surface_y = HEIGHT - 1 - h;
        float vel = fabsf(wv[col]);

        /* ---- Surface foam band ---- */
        /* Foam width scales with surface velocity */
        int foam_rows = (vel > 0.8f) ? 3 : 2;

        /* Draw foam band (always solid or near-solid at surface) */
        for (int r = 0; r < foam_rows && surface_y + r < HEIGHT; r++) {
            px(col, surface_y + r);
        }

        /* ---- Filled water body with depth-based dithering ---- */
        for (int y = surface_y + foam_rows; y < HEIGHT; y++) {
            int depth = y - surface_y;

            /* Base density increases with depth */
            int threshold;
            if (depth < 4)
                threshold = 10;    /* sparse near surface */
            else if (depth < 10)
                threshold = 5;     /* medium density */
            else if (depth < 20)
                threshold = 3;     /* quite dense */
            else
                threshold = 1;     /* nearly solid at bottom */

            /* Sub-surface caustic effect: slowly drifting light bands
             * that respond to the local wave height above.
             * Uses two sine waves at incommensurate frequencies
             * to create a non-periodic shimmering pattern. */
            float caustic = sinf((float)col * 0.15f +
                                 (float)frame * 0.04f +
                                 wh_cur[col > 0 ? col-1 : 0] * 0.3f)
                          * cosf((float)depth * 0.25f -
                                 (float)frame * 0.025f);

            /* Brighten (lower threshold) where caustic is positive */
            if (caustic > 0.3f && depth > 3 && depth < 25) {
                threshold -= 2;
                if (threshold < 0) threshold = 0;
            }
            /* Darken slightly in caustic troughs for contrast */
            if (caustic < -0.5f && depth > 5) {
                threshold += 3;
                if (threshold > 15) threshold = 15;
            }

            if (dither4x4[y & 3][col & 3] >= (unsigned)threshold)
                px(col, y);
        }

        /* ---- Surface highlight on wave crests ---- */
        /* Bright line 1px above surface on peaks (high water columns) */
        float avg_h = BLUE_H / 2.0f;
        if (wh_cur[col] > avg_h + 2.0f && surface_y - 1 >= BLUE_Y) {
            /* Stippled highlight above surface for reflected-light effect */
            if ((col + frame) & 1)
                px(col, surface_y - 1);
        }

        /* ---- Spawn spray from fast-moving peaks ---- */
        /* Probability scales with velocity for more natural bursts */
        if (wv[col] > 1.2f) {
            int spawn_chance = (int)(wv[col] * 4.0f);
            if (spawn_chance > 15) spawn_chance = 15;
            if ((rand() & 15) < spawn_chance - 4) {
                float sx = col + ((rand() % 100) - 50) * 0.02f;
                float svx = ((rand() % 100) - 50) * 0.02f;
                float svy = -wv[col] * (0.5f + (rand() % 40) * 0.01f);
                spawn_drop(sx, (float)surface_y, svx, svy);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Sine scroller for demoscene flair                                  */
/* ------------------------------------------------------------------ */

static void draw_sine_separator(int y, int frame) {
    for (int x = 0; x < WIDTH; x++) {
        int sy = y + (int)(1.5f * sinf((float)x * 0.08f + (float)frame * 0.05f));
        px(x, sy);
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
    srand((unsigned)time(NULL));

    water_init();
    memset(drops, 0, sizeof(drops));

    struct timespec t0, t1;
    long frame_ns = 1000000000L / TARGET_FPS;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    int frame = 0;
    float fps = 0;
    float disturb_phase = 0;

    while (running) {
        clock_gettime(CLOCK_MONOTONIC, &t0);
        float dt = 1.0f / TARGET_FPS;

        /* ---- Organic disturbances ---- */
        disturb_phase += dt;

        /* Multi-octave noise-like disturbances using incommensurate
         * sine frequencies — produces pseudo-random, non-repeating
         * patterns that feel organic */
        float wave1 = sinf(disturb_phase * 0.7f) * cosf(disturb_phase * 0.31f);
        float wave2 = sinf(disturb_phase * 1.3f + 2.0f);
        float wave3 = cosf(disturb_phase * 0.41f + 1.0f);
        float wave4 = sinf(disturb_phase * 2.17f) * 0.5f;
        float wave5 = cosf(disturb_phase * 0.13f);  /* very slow tide */

        /* Slow tidal oscillation — broad, gentle rise/fall */
        {
            float tide = wave5 * 0.8f;
            for (int i = 0; i < WCOLS; i++) {
                float spatial = sinf((float)i * 3.14159f / (float)WCOLS
                                     + disturb_phase * 0.1f);
                wh_cur[i] += tide * spatial * dt * 2.0f;
            }
        }

        /* Big slow slosh */
        if (fabsf(wave1) > 0.82f) {
            int col = (int)((0.5f + 0.45f * sinf(disturb_phase * 0.2f)) * WCOLS);
            water_disturb(col, wave1 * 3.0f);
        }

        /* Medium ripples */
        if (wave2 > 0.65f) {
            int col = (int)((0.5f + 0.4f * cosf(disturb_phase * 0.9f)) * WCOLS);
            water_disturb(col, wave2 * 1.8f);
        }

        /* Small high-frequency ripples */
        if (wave4 > 0.3f) {
            int col = (int)((0.5f + 0.35f * sinf(disturb_phase * 1.7f)) * WCOLS);
            water_disturb(col, wave4 * 1.2f);
        }

        /* Random drips with varying intensity */
        if ((rand() & 31) == 0) {
            int col = rand() % WCOLS;
            float force = ((rand() % 100) - 30) * 0.05f;
            water_disturb(col, force);
        }

        /* Occasional big splash from the sides */
        if (fabsf(wave3) > 0.93f && (frame & 15) == 0) {
            int side = wave3 > 0 ? 4 : WCOLS - 5;
            water_disturb(side, wave3 * 5.0f);
        }

        /* Physics */
        water_step(dt);
        update_drops(dt);

        /* -- Render -- */
        memset(fb, 0, sizeof(fb));

        /* Yellow zone: status bar */
        char hud1[24], hud2[24];
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);

        snprintf(hud1, sizeof(hud1), "%02d:%02d:%02d",
                 tm->tm_hour, tm->tm_min, tm->tm_sec);
        snprintf(hud2, sizeof(hud2), "FPS:%2.0f  frm:%05d", fps, frame);

        draw_str(1, 0, hud1);

        /* Scrolling label in yellow zone */
        {
            const char *msg = "~ WATER DEMO ~";
            int len = (int)strlen(msg) * 6;
            int ox = WIDTH - 1 - ((frame * 2) % (len + WIDTH));
            draw_str(ox, 0, msg);
        }

        draw_str(1, 8, hud2);

        /* Wavy separator between yellow and blue */
        draw_sine_separator(YELLOW_H - 1, frame);

        /* Blue zone: water + spray */
        draw_water(frame);
        draw_drops();

        flush();
        frame++;

        /* FPS measurement */
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long elapsed = (t1.tv_sec - t0.tv_sec) * 1000000000L
                      + (t1.tv_nsec - t0.tv_nsec);
        if (elapsed > 0)
            fps = fps * 0.9f + 0.1f * (1000000000.0f / (float)elapsed);

        /* Frame pacing */
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
