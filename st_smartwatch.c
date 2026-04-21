/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-06
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * st_smartwatch.c - LCARS Star Trek Style Smartwatch
 * 
 * Features:
 * - 15-second dynamic scene rotation (Chronometer, Bio-monitor, Engineering, Diagnostics)
 * - Yellow/Blue OLED physical integration creating the LCARS L-bracket
 * - Thick block vector fonts & solid LCARS structural geometry
 *
 * Compile:  make st_smartwatch or ./build.sh pi
 * Run:      sudo ./st_smartwatch
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
        uint8_t buf[33]; buf[0] = 0x40;
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
            if (x*x + y*y <= r*r) px(cx+x, cy+y);
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
            if (bits & (1 << row)) px(x + col, y + row);
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
                if (bits & (1 << row)) clear_px(x + col, y + row);
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
            if (bits & (1 << row)) {
                fill_rect(x + col * scale, y + row * scale, 
                          x + col * scale + scale - 1, y + row * scale + scale - 1);
            }
        }
    }
}

static void draw_large_str(int x, int y, const char *s, int scale) {
    while (*s) { draw_large_char(x, y, *s++, scale); x += (5 + 1) * scale; }
}

/* ------------------------------------------------------------------ */
/*  LCARS UI                                                           */
/* ------------------------------------------------------------------ */

static void draw_lcars_frame(float sim_t, int scene) {
    /* 1. Yellow Header (Top 16 pixels) */
    /* Fills the whole top bar EXCEPT the right tip where it rounds off */
    fill_rect(0, 0, WIDTH - 6, 14);
    
    /* Round the right tip of the header */
    for(int r = 0; r <= 7; r++) {
       for(int dy = -r; dy <= r; dy++) {
           for(int dx = -r; dx <= r; dx++) {
               if(dx*dx + dy*dy <= r*r) px(WIDTH - 7 + dx, 7 + dy);
           }
       }
    }
    /* Cut out top and bottom row so it's vertically padded natively */
    clear_rect(0, 0, WIDTH-1, 1);
    clear_rect(0, 15, WIDTH-1, 15);
    
    /* Invert LCARS text on the yellow header */
    char buf[24];
    snprintf(buf, sizeof(buf), "LCARS 47-%02d", scene);
    int tw = 11 * 6; // strictly length
    int str_x = WIDTH - 8 - tw;
    int str_y = 5;
    
    /* Pre-draw yellow background for text bounding box to keep it clean */
    fill_rect(str_x, str_y, str_x + tw, str_y + 7);
    draw_str_inv(str_x, str_y, buf);
    
    /* 2. Vertical Spine in Blue Zone */
    fill_rect(0, 17, 16, HEIGHT - 1);
    
    /* Round inner corner connecting top bar to left spine */
    clear_rect(17, 17, 30, 30);
    for(int y=-8; y<=8; y++) {
        for(int x=-8; x<=8; x++) {
            if(x*x + y*y > 64 && x<0 && y<0) px(25+x, 25+y);
        }
    }

    /* Cut horizontal gaps in spine for classic LCARS 'button' look */
    clear_rect(0, 30, 16, 32);
    clear_rect(0, 48, 16, 50);

    /* Tiny LCARS IDs on the buttons (inverted text) */
    char id2[5]; snprintf(id2, 5, "%02d", 24 + scene);
    fill_rect(0, 38, 16, 45); /* insure background */
    draw_str_inv(2, 38, id2);
}

static void draw_scenes(const struct tm *tm, int steps, int batt, float sim_t, float ecg_phase, int scene) {
    int PLAY_X = 24;
    int PLAY_Y = 18;
    
    char buf[32];

    switch(scene) {
        case 0: /* CHRONOMETER (0-15s) */
            snprintf(buf, sizeof(buf), "%02d:%02d", tm->tm_hour, tm->tm_min);
            draw_large_str(PLAY_X, PLAY_Y + 6, buf, 3);
            
            snprintf(buf, sizeof(buf), "%02d", tm->tm_sec);
            draw_large_str(WIDTH - 24, PLAY_Y + 6, buf, 1);
            
            draw_str(PLAY_X, PLAY_Y + 34, "MAIN TIME");
            
            fill_rect(PLAY_X, PLAY_Y + 42, PLAY_X + (batt*70)/100, PLAY_Y + 44);
            break;
            
        case 1: /* BIO-MONITOR (15-30s) */
            draw_str(PLAY_X, PLAY_Y, "BIO-MONITOR");
            
            int bpm = 72 + (int)(sinf(sim_t)*5.0f);
            snprintf(buf, sizeof(buf), "%d", bpm);
            draw_large_str(PLAY_X, PLAY_Y + 12, buf, 2);
            draw_str(PLAY_X + 28, PLAY_Y + 20, "BPM");
            
            /* Big ECG */
            for(int x=0; x < 60; x++) {
                float ph = (float)x * 0.1f - ecg_phase * 10.0f;
                int cy = PLAY_Y + 14 + (int)(expf(-powf((fmodf(fabsf(ph), 10.0f)-5.0f)*2.0f, 2)) * -14.0f);
                px(PLAY_X + 44 + x, cy);
                px(PLAY_X + 44 + x, cy+1); /* thick */
            }
            
            draw_str(PLAY_X, PLAY_Y + 36, "VITALS: NOMINAL");
            break;
            
        case 2: /* ENGINEERING (30-45s) */
            draw_str(PLAY_X, PLAY_Y, "ENGINEERING");
            
            /* Step Bar Graph */
            draw_str(PLAY_X, PLAY_Y + 12, "STP");
            int step_w = (steps * 60) / 10000;
            if (step_w > 60) step_w = 60;
            fill_rect(PLAY_X + 25, PLAY_Y + 12, PLAY_X + 25 + step_w, PLAY_Y + 18);
            /* Hash marks dissecting the block graph */
            for(int x=0; x<=60; x+=8) clear_rect(PLAY_X + 25 + x, PLAY_Y + 12, PLAY_X + 25 + x, PLAY_Y + 18);
            
            /* Battery Matrix */
            draw_str(PLAY_X, PLAY_Y + 24, "PWR");
            int b_cells = (batt * 10) / 100;
            for(int i=0; i<10; i++) {
                if(i < b_cells) fill_rect(PLAY_X + 25 + i*6, PLAY_Y + 24, PLAY_X + 25 + i*6 + 4, PLAY_Y + 30);
                else {
                    /* Outline for empty cell */
                    px(PLAY_X + 25 + i*6, PLAY_Y + 30); px(PLAY_X + 25 + i*6 + 4, PLAY_Y + 30);
                }
            }
            
            snprintf(buf, sizeof(buf), "T %02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
            draw_str(PLAY_X, PLAY_Y + 36, buf);
            break;
            
        case 3: /* DIAGNOSTIC (45-60s) */
            draw_str(PLAY_X, PLAY_Y, "DIAGNOSTIC");
            
            for(int r=0; r<4; r++) {
                snprintf(buf, sizeof(buf), "0x%04X %04X", rand()%0xFFFF, rand()%0xFFFF);
                draw_str(PLAY_X, PLAY_Y + 10 + r*8, buf);
            }
            
            /* Sweeping radar wedge */
            float ang = sim_t * 5.0f;
            int r_cx = WIDTH - 16;
            int r_cy = PLAY_Y + 20;
            fill_circle(r_cx, r_cy, 1);
            px(r_cx + (int)(12*cosf(ang)), r_cy + (int)(12*sinf(ang)));
            px(r_cx + (int)(10*cosf(ang-0.2f)), r_cy + (int)(10*sinf(ang-0.2f)));
            px(r_cx + (int)(8*cosf(ang-0.4f)), r_cy + (int)(8*sinf(ang-0.4f)));
            break;
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
    
    long frame_ns = 1000000000L / TARGET_FPS;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    struct timespec prev;
    clock_gettime(CLOCK_MONOTONIC, &prev);

    float sim_t = 0;
    float ecg_phase = 0;
    int base_steps = 5500 + (rand() % 1000);

    while (running) {
        struct timespec curr;
        clock_gettime(CLOCK_MONOTONIC, &curr);
        float dt = (curr.tv_sec - prev.tv_sec) + (curr.tv_nsec - prev.tv_nsec) / 1e9f;
        /* cap huge dt spikes in case of hangs to prevent physics exploding */
        if (dt > 1.0f) dt = 1.0f;
        prev = curr;
        
        sim_t += dt;

        struct timespec rts;
        clock_gettime(CLOCK_REALTIME, &rts);
        time_t now = rts.tv_sec;
        struct tm *tm = localtime(&now);

        float bpm = 72.0f;
        int steps = base_steps + (int)(sim_t * 2.8f);
        if (steps > 14000) steps = 14000;
        int batt = 87 - (int)(sim_t * 0.005f);
        if (batt < 5) batt = 5;

        /* samp_per_beat actively deprecated */
        /* Native ECG drift scaling */
        ecg_phase += dt / (60.0f / bpm);
        if (ecg_phase >= 1.0f) ecg_phase -= 1.0f;

        memset(fb, 0, sizeof(fb));
        
        int scene = (int)((now / 15) % 4);
        
        draw_lcars_frame(sim_t, scene);
        draw_scenes(tm, steps, batt, sim_t, ecg_phase, scene);
        
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
