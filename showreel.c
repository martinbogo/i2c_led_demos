/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-06
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * showreel.c - Hardware-aware multi-scene demoscene reel for SSD1306 128x64 OLED
 *
 * Features:
 * - Eight-scene reel tuned for a 128x64 display with a 16px yellow header and 48px blue playfield
 * - 100 kbps I2C friendly pacing at 10 FPS, close to the full-frame transfer ceiling
 * - Vector boot crackle, hyperspace burst, checkerboard dive, lit tunnel, PI5 morph,
 *   cinematic palace platformer, pre-rendered raymarched hero shot, and sine scroller finale
 * - Burn-in-friendly motion with continuous scene changes and a whiteout loop reset
 *
 * Compile:  make showreel or ./build.sh pi
 * Run:      sudo ./showreel
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

#define WIDTH            128
#define HEIGHT           64
#define PAGES            (HEIGHT / 8)
#define ADDR             0x3C
#define TARGET_FPS       10
#define YELLOW_H         16
#define BLUE_Y           16
#define BLUE_H           48
#define BLUE_PAGES       (BLUE_H / 8)
#define SCENE_COUNT      8
#define SCENE_SECONDS    6.0f
#define FLASH_TIME       0.55f
#define WHITEOUT_TIME    1.00f
#define MORPH_PARTICLES  320
#define HERO_FRAME_COUNT 30
#define HERO_FRAME_BYTES (WIDTH * BLUE_PAGES)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float x, y, z;
} vec3_t;

typedef struct {
    float x, y;
} vec2_t;

typedef struct {
    vec2_t neck;
    vec2_t elbows[2];
    vec2_t hands[2];
    vec2_t knees[2];
    vec2_t feet[2];
    float head_r;
} actor_pose_t;

static const char *scene_titles[SCENE_COUNT] = {
    "BOOT", "BURST", "ROTO", "TUNNEL",
    "MORPH", "PALACE", "HERO", "FINALE"
};

static const float scene_durations[SCENE_COUNT] = {
    6.0f, 6.0f, 6.0f, 6.0f,
    6.0f, 11.0f, 11.0f, 12.5f
};

static int i2c_fd;
static uint8_t fb[WIDTH * PAGES];
static volatile int running = 1;

static uint8_t hero_frames[HERO_FRAME_COUNT][HERO_FRAME_BYTES];
static int hero_ready = 0;
static float morph_tx[MORPH_PARTICLES];
static float morph_ty[MORPH_PARTICLES];
static int morph_target_count = 0;

static float scene_cycle_duration(void) {
    float total = 0.0f;
    for (int i = 0; i < SCENE_COUNT; i++) total += scene_durations[i];
    return total;
}

static void resolve_scene_time(float sim_t, int *scene_idx, float *scene_t, float *scene_duration) {
    float cycle = scene_cycle_duration();
    float t = fmodf(sim_t, cycle);
    if (t < 0.0f) t += cycle;

    for (int i = 0; i < SCENE_COUNT; i++) {
        if (t < scene_durations[i]) {
            *scene_idx = i;
            *scene_t = t;
            *scene_duration = scene_durations[i];
            return;
        }
        t -= scene_durations[i];
    }

    *scene_idx = SCENE_COUNT - 1;
    *scene_t = scene_durations[SCENE_COUNT - 1];
    *scene_duration = scene_durations[SCENE_COUNT - 1];
}

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
    ssize_t written = write(i2c_fd, cmds, sizeof(cmds));
    (void)written;
}

static void flush(void) {
    uint8_t ac[] = {0x00, 0x21, 0, WIDTH - 1, 0x22, 0, PAGES - 1};
    ssize_t written = write(i2c_fd, ac, sizeof(ac));
    (void)written;
    for (int i = 0; i < WIDTH * PAGES; i += 32) {
        uint8_t buf[33];
        buf[0] = 0x40;
        int len = WIDTH * PAGES - i;
        if (len > 32) len = 32;
        memcpy(buf + 1, fb + i, len);
        written = write(i2c_fd, buf, len + 1);
        (void)written;
    }
}

static void px(int x, int y) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
        fb[x + (y / 8) * WIDTH] |= 1 << (y & 7);
}

static void frame_px(uint8_t *frame, int x, int y) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < BLUE_H)
        frame[x + (y / 8) * WIDTH] |= 1 << (y & 7);
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

static float fractf_local(float v) {
    return v - floorf(v);
}

static float hash01(int n) {
    float s = sinf((float)n * 12.9898f + 78.233f) * 43758.5453f;
    return fractf_local(s);
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

static void draw_line_shaded(int x0, int y0, int x1, int y1, int level) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int step = 0;

    if (level < 1) level = 1;
    if (level > 4) level = 4;

    for (;;) {
        int draw = 0;
        if (level >= 4) draw = 1;
        else if (level == 3) draw = ((step & 1) == 0);
        else if (level == 2) draw = ((step % 3) != 1);
        else draw = ((step % 4) == 0);
        if (draw) px(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        step++;
    }
}

static void fill_circle(int cx, int cy, int r) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                px(cx + x, cy + y);
}

static vec2_t v2(float x, float y) {
    vec2_t v = {x, y};
    return v;
}

static vec2_t v2_add(vec2_t a, vec2_t b) {
    return v2(a.x + b.x, a.y + b.y);
}

static vec2_t v2_lerp(vec2_t a, vec2_t b, float t) {
    return v2(mixf_local(a.x, b.x, t), mixf_local(a.y, b.y, t));
}

static float v2_len(vec2_t a) {
    return sqrtf(a.x * a.x + a.y * a.y);
}

static vec2_t v2_norm(vec2_t a) {
    float l = v2_len(a);
    if (l <= 1e-6f) return v2(0.0f, -1.0f);
    return v2(a.x / l, a.y / l);
}

static void fill_circlef(float cx, float cy, float r) {
    int ir = (int)lroundf(r);
    if (ir < 1) ir = 1;
    fill_circle((int)lroundf(cx), (int)lroundf(cy), ir);
}

static void draw_capsulef(float x0, float y0, float x1, float y1, int r) {
    float dx = x1 - x0;
    float dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    int steps = (int)lroundf(len * 1.5f) + 1;

    if (steps < 1) steps = 1;
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        fill_circlef(mixf_local(x0, x1, t), mixf_local(y0, y1, t), (float)r);
    }
}

static actor_pose_t actor_pose_mix(actor_pose_t a, actor_pose_t b, float t) {
    actor_pose_t pose;

    pose.neck = v2_lerp(a.neck, b.neck, t);
    for (int i = 0; i < 2; i++) {
        pose.elbows[i] = v2_lerp(a.elbows[i], b.elbows[i], t);
        pose.hands[i] = v2_lerp(a.hands[i], b.hands[i], t);
        pose.knees[i] = v2_lerp(a.knees[i], b.knees[i], t);
        pose.feet[i] = v2_lerp(a.feet[i], b.feet[i], t);
    }
    pose.head_r = mixf_local(a.head_r, b.head_r, t);
    return pose;
}

static actor_pose_t sample_actor_cycle(const actor_pose_t *poses, int count, float phase) {
    if (count <= 1) return poses[0];

    float f = fractf_local(phase) * (float)count;
    int i0 = (int)floorf(f);
    int i1 = (i0 + 1) % count;
    return actor_pose_mix(poses[i0], poses[i1], f - (float)i0);
}

static float actor_pose_max_foot_y(const actor_pose_t *pose) {
    float max_y = pose->feet[0].y;
    if (pose->feet[1].y > max_y) max_y = pose->feet[1].y;
    return max_y;
}

static float actor_ground_hip_y(float ground_y, const actor_pose_t *pose) {
    return ground_y - actor_pose_max_foot_y(pose);
}

static void build_keyed_gait_pose(float actor_x, float ground_y,
                                  const actor_pose_t *cycle, int cycle_count,
                                  float stride, float phase_bias,
                                  actor_pose_t *pose_out, float *hip_y_out) {
    actor_pose_t pose = sample_actor_cycle(cycle, cycle_count, actor_x / stride + phase_bias);
    *pose_out = pose;
    *hip_y_out = actor_ground_hip_y(ground_y, &pose);
}

static void draw_actor_pose(float x, float hip_y, const actor_pose_t *pose) {
    vec2_t hip = v2(x, hip_y);
    vec2_t neck = v2_add(hip, pose->neck);
    vec2_t head_dir = v2_norm(pose->neck);
    vec2_t head_center = v2_add(neck, v2(head_dir.x * pose->head_r * 1.7f,
                                          head_dir.y * pose->head_r * 1.7f));

    vec2_t knee_back = v2_add(hip, pose->knees[0]);
    vec2_t foot_back = v2_add(hip, pose->feet[0]);
    vec2_t elbow_back = v2_add(neck, pose->elbows[0]);
    vec2_t hand_back = v2_add(neck, pose->hands[0]);
    draw_capsulef(hip.x, hip.y, knee_back.x, knee_back.y, 1);
    draw_capsulef(knee_back.x, knee_back.y, foot_back.x, foot_back.y, 1);
    draw_capsulef(foot_back.x - 1.0f, foot_back.y, foot_back.x + 2.0f, foot_back.y, 1);
    draw_capsulef(neck.x, neck.y, elbow_back.x, elbow_back.y, 1);
    draw_capsulef(elbow_back.x, elbow_back.y, hand_back.x, hand_back.y, 1);

    draw_capsulef(hip.x, hip.y, neck.x, neck.y, 2);
    fill_circlef(head_center.x, head_center.y, pose->head_r);
    fill_circlef(hip.x, hip.y, 1.2f);
    fill_circlef(neck.x, neck.y, 1.0f);

    vec2_t knee_front = v2_add(hip, pose->knees[1]);
    vec2_t foot_front = v2_add(hip, pose->feet[1]);
    vec2_t elbow_front = v2_add(neck, pose->elbows[1]);
    vec2_t hand_front = v2_add(neck, pose->hands[1]);
    draw_capsulef(hip.x, hip.y, knee_front.x, knee_front.y, 1);
    draw_capsulef(knee_front.x, knee_front.y, foot_front.x, foot_front.y, 1);
    draw_capsulef(foot_front.x - 1.0f, foot_front.y, foot_front.x + 2.0f, foot_front.y, 1);
    draw_capsulef(neck.x, neck.y, elbow_front.x, elbow_front.y, 1);
    draw_capsulef(elbow_front.x, elbow_front.y, hand_front.x, hand_front.y, 1);
    fill_circlef(hand_back.x, hand_back.y, 1.0f);
    fill_circlef(hand_front.x, hand_front.y, 1.0f);
}

static void __attribute__((unused)) draw_actor_grounded(float x, float ground_y, const actor_pose_t *pose, float y_bias) {
    draw_actor_pose(x, actor_ground_hip_y(ground_y, pose) + y_bias, pose);
}

static void draw_crackle(int x0, int y0, int x1, int y1, float phase, int seed) {
    float n0 = hash01(seed * 13 + 1) - 0.5f;
    float n1 = hash01(seed * 17 + 5) - 0.5f;
    int mx = (x0 + x1) / 2 + (int)lroundf(n0 * 24.0f * sinf(phase * 3.0f + seed));
    int my = (y0 + y1) / 2 + (int)lroundf(n1 * 18.0f * cosf(phase * 2.0f + seed * 0.7f));
    draw_line(x0, y0, mx, my);
    draw_line(mx, my, x1, y1);
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

static void draw_large_char(int x, int y, char c, int scale) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                fill_rect(x + col * scale, y + row * scale,
                          x + col * scale + scale - 1,
                          y + row * scale + scale - 1);
            }
        }
    }
}

static void __attribute__((unused)) draw_large_str(int x, int y, const char *s, int scale) {
    while (*s) {
        draw_large_char(x, y, *s++, scale);
        x += 6 * scale;
    }
}

static void mask_px(uint8_t *mask, int x, int y) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < BLUE_H)
        mask[x + y * WIDTH] = 1;
}

static void mask_large_char(uint8_t *mask, int x, int y, char c, int scale) {
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = font5x7[c - 32];
    for (int col = 0; col < 5; col++) {
        uint8_t bits = g[col];
        for (int row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
                for (int yy = 0; yy < scale; yy++)
                    for (int xx = 0; xx < scale; xx++)
                        mask_px(mask, x + col * scale + xx, y + row * scale + yy);
            }
        }
    }
}

static void mask_large_str(uint8_t *mask, int x, int y, const char *s, int scale) {
    while (*s) {
        mask_large_char(mask, x, y, *s++, scale);
        x += 6 * scale;
    }
}

static void build_morph_targets(void) {
    uint8_t mask[WIDTH * BLUE_H];
    int points[WIDTH * BLUE_H][2];
    int point_count = 0;

    memset(mask, 0, sizeof(mask));
    mask_large_str(mask, 26, 8, "PI5", 4);
    for (int x = 22; x <= 105; x++) mask_px(mask, x, 40);
    for (int x = 26; x <= 101; x++) {
        if (((x / 3) & 1) == 0) mask_px(mask, x, 42);
    }

    for (int y = 0; y < BLUE_H; y++) {
        for (int x = 0; x < WIDTH; x++) {
            if (mask[x + y * WIDTH]) {
                points[point_count][0] = x;
                points[point_count][1] = y + BLUE_Y;
                point_count++;
            }
        }
    }

    if (point_count == 0) {
        morph_target_count = 1;
        morph_tx[0] = WIDTH / 2.0f;
        morph_ty[0] = BLUE_Y + BLUE_H / 2.0f;
        return;
    }

    morph_target_count = point_count;
    for (int i = 0; i < MORPH_PARTICLES; i++) {
        int idx = (i * point_count) / MORPH_PARTICLES;
        if (idx >= point_count) idx = point_count - 1;
        morph_tx[i] = (float)points[idx][0];
        morph_ty[i] = (float)points[idx][1];
    }
}

static const uint8_t dither4x4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5}
};

static vec3_t v3(float x, float y, float z) {
    vec3_t v = {x, y, z};
    return v;
}

static vec3_t v3_add(vec3_t a, vec3_t b) {
    return v3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static vec3_t v3_scale(vec3_t a, float s) {
    return v3(a.x * s, a.y * s, a.z * s);
}

static float v3_dot(vec3_t a, vec3_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static float v3_len(vec3_t a) {
    return sqrtf(v3_dot(a, a));
}

static vec3_t v3_norm(vec3_t a) {
    float l = v3_len(a);
    if (l <= 1e-6f) return v3(0.0f, 0.0f, 0.0f);
    return v3_scale(a, 1.0f / l);
}

static vec3_t rot_x(vec3_t p, float a) {
    float c = cosf(a), s = sinf(a);
    return v3(p.x, p.y * c - p.z * s, p.y * s + p.z * c);
}

static vec3_t rot_y(vec3_t p, float a) {
    float c = cosf(a), s = sinf(a);
    return v3(p.x * c + p.z * s, p.y, -p.x * s + p.z * c);
}

static float sd_box(vec3_t p, vec3_t b) {
    float qx = fabsf(p.x) - b.x;
    float qy = fabsf(p.y) - b.y;
    float qz = fabsf(p.z) - b.z;
    float ox = fmaxf(qx, 0.0f);
    float oy = fmaxf(qy, 0.0f);
    float oz = fmaxf(qz, 0.0f);
    float outside = sqrtf(ox * ox + oy * oy + oz * oz);
    float inside = fminf(fmaxf(qx, fmaxf(qy, qz)), 0.0f);
    return outside + inside;
}

static float sd_torus(vec3_t p, float major_r, float minor_r) {
    float qx = sqrtf(p.x * p.x + p.z * p.z) - major_r;
    return sqrtf(qx * qx + p.y * p.y) - minor_r;
}

static float hero_map(vec3_t p, float u) {
    float spin = 0.55f + u * 2.3f;
    float door_open = smoothstep_local((u - 0.16f) / 0.28f);
    float core_reveal = smoothstep_local((u - 0.34f) / 0.22f);
    float pulse = 0.5f + 0.5f * sinf(u * 24.0f);

    vec3_t q = rot_y(rot_x(p, 0.25f * sinf(u * 5.0f)), spin);
    float shell = fabsf(sd_box(q, v3(1.48f, 0.84f, 0.36f))) - 0.05f;

    vec3_t shutters = q;
    shutters.x = fabsf(shutters.x) - mixf_local(0.30f, 0.95f, door_open);
    float shutter = sd_box(shutters, v3(0.18f, 0.72f, 0.18f));

    vec3_t ring_p = rot_y(q, u * 5.4f + pulse * 0.6f);
    float ring = sd_torus(v3(ring_p.x, ring_p.y, ring_p.z + 0.04f),
                          0.72f + 0.09f * sinf(u * 17.0f), 0.10f + 0.02f * pulse);

    vec3_t core_p = q;
    core_p.z -= mixf_local(0.78f, 0.18f, core_reveal);
    float core = v3_len(core_p) - mixf_local(0.05f, 0.24f, core_reveal);

    vec3_t halo_p = q;
    halo_p.z += 0.50f;
    float halo = sd_torus(halo_p, 1.08f, 0.05f);

    vec3_t corridor_p = q;
    corridor_p.z = fmodf(corridor_p.z + u * 3.5f + 4.0f, 0.95f) - 0.475f;
    float corridor = sd_torus(corridor_p, 0.94f, 0.035f);

    return fminf(fminf(fminf(shell, shutter), fminf(ring, core)), fminf(halo, corridor));
}

static vec3_t hero_normal(vec3_t p, float u) {
    const float e = 0.02f;
    float dx = hero_map(v3(p.x + e, p.y, p.z), u) - hero_map(v3(p.x - e, p.y, p.z), u);
    float dy = hero_map(v3(p.x, p.y + e, p.z), u) - hero_map(v3(p.x, p.y - e, p.z), u);
    float dz = hero_map(v3(p.x, p.y, p.z + e), u) - hero_map(v3(p.x, p.y, p.z - e), u);
    return v3_norm(v3(dx, dy, dz));
}

static void precompute_hero_frames(void) {
    if (hero_ready) return;

    for (int frame = 0; frame < HERO_FRAME_COUNT; frame++) {
        float u = (float)frame / (float)(HERO_FRAME_COUNT - 1);
        float pulse = 0.5f + 0.5f * sinf(u * 24.0f);
        memset(hero_frames[frame], 0, sizeof(hero_frames[frame]));

        for (int y = 0; y < BLUE_H; y++) {
            for (int x = 0; x < WIDTH; x++) {
                float uvx = (((float)x + 0.5f) / (float)WIDTH - 0.5f) * 2.3f;
                float uvy = (((float)y + 0.5f) / (float)BLUE_H - 0.5f) * 1.6f;
                float cam_z = mixf_local(-5.0f, -2.3f, smoothstep_local((u - 0.05f) / 0.78f));
                vec3_t ro = v3(0.0f, 0.0f, cam_z);
                vec3_t rd = v3_norm(v3(uvx, uvy, 1.55f + u * 0.55f));
                float sway = sinf(u * 6.2f) * 0.34f;
                ro = rot_y(ro, sway);
                rd = rot_y(rd, sway);
                ro = rot_x(ro, cosf(u * 5.1f) * 0.12f);
                rd = rot_x(rd, cosf(u * 5.1f) * 0.12f);

                float dist = 0.0f;
                int hit = 0;
                for (int step = 0; step < 28; step++) {
                    vec3_t p = v3_add(ro, v3_scale(rd, dist));
                    float d = hero_map(p, u);
                    if (d < 0.02f) {
                        hit = 1;
                        break;
                    }
                    dist += d;
                    if (dist > 9.5f) break;
                }

                if (hit) {
                    vec3_t p = v3_add(ro, v3_scale(rd, dist));
                    vec3_t n = hero_normal(p, u);
                    vec3_t light_a = v3_norm(v3(-0.62f, 0.58f, -0.40f));
                    vec3_t light_b = v3_norm(v3(0.45f, -0.18f, -0.78f));
                    float diff = clampf_local(v3_dot(n, light_a), 0.0f, 1.0f) * 0.78f
                               + clampf_local(v3_dot(n, light_b), 0.0f, 1.0f) * 0.28f;
                    float fres = powf(1.0f - clampf_local(v3_dot(n, v3_scale(rd, -1.0f)), 0.0f, 1.0f), 3.1f);
                    float fog = expf(-dist * 0.16f);
                    float inten = clampf_local((0.12f + diff * 0.72f + fres * 0.42f + pulse * 0.10f) * fog,
                                               0.0f, 1.0f);
                    int threshold = dither4x4[y & 3][x & 3];
                    if ((int)lroundf(inten * 15.0f) >= threshold)
                        frame_px(hero_frames[frame], x, y);
                } else {
                    float vignette = 1.0f - clampf_local(sqrtf(uvx * uvx * 0.25f + uvy * uvy * 0.8f), 0.0f, 1.0f);
                    float beams = 0.5f + 0.5f * sinf(uvx * 13.0f + u * 18.0f);
                    float shaft = expf(-uvx * uvx * 9.0f) * (0.16f + 0.14f * pulse);
                    float inten = vignette * (0.14f * beams + shaft);
                    if ((int)lroundf(inten * 15.0f) > dither4x4[y & 3][x & 3] + 3)
                        frame_px(hero_frames[frame], x, y);
                }
            }
        }
    }

    hero_ready = 1;
}

static void blit_hero_frame(int idx) {
    if (!hero_ready) return;
    idx %= HERO_FRAME_COUNT;
    if (idx < 0) idx += HERO_FRAME_COUNT;
    for (int page = 0; page < BLUE_PAGES; page++) {
        memcpy(fb + WIDTH * (page + YELLOW_H / 8),
               hero_frames[idx] + WIDTH * page,
               WIDTH);
    }
}

static void init_assets(void) {
    build_morph_targets();
    precompute_hero_frames();
}

static const actor_pose_t palace_stand_pose = {
    { 0.0f, -9.0f },
    {{ -2.0f, -6.0f }, { 2.0f, -6.0f }},
    {{ -2.0f, -2.0f }, { 3.0f, -2.0f }},
    {{ -2.0f, 4.0f }, { 2.0f, 4.0f }},
    {{ -2.0f, 10.0f }, { 2.0f, 10.0f }},
    2.5f
};

static const actor_pose_t palace_ready_pose = {
    { 0.5f, -9.0f },
    {{ 0.0f, -6.0f }, { 3.5f, -5.5f }},
    {{ 0.0f, -2.0f }, { 6.0f, -3.0f }},
    {{ 0.0f, 4.0f }, { 2.0f, 4.0f }},
    {{ 0.0f, 10.0f }, { 3.0f, 10.0f }},
    2.5f
};

static const actor_pose_t palace_walk_cycle[4] = {
    {
        { -0.6f, -9.2f },
        {{ 2.0f, -5.8f }, { -2.0f, -5.0f }},
        {{ 5.5f, -2.4f }, { -5.0f, -2.0f }},
        {{ -4.0f, 4.8f }, { 3.8f, 3.7f }},
        {{ -7.0f, 10.0f }, { 6.6f, 10.0f }},
        2.5f
    },
    {
        { 1.0f, -9.6f },
        {{ 1.8f, -5.5f }, { -1.2f, -5.8f }},
        {{ 4.2f, -2.4f }, { -4.4f, -2.8f }},
        {{ 1.5f, 5.4f }, { 0.0f, 2.2f }},
        {{ 1.5f, 11.0f }, { 2.6f, 7.8f }},
        2.5f
    },
    {
        { 0.6f, -9.2f },
        {{ -2.0f, -5.0f }, { 2.0f, -5.8f }},
        {{ -5.0f, -2.0f }, { 5.5f, -2.4f }},
        {{ -3.8f, 3.7f }, { 4.0f, 4.8f }},
        {{ -6.6f, 10.0f }, { 7.0f, 10.0f }},
        2.5f
    },
    {
        { -1.0f, -9.6f },
        {{ -1.2f, -5.8f }, { 1.8f, -5.5f }},
        {{ -4.4f, -2.8f }, { 4.2f, -2.4f }},
        {{ 0.0f, 2.2f }, { 1.5f, 5.4f }},
        {{ 2.6f, 7.8f }, { 1.5f, 11.0f }},
        2.5f
    }
};

static const actor_pose_t palace_run_cycle[4] = {
    {
        { 1.8f, -8.7f },
        {{ 3.2f, -6.0f }, { -1.0f, -5.0f }},
        {{ 6.8f, -2.2f }, { -4.6f, -1.4f }},
        {{ -5.0f, 4.4f }, { 5.2f, 2.9f }},
        {{ -8.2f, 10.2f }, { 8.8f, 9.4f }},
        2.4f
    },
    {
        { 2.8f, -8.3f },
        {{ 2.6f, -5.2f }, { -0.4f, -5.8f }},
        {{ 5.0f, -1.8f }, { -3.8f, -2.6f }},
        {{ 1.5f, 4.2f }, { -0.6f, 1.2f }},
        {{ 3.5f, 10.4f }, { -1.8f, 6.2f }},
        2.4f
    },
    {
        { 2.2f, -8.8f },
        {{ -1.0f, -5.0f }, { 3.2f, -6.0f }},
        {{ -4.6f, -1.4f }, { 6.8f, -2.2f }},
        {{ -5.2f, 2.9f }, { 5.0f, 4.4f }},
        {{ -8.8f, 9.4f }, { 8.2f, 10.2f }},
        2.4f
    },
    {
        { 1.2f, -8.4f },
        {{ -0.4f, -5.8f }, { 2.6f, -5.2f }},
        {{ -3.8f, -2.6f }, { 5.0f, -1.8f }},
        {{ 0.6f, 1.2f }, { -1.5f, 4.2f }},
        {{ 1.8f, 6.2f }, { -3.5f, 10.4f }},
        2.4f
    }
};

static const actor_pose_t palace_skid_pose = {
    { -6.2f, -7.7f },
    {{ -7.6f, -7.2f }, { -3.4f, -6.0f }},
    {{ -10.2f, -4.1f }, { -1.4f, -1.8f }},
    {{ 2.8f, 5.1f }, { 8.0f, 3.5f }},
    {{ 4.8f, 10.0f }, { 12.2f, 10.0f }},
    2.5f
};

static const actor_pose_t palace_skid_recover_pose = {
    { -3.8f, -8.2f },
    {{ -4.8f, -6.8f }, { -1.0f, -5.8f }},
    {{ -7.2f, -3.4f }, { 2.2f, -1.8f }},
    {{ 1.8f, 4.7f }, { 6.0f, 3.8f }},
    {{ 3.6f, 10.0f }, { 9.0f, 10.0f }},
    2.5f
};

static const actor_pose_t palace_crouch_pose = {
    { 0.0f, -7.0f },
    {{ 1.0f, -5.0f }, { 4.0f, -5.0f }},
    {{ -1.0f, -1.0f }, { 6.0f, -1.0f }},
    {{ 1.0f, 4.0f }, { 4.0f, 4.0f }},
    {{ 1.0f, 9.0f }, { 6.0f, 9.0f }},
    2.3f
};

static const actor_pose_t palace_leap_pose = {
    { 3.0f, -9.0f },
    {{ 3.0f, -10.0f }, { 0.0f, -9.0f }},
    {{ 6.0f, -13.0f }, { -2.0f, -11.0f }},
    {{ 2.0f, 0.0f }, { -1.0f, 1.0f }},
    {{ 4.0f, 5.0f }, { -4.0f, 6.0f }},
    2.4f
};

static const actor_pose_t palace_hang_pose = {
    { 0.0f, -7.0f },
    {{ 1.0f, -10.0f }, { 4.0f, -10.0f }},
    {{ 1.0f, -14.0f }, { 4.0f, -14.0f }},
    {{ 1.0f, 2.0f }, { 4.0f, 2.0f }},
    {{ 1.0f, 8.0f }, { 4.0f, 8.0f }},
    2.3f
};

static const actor_pose_t palace_hang_swing_pose = {
    { -1.0f, -7.0f },
    {{ 0.0f, -10.0f }, { 3.0f, -10.0f }},
    {{ 1.0f, -14.0f }, { 4.0f, -14.0f }},
    {{ -1.0f, 3.0f }, { 3.0f, 2.0f }},
    {{ -2.0f, 9.0f }, { 4.0f, 7.0f }},
    2.3f
};

static const actor_pose_t palace_climb_pose = {
    { 2.0f, -5.0f },
    {{ 2.0f, -9.0f }, { 6.0f, -6.0f }},
    {{ 2.0f, -13.0f }, { 9.0f, -7.0f }},
    {{ 4.0f, -1.0f }, { 1.0f, 3.0f }},
    {{ 6.0f, 4.0f }, { 2.0f, 8.0f }},
    2.3f
};

static const actor_pose_t palace_mantle_pose = {
    { 5.0f, -4.0f },
    {{ 2.0f, -4.0f }, { 7.0f, -3.5f }},
    {{ -1.0f, -1.0f }, { 10.0f, -1.0f }},
    {{ 2.0f, 2.0f }, { 7.0f, 2.5f }},
    {{ 0.0f, 8.0f }, { 10.0f, 8.0f }},
    2.2f
};

static const actor_pose_t palace_crawl_cycle[2] = {
    {
        { 7.0f, -4.0f },
        {{ 5.0f, -4.0f }, { 10.0f, -4.0f }},
        {{ 2.0f, -1.0f }, { 13.0f, -1.0f }},
        {{ 2.0f, 2.0f }, { 9.0f, 3.0f }},
        {{ 0.0f, 7.0f }, { 11.0f, 7.0f }},
        2.0f
    },
    {
        { 7.0f, -4.0f },
        {{ 4.0f, -4.0f }, { 9.0f, -3.0f }},
        {{ 1.0f, -1.0f }, { 12.0f, 0.0f }},
        {{ 4.0f, 3.0f }, { 7.0f, 2.0f }},
        {{ 1.0f, 7.0f }, { 10.0f, 6.0f }},
        2.0f
    }
};

static void draw_sine_separator(int y, float t) {
    for (int x = 0; x < WIDTH; x++) {
        int sy = y + (int)lroundf(sinf((float)x * 0.10f + t * 2.1f) * 1.2f);
        px(x, sy);
    }
}

static void draw_header(const char *title, int scene_idx, float scene_t, float scene_duration, float sim_t) {
    char buf[16];
    float progress = clampf_local(scene_duration > 0.0f ? scene_t / scene_duration : 0.0f, 0.0f, 1.0f);
    int fill_w = (int)lroundf(progress * 44.0f);

    outline_rect(0, 0, WIDTH - 1, YELLOW_H - 1);
    draw_str(3, 4, title);
    outline_rect(44, 4, 89, 10);
    if (fill_w > 0)
        fill_rect(45, 5, 44 + fill_w, 9);

    snprintf(buf, sizeof(buf), "%d/%d", scene_idx + 1, SCENE_COUNT);
    draw_str(100, 4, buf);

    for (int i = 0; i < SCENE_COUNT; i++) {
        int bx = 4 + i * 8;
        outline_rect(bx, 11, bx + 4, 14);
        if (i == scene_idx)
            fill_rect(bx + 1, 12, bx + 3, 13);
    }

    for (int x = 72; x < WIDTH; x += 8) {
        int yy = 12 + (int)lroundf(sinf(sim_t * 1.5f + x * 0.18f) * 1.0f);
        px(x, yy);
    }

    draw_sine_separator(YELLOW_H - 1, sim_t);
}

static void draw_flash_overlay(int scene_idx, float scene_t, float scene_duration) {
    float edge = scene_t;
    float tail = scene_duration - scene_t;
    float strength = 0.0f;

    if (edge < FLASH_TIME) strength = 1.0f - edge / FLASH_TIME;
    if (tail < FLASH_TIME) {
        float tail_strength = 1.0f - tail / FLASH_TIME;
        if (tail_strength > strength) strength = tail_strength;
    }
    if (scene_idx == SCENE_COUNT - 1 && tail < WHITEOUT_TIME) {
        float whiteout = 1.0f - tail / WHITEOUT_TIME;
        if (whiteout > strength) strength = whiteout;
    }
    if (strength <= 0.0f) return;

    int level = (int)lroundf(clampf_local(strength, 0.0f, 1.0f) * 15.0f);
    for (int y = BLUE_Y; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int bias = ((x + y) & 1) ? 1 : 0;
            if (dither4x4[y & 3][x & 3] <= level + bias)
                px(x, y);
        }
    }
}

static void draw_scene_boot(float scene_t, float sim_t) {
    int cx = WIDTH / 2;
    int cy = BLUE_Y + BLUE_H / 2;
    int text_y = BLUE_Y + 10;

    for (int i = 0; i < 18; i++) {
        int ex, ey;
        switch (i & 3) {
            case 0: ex = (i * 11) % WIDTH; ey = BLUE_Y; break;
            case 1: ex = WIDTH - 1; ey = BLUE_Y + (i * 7) % BLUE_H; break;
            case 2: ex = WIDTH - 1 - ((i * 13) % WIDTH); ey = HEIGHT - 1; break;
            default: ex = 0; ey = BLUE_Y + (i * 9) % BLUE_H; break;
        }
        draw_crackle(ex, ey, cx, cy, sim_t, i + 1);
    }

    for (int i = 0; i < 3; i++) {
        int band = (int)lroundf(fmodf(scene_t * (18.0f + i * 7.0f), (float)(BLUE_H + 10))) - 4;
        int y = BLUE_Y + band;
        fill_rect(0, y, WIDTH - 1, y + 1);
    }

    if (scene_t > 0.5f) draw_large_char(22, text_y, 'P', 4);
    if (scene_t > 1.1f) draw_large_char(50, text_y, 'I', 4);
    if (scene_t > 1.7f) draw_large_char(72, text_y, '5', 4);
    if (scene_t > 2.2f) draw_str(23, BLUE_Y + 39, "DEMOSCENE REEL");

    if (((int)(scene_t * 6.0f)) & 1)
        outline_rect(18, BLUE_Y + 6, 109, HEIGHT - 6);
}

static void draw_scene_starfield(float scene_t, float sim_t) {
    (void)sim_t;

    int cx = WIDTH / 2;
    int cy = BLUE_Y + BLUE_H / 2;
    float burst = scene_t * (0.45f + scene_t * 0.75f);

    for (int i = 0; i < 112; i++) {
        float speed = 0.18f + hash01(i * 17 + 9) * 0.65f;
        float z = fractf_local(hash01(i * 11 + 3) + burst * speed);
        float z_prev = z - speed * (1.0f / TARGET_FPS);
        if (z_prev < 0.0f) z_prev += 1.0f;

        float bx = (hash01(i * 13 + 1) - 0.5f) * 2.4f;
        float by = (hash01(i * 29 + 5) - 0.5f) * 1.8f;
        float scale = 1.5f + z * z * 90.0f;
        float prev_scale = 1.5f + z_prev * z_prev * 90.0f;

        int x0 = cx + (int)lroundf(bx * prev_scale);
        int y0 = cy + (int)lroundf(by * prev_scale);
        int x1 = cx + (int)lroundf(bx * scale);
        int y1 = cy + (int)lroundf(by * scale);
        draw_line(x0, y0, x1, y1);
        if (z > 0.86f) {
            px(x1 + 1, y1);
            px(x1, y1 + 1);
        }
    }

    if (scene_t < 1.0f) {
        int r = 2 + (int)lroundf(scene_t * 10.0f);
        outline_rect(cx - r, cy - r, cx + r, cy + r);
    }
    draw_line(cx - 8, cy, cx + 8, cy);
    draw_line(cx, cy - 8, cx, cy + 8);
}

static void draw_scene_roto(float scene_t, float sim_t) {
    (void)sim_t;

    int horizon = BLUE_Y + 4 + (int)lroundf(sinf(scene_t * 1.2f) * 1.5f);
    float ang = scene_t * 0.85f;
    float ca = cosf(ang), sa = sinf(ang);
    float cam_z = scene_t * 2.8f;

    for (int y = horizon; y < HEIGHT; y++) {
        float p = (float)(y - horizon + 1) / (float)(HEIGHT - horizon + 1);
        float depth = 0.9f / (p + 0.08f) + cam_z;
        float edge_glow = 1.0f / (p + 0.2f);

        for (int x = 0; x < WIDTH; x++) {
            float nx = (float)(x - WIDTH / 2) / 24.0f;
            float wx = nx * depth;
            float wz = depth;
            float rx = wx * ca - wz * sa;
            float rz = wx * sa + wz * ca;
            float cell_u = rx / 1.45f;
            float cell_v = rz / 1.45f;
            int iu = (int)floorf(cell_u);
            int iv = (int)floorf(cell_v);
            float fu = fabsf(fractf_local(cell_u) - 0.5f);
            float fv = fabsf(fractf_local(cell_v) - 0.5f);
            int boundary = (fu > 0.46f) || (fv > 0.46f);
            int checker = ((iu + iv) & 1) == 0;

            if (boundary || (checker && (edge_glow > 1.6f || ((x + y) & 1) == 0)))
                px(x, y);
        }
    }

    draw_line(WIDTH / 2 - 8, horizon, WIDTH / 2 + 8, horizon);
    draw_line(WIDTH / 2, horizon - 4, WIDTH / 2, horizon + 4);
}

static void draw_scene_tunnel(float scene_t, float sim_t) {
    (void)sim_t;

    enum { RINGS = 9, SEGMENTS = 12 };
    int prev_x[SEGMENTS] = {0};
    int prev_y[SEGMENTS] = {0};
    int have_prev = 0;
    float light_a = scene_t * 1.6f;

    for (int r = 0; r < RINGS; r++) {
        int ring_x[SEGMENTS];
        int ring_y[SEGMENTS];
        float p = 1.0f - fractf_local(scene_t * 0.50f + (float)r / (float)RINGS);
        float cx = WIDTH / 2.0f + sinf(scene_t * 1.3f + p * 5.1f) * 15.0f * (1.0f - p * 0.25f);
        float cy = BLUE_Y + BLUE_H / 2.0f + cosf(scene_t * 1.7f + p * 6.0f) * 7.0f * (1.0f - p * 0.35f);
        float rx = 6.0f + p * p * 58.0f;
        float ry = 4.0f + p * p * 20.0f;
        float spin = scene_t * 2.0f + p * 7.0f;

        for (int s = 0; s < SEGMENTS; s++) {
            float a = spin + (float)s * 2.0f * (float)M_PI / (float)SEGMENTS;
            ring_x[s] = (int)lroundf(cx + cosf(a) * rx);
            ring_y[s] = (int)lroundf(cy + sinf(a) * ry);
        }

        for (int s = 0; s < SEGMENTS; s++) {
            int n = (s + 1) % SEGMENTS;
            float a = spin + (float)s * 2.0f * (float)M_PI / (float)SEGMENTS;
            float light = 0.5f + 0.5f * cosf(a - light_a);
            int shade = 1 + (int)lroundf(light * 3.0f);
            draw_line_shaded(ring_x[s], ring_y[s], ring_x[n], ring_y[n], shade);
            if (have_prev && (s % 2 == 0))
                draw_line_shaded(prev_x[s], prev_y[s], ring_x[s], ring_y[s], shade);
            if (light > 0.75f)
                px(ring_x[s], ring_y[s]);
        }

        memcpy(prev_x, ring_x, sizeof(prev_x));
        memcpy(prev_y, ring_y, sizeof(prev_y));
        have_prev = 1;
    }
}

static void draw_scene_morph(float scene_t, float sim_t) {
    float form = smoothstep_local(scene_t / 1.9f);
    float hold = smoothstep_local((scene_t - 1.9f) / 0.8f);
    float shatter = smoothstep_local((scene_t - 3.0f) / 0.7f);
    float reform = smoothstep_local((scene_t - 4.5f) / 0.9f);
    float burst = shatter * (1.0f - reform);
    float cx = WIDTH / 2.0f;
    float cy = BLUE_Y + BLUE_H / 2.0f;

    for (int i = 0; i < MORPH_PARTICLES; i++) {
        float seed_a = hash01(i * 17 + 7);
        float seed_b = hash01(i * 29 + 3);
        float seed_c = hash01(i * 43 + 19);
        float angle = seed_a * 2.0f * (float)M_PI + scene_t * (1.7f + seed_b * 1.3f);
        float radius = 10.0f + 34.0f * seed_b + 7.0f * sinf(scene_t * 2.4f + i * 0.05f);
        float sx = cx + cosf(angle) * radius;
        float sy = cy + sinf(angle * 1.6f) * radius * 0.52f + cosf(scene_t * 3.5f + seed_c * 8.0f) * 2.0f;
        float tx = morph_tx[i];
        float ty = morph_ty[i];
        float dx = tx - cx;
        float dy = ty - cy;
        float dl = sqrtf(dx * dx + dy * dy) + 0.01f;
        float burst_x = tx + dx / dl * (12.0f + 22.0f * seed_b) + cosf(angle * 2.0f + scene_t * 8.0f) * 4.5f;
        float burst_y = ty + dy / dl * (12.0f + 22.0f * seed_b) + sinf(angle * 1.7f + scene_t * 7.0f) * 4.0f;

        float x = mixf_local(sx, tx, form);
        float y = mixf_local(sy, ty, form);
        x = mixf_local(x, burst_x, burst);
        y = mixf_local(y, burst_y, burst);
        x = mixf_local(x, tx, reform);
        y = mixf_local(y, ty, reform);

        if ((i % 7 == 0) && (form < 0.98f || burst > 0.05f)) {
            int trail_x = (int)lroundf(burst > 0.10f ? tx : sx);
            int trail_y = (int)lroundf(burst > 0.10f ? ty : sy);
            draw_line_shaded(trail_x, trail_y, (int)lroundf(x), (int)lroundf(y), burst > 0.10f ? 2 : 1);
        }

        px((int)lroundf(x), (int)lroundf(y));
        if ((hold > 0.35f || burst > 0.15f) && ((i + (int)(scene_t * 5.0f)) % 9 == 0)) {
            px((int)lroundf(x) + 1, (int)lroundf(y));
            px((int)lroundf(x), (int)lroundf(y) + 1);
        }
    }

    if (hold > 0.20f && burst < 0.90f) {
        int pulse_r = 7 + (int)lroundf((1.0f - burst) * 4.0f + sinf(scene_t * 6.0f) * 1.2f);
        outline_rect(18, BLUE_Y + 5, 109, HEIGHT - 5);
        if (((int)(scene_t * 4.0f)) & 1)
            outline_rect(24, BLUE_Y + 9, 103, HEIGHT - 9);
        draw_line((int)cx - pulse_r, (int)cy, (int)cx + pulse_r, (int)cy);
        draw_line((int)cx, (int)cy - pulse_r / 2, (int)cx, (int)cy + pulse_r / 2);
        draw_line(20, BLUE_Y + 43, 108, BLUE_Y + 43);
    }

    if (burst > 0.18f) {
        for (int i = 0; i < 10; i++) {
            float a = scene_t * 3.2f + (float)i * 2.0f * (float)M_PI / 10.0f;
            int x1 = (int)lroundf(cx + cosf(a) * (12.0f + burst * 26.0f));
            int y1 = (int)lroundf(cy + sinf(a) * (6.0f + burst * 12.0f));
            draw_line_shaded((int)cx, (int)cy, x1, y1, 2);
        }
    }
}

static void draw_palace_floor_segment(int world_x0, int world_x1, int top_y, float camera_x) {
    int x0 = (int)lroundf((float)world_x0 - camera_x);
    int x1 = (int)lroundf((float)world_x1 - camera_x);
    int tile_start = world_x0 + ((8 - (world_x0 & 7)) & 7);
    int brick_start = world_x0 + ((16 - (world_x0 & 15)) & 15);

    draw_line(x0, top_y, x1, top_y);
    draw_line(x0, top_y + 5, x1, top_y + 5);

    for (int x = tile_start; x <= world_x1; x += 8) {
        int sx = (int)lroundf((float)x - camera_x);
        draw_line(sx, top_y, sx, top_y + 5);
    }

    for (int x = brick_start; x < world_x1; x += 16) {
        int sx0 = (int)lroundf((float)(x - 8) - camera_x);
        int sx1 = (int)lroundf((float)x - camera_x);
        draw_line(sx0, top_y + 3, sx1, top_y + 3);
    }
}

static void draw_palace_backdrop(float camera_x, float sim_t) {
    for (int i = 0; i < 6; i++) {
        int x = (int)lroundf(10.0f + (float)i * 28.0f - camera_x * 0.35f);
        int top = BLUE_Y + 7 + (i & 1);
        int base = BLUE_Y + 24;

        draw_line(x, base, x, top + 6);
        draw_line(x + 10, base, x + 10, top + 6);
        draw_line(x, top + 6, x + 3, top + 2);
        draw_line(x + 3, top + 2, x + 5, top);
        draw_line(x + 5, top, x + 7, top + 2);
        draw_line(x + 7, top + 2, x + 10, top + 6);
        if (((i + (int)floorf(sim_t * 1.5f)) & 1) == 0)
            draw_line(x + 5, top + 6, x + 5, base);
    }

    for (int i = 0; i < 2; i++) {
        int sx = (int)lroundf((i == 0 ? 108.0f : 145.0f) - camera_x);
        int flame_y = BLUE_Y + 10 + (int)lroundf(sinf(sim_t * 6.0f + i * 1.7f) * 1.5f);
        draw_line(sx, BLUE_Y + 16, sx, BLUE_Y + 20);
        px(sx, flame_y);
        px(sx - 1, flame_y + 1);
        if ((((int)(sim_t * 12.0f)) + i) & 1) px(sx + 1, flame_y + 1);
    }
}

static void draw_palace_archway(int world_x0, int world_x1, int floor_y, float camera_x) {
    int x0 = (int)lroundf((float)world_x0 - camera_x);
    int x1 = (int)lroundf((float)world_x1 - camera_x);
    int mid = (x0 + x1) / 2;
    int shoulder_y = floor_y - 10;
    int crown_y = floor_y - 15;

    draw_line(x0, floor_y, x0, shoulder_y);
    draw_line(x1, floor_y, x1, shoulder_y);
    draw_line(x0, shoulder_y, x0 + 5, shoulder_y - 2);
    draw_line(x0 + 5, shoulder_y - 2, mid, crown_y);
    draw_line(mid, crown_y, x1 - 5, shoulder_y - 2);
    draw_line(x1 - 5, shoulder_y - 2, x1, shoulder_y);
    draw_line(x0 + 1, shoulder_y + 1, x1 - 1, shoulder_y + 1);
}

static void draw_palace_gate(int world_x0, int world_x1, int top_y, int floor_y, float open_u, float camera_x) {
    int x0 = (int)lroundf((float)world_x0 - camera_x);
    int x1 = (int)lroundf((float)world_x1 - camera_x);
    int rise = (int)lroundf(clampf_local(open_u, 0.0f, 1.0f) * (float)(floor_y - top_y - 2));

    outline_rect(x0, top_y, x1, floor_y);
    draw_line(x0 + 1, top_y + 2, x1 - 1, top_y + 2);
    for (int x = x0 + 2; x <= x1 - 2; x += 3)
        draw_line(x, top_y + 3 - rise, x, floor_y - 1 - rise);
}

static void draw_scene_palace(float scene_t, float scene_duration, float sim_t) {
    const float lower_floor = 56.0f;
    const float upper_floor = 44.0f;
    const float ledge_x = 92.0f;
    const float gate_u = smoothstep_local((scene_t - 9.35f) / 0.90f);
    float actor_x = 12.0f;
    float actor_hip_y = actor_ground_hip_y(lower_floor, &palace_stand_pose);
    float camera_x;
    actor_pose_t pose = palace_stand_pose;
    int grounded = 1;
    int effect = 0;
    float p = 0.0f;

    (void)scene_duration;

    if (scene_t < 1.20f) {
        p = smoothstep_local(scene_t / 1.20f);
        actor_x = mixf_local(10.0f, 27.0f, p);
        build_keyed_gait_pose(actor_x, lower_floor, palace_walk_cycle, 4, 12.0f, 0.18f, &pose, &actor_hip_y);
        pose = actor_pose_mix(palace_stand_pose, pose, p);
        actor_hip_y = mixf_local(actor_ground_hip_y(lower_floor, &palace_stand_pose), actor_hip_y, p);
    } else if (scene_t < 3.50f) {
        p = (scene_t - 1.20f) / 2.30f;
        actor_x = mixf_local(27.0f, 74.0f, p);
        build_keyed_gait_pose(actor_x, lower_floor, palace_run_cycle, 4, 14.0f, 0.10f, &pose, &actor_hip_y);
    } else if (scene_t < 4.10f) {
        p = smoothstep_local((scene_t - 3.50f) / 0.60f);
        actor_x = mixf_local(74.0f, 78.0f, p);
        actor_pose_t run_pose;
        float run_hip_y;
        build_keyed_gait_pose(actor_x, lower_floor, palace_run_cycle, 4, 14.0f, 0.10f, &run_pose, &run_hip_y);
        pose = actor_pose_mix(run_pose, palace_skid_pose, p);
        actor_hip_y = mixf_local(run_hip_y, actor_ground_hip_y(lower_floor, &palace_skid_pose) + 0.9f, p);
        effect = 1;
    } else if (scene_t < 4.45f) {
        p = smoothstep_local((scene_t - 4.10f) / 0.35f);
        actor_x = 78.0f;
        if (p < 0.5f) {
            float u = smoothstep_local(p / 0.5f);
            pose = actor_pose_mix(palace_skid_pose, palace_skid_recover_pose, u);
            actor_hip_y = mixf_local(actor_ground_hip_y(lower_floor, &palace_skid_pose) + 0.9f,
                                     actor_ground_hip_y(lower_floor, &palace_skid_recover_pose) + 0.5f,
                                     u);
        } else {
            float u = smoothstep_local((p - 0.5f) / 0.5f);
            pose = actor_pose_mix(palace_skid_recover_pose, palace_crouch_pose, u);
            actor_hip_y = mixf_local(actor_ground_hip_y(lower_floor, &palace_skid_recover_pose) + 0.5f,
                                     actor_ground_hip_y(lower_floor, &palace_crouch_pose),
                                     u);
        }
        effect = 1;
    } else if (scene_t < 5.15f) {
        float jump_p = clampf_local((scene_t - 4.45f) / 0.70f, 0.0f, 1.0f);
        float start_hip = actor_ground_hip_y(lower_floor, &palace_crouch_pose);
        float end_hip = 57.0f;
        actor_x = mixf_local(78.0f, ledge_x + 0.5f, smoothstep_local(jump_p));
        grounded = 0;
        actor_hip_y = mixf_local(start_hip, end_hip, jump_p) - sinf(jump_p * (float)M_PI) * 9.0f;
        if (jump_p < 0.45f)
            pose = actor_pose_mix(palace_crouch_pose, palace_leap_pose,
                                  smoothstep_local(jump_p / 0.45f));
        else
            pose = actor_pose_mix(palace_leap_pose, palace_hang_pose,
                                  smoothstep_local((jump_p - 0.45f) / 0.55f));
        effect = 2;
    } else if (scene_t < 5.85f) {
        float swing = 0.5f + 0.5f * sinf(scene_t * 5.0f);
        actor_x = ledge_x + sinf(sim_t * 4.2f) * 0.25f;
        grounded = 0;
        actor_hip_y = 57.0f + sinf(scene_t * 5.0f) * 0.4f;
        pose = actor_pose_mix(palace_hang_pose, palace_hang_swing_pose, swing);
    } else if (scene_t < 6.70f) {
        float climb_p = smoothstep_local((scene_t - 5.85f) / 0.85f);
        float end_hip = actor_ground_hip_y(upper_floor, &palace_crawl_cycle[0]);
        actor_x = mixf_local(ledge_x, 97.0f, climb_p);
        grounded = 0;
        actor_hip_y = mixf_local(57.0f, end_hip, climb_p);
        if (climb_p < 0.5f)
            pose = actor_pose_mix(palace_hang_swing_pose, palace_climb_pose,
                                  smoothstep_local(climb_p * 2.0f));
        else
            pose = actor_pose_mix(palace_climb_pose, palace_mantle_pose,
                                  smoothstep_local((climb_p - 0.5f) * 2.0f));
    } else if (scene_t < 8.50f) {
        p = (scene_t - 6.70f) / 1.80f;
        actor_x = mixf_local(97.0f, 133.0f, p);
        pose = sample_actor_cycle(palace_crawl_cycle, 2, p * 2.80f);
        actor_hip_y = actor_ground_hip_y(upper_floor, &pose);
    } else if (scene_t < 9.10f) {
        p = smoothstep_local((scene_t - 8.50f) / 0.60f);
        actor_x = mixf_local(133.0f, 138.0f, p);
        pose = actor_pose_mix(palace_crawl_cycle[0], palace_ready_pose, p);
        actor_hip_y = mixf_local(actor_ground_hip_y(upper_floor, &palace_crawl_cycle[0]),
                                 actor_ground_hip_y(upper_floor, &palace_ready_pose), p);
    } else if (scene_t < 10.20f) {
        p = (scene_t - 9.10f) / 1.10f;
        actor_x = mixf_local(138.0f, 154.0f, p);
        actor_pose_t run_pose;
        float run_hip_y;
        float blend = smoothstep_local(clampf_local(p * 1.7f, 0.0f, 1.0f));
        build_keyed_gait_pose(actor_x, upper_floor, palace_run_cycle, 4, 14.0f, 0.10f, &run_pose, &run_hip_y);
        pose = actor_pose_mix(palace_ready_pose, run_pose, blend);
        actor_hip_y = mixf_local(actor_ground_hip_y(upper_floor, &palace_ready_pose), run_hip_y, blend);
    } else {
        p = smoothstep_local((scene_t - 10.20f) / 0.80f);
        actor_x = mixf_local(154.0f, 160.0f, p);
        actor_pose_t run_pose;
        float run_hip_y;
        build_keyed_gait_pose(actor_x, upper_floor, palace_run_cycle, 4, 14.0f, 0.10f, &run_pose, &run_hip_y);
        pose = actor_pose_mix(run_pose, palace_ready_pose, p);
        actor_hip_y = mixf_local(run_hip_y, actor_ground_hip_y(upper_floor, &palace_ready_pose), p);
    }

    camera_x = clampf_local(actor_x - 68.0f, 0.0f, 54.0f);

    draw_palace_backdrop(camera_x, sim_t);
    draw_palace_floor_segment(0, 76, (int)lower_floor, camera_x);
    draw_palace_floor_segment(92, 174, (int)upper_floor, camera_x);

    int ledge_sx = (int)lroundf(ledge_x - camera_x);
    draw_line(ledge_sx, (int)upper_floor, ledge_sx, (int)lower_floor);
    for (int y = (int)upper_floor + 2; y < (int)lower_floor; y += 4)
        draw_line(ledge_sx, y, ledge_sx + 4, y);

    for (int world_x = 78; world_x < 92; world_x += 4) {
        int sx = (int)lroundf((float)world_x - camera_x);
        draw_line(sx, (int)lower_floor, sx + 2, (int)lower_floor - 5);
        draw_line(sx + 2, (int)lower_floor - 5, sx + 4, (int)lower_floor);
    }

    draw_palace_archway(112, 138, (int)upper_floor, camera_x);
    draw_palace_gate(152, 160, BLUE_Y + 10, (int)upper_floor, gate_u, camera_x);

    int chain_x = (int)lroundf(99.0f - camera_x);
    draw_line(chain_x, BLUE_Y + 2, chain_x, BLUE_Y + 17);
    if (((int)(scene_t * 4.0f)) & 1) px(chain_x + 1, BLUE_Y + 10);

    if (grounded)
        draw_actor_pose(actor_x - camera_x, actor_hip_y, &pose);
    else
        draw_actor_pose(actor_x - camera_x, actor_hip_y, &pose);

    if (effect == 1) {
        int sx = (int)lroundf(actor_x - camera_x);
        int sy = (int)lower_floor - 1;
        for (int i = 0; i < 4; i++)
            draw_line(sx - 10 - i * 2, sy - (i & 1), sx - 5 - i * 2, sy - 1 - (i & 1));
    } else if (effect == 2) {
        int sx = (int)lroundf(actor_x - camera_x);
        int sy = (int)lroundf(actor_hip_y);
        draw_line(sx - 7, sy + 5, sx - 12, sy + 4);
        draw_line(sx - 5, sy + 2, sx - 10, sy + 1);
    }
}

static void draw_scene_hero(float scene_t, float scene_duration, float sim_t) {
    float u = clampf_local(scene_duration > 0.0f ? scene_t / scene_duration : 0.0f, 0.0f, 0.999f);
    int frame = (int)floorf(u * (float)HERO_FRAME_COUNT);
    int cx = WIDTH / 2;
    int cy = BLUE_Y + BLUE_H / 2;
    int iris = (int)lroundf(mixf_local(46.0f, 18.0f, smoothstep_local((u - 0.05f) / 0.30f)));

    (void)sim_t;

    blit_hero_frame(frame);

    if (((int)(scene_t * 3.0f) & 1) == 0)
        outline_rect(cx - iris, BLUE_Y + 4 + iris / 5, cx + iris, HEIGHT - 4 - iris / 5);
    if (u > 0.18f) {
        int pulse = 5 + (int)lroundf(2.0f * sinf(scene_t * 6.0f) + 2.0f);
        draw_line(cx - pulse, cy, cx + pulse, cy);
        draw_line(cx, cy - pulse, cx, cy + pulse);
    }
    if (u > 0.52f) {
        for (int i = 0; i < 6; i++) {
            float a = scene_t * 2.4f + (float)i * 2.0f * (float)M_PI / 6.0f;
            int x1 = cx + (int)lroundf(cosf(a) * 18.0f);
            int y1 = cy + (int)lroundf(sinf(a) * 8.0f);
            draw_line_shaded(cx, cy, x1, y1, 2);
        }
    }
}

static void draw_sine_str(int x, int y, const char *s, float t) {
    int i = 0;
    while (*s) {
        int oy = (int)lroundf(sinf(t * 4.0f + i * 0.65f) * 2.0f);
        draw_char(x + i * 6, y + oy, *s++);
        i++;
    }
}

static void draw_scene_finale(float scene_t, float sim_t) {
    const char *msg = "  I2C OLED DEMOSCENE FOREVER  ";
    int len_px = (int)strlen(msg) * 6;
    float scroll_t = scene_t - 1.2f;
    int scroll_x = WIDTH;
    int horizon = BLUE_Y + 13;
    int prev_y = horizon;

    if (scroll_t > 0.0f)
        scroll_x = WIDTH - ((int)(scroll_t * 14.0f) % (len_px + WIDTH));

    fill_circle(98, BLUE_Y + 8, 7);
    outline_rect(90, BLUE_Y + 1, 106, BLUE_Y + 17);

    for (int x = 0; x < WIDTH; x++) {
        float ridge = sinf(x * 0.06f + sim_t * 0.7f) * 5.0f
                    + sinf(x * 0.13f - sim_t * 0.9f) * 3.0f
                    + cosf(x * 0.03f + sim_t * 0.25f) * 4.0f;
        int y = horizon + (int)lroundf(ridge);
        if (x > 0) draw_line(x - 1, prev_y, x, y);
        draw_line(x, y, x, HEIGHT - 1);
        prev_y = y;
    }

    for (int x = -16; x <= WIDTH + 16; x += 16) {
        int sway = (int)lroundf(sinf(scene_t * 1.1f + x * 0.08f) * 2.0f);
        draw_line(x, HEIGHT - 1, WIDTH / 2, horizon + sway);
    }

    draw_sine_str(scroll_x, HEIGHT - 9, msg, scene_t);
}

static void draw_scene(int scene_idx, float scene_t, float sim_t) {
    switch (scene_idx) {
        case 0: draw_scene_boot(scene_t, sim_t); break;
        case 1: draw_scene_starfield(scene_t, sim_t); break;
        case 2: draw_scene_roto(scene_t, sim_t); break;
        case 3: draw_scene_tunnel(scene_t, sim_t); break;
        case 4: draw_scene_morph(scene_t, sim_t); break;
        case 5: draw_scene_palace(scene_t, scene_durations[scene_idx], sim_t); break;
        case 6: draw_scene_hero(scene_t, scene_durations[scene_idx], sim_t); break;
        case 7: draw_scene_finale(scene_t, sim_t); break;
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

    i2c_fd = open("/dev/i2c-1", O_RDWR);
    if (i2c_fd < 0) {
        perror("open i2c");
        return 1;
    }
    if (ioctl(i2c_fd, I2C_SLAVE, ADDR) < 0) {
        perror("ioctl");
        return 1;
    }

    init_display();
    init_assets();
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    long frame_ns = 1000000000L / TARGET_FPS;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    struct timespec prev;
    clock_gettime(CLOCK_MONOTONIC, &prev);

    float sim_t = 0.0f;

    while (running) {
        struct timespec curr;
        clock_gettime(CLOCK_MONOTONIC, &curr);
        float dt = (curr.tv_sec - prev.tv_sec) + (curr.tv_nsec - prev.tv_nsec) / 1e9f;
        if (dt > 1.0f) dt = 1.0f;
        prev = curr;
        sim_t += dt;

        int scene_idx = 0;
        float scene_t = 0.0f;
        float scene_duration = SCENE_SECONDS;
        resolve_scene_time(sim_t, &scene_idx, &scene_t, &scene_duration);

        memset(fb, 0, sizeof(fb));
        draw_header(scene_titles[scene_idx], scene_idx, scene_t, scene_duration, sim_t);
        draw_scene(scene_idx, scene_t, sim_t);
        draw_flash_overlay(scene_idx, scene_t, scene_duration);
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
    ssize_t written = write(i2c_fd, off, 2);
    (void)written;
    close(i2c_fd);
    return 0;
}
