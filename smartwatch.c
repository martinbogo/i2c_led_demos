/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-06
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * smartwatch.c - "Orbital Particle Swarm" tactical watch face
 *
 * Modified for OLED Burn-In Protection:
 * - Yellow zone is dark/positive drawing (no inverted headers)
 * - Orbital progress rings are rotating segmented blobs 
 * - Digital time features slow pixel-shifting drift
 *
 * Compile:  gcc -O2 -o smartwatch smartwatch.c -lm
 * Run:      sudo ./smartwatch
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

#define YELLOW_H    16
#define BLUE_Y      16

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Swarm constraints */
#define P_PER_DIGIT 280
#define NUM_DIGITS  4   /* HH MM */

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

static void draw_str(int x, int y, const char *s) {
    while (*s) { draw_char(x, y, *s++); x += 6; }
}

/* ------------------------------------------------------------------ */
/*  Drawing primitives                                                 */
/* ------------------------------------------------------------------ */

static void fill_rect(int x0, int y0, int x1, int y1) {
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            px(x, y);
}

static void draw_organic_arc(int cx, int cy, float r, float a0, float a1, float max_r, int is_comet) {
    while (a1 < a0) a1 += 360.0f;
    for (float a = a0; a <= a1; a += 0.5f) {
        float rad = a * (float)M_PI / 180.0f;
        float progress = (a - a0) / (a1 - a0);
        float taper = is_comet ? (progress * progress) : sinf(progress * (float)M_PI);
        
        int draw_r = (int)(max_r * taper + 0.5f);
        int px_cx = cx + (int)(r * cosf(rad) + 0.5f);
        int py_cy = cy + (int)(r * sinf(rad) + 0.5f);
        
        for (int tx = -draw_r; tx <= draw_r; tx++) {
            for (int ty = -draw_r; ty <= draw_r; ty++) {
                if (tx*tx + ty*ty <= draw_r*draw_r) {
                    px(px_cx + tx, py_cy + ty);
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Smooth noise                                                       */
/* ------------------------------------------------------------------ */

static float smooth_val(float t, float s1, float s2, float s3) {
    return sinf(t*s1)*0.5f + sinf(t*s2+1.7f)*0.3f + cosf(t*s3+0.5f)*0.2f;
}

/* ------------------------------------------------------------------ */
/*  ECG & Yellow Zone (Burn-in Safe: No background inversion)          */
/* ------------------------------------------------------------------ */

#define ECG_LEN 84
static float ecg_buffer[ECG_LEN];

static float ecg_sample(float phase) {
    float v = 0;
    v += 0.15f * expf(-powf((phase-0.10f)*30,2));
    v -= 0.10f * expf(-powf((phase-0.18f)*50,2));
    v += 1.00f * expf(-powf((phase-0.22f)*60,2));
    v -= 0.20f * expf(-powf((phase-0.26f)*50,2));
    v += 0.25f * expf(-powf((phase-0.38f)*18,2));
    return v;
}

static void draw_yellow_hud(int batt, float ecg_phase, float sim_t) {
    /* Drawing positive to prevent burn-in (no fill_rect) */

    /* Organic moving DNA Double-Helix battery gauge */
    int bx_start = 2;
    int max_bx = 22;
    int fill_w = bx_start + (int)((float)batt / 100.0f * (max_bx - bx_start));
    
    for (int x = bx_start; x < fill_w; x++) {
        float phase = x * 0.5f - sim_t * 6.0f;
        int y1 = 7 + (int)(sinf(phase) * 4.0f);
        int y2 = 7 + (int)(sinf(phase + (float)M_PI) * 4.0f);
        
        px(x, y1);
        px(x, y2);
        
        /* Draw rungs between strands to complete the organic helix look */
        if (((x + (int)(sim_t * 12.0f)) % 6) == 0) {
            int starty = y1 < y2 ? y1 : y2;
            int endy = y1 > y2 ? y1 : y2;
            if (endy - starty > 1) {
                for (int ry = starty + 1; ry < endy; ry++) px(x, ry);
            }
        }
    }
    
    /* Text next to it */
    char btext[16];
    snprintf(btext, sizeof(btext), "%d%%", batt);
    draw_str(25, 4, btext);
    
    /* ECG Scroller on the right */
    int ecg_x = 44;
    
    /* Shift buffer */
    for (int i = 0; i < ECG_LEN - 1; i++) {
        ecg_buffer[i] = ecg_buffer[i+1];
    }
    ecg_buffer[ECG_LEN-1] = ecg_sample(ecg_phase);
    
    /* Draw wave positively */
    for (int i = 0; i < ECG_LEN - 1; i++) {
        int y1 = 10 - (int)(ecg_buffer[i] * 6.0f);
        int y2 = 10 - (int)(ecg_buffer[i+1] * 6.0f);
        int x0 = ecg_x + i;
        int x1 = ecg_x + i + 1;
        
        int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
        int dy = -abs(y2 - y1), sy = y1 < y2 ? 1 : -1;
        int err = dx + dy, e2;
        int cx = x0, cy = y1;
        for (;;) {
            px(cx, cy);
            /* Double thickness for visibility */
            px(cx, cy+1); 
            if (cx == x1 && cy == y2) break;
            e2 = 2 * err;
            if (e2 >= dy) { err += dy; cx += sx; }
            if (e2 <= dx) { err += dx; cy += sy; }
        }
    }

    /* Small scrolling tracking pip line under ECG to keep moving pixels */
    int pip_offset = (int)(sim_t * 15.0f) % 4;
    for (int x = ecg_x - pip_offset; x < WIDTH; x += 4) {
        if (x >= ecg_x) px(x, 15);
    }
}

/* ------------------------------------------------------------------ */
#define BIG_W 16
#define BIG_H 26
#define BIG_THK 3

typedef struct {
    float x, y;
    float vx, vy;
    float tx, ty;
} particle_t;

static particle_t swarms[NUM_DIGITS][P_PER_DIGIT];
static int current_digits[NUM_DIGITS];

static void add_thick_line(int x0, int y0, int x1, int y1, int thk, uint8_t *shape) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    int cx = x0, cy = y0;
    for (;;) {
        for (int tx_o = -thk/2; tx_o <= thk/2; tx_o++) {
            for (int ty_o = -thk/2; ty_o <= thk/2; ty_o++) {
                if (tx_o*tx_o + ty_o*ty_o <= (thk/2)*(thk/2) + 1) { 
                    int p_val_x = cx + tx_o, p_val_y = cy + ty_o;
                    if (p_val_x >= 0 && p_val_x < BIG_W && p_val_y >= 0 && p_val_y < BIG_H) {
                        shape[p_val_y * BIG_W + p_val_x] = 1;
                    }
                }
            }
        }
        if (cx == x1 && cy == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; cx += sx; }
        if (e2 <= dx) { err += dx; cy += sy; }
    }
}

static void generate_digit_targets(int digit, int offset_x, int offset_y, float *tx, float *ty, int count) {
    uint8_t shape[BIG_W * BIG_H];
    memset(shape, 0, sizeof(shape));

    #define L(x0,y0,x1,y1,t) add_thick_line(x0,y0,x1,y1,t,shape)
    #define D(x,y,t)         add_thick_line(x,y,x,y,t,shape)

    /* Custom organic cyber/aero font styling */
    switch(digit) {
        case 0:
            L(5,2, 10,2, 3); L(13,6, 13,19, 3); 
            L(10,23, 5,23, 3); L(2,19, 2,6, 3);
            D(3,3,2); D(12,3,2); D(3,22,2); D(12,22,2); /* corners */
            D(7,13,5); /* dense core */
            break;
        case 1:
            L(5,6, 8,2, 3); L(8,2, 8,23, 4);
            L(3,23, 13,23, 3);
            D(8, 27, 2); /* floating data point */
            break;
        case 2:
            L(2,6, 5,2, 3); L(5,2, 11,2, 3); L(11,2, 14,6, 3);
            L(14,8, 14,11, 3); L(14,11, 2,21, 3); L(2,23, 14,23, 4);
            D(5, 14, 3); /* interior core */
            break;
        case 3:
            L(2,2, 12,2, 3); L(12,2, 8,10, 3);
            L(8,10, 13,13, 3); L(13,13, 13,18, 3); L(13,18, 9,23, 3); L(9,23, 2,23, 3);
            D(4, 15, 3);
            break;
        case 4:
            L(11,2, 2,14, 3); L(2,14, 14,14, 3);
            L(11,2, 11,23, 4);
            D(3, 19, 3);
            break;
        case 5:
            L(14,2, 4,2, 3); L(4,2, 3,11, 3);
            L(3,11, 10,10, 3); L(10,10, 14,14, 3); L(14,14, 13,20, 3); 
            L(13,20, 8,23, 3); L(8,23, 2,21, 3);
            D(6, 16, 3);
            break;
        case 6:
            L(11,3, 5,6, 3); L(5,6, 2,14, 3); L(2,14, 4,22, 3);
            L(4,22, 10,23, 3); L(10,23, 14,18, 3); L(14,18, 11,13, 3); L(11,13, 4,14, 3);
            D(8, 18, 3);
            break;
        case 7:
            L(2,2, 14,2, 4); L(14,2, 6,14, 3); L(6,14, 6,23, 3);
            D(10, 16, 2); D(12, 18, 2);
            break;
        case 8:
            L(5,2, 10,2, 3); L(13,5, 10,10, 3); L(2,5, 5,10, 3);
            L(5,23, 10,23, 3); L(13,20, 10,15, 3); L(2,20, 5,15, 3);
            D(7, 12, 4); /* central x-cross core */
            break;
        case 9:
            L(10,13, 4,12, 3); L(4,12, 2,7, 3); L(2,7, 5,2, 3);
            L(5,2, 11,2, 3); L(11,2, 14,8, 3); L(14,8, 11,21, 3); L(11,21, 6,24, 3);
            D(7, 7, 3);
            break;
    }

    #undef L
    #undef D

    int valid_points[800][2];
    int v_count = 0;
    
    for (int y = 0; y < BIG_H; y++) {
        for (int x = 0; x < BIG_W; x++) {
            if (shape[y * BIG_W + x] && v_count < 800) {
                valid_points[v_count][0] = offset_x + x;
                valid_points[v_count][1] = offset_y + y;
                v_count++;
            }
        }
    }

    /* Assign one particle PER exact solid pixel for perfect tiling */
    for (int i = 0; i < count; i++) {
        if (v_count > 0) {
            int idx = i % v_count; /* deterministic assignment ensures fully solid geometry */
            tx[i] = (float)valid_points[idx][0];
            ty[i] = (float)valid_points[idx][1];
        } else {
            tx[i] = offset_x + BIG_W/2;
            ty[i] = offset_y + BIG_H/2;
        }
    }
}

static void update_swarm(int slot, int new_digit, int offset_x, int offset_y, float dt) {
    if (current_digits[slot] != new_digit) {
        current_digits[slot] = new_digit;
        
        float tx[P_PER_DIGIT];
        float ty[P_PER_DIGIT];
        generate_digit_targets(new_digit, offset_x, offset_y, tx, ty, P_PER_DIGIT);
        
        /* Scatter slightly and set new targets */
        for (int i = 0; i < P_PER_DIGIT; i++) {
            swarms[slot][i].tx = tx[i];
            swarms[slot][i].ty = ty[i];
            
            /* Add explosion impulse */
            swarms[slot][i].vx += ((rand() % 100) - 50) * 1.5f;
            swarms[slot][i].vy += ((rand() % 100) - 50) * 1.5f;
        }
    }
    
    float s = dt * 60.0f;
    /* unused center pointers */

    for (int i = 0; i < P_PER_DIGIT; i++) {
        particle_t *p = &swarms[slot][i];
        
        float dx = p->tx - p->x;
        float dy = p->ty - p->y;
        
        /* Organic magnetic pull to target without bouncy overshooting */
        p->vx += dx * 0.15f * s;
        p->vy += dy * 0.15f * s;
        
        /* Heavy fluid damping */
        p->vx *= 0.65f;
        p->vy *= 0.65f;
        
        p->x += p->vx * s;
        p->y += p->vy * s;
        
        /* Draw as a clean 1x1 dot */
        px((int)(p->x + 0.5f), (int)(p->y + 0.5f));
    }
}

/* ------------------------------------------------------------------ */
/*  Main Blue Zone Rendering                                           */
/* ------------------------------------------------------------------ */

static void draw_blue_zone(const struct tm *tm, float sub_sec, int steps, int step_goal, int batt, float frame_time) {
    /* Slowly shifting grid points to prevent background burn-in */
    float drift = frame_time * 5.0f;
    for (int y = BLUE_Y + 4; y < HEIGHT; y += 8) {
        for (int gx = 4; gx < WIDTH; gx += 8) {
            int drift_x = (int)(gx + drift) % WIDTH;
            if ((gx + y/8) % 3 == 0) px(drift_x, y);
        }
    }

    int h12 = tm->tm_hour % 12;
    if (h12 == 0) h12 = 12;

    int d0 = h12 / 10;
    int d1 = h12 % 10;
    int d2 = tm->tm_min / 10;
    int d3 = tm->tm_min % 10;

    /* Pixel shifting: Slowly drift the entire time block in a tiny 4x4 circle over huge periods */
    int shift_x = (int)(sinf(frame_time * 0.2f) * 2.0f);
    int shift_y = (int)(cosf(frame_time * 0.15f) * 2.0f);

    /* Subtle tactile heartbeat: shake 1 pixel in any direction briefly at the start of every second */
    int beat_x = 0;
    int beat_y = 0;
    if (sub_sec < 0.1f) {
        beat_x = (rand() % 3) - 1; /* -1, 0, or 1 */
        beat_y = (rand() % 3) - 1;
    }

    int start_x = 1 + shift_x + beat_x;
    int y_pos = BLUE_Y + 8 + shift_y + beat_y;
    int gap = 2;
    
    if (d0 > 0) update_swarm(0, d0, start_x, y_pos, 1.0f/TARGET_FPS);
    else update_swarm(0, 0, -20, y_pos, 1.0f/TARGET_FPS);
    
    update_swarm(1, d1, start_x + BIG_W + gap, y_pos, 1.0f/TARGET_FPS);
    
    /* Blinking colon */
    int r_offset = start_x + 2*BIG_W + gap + gap/2;
    if (sub_sec < 0.5f) {
        fill_rect(r_offset, y_pos + 6, r_offset+2, y_pos+8);
        fill_rect(r_offset, y_pos + 16, r_offset+2, y_pos+18);
    }
    
    update_swarm(2, d2, start_x + 2*BIG_W + gap*3, y_pos, 1.0f/TARGET_FPS);
    update_swarm(3, d3, start_x + 3*BIG_W + gap*4, y_pos, 1.0f/TARGET_FPS);

    /* Orbitals on the right side - Segmented moving blobs for burn-in protection */
    int orb_cx = 103;
    int orb_cy = BLUE_Y + 24;
    
    float st_pct = (float)steps / step_goal;
    if (st_pct > 1.0f) st_pct = 1.0f;
    float bt_pct = (float)batt / 100.0f;
    
    /* Step arc: Outer ring, r=24 */
    /* Draw step tracking as moving blobs. Max 12 blobs. */
    int st_blobs_total = 12;
    int st_blobs_active = (int)(st_pct * st_blobs_total);
    if (st_pct > 0.01f && st_blobs_active == 0) st_blobs_active = 1;
    
    float blob_width_deg = 15.0f; 
    float angle_spacing = 360.0f / st_blobs_total;
    float orb_rot_st = frame_time * 35.0f; /* continuously orbits */

    for (int i = 0; i < st_blobs_active; i++) {
        float start_deg = orb_rot_st + i * angle_spacing;
        draw_organic_arc(orb_cx, orb_cy, 24, start_deg, start_deg + blob_width_deg, 1.8f, 0);
    }
    
    /* Battery arc: Inner ring, r=18 */
    /* Max 10 blobs */
    int bt_blobs_total = 10;
    int bt_blobs_active = (int)(bt_pct * bt_blobs_total);
    if (bt_pct > 0.01f && bt_blobs_active == 0) bt_blobs_active = 1;
    
    float bt_angle_spacing = 360.0f / bt_blobs_total;
    float orb_rot_bt = -frame_time * 25.0f; /* orbits opposite direction */
    
    for (int i = 0; i < bt_blobs_active; i++) {
        float start_deg = orb_rot_bt + i * bt_angle_spacing;
        draw_organic_arc(orb_cx, orb_cy, 18, start_deg, start_deg + blob_width_deg, 1.5f, 0);
    }

    /* Seconds - smooth continuously sweeping solitary comet */
    float sec_ang = ((float)tm->tm_sec + sub_sec) / 60.0f * 360.0f;
    draw_organic_arc(orb_cx, orb_cy, 12, sec_ang - 45.0f, sec_ang, 2.0f, 1);
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

    /* Initialize swarms just slightly off screen first */
    for (int i = 0; i < NUM_DIGITS; i++) {
        current_digits[i] = -1;
        for (int p = 0; p < P_PER_DIGIT; p++) {
            swarms[i][p].x = WIDTH / 2.0f;
            swarms[i][p].y = HEIGHT + 10.0f;
            swarms[i][p].vx = 0;
            swarms[i][p].vy = 0;
        }
    }
    
    memset(ecg_buffer, 0, sizeof(ecg_buffer));

    long frame_ns = 1000000000L / TARGET_FPS;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    float sim_t = 0;
    float ecg_phase = 0;
    int base_steps = 4200 + (rand() % 3000);
    int step_goal = 10000;

    while (running) {
        float dt = 1.0f / TARGET_FPS;
        sim_t += dt;

        struct timespec rts;
        clock_gettime(CLOCK_REALTIME, &rts);
        time_t now = rts.tv_sec;
        struct tm *tm = localtime(&now);
        float sub_sec = (float)rts.tv_nsec / 1e9f;

        float bpm = 72.0f + 8.0f * smooth_val(sim_t, 0.13f, 0.31f, 0.07f);
        int steps = base_steps + (int)(sim_t * 2.8f);
        if (steps > 14000) steps = 14000;
        int batt = 78 - (int)(sim_t * 0.015f);
        if (batt < 5) batt = 5;

        float samp_per_beat = (60.0f / bpm) * TARGET_FPS;
        ecg_phase += 1.0f / samp_per_beat;
        if (ecg_phase >= 1.0f) ecg_phase -= 1.0f;

        memset(fb, 0, sizeof(fb));
        
        draw_yellow_hud(batt, ecg_phase, sim_t);
        draw_blue_zone(tm, sub_sec, steps, step_goal, batt, sim_t);
        
        flush();

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
