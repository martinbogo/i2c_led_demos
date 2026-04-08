/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-06
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * sysmon.c - System monitor for SSD1306 128x64 yellow/blue OLED (Pi 5)
 *
 * Yellow zone (top 16px): date, time, load average
 * Blue zone (bottom 48px): scrolling graphs of CPU%, MEM%, IO%
 *
 * Compile:  gcc -o sysmon sysmon.c -lm
 * Run:      sudo ./sysmon
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

/* Layout */
#define YELLOW_H   16   /* yellow section height */
#define BLUE_Y     16   /* blue section starts here */
#define BLUE_H     48   /* blue section height */
#define GRAPH_W    128  /* graph width = full screen width */

/* How many graph rows per metric (3 metrics stacked in 48px) */
#define GRAPH_H    16

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
/*  I2C / SSD1306 helpers                                             */
/* ------------------------------------------------------------------ */

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
        0x81, 0xCF,
        0xD9, 0xF1,
        0xDB, 0x40,
        0xA4,
        0xA6,
        0xAF
    };
    if (write(i2c_fd, init_cmds, sizeof(init_cmds)) < 0) {
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
    if (write(i2c_fd, addr_cmds, sizeof(addr_cmds)) < 0) {
        perror("write addr");
        running = 0;
        return;
    }
    for (int i = 0; i < WIDTH * PAGES; i += 32) {
        uint8_t buf[33];
        buf[0] = 0x40;
        int len = WIDTH * PAGES - i;
        if (len > 32) len = 32;
        memcpy(buf + 1, fb + i, len);
        if (write(i2c_fd, buf, len + 1) < 0) {
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

/* ------------------------------------------------------------------ */
/*  Minimal 5x7 font (printable ASCII 32-126)                        */
/* ------------------------------------------------------------------ */

static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 32 (space) */
    {0x00,0x00,0x5F,0x00,0x00}, /* 33 ! */
    {0x00,0x07,0x00,0x07,0x00}, /* 34 " */
    {0x14,0x7F,0x14,0x7F,0x14}, /* 35 # */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* 36 $ */
    {0x23,0x13,0x08,0x64,0x62}, /* 37 % */
    {0x36,0x49,0x55,0x22,0x50}, /* 38 & */
    {0x00,0x05,0x03,0x00,0x00}, /* 39 ' */
    {0x00,0x1C,0x22,0x41,0x00}, /* 40 ( */
    {0x00,0x41,0x22,0x1C,0x00}, /* 41 ) */
    {0x08,0x2A,0x1C,0x2A,0x08}, /* 42 * */
    {0x08,0x08,0x3E,0x08,0x08}, /* 43 + */
    {0x00,0x50,0x30,0x00,0x00}, /* 44 , */
    {0x08,0x08,0x08,0x08,0x08}, /* 45 - */
    {0x00,0x60,0x60,0x00,0x00}, /* 46 . */
    {0x20,0x10,0x08,0x04,0x02}, /* 47 / */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 48 0 */
    {0x00,0x42,0x7F,0x40,0x00}, /* 49 1 */
    {0x42,0x61,0x51,0x49,0x46}, /* 50 2 */
    {0x21,0x41,0x45,0x4B,0x31}, /* 51 3 */
    {0x18,0x14,0x12,0x7F,0x10}, /* 52 4 */
    {0x27,0x45,0x45,0x45,0x39}, /* 53 5 */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 54 6 */
    {0x01,0x71,0x09,0x05,0x03}, /* 55 7 */
    {0x36,0x49,0x49,0x49,0x36}, /* 56 8 */
    {0x06,0x49,0x49,0x29,0x1E}, /* 57 9 */
    {0x00,0x36,0x36,0x00,0x00}, /* 58 : */
    {0x00,0x56,0x36,0x00,0x00}, /* 59 ; */
    {0x00,0x08,0x14,0x22,0x41}, /* 60 < */
    {0x14,0x14,0x14,0x14,0x14}, /* 61 = */
    {0x41,0x22,0x14,0x08,0x00}, /* 62 > */
    {0x02,0x01,0x51,0x09,0x06}, /* 63 ? */
    {0x32,0x49,0x79,0x41,0x3E}, /* 64 @ */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 65 A */
    {0x7F,0x49,0x49,0x49,0x36}, /* 66 B */
    {0x3E,0x41,0x41,0x41,0x22}, /* 67 C */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 68 D */
    {0x7F,0x49,0x49,0x49,0x41}, /* 69 E */
    {0x7F,0x09,0x09,0x01,0x01}, /* 70 F */
    {0x3E,0x41,0x41,0x51,0x32}, /* 71 G */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 72 H */
    {0x00,0x41,0x7F,0x41,0x00}, /* 73 I */
    {0x20,0x40,0x41,0x3F,0x01}, /* 74 J */
    {0x7F,0x08,0x14,0x22,0x41}, /* 75 K */
    {0x7F,0x40,0x40,0x40,0x40}, /* 76 L */
    {0x7F,0x02,0x04,0x02,0x7F}, /* 77 M */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 78 N */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 79 O */
    {0x7F,0x09,0x09,0x09,0x06}, /* 80 P */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 81 Q */
    {0x7F,0x09,0x19,0x29,0x46}, /* 82 R */
    {0x46,0x49,0x49,0x49,0x31}, /* 83 S */
    {0x01,0x01,0x7F,0x01,0x01}, /* 84 T */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 85 U */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 86 V */
    {0x7F,0x20,0x18,0x20,0x7F}, /* 87 W */
    {0x63,0x14,0x08,0x14,0x63}, /* 88 X */
    {0x03,0x04,0x78,0x04,0x03}, /* 89 Y */
    {0x61,0x51,0x49,0x45,0x43}, /* 90 Z */
    {0x00,0x00,0x7F,0x41,0x41}, /* 91 [ */
    {0x02,0x04,0x08,0x10,0x20}, /* 92 \ */
    {0x41,0x41,0x7F,0x00,0x00}, /* 93 ] */
    {0x04,0x02,0x01,0x02,0x04}, /* 94 ^ */
    {0x40,0x40,0x40,0x40,0x40}, /* 95 _ */
    {0x00,0x01,0x02,0x04,0x00}, /* 96 ` */
    {0x20,0x54,0x54,0x54,0x78}, /* 97 a */
    {0x7F,0x48,0x44,0x44,0x38}, /* 98 b */
    {0x38,0x44,0x44,0x44,0x20}, /* 99 c */
    {0x38,0x44,0x44,0x48,0x7F}, /*100 d */
    {0x38,0x54,0x54,0x54,0x18}, /*101 e */
    {0x08,0x7E,0x09,0x01,0x02}, /*102 f */
    {0x08,0x14,0x54,0x54,0x3C}, /*103 g */
    {0x7F,0x08,0x04,0x04,0x78}, /*104 h */
    {0x00,0x44,0x7D,0x40,0x00}, /*105 i */
    {0x20,0x40,0x44,0x3D,0x00}, /*106 j */
    {0x00,0x7F,0x10,0x28,0x44}, /*107 k */
    {0x00,0x41,0x7F,0x40,0x00}, /*108 l */
    {0x7C,0x04,0x18,0x04,0x78}, /*109 m */
    {0x7C,0x08,0x04,0x04,0x78}, /*110 n */
    {0x38,0x44,0x44,0x44,0x38}, /*111 o */
    {0x7C,0x14,0x14,0x14,0x08}, /*112 p */
    {0x08,0x14,0x14,0x18,0x7C}, /*113 q */
    {0x7C,0x08,0x04,0x04,0x08}, /*114 r */
    {0x48,0x54,0x54,0x54,0x20}, /*115 s */
    {0x04,0x3F,0x44,0x40,0x20}, /*116 t */
    {0x3C,0x40,0x40,0x20,0x7C}, /*117 u */
    {0x1C,0x20,0x40,0x20,0x1C}, /*118 v */
    {0x3C,0x40,0x30,0x40,0x3C}, /*119 w */
    {0x44,0x28,0x10,0x28,0x44}, /*120 x */
    {0x0C,0x50,0x50,0x50,0x3C}, /*121 y */
    {0x44,0x64,0x54,0x4C,0x44}, /*122 z */
    {0x00,0x08,0x36,0x41,0x00}, /*123 { */
    {0x00,0x00,0x7F,0x00,0x00}, /*124 | */
    {0x00,0x41,0x36,0x08,0x00}, /*125 } */
    {0x08,0x08,0x2A,0x1C,0x08}, /*126 ~ */
};

static void draw_char(int x, int y, char c) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *glyph = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row))
                set_pixel(x + col, y + row);
        }
    }
}

static void draw_str(int x, int y, const char *s) {
    while (*s) {
        draw_char(x, y, *s);
        x += 6; /* 5px glyph + 1px gap */
        s++;
    }
}

/* ------------------------------------------------------------------ */
/*  System stats                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    unsigned long long user, nice, sys, idle, iowait, irq, softirq, steal;
} cpu_snap_t;

static void read_cpu(cpu_snap_t *s) {
    FILE *f = fopen("/proc/stat", "r");
    if (!f) return;
    fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
           &s->user, &s->nice, &s->sys, &s->idle,
           &s->iowait, &s->irq, &s->softirq, &s->steal);
    fclose(f);
}

static float calc_cpu_pct(const cpu_snap_t *a, const cpu_snap_t *b) {
    unsigned long long idle_a = a->idle + a->iowait;
    unsigned long long idle_b = b->idle + b->iowait;
    unsigned long long total_a = a->user + a->nice + a->sys + a->idle +
                                  a->iowait + a->irq + a->softirq + a->steal;
    unsigned long long total_b = b->user + b->nice + b->sys + b->idle +
                                  b->iowait + b->irq + b->softirq + b->steal;
    unsigned long long totald = total_b - total_a;
    unsigned long long idled  = idle_b - idle_a;
    if (totald == 0) return 0;
    return 100.0f * (float)(totald - idled) / (float)totald;
}

static float calc_iowait_pct(const cpu_snap_t *a, const cpu_snap_t *b) {
    unsigned long long total_a = a->user + a->nice + a->sys + a->idle +
                                  a->iowait + a->irq + a->softirq + a->steal;
    unsigned long long total_b = b->user + b->nice + b->sys + b->idle +
                                  b->iowait + b->irq + b->softirq + b->steal;
    unsigned long long totald = total_b - total_a;
    if (totald == 0) return 0;
    return 100.0f * (float)(b->iowait - a->iowait) / (float)totald;
}

static float read_mem_pct(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return 0;
    unsigned long total = 0, avail = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemTotal: %lu kB", &total) == 1) continue;
        if (sscanf(line, "MemAvailable: %lu kB", &avail) == 1) break;
    }
    fclose(f);
    if (total == 0) return 0;
    return 100.0f * (float)(total - avail) / (float)total;
}

static float read_load(void) {
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return 0;
    float load1 = 0;
    fscanf(f, "%f", &load1);
    fclose(f);
    return load1;
}

/* ------------------------------------------------------------------ */
/*  Graph drawing                                                      */
/* ------------------------------------------------------------------ */

#define HIST_LEN GRAPH_W

static float cpu_hist[HIST_LEN];
static float mem_hist[HIST_LEN];
static float io_hist[HIST_LEN];

static void push_hist(float *hist, float val) {
    memmove(hist, hist + 1, (HIST_LEN - 1) * sizeof(float));
    hist[HIST_LEN - 1] = val;
}

static void draw_graph(int y_top, int h, const float *hist,
                       const char *label) {
    /* Label on the left */
    draw_str(0, y_top + (h / 2) - 3, label);

    int graph_x0 = 6 * (int)strlen(label) + 2;
    int graph_w  = WIDTH - graph_x0;

    /* Dotted baseline */
    for (int x = graph_x0; x < WIDTH; x += 2)
        set_pixel(x, y_top + h - 1);

    /* Graph bars */
    for (int i = 0; i < graph_w; i++) {
        int idx = HIST_LEN - graph_w + i;
        if (idx < 0) continue;
        float v = hist[idx];
        if (v < 0) v = 0;
        if (v > 100) v = 100;
        int bar = (int)(v * (h - 1) / 100.0f);
        for (int dy = 0; dy < bar; dy++)
            set_pixel(graph_x0 + i, y_top + h - 1 - dy);
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

    memset(cpu_hist, 0, sizeof(cpu_hist));
    memset(mem_hist, 0, sizeof(mem_hist));
    memset(io_hist,  0, sizeof(io_hist));

    cpu_snap_t prev_cpu;
    read_cpu(&prev_cpu);

    while (running) {
        /* Sample system stats */
        cpu_snap_t cur_cpu;
        read_cpu(&cur_cpu);
        float cpu_pct = calc_cpu_pct(&prev_cpu, &cur_cpu);
        float io_pct  = calc_iowait_pct(&prev_cpu, &cur_cpu);
        float mem_pct = read_mem_pct();
        float load    = read_load();
        prev_cpu = cur_cpu;

        push_hist(cpu_hist, cpu_pct);
        push_hist(mem_hist, mem_pct);
        push_hist(io_hist,  io_pct);

        /* -- Draw frame -- */
        memset(fb, 0, sizeof(fb));

        /* Yellow zone: date, time, load */
        time_t now = time(NULL);
        struct tm *t = localtime(&now);
        char line1[64], line2[64];

        snprintf(line1, sizeof(line1), "%04d-%02d-%02d  %02d:%02d:%02d",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
        snprintf(line2, sizeof(line2), "Load:%.1f C:%02.0f%% M:%02.0f%%",
                 load, cpu_pct, mem_pct);

        draw_str(1, 0, line1);
        draw_str(1, 8, line2);

        /* Blue zone: three stacked graphs */
        draw_graph(BLUE_Y,              GRAPH_H, cpu_hist, "CPU");
        draw_graph(BLUE_Y + GRAPH_H,    GRAPH_H, mem_hist, "MEM");
        draw_graph(BLUE_Y + GRAPH_H * 2, GRAPH_H, io_hist,  "IO");

        flush();

        /* Update once per second */
        sleep(1);
    }

    memset(fb, 0, sizeof(fb));
    flush();
    uint8_t off[2] = {0x00, 0xAE};
    write(i2c_fd, off, 2);
    close(i2c_fd);
    return 0;
}
