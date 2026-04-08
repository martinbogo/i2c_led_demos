/*
 * elevated.c - SSD1306 OLED adaptation of "Elevated" by RGBA and TBC
 *
 * This is a monochrome reinterpretation for the 128x64 split yellow/blue
 * SSD1306 panel used elsewhere in this repository. The original 4k intro is a
 * Windows + Direct3D fullscreen shader; this version preserves the synchronized
 * terrain flyover, water plane, seasonal changes, sun motion, and cinematic
 * camera language in a CPU renderer suitable for Raspberry Pi over I2C.
 *
 * Compile: gcc -O2 -o elevated elevated.c -lm
 * Run:     sudo ./elevated
 */

#include <fcntl.h>
#include <errno.h>
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
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "elevated_music.h"

#define WIDTH             128
#define HEIGHT            64
#define PAGES             (HEIGHT / 8)
#define ADDR              0x3C
#define MOTION_FPS        15
#define PDM_PHASES        3
#define PDM_SUBFRAME_HZ   90
#define SUBFRAMES_PER_MOTION_FRAME (PDM_SUBFRAME_HZ / MOTION_FPS)
#define YELLOW_H          16
#define BLUE_Y            16
#define BLUE_H            48
#define BLUE_START_PAGE   (BLUE_Y / 8)
#define BLUE_PLANE_BYTES  (WIDTH * (BLUE_H / 8))
#define I2C_CHUNK         255
#define SAMPLE_RATE       44100.0f
#define MAX_NOTE_SAMPLES  5210.0f
#define TRACK_ROW_SECONDS ((MAX_NOTE_SAMPLES * 4.0f) / SAMPLE_RATE)
#define DEMO_ROWS         448.0f
#define DEMO_SECONDS      (DEMO_ROWS * TRACK_ROW_SECONDS)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float x, y, z;
} vec3_t;

typedef struct {
    int row;
    float value;
    int interpolation;
} TrackData;

typedef struct {
    float time;
    float cam_seed_x;
    float cam_seed_y;
    float cam_speed;
    float cam_fov;
    float cam_pos_y;
    float cam_tar_y;
    float sun_angle;
    float water_level;
    float season;
    float brightness;
    float contrast;
    float terrain_scale;
    float snow;
    vec3_t sun_dir;
} ElevatedParams;

typedef struct {
    float scene_t;
    ElevatedParams params;
    uint8_t blue_luma[WIDTH * BLUE_H];
    int ybuf[WIDTH];
    float horizon_y;
    int sun_x;
    int sun_y;
} ElevatedFrameCache;

typedef struct {
    pid_t pid;
    char wav_path[32];
    int have_wav;
    int disabled;
    struct timespec launched_at;
} AudioPlayback;

static int i2c_fd;
static uint8_t fb[WIDTH * PAGES];
static volatile int running = 1;
static ElevatedFrameCache frame_cache;
static AudioPlayback audio_playback = { -1, "", 0, 0, { 0, 0 } };

static void bpx(int x, int y);

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

/*
 * Calibrated transfer curve from oled_gamma_calibration.txt, used by the
 * scene-10 PDM video pipeline via convert_pdm_48.py.
 */
static const uint8_t oled_gray_lut[256] = {
      0,   1,   1,   2,   2,   3,   4,   4,   5,   5,   6,   6,   7,   8,   8,   9,
      9,  10,  11,  11,  12,  13,  13,  14,  15,  15,  16,  16,  17,  18,  18,  19,
     20,  20,  21,  22,  22,  23,  24,  24,  25,  26,  26,  27,  27,  28,  29,  29,
     30,  31,  31,  32,  33,  33,  34,  35,  36,  36,  37,  38,  38,  39,  40,  40,
     41,  42,  43,  43,  44,  45,  45,  46,  47,  48,  48,  49,  50,  50,  51,  52,
     52,  53,  54,  55,  55,  56,  57,  58,  58,  59,  60,  61,  61,  62,  63,  64,
     64,  65,  66,  67,  67,  68,  69,  70,  71,  71,  72,  73,  74,  74,  75,  76,
     77,  77,  78,  79,  80,  80,  81,  82,  83,  84,  84,  85,  86,  87,  87,  88,
     89,  90,  90,  91,  92,  93,  93,  94,  95,  96,  97,  97,  98,  99, 100, 100,
    101, 102, 103, 103, 104, 105, 106, 106, 107, 108, 109, 110, 110, 111, 112, 113,
    114, 115, 115, 116, 117, 118, 119, 120, 120, 121, 122, 123, 124, 124, 125, 126,
    127, 128, 129, 129, 130, 131, 132, 133, 134, 134, 135, 136, 137, 138, 138, 139,
    140, 141, 142, 143, 143, 144, 145, 146, 147, 148, 148, 149, 150, 151, 152, 152,
    153, 154, 155, 156, 157, 157, 158, 159, 160, 161, 162, 162, 163, 164, 165, 166,
    167, 168, 168, 169, 170, 171, 172, 173, 174, 175, 175, 176, 177, 178, 179, 183,
    188, 192, 197, 201, 206, 210, 215, 219, 224, 228, 233, 237, 242, 246, 251, 255
};

#define TRACK_END {512, 0.0f, 0}

static const TrackData camSeedX[] = {
    { 0, 98.0f, 0}, { 16, 5.0f, 0}, { 32, 17.0f, 0}, { 44, 113.0f, 0},
    { 56, 108.0f, 0}, { 62, 18.0f, 0}, { 72, 9.0f, 0}, { 80, 105.0f, 0},
    { 88, 6.0f, 0}, { 92, 101.0f, 0}, { 104, 186.0f, 0}, { 120, 12.0f, 0},
    { 140, 81.0f, 0}, { 150, 98.0f, 0}, { 168, 153.0f, 0}, { 196, 114.0f, 0},
    { 212, 48.0f, 0}, { 228, 83.0f, 0}, { 260, 11.0f, 0}, { 268, 8.0f, 0},
    { 276, 22.0f, 0}, { 292, 11.0f, 0}, { 308, 3.0f, 0}, { 328, 9.0f, 0},
    { 344, 50.0f, 0}, { 360, 1.0f, 0}, { 392, 125.0f, 0}, TRACK_END
};

static const TrackData camSeedY[] = {
    { 0, 0.0f, 0}, { 150, 1.0f, 0}, { 308, 0.0f, 0}, { 344, 1.0f, 0},
    { 360, 0.0f, 0}, TRACK_END
};

static const TrackData camSpeed[] = {
    { 0, 1.0f, 0}, { 92, 5.0f, 0}, { 104, 4.0f, 0}, { 140, 24.0f, 0},
    { 150, 58.0f, 0}, { 168, 87.0f, 0}, { 196, 255.0f, 0}, { 228, 188.0f, 0},
    { 260, 255.0f, 0}, { 292, 16.0f, 0}, { 308, 64.0f, 0}, { 328, 179.0f, 0},
    { 360, 226.0f, 0}, { 392, 30.0f, 0}, TRACK_END
};

static const TrackData camFov[] = {
    { 0, 53.0f, 0}, { 16, 160.0f, 0}, { 26, 8.0f, 0}, { 62, 4.0f, 0},
    { 75, 2.0f, 0}, { 80, 20.0f, 0}, { 83, 12.0f, 0}, { 88, 8.0f, 0},
    { 92, 60.0f, 0}, { 120, 24.0f, 0}, { 140, 18.0f, 0}, { 150, 28.0f, 0},
    { 168, 48.0f, 0}, { 196, 160.0f, 0}, { 212, 120.0f, 0}, { 228, 64.0f, 0},
    { 260, 128.0f, 0}, { 292, 53.0f, 0}, { 328, 120.0f, 0}, TRACK_END
};

static const TrackData camPosY[] = {
    { 0, 4.0f, 0}, { 16, 128.0f, 0}, { 26, 9.0f, 0}, { 32, 4.0f, 0},
    { 44, 5.0f, 0}, { 72, 14.0f, 0}, { 88, 32.0f, 0}, { 92, 8.0f, 0},
    { 140, 80.0f, 0}, { 150, 140.0f, 0}, { 168, 16.0f, 0}, { 196, 8.0f, 0},
    { 268, 4.0f, 0}, { 276, 16.0f, 0}, { 300, 48.0f, 0}, { 308, 190.0f, 0},
    { 328, 14.0f, 0}, { 344, 20.0f, 0}, { 360, 14.0f, 0}, TRACK_END
};

static const TrackData camTarY[] = {
    { 0, 32.0f, 0}, { 16, 255.0f, 0}, { 26, 128.0f, 0}, { 72, 127.0f, 0},
    { 88, 128.0f, 0}, { 140, 106.0f, 0}, { 150, 108.0f, 0}, { 168, 115.0f, 0},
    { 196, 128.0f, 0}, { 268, 200.0f, 0}, { 276, 128.0f, 0}, { 300, 111.0f, 0},
    { 308, 80.0f, 0}, { 344, 100.0f, 0}, { 360, 120.0f, 0}, TRACK_END
};

static const TrackData sun_angle[] = {
    { 0, 64.0f, 0}, { 26, 90.0f, 0}, { 32, 32.0f, 0}, { 62, 56.0f, 0},
    { 72, 160.0f, 0}, { 80, 64.0f, 0}, { 88, 160.0f, 0}, { 92, 180.0f, 0},
    { 104, 140.0f, 0}, { 120, 165.0f, 0}, { 140, 110.0f, 0}, { 150, 80.0f, 0},
    { 168, 105.0f, 0}, { 196, 50.0f, 0}, { 228, 10.0f, 0}, { 260, 150.0f, 0},
    { 276, 85.0f, 0}, { 292, 64.0f, 0}, { 308, 170.0f, 0}, { 328, 100.0f, 0},
    { 344, 170.0f, 0}, { 360, 0.0f, 0}, { 392, 35.0f, 0}, TRACK_END
};

static const TrackData terWaterLevel[] = {
    { 0, 154.0f, 0}, { 26, 200.0f, 0}, { 32, 0.0f, 0}, { 72, 170.0f, 0},
    { 92, 0.0f, 0}, { 168, 120.0f, 0}, { 196, 160.0f, 0}, { 212, 40.0f, 0},
    { 308, 180.0f, 0}, { 344, 0.0f, 0}, { 360, 193.0f, 0}, { 392, 170.0f, 0},
    TRACK_END
};

static const TrackData terSeason[] = {
    { 0, 0.0f, 0}, { 292, 0.0f, 1}, { 300, 64.0f, 1}, { 308, 128.0f, 1},
    { 322, 255.0f, 0}, { 392, 255.0f, 1}, { 424, 0.0f, 0}, TRACK_END
};

static const TrackData imgBrightness[] = {
    { 0, 0.0f, 1}, { 8, 128.0f, 0}, { 26, 110.0f, 0}, { 62, 32.0f, 0},
    { 72, 90.0f, 0}, { 92, 110.0f, 0}, { 120, 128.0f, 0}, { 140, 90.0f, 0},
    { 160, 90.0f, 1}, { 167, 0.0f, 0}, { 168, 128.0f, 0}, { 196, 120.0f, 0},
    { 228, 105.0f, 0}, { 250, 105.0f, 1}, { 251, 128.0f, 0}, { 260, 100.0f, 0},
    { 308, 24.0f, 0}, { 328, 120.0f, 0}, { 360, 110.0f, 0}, { 392, 100.0f, 0},
    { 424, 100.0f, 1}, { 448, 0.0f, 0}, TRACK_END
};

static const TrackData imgContrast[] = {
    { 0, 150.0f, 0}, { 62, 250.0f, 0}, { 72, 180.0f, 0}, { 92, 0.0f, 1},
    { 102, 160.0f, 0}, { 120, 128.0f, 0}, { 140, 190.0f, 0}, { 160, 190.0f, 1},
    { 167, 130.0f, 0}, { 168, 160.0f, 0}, { 196, 140.0f, 0}, { 228, 180.0f, 0},
    { 292, 0.0f, 1}, { 293, 190.0f, 0}, { 308, 255.0f, 0}, { 328, 150.0f, 0},
    { 360, 170.0f, 0}, { 392, 180.0f, 0}, { 424, 180.0f, 1}, { 448, 128.0f, 0},
    TRACK_END
};

static const TrackData terScale[] = {
    { 0, 200.0f, 0}, { 26, 140.0f, 0}, { 32, 200.0f, 0}, { 120, 255.0f, 0},
    { 260, 220.0f, 0}, { 292, 255.0f, 0}, { 328, 20.0f, 0}, { 360, 230.0f, 0},
    TRACK_END
};

static void stop(int sig) { (void)sig; running = 0; }

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

static uint8_t luma_byte(float luma) {
    return (uint8_t)lroundf(clampf_local(luma, 0.0f, 1.0f) * 255.0f);
}

static int pdm_on_level(int x, int y, int level, unsigned phase) {
    int scaled = level * (PDM_PHASES * 64 + 1) / 256;
    int bayer = bayer8x8[y & 7][x & 7];
    unsigned rotate = (unsigned)((x * 3 + y * 5) % PDM_PHASES);
    unsigned rank = temporal_order[(phase + rotate) % PDM_PHASES];
    return scaled > (int)(rank * 64u) + bayer;
}

static void draw_blue_luma_pdm(const uint8_t *terrain_luma, unsigned phase) {
    unsigned pdm_phase = phase % PDM_PHASES;

    for (int y = 0; y < BLUE_H; y++) {
        for (int x = 0; x < WIDTH; x++) {
            int idx = y * WIDTH + x;
            int level = oled_gray_lut[terrain_luma[idx]];
            if (pdm_on_level(x, y, level, pdm_phase)) bpx(x, y);
        }
    }
}

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

static float v3_dot(vec3_t a, vec3_t b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static float v3_len(vec3_t a) {
    return sqrtf(v3_dot(a, a));
}

static vec3_t v3_norm(vec3_t a) {
    float len = v3_len(a);
    if (len <= 1e-6f) return v3(0.0f, 1.0f, 0.0f);
    return v3_scale(a, 1.0f / len);
}

static vec3_t rot_x(vec3_t p, float a) {
    float c = cosf(a), s = sinf(a);
    return v3(p.x, p.y * c - p.z * s, p.y * s + p.z * c);
}

static vec3_t rot_y(vec3_t p, float a) {
    float c = cosf(a), s = sinf(a);
    return v3(p.x * c - p.z * s, p.y, p.x * s + p.z * c);
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

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [-d] [-t seconds] [--dump-frame FILE] [-h|-?]\n"
            "  -d               run as a daemon\n"
            "  -t seconds       start or dump from a given timeline position\n"
            "  --dump-frame F   render one frame to F (PGM) and exit\n"
            "  -h, -?           show this help message\n",
            argv0);
}

static int parse_float_arg(const char *arg, float *out) {
    char *end = NULL;
    float v = strtof(arg, &end);
    if (!arg[0] || (end && *end)) return 0;
    *out = v;
    return 1;
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
        int len = total - i;
        if (len > I2C_CHUNK) len = I2C_CHUNK;
        buf[0] = 0x40;
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

static float monotonic_elapsed(const struct timespec *since) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (float)(now.tv_sec - since->tv_sec)
         + (float)(now.tv_nsec - since->tv_nsec) / 1e9f;
}

static size_t demo_cycle_frames(void) {
    size_t frames = (size_t)lroundf(DEMO_SECONDS * SAMPLE_RATE);

    if (frames == 0 || frames > ELEVATED_MUSIC_TOTAL_SAMPLES)
        frames = ELEVATED_MUSIC_TOTAL_SAMPLES;

    return frames;
}

static int build_audio_cycle(int16_t **cycle_pcm, size_t *cycle_frames, float start_time) {
    int16_t *full_pcm = NULL;
    size_t full_frames = 0;
    size_t frames = demo_cycle_frames();
    size_t offset_frames;
    int16_t *loop_pcm;
    float wrapped_time = fmodf(start_time, DEMO_SECONDS);

    if (wrapped_time < 0.0f) wrapped_time += DEMO_SECONDS;

    *cycle_pcm = NULL;
    *cycle_frames = 0;

    if (!elevated_music_generate_pcm16(&full_pcm, &full_frames))
        return 0;

    if (full_frames < frames)
        frames = full_frames;

    offset_frames = (size_t)floorf(wrapped_time * SAMPLE_RATE);
    if (frames > 0)
        offset_frames %= frames;

    loop_pcm = (int16_t *)malloc(frames * 2u * sizeof(*loop_pcm));
    if (!loop_pcm) {
        free(full_pcm);
        return 0;
    }

    if (frames > 0) {
        size_t first_chunk = frames - offset_frames;
        memcpy(loop_pcm,
               full_pcm + offset_frames * 2u,
               first_chunk * 2u * sizeof(*loop_pcm));
        if (offset_frames > 0) {
            memcpy(loop_pcm + first_chunk * 2u,
                   full_pcm,
                   offset_frames * 2u * sizeof(*loop_pcm));
        }
    }

    free(full_pcm);
    *cycle_pcm = loop_pcm;
    *cycle_frames = frames;
    return 1;
}

static void write_le32(uint8_t *dst, uint32_t value) {
    dst[0] = (uint8_t)(value & 0xFFu);
    dst[1] = (uint8_t)((value >> 8) & 0xFFu);
    dst[2] = (uint8_t)((value >> 16) & 0xFFu);
    dst[3] = (uint8_t)((value >> 24) & 0xFFu);
}

static int write_full_fd(int fd, const void *data, size_t len) {
    const uint8_t *ptr = (const uint8_t *)data;

    while (len > 0) {
        ssize_t written = write(fd, ptr, len);
        if (written < 0) {
            if (errno == EINTR) continue;
            return 0;
        }
        ptr += (size_t)written;
        len -= (size_t)written;
    }

    return 1;
}

static int write_wav_file(int fd, const int16_t *pcm, size_t frames) {
    uint8_t header[44] = {
        'R','I','F','F', 0,0,0,0, 'W','A','V','E',
        'f','m','t',' ', 16,0,0,0, 1,0, 2,0,
        0,0,0,0, 0,0,0,0, 4,0, 16,0,
        'd','a','t','a', 0,0,0,0
    };
    uint32_t data_bytes = (uint32_t)(frames * 2u * sizeof(*pcm));

    write_le32(header + 4, 36u + data_bytes);
    write_le32(header + 24, (uint32_t)ELEVATED_MUSIC_SAMPLE_RATE);
    write_le32(header + 28, (uint32_t)ELEVATED_MUSIC_SAMPLE_RATE * 4u);
    write_le32(header + 40, data_bytes);

    return write_full_fd(fd, header, sizeof(header))
        && write_full_fd(fd, pcm, data_bytes);
}

static int create_audio_wav(AudioPlayback *audio, const int16_t *pcm, size_t frames) {
    static const char template_path[] = "/tmp/elevated-audio-XXXXXX";
    int fd;
    size_t n = sizeof(template_path);

    memcpy(audio->wav_path, template_path, n);
    fd = mkstemp(audio->wav_path);
    if (fd < 0) {
        audio->wav_path[0] = '\0';
        return 0;
    }

    if (!write_wav_file(fd, pcm, frames)) {
        close(fd);
        unlink(audio->wav_path);
        audio->wav_path[0] = '\0';
        return 0;
    }

    close(fd);
    audio->have_wav = 1;
    return 1;
}

static pid_t spawn_audio_player(const char *wav_path) {
    int err_pipe[2];
    int exec_errno = 0;
    pid_t pid;
    ssize_t nread;
    int flags;

    if (pipe(err_pipe) != 0)
        return -1;

    flags = fcntl(err_pipe[1], F_GETFD);
    if (flags >= 0)
        (void)fcntl(err_pipe[1], F_SETFD, flags | FD_CLOEXEC);

    pid = fork();
    if (pid < 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        return -1;
    }

    if (pid == 0) {
        close(err_pipe[0]);
        execlp("aplay", "aplay", "-q", wav_path, (char *)NULL);
        exec_errno = errno;
        (void)write(err_pipe[1], &exec_errno, sizeof(exec_errno));
        _exit(127);
    }

    close(err_pipe[1]);
    do {
        nread = read(err_pipe[0], &exec_errno, sizeof(exec_errno));
    } while (nread < 0 && errno == EINTR);
    close(err_pipe[0]);

    if (nread > 0) {
        (void)waitpid(pid, NULL, 0);
        errno = exec_errno;
        return -1;
    }

    return pid;
}

static int launch_audio_playback(AudioPlayback *audio) {
    pid_t pid;

    if (audio->disabled || !audio->have_wav)
        return 0;

    pid = spawn_audio_player(audio->wav_path);
    if (pid < 0)
        return 0;

    audio->pid = pid;
    clock_gettime(CLOCK_MONOTONIC, &audio->launched_at);
    return 1;
}

static void poll_audio_playback(AudioPlayback *audio) {
    int status;
    pid_t waited;

    if (audio->disabled || audio->pid <= 0)
        return;

    waited = waitpid(audio->pid, &status, WNOHANG);
    if (waited != audio->pid)
        return;

    audio->pid = -1;
    if (monotonic_elapsed(&audio->launched_at) < 1.0f) {
        audio->disabled = 1;
        return;
    }

    if (!launch_audio_playback(audio))
        audio->disabled = 1;
}

static void cleanup_audio_playback(AudioPlayback *audio) {
    if (audio->pid > 0) {
        (void)kill(audio->pid, SIGTERM);
        (void)waitpid(audio->pid, NULL, 0);
        audio->pid = -1;
    }

    if (audio->have_wav) {
        unlink(audio->wav_path);
        audio->wav_path[0] = '\0';
        audio->have_wav = 0;
    }
}

static void px(int x, int y) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
        fb[x + (y / 8) * WIDTH] |= 1 << (y & 7);
}

static void bpx(int x, int y) {
    px(x, y + BLUE_Y);
}

static int fb_on(int x, int y) {
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return 0;
    return (fb[x + (y / 8) * WIDTH] >> (y & 7)) & 1;
}

static void fill_rect(int x0, int y0, int x1, int y1) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            px(x, y);
}

static void draw_line(int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        px(x0, y0);
        if (x0 == x1 && y0 == y1) break;
        {
            int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
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

static void draw_char(int x, int y, char c) {
    if (c < 32 || c > 126) c = '?';
    {
        const uint8_t *g = font5x7[c - 32];
        for (int col = 0; col < 5; col++) {
            uint8_t bits = g[col];
            for (int row = 0; row < 7; row++)
                if (bits & (1 << row))
                    px(x + col, y + row);
        }
    }
}

static void draw_str(int x, int y, const char *s) {
    while (*s) {
        draw_char(x, y, *s++);
        x += 6;
    }
}

static float sample_track(const TrackData *track, float scene_t) {
    float rowf = scene_t / TRACK_ROW_SECONDS;
    int idx = 0;

    while (track[idx + 1].row < 512 && track[idx + 1].row <= (int)floorf(rowf)) idx++;
    if (!track[idx].interpolation) return track[idx].value;
    if (track[idx + 1].row >= 512 || track[idx + 1].row <= track[idx].row) return track[idx].value;

    return mixf_local(track[idx].value,
                      track[idx + 1].value,
                      clampf_local((rowf - (float)track[idx].row) /
                                   (float)(track[idx + 1].row - track[idx].row),
                                   0.0f, 1.0f));
}

static void sample_params(float scene_t, ElevatedParams *p) {
    float loop_t = fmodf(scene_t, DEMO_SECONDS);
    if (loop_t < 0.0f) loop_t += DEMO_SECONDS;

    p->time = loop_t;
    p->cam_seed_x = sample_track(camSeedX, loop_t) / 256.0f;
    p->cam_seed_y = sample_track(camSeedY, loop_t) / 256.0f;
    p->cam_speed = sample_track(camSpeed, loop_t) / 4096.0f;
    p->cam_fov = sample_track(camFov, loop_t) / 96.0f;
    p->cam_pos_y = sample_track(camPosY, loop_t) / 64.0f;
    p->cam_tar_y = (sample_track(camTarY, loop_t) - 128.0f) / 4.0f;
    p->sun_angle = sample_track(sun_angle, loop_t) / 32.0f;
    p->water_level = (sample_track(terWaterLevel, loop_t) - 192.0f) / 128.0f;
    p->season = sample_track(terSeason, loop_t) / 256.0f;
    p->brightness = (sample_track(imgBrightness, loop_t) - 128.0f) / 128.0f;
    p->contrast = sample_track(imgContrast, loop_t) / 128.0f;
    p->terrain_scale = (sample_track(terScale, loop_t) - 128.0f) / 128.0f;
    p->snow = smoothstep_local((p->season - 0.33f) / 0.45f);
    p->sun_dir = v3_norm(v3(cosf(p->sun_angle), 0.32f + 0.06f * (1.0f - p->snow), sinf(p->sun_angle)));
}

static float graded_luma(float luma, const ElevatedParams *p) {
    float contrast = clampf_local(p->contrast, 0.55f, 1.75f);
    float brightness = p->brightness * 0.22f;
    return clampf_local((luma - 0.5f) * contrast + 0.5f + brightness, 0.0f, 1.0f);
}

static void elevated_terrain_sample(float x, float z, const ElevatedParams *p,
                                    float *height, float *slope_x, float *slope_z) {
    float freq = mixf_local(0.72f, 1.42f, clampf_local((p->terrain_scale + 1.0f) * 0.5f, 0.0f, 1.0f));
    float amp = mixf_local(0.85f, 1.26f, clampf_local((p->terrain_scale + 1.0f) * 0.5f, 0.0f, 1.0f));
    float xx = x * freq;
    float zz = z * freq * 0.94f;

    float sx0 = sinf(xx * 0.13f);
    float cx0 = cosf(xx * 0.13f);
    float cz0 = cosf(zz * 0.10f);
    float sz0 = sinf(zz * 0.10f);
    float ssum = sinf((xx + zz) * 0.05f);
    float csum = cosf((xx + zz) * 0.05f);
    float cdiff = cosf((xx - zz) * 0.07f);
    float sdiff = sinf((xx - zz) * 0.07f);
    float fine0 = sinf(xx * 0.29f + zz * 0.11f + p->cam_seed_x * 6.2831853f);
    float fine1 = cosf(zz * 0.24f - xx * 0.18f + p->cam_seed_y * 6.2831853f);
    float macro = amp * (4.2f * sx0 + 3.6f * cz0 + 2.2f * ssum + 1.7f * cdiff);
    float detail = 1.15f * fine0 + 0.80f * fine1;
    float winter_bulge = p->snow * 1.25f * sinf(zz * 0.03f + p->cam_seed_y * 4.0f);

    if (height)
        *height = macro + detail + winter_bulge;
    if (slope_x)
        *slope_x = amp * freq * (4.2f * 0.13f * cx0 + 2.2f * 0.05f * csum - 1.7f * 0.07f * sdiff)
                 + 1.15f * freq * 0.29f * cosf(xx * 0.29f + zz * 0.11f + p->cam_seed_x * 6.2831853f)
                 + 0.80f * freq * 0.18f * sinf(zz * 0.24f - xx * 0.18f + p->cam_seed_y * 6.2831853f);
    if (slope_z)
        *slope_z = amp * freq * 0.94f * (-3.6f * 0.10f * sz0 + 2.2f * 0.05f * csum + 1.7f * 0.07f * sdiff)
                 + 1.15f * freq * 0.94f * 0.11f * cosf(xx * 0.29f + zz * 0.11f + p->cam_seed_x * 6.2831853f)
                 - 0.80f * freq * 0.94f * 0.24f * sinf(zz * 0.24f - xx * 0.18f + p->cam_seed_y * 6.2831853f)
                 + p->snow * 1.25f * freq * 0.94f * 0.03f * cosf(zz * 0.03f + p->cam_seed_y * 4.0f);
}

static float elevated_terrain_height(float x, float z, const ElevatedParams *p) {
    float h;
    elevated_terrain_sample(x, z, p, &h, NULL, NULL);
    return h;
}

static float terrain_sunlight(float slope_x, float slope_z, const ElevatedParams *p) {
    float nx = -slope_x;
    float ny = 1.0f;
    float nz = -slope_z;
    float inv_len = 1.0f / sqrtf(nx * nx + ny * ny + nz * nz);
    vec3_t n = v3(nx * inv_len, ny * inv_len, nz * inv_len);
    return clampf_local(v3_dot(n, p->sun_dir), 0.0f, 1.0f);
}

static void build_camera(const ElevatedParams *p, vec3_t *cam, vec3_t *focus,
                         float *yaw, float *pitch, float *proj_scale) {
    float seed_x = p->cam_seed_x * 6.2831853f;
    float seed_y = p->cam_seed_y * 6.2831853f;
    float wobble_t = p->time * (0.34f + p->cam_speed * 7.0f);
    float travel_speed = 1.25f + p->cam_speed * 88.0f;
    float travel = p->time * travel_speed;
    float cam_x = 17.0f * sinf(wobble_t * 0.58f + seed_x)
                + 8.0f * cosf(wobble_t * 1.17f + seed_y * 1.9f)
                + 4.5f * sinf(p->time * 0.16f + seed_y * 4.5f);
    float cam_z = travel + 16.0f * sinf(wobble_t * 0.27f + seed_y * 1.3f)
                + 7.0f * cosf(wobble_t * 0.69f + seed_x * 0.8f);
    float ground = elevated_terrain_height(cam_x, cam_z, p);
    float look_dist = 11.0f + p->cam_fov * 4.5f;
    float ahead_x = cam_x + 9.0f * sinf(wobble_t * 0.42f + seed_x * 1.5f);
    float ahead_z = cam_z + look_dist + 5.0f * cosf(wobble_t * 0.24f + seed_y * 1.8f);
    float ahead_ground = elevated_terrain_height(ahead_x, ahead_z, p);
    vec3_t delta;
    float horiz;

    *cam = v3(cam_x, ground + 6.2f + p->cam_pos_y * 1.35f, cam_z);
    *focus = v3(ahead_x,
                ahead_ground + 2.6f + p->cam_tar_y * 0.10f,
                ahead_z);

    delta = v3_sub(*focus, *cam);
    horiz = sqrtf(delta.x * delta.x + delta.z * delta.z);
    if (horiz < 0.001f) horiz = 0.001f;

    *yaw = -atan2f(delta.x, delta.z);
    *pitch = atan2f(delta.y, horiz);
    *proj_scale = mixf_local(96.0f, 58.0f, clampf_local((p->cam_fov - 0.45f) / 1.15f, 0.0f, 1.0f));
}

static void prefill_sky(uint8_t *luma, float horizon_y, int sun_x, int sun_y,
                        const ElevatedParams *p) {
    for (int y = 0; y < BLUE_H; y++) {
        float t = horizon_y > 1.0f ? clampf_local((float)y / (horizon_y + 10.0f), 0.0f, 1.0f) : 1.0f;
        for (int x = 0; x < WIDTH; x++) {
            float dx = (float)(x - sun_x);
            float dy = (float)(y - sun_y);
            float base = mixf_local(0.82f - 0.10f * p->snow, 0.24f + 0.04f * p->snow, smoothstep_local(t));
            float halo = expf(-(dx * dx) * 0.0065f - (dy * dy) * 0.022f) * (0.34f + 0.10f * (1.0f - p->snow));
            float cloud = 0.08f * (0.5f + 0.5f * sinf((float)x * 0.055f + p->time * 0.22f + p->cam_seed_x * 9.0f)
                                          * cosf((float)y * 0.18f - p->time * 0.12f + p->cam_seed_y * 7.0f));
            float beam = halo * fmaxf(0.0f, cosf(dx * 0.20f + p->time * 0.50f)) * 0.18f;
            float grain = (hash01(x + y * 131 + (int)(p->time * 31.0f)) - 0.5f) * 0.03f;
            luma[y * WIDTH + x] = luma_byte(graded_luma(base + halo + beam + cloud + grain, p));
        }
    }
}

static void render_elevated_luma(ElevatedFrameCache *cache) {
    const ElevatedParams *p = &cache->params;
    vec3_t cam, focus;
    float yaw, pitch, proj_scale;
    float horizon_y;
    int sun_x = WIDTH / 2;
    int sun_y = 4;
    int sparkle_phase = (int)floorf(p->time * 14.0f);

    build_camera(p, &cam, &focus, &yaw, &pitch, &proj_scale);
    horizon_y = 15.0f + pitch * 20.0f;
    cache->horizon_y = horizon_y;

    if (project_world(v3_add(cam, v3_scale(p->sun_dir, 110.0f)), cam, yaw, pitch, proj_scale, &sun_x, &sun_y)) {
        if (sun_x < -20) sun_x = -20;
        if (sun_x > WIDTH + 20) sun_x = WIDTH + 20;
    } else {
        sun_x = WIDTH / 2;
        sun_y = (int)lroundf(horizon_y * 0.35f);
    }

    cache->sun_x = sun_x;
    cache->sun_y = sun_y;

    prefill_sky(cache->blue_luma, horizon_y, sun_x, sun_y, p);
    for (int x = 0; x < WIDTH; x++) cache->ybuf[x] = BLUE_H - 1;

    for (float depth = 70.0f; depth > 1.0f; depth -= 0.50f) {
        for (int x = 0; x < WIDTH; x++) {
            float ray = yaw + (((float)x - WIDTH * 0.5f) / (float)WIDTH)
                              * mixf_local(0.72f, 1.18f, clampf_local((p->cam_fov - 0.45f) / 1.15f, 0.0f, 1.0f));
            float wx = cam.x + sinf(ray) * depth;
            float wz = cam.z + cosf(ray) * depth;
            float h, slope_x, slope_z;
            float water_y = p->water_level;
            float surface_y;
            int sy;
            int is_land;

            elevated_terrain_sample(wx, wz, p, &h, &slope_x, &slope_z);
            is_land = h >= water_y;
            surface_y = is_land ? h : water_y;
            sy = (int)lroundf(horizon_y + (cam.y - surface_y) * proj_scale / depth);
            if (sy < 0) sy = 0;

            if (sy < cache->ybuf[x]) {
                int shell_rows = depth < 10.0f ? 3 : depth < 22.0f ? 2 : 1;
                for (int y = sy; y <= cache->ybuf[x]; y++) {
                    int idx = y * WIDTH + x;
                    int top_offset = y - sy;
                    float shell_t = 1.0f - clampf_local((float)top_offset / (float)shell_rows, 0.0f, 1.0f);
                    float face_t = smoothstep_local(clampf_local((float)top_offset / 7.0f, 0.0f, 1.0f));
                    float pixel_luma;

                    if (is_land) {
                        float sun = terrain_sunlight(slope_x, slope_z, p);
                        float ridge = clampf_local((fabsf(slope_x) + fabsf(slope_z) - 0.55f) / 0.80f, 0.0f, 1.0f);
                        float snowline = mixf_local(7.5f, -0.5f, p->snow);
                        float snow_cover = p->snow * smoothstep_local((h - snowline) / 3.5f);
                        float contour = fabsf(fractf_local((h + depth * 0.16f) * 0.33f) - 0.5f) < 0.10f ? 1.0f : 0.0f;
                        float base_luma = depth < 8.0f ? 0.70f : depth < 18.0f ? 0.55f : depth < 38.0f ? 0.41f : 0.28f;
                        float surface_luma = mixf_local(base_luma * 0.72f, 0.18f + sun * 0.70f, 0.58f);
                        float interior_luma = base_luma * 0.22f + 0.05f + (1.0f - ridge) * 0.03f;
                        float tex = ((int)floorf(wx * 0.45f) + (int)floorf(wz * 0.32f) + top_offset) % 5 == 0 ? 0.04f : -0.02f;

                        surface_luma += ridge * 0.05f;
                        surface_luma = mixf_local(surface_luma, 0.88f, snow_cover);
                        interior_luma = mixf_local(interior_luma, 0.72f, snow_cover * 0.75f);
                        pixel_luma = mixf_local(surface_luma, interior_luma, face_t);
                        pixel_luma += shell_t * (0.08f * sun + 0.04f * ridge);
                        pixel_luma += tex * (1.0f - snow_cover);
                        if (top_offset == 0) pixel_luma += 0.08f + ridge * 0.06f;
                        if (contour > 0.5f && top_offset == 0) pixel_luma = fmaxf(pixel_luma, 0.64f + 0.10f * snow_cover);
                        if (top_offset > 4)
                            pixel_luma -= 0.06f * smoothstep_local(clampf_local((float)(top_offset - 4) / 5.0f, 0.0f, 1.0f));
                    } else {
                        float submerged = clampf_local((water_y - h) * 1.3f, 0.0f, 1.0f);
                        float ripple = 0.5f + 0.5f * sinf(wx * 0.22f + p->time * 1.8f)
                                             * cosf(wz * 0.08f - p->time * 1.4f);
                        float stripe = 0.5f + 0.5f * cosf(wx * 0.10f - p->sun_angle * 3.5f + p->time * 0.55f);
                        float spec = powf(clampf_local(ripple * stripe, 0.0f, 1.0f), 3.5f);
                        float water_base = 0.13f + 0.06f * ripple + 0.12f * (1.0f - p->snow);
                        float water_top = water_base + 0.16f * spec + 0.14f * submerged;

                        water_top = mixf_local(water_top, 0.62f, p->snow * (0.35f + 0.35f * submerged));
                        pixel_luma = mixf_local(water_top, water_base * 0.64f, face_t);
                        if (top_offset == 0) pixel_luma += 0.06f;
                        if (((x + y + sparkle_phase) & 7) == 0) pixel_luma += 0.03f;
                    }

                    cache->blue_luma[idx] = luma_byte(graded_luma(pixel_luma, p));
                }

                cache->ybuf[x] = sy;
            }
        }
    }
}

static void draw_elevated_overlays(const ElevatedFrameCache *cache) {
    for (int x = 1; x < WIDTH; x++) {
        if (abs(cache->ybuf[x] - cache->ybuf[x - 1]) < 7 &&
            (cache->ybuf[x] > (int)cache->horizon_y + 1 || cache->ybuf[x - 1] > (int)cache->horizon_y + 1)) {
            bline(x - 1, cache->ybuf[x - 1], x, cache->ybuf[x]);
        }
    }

    if (cache->sun_y >= 1 && cache->sun_y < (int)cache->horizon_y - 1 &&
        cache->sun_x >= 1 && cache->sun_x < WIDTH - 1) {
        bfill_circle(cache->sun_x, cache->sun_y, cache->params.snow > 0.6f ? 1 : 2);
    }
}

static void draw_header(float scene_t, const ElevatedParams *p) {
    int dots = 20;
    int filled = (int)lroundf((scene_t / DEMO_SECONDS) * (float)(dots - 1));
    if (filled < 0) filled = 0;
    if (filled >= dots) filled = dots - 1;

    draw_str(2, 4, "ELEVATED RGBA/TBC");
    for (int i = 0; i < dots; i++) {
        int x = 4 + i * 6;
        if (i <= filled) fill_rect(x, 14, x + 2, 15);
        else px(x, 15);
    }

    {
        int season_x = 108 + (int)lroundf(clampf_local(p->season, 0.0f, 1.0f) * 14.0f);
        fill_rect(106, 12, 122, 12);
        fill_rect(season_x, 11, season_x + 1, 15);
    }
}

static void prepare_frame_cache(float scene_t, ElevatedFrameCache *cache) {
    cache->scene_t = scene_t;
    sample_params(scene_t, &cache->params);
    render_elevated_luma(cache);
}

static void draw_cached_frame(const ElevatedFrameCache *cache, unsigned phase, int full_refresh) {
    if (full_refresh) {
        memset(fb, 0, sizeof(fb));
        draw_header(cache->scene_t, &cache->params);
    } else {
        memset(fb + WIDTH * BLUE_START_PAGE, 0, BLUE_PLANE_BYTES);
    }

    draw_blue_luma_pdm(cache->blue_luma, phase);
    draw_elevated_overlays(cache);
}

static int write_pgm(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror(path);
        return 0;
    }

    fprintf(f, "P5\n# elevated-oled-frame\n%d %d\n255\n", WIDTH, HEIGHT);
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            uint8_t pxv = fb_on(x, y) ? 255 : 0;
            if (fwrite(&pxv, 1, 1, f) != 1) {
                perror("fwrite");
                fclose(f);
                return 0;
            }
        }
    }

    fclose(f);
    return 1;
}

int main(int argc, char *argv[]) {
    int daemonize = 0;
    float start_time = 0.0f;
    const char *dump_frame_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0) {
            daemonize = 1;
        } else if (strcmp(argv[i], "-t") == 0) {
            if (++i >= argc || !parse_float_arg(argv[i], &start_time)) {
                usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "--dump-frame") == 0) {
            if (++i >= argc) {
                usage(argv[0]);
                return 1;
            }
            dump_frame_path = argv[i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "-?") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    if (start_time < 0.0f) start_time = 0.0f;
    start_time = fmodf(start_time, DEMO_SECONDS);
    if (start_time < 0.0f) start_time += DEMO_SECONDS;

    if (dump_frame_path) {
        unsigned subframe_tick = (unsigned)floorf(start_time * PDM_SUBFRAME_HZ);
        prepare_frame_cache(start_time, &frame_cache);
        draw_cached_frame(&frame_cache, subframe_tick % PDM_PHASES, 1);
        return write_pgm(dump_frame_path) ? 0 : 1;
    }

    if (daemonize && daemonize_process() != 0) {
        perror("daemonize");
        return 1;
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

    {
        int16_t *cycle_pcm = NULL;
        size_t cycle_frames = 0;

        audio_playback.pid = -1;
        audio_playback.wav_path[0] = '\0';
        audio_playback.have_wav = 0;
        audio_playback.disabled = 0;

        if (build_audio_cycle(&cycle_pcm, &cycle_frames, start_time)
            && create_audio_wav(&audio_playback, cycle_pcm, cycle_frames)) {
            if (!launch_audio_playback(&audio_playback)) {
                audio_playback.disabled = 1;
                if (!daemonize)
                    fprintf(stderr, "warning: audio playback unavailable: %s\n", strerror(errno));
            }
        } else if (!daemonize) {
            fprintf(stderr, "warning: failed to generate Elevated soundtrack\n");
        }

        free(cycle_pcm);
    }

    {
        long frame_ns = 1000000000L / PDM_SUBFRAME_HZ;
        struct timespec next, prev;
        float sim_t = start_time;
        unsigned last_motion_tick = (unsigned)-1;

        clock_gettime(CLOCK_MONOTONIC, &next);
        clock_gettime(CLOCK_MONOTONIC, &prev);

        while (running) {
            struct timespec now;
            float dt;
            unsigned subframe_tick;
            unsigned motion_tick;
            unsigned phase;
            int full_refresh;

            clock_gettime(CLOCK_MONOTONIC, &now);
            dt = (float)(now.tv_sec - prev.tv_sec) + (float)(now.tv_nsec - prev.tv_nsec) / 1e9f;
            if (dt > 1.0f) dt = 1.0f;
            prev = now;

            poll_audio_playback(&audio_playback);
            if (!audio_playback.disabled && audio_playback.pid > 0) {
                sim_t = fmodf(start_time + monotonic_elapsed(&audio_playback.launched_at), DEMO_SECONDS);
            } else {
                sim_t += dt;
                if (sim_t >= DEMO_SECONDS) sim_t = fmodf(sim_t, DEMO_SECONDS);
            }

            subframe_tick = (unsigned)floorf(sim_t * PDM_SUBFRAME_HZ);
            motion_tick = subframe_tick / SUBFRAMES_PER_MOTION_FRAME;
            phase = subframe_tick % PDM_PHASES;
            full_refresh = motion_tick != last_motion_tick;

            if (full_refresh) prepare_frame_cache(sim_t, &frame_cache);
            draw_cached_frame(&frame_cache, phase, full_refresh);

            if (full_refresh) flush();
            else flush_pages(BLUE_START_PAGE, PAGES - 1);

            last_motion_tick = motion_tick;

            next.tv_nsec += frame_ns;
            if (next.tv_nsec >= 1000000000L) {
                next.tv_nsec -= 1000000000L;
                next.tv_sec += 1;
            }
            sleep_until(&next);
        }
    }

    cleanup_audio_playback(&audio_playback);

    memset(fb, 0, sizeof(fb));
    flush();
    {
        uint8_t off[2] = {0x00, 0xAE};
        ssize_t written = write(i2c_fd, off, 2);
        (void)written;
    }
    close(i2c_fd);
    return 0;
}
