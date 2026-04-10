/*
 * elevated.c - SSD1306 OLED adaptation of "Elevated" by RGBA and TBC
 *
 * This is a monochrome reinterpretation for the 128x64 split yellow/blue
 * SSD1306 panel used elsewhere in this repository. The original 4k intro is a
 * Windows + Direct3D fullscreen shader; this version preserves the synchronized
 * terrain flyover, water plane, seasonal changes, sun motion, and cinematic
 * camera language in a CPU renderer suitable for Raspberry Pi over I2C.
 *
 * Compile: make elevated
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

#if defined(__has_include)
#if __has_include("oled_build_lut.h")
#include "oled_build_lut.h"
#define HAVE_OLED_BUILD_LUT 1
#else
#define HAVE_OLED_BUILD_LUT 0
#endif
#else
#define HAVE_OLED_BUILD_LUT 0
#endif

#include "elevated_music.h"
#include "elevated_sync_data.h"

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
#define TRACK_ROW_SECONDS (((float)ELEVATED_MUSIC_MAX_NOTE_SAMPLES * 4.0f) / SAMPLE_RATE)
#define DEMO_SECONDS      ((float)ELEVATED_MUSIC_CONTENT_SAMPLES / SAMPLE_RATE)

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    float x, y, z;
} vec3_t;

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
 * Transfer curve for the OLED PDM pipeline. Normal builds generate this from
 * oled_gamma_calibration.txt; when that generated header is unavailable, fall
 * back to the repository default packed LUT.
 */
static uint8_t oled_gray_lut[256];

#if !HAVE_OLED_BUILD_LUT
#define OLED_GRAY_LUT_FROM_CALIBRATION 0
#define OLED_GRAY_LUT_SOURCE_PATH "repository default"
#define OLED_GRAY_LUT_SOURCE_SYMBOL "default packed LUT"
#define OLED_GRAY_LUT_FIRST_VALUE 0
static const uint8_t oled_gray_lut_packed[] = {
    0x01, 0x01, 0x11, 0x10, 0x10, 0x10, 0x01, 0x01, 0x11, 0x10, 0x01, 0x11, 0x10, 0x10, 0x01, 0x11,
    0x10, 0x01, 0x11, 0x10, 0x01, 0x01, 0x11, 0x10, 0x01, 0x11, 0x10, 0x11, 0x10, 0x01, 0x11, 0x10,
    0x11, 0x10, 0x01, 0x11, 0x01, 0x11, 0x10, 0x01, 0x11, 0x01, 0x11, 0x01, 0x11, 0x01, 0x11, 0x01,
    0x11, 0x01, 0x11, 0x11, 0x10, 0x11, 0x10, 0x11, 0x10, 0x11, 0x10, 0x11, 0x01, 0x11, 0x01, 0x11,
    0x01, 0x11, 0x01, 0x11, 0x11, 0x10, 0x11, 0x10, 0x11, 0x10, 0x11, 0x10, 0x11, 0x01, 0x11, 0x11,
    0x01, 0x11, 0x11, 0x01, 0x11, 0x11, 0x10, 0x11, 0x11, 0x10, 0x11, 0x11, 0x10, 0x11, 0x01, 0x11,
    0x11, 0x01, 0x11, 0x11, 0x01, 0x11, 0x11, 0x10, 0x11, 0x11, 0x10, 0x11, 0x11, 0x10, 0x11, 0x11,
    0x01, 0x11, 0x11, 0x11, 0x01, 0x11, 0x11, 0x54, 0x54, 0x54, 0x54, 0x54, 0x54, 0x54, 0x54, 0x04
};
#endif

static void init_oled_gray_lut(void) {
    uint8_t value = (uint8_t)OLED_GRAY_LUT_FIRST_VALUE;

    oled_gray_lut[0] = value;
    for (size_t i = 0; i < sizeof(oled_gray_lut_packed); i++) {
        size_t dst = i * 2u + 1u;
        uint8_t packed = oled_gray_lut_packed[i];

        value = (uint8_t)(value + (packed & 0x0Fu));
        oled_gray_lut[dst] = value;
        if (dst + 1u < sizeof(oled_gray_lut)) {
            value = (uint8_t)(value + (packed >> 4));
            oled_gray_lut[dst + 1u] = value;
        }
    }
}

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
    fprintf(stderr, "usage: %s [-n] [-t seconds] [-h|-?]\n", argv0);
}

static int parse_float_arg(const char *arg, float *out) {
    char *end = NULL;
    float v = strtof(arg, &end);
    if (!arg[0] || (end && *end)) return 0;
    *out = v;
    return 1;
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
    return ELEVATED_MUSIC_CONTENT_SAMPLES;
}

static int build_audio_segment(int16_t **segment_pcm,
                               size_t *segment_frames,
                               float start_time) {
    int16_t *full_pcm = NULL;
    size_t full_frames = 0;
    size_t frames = demo_cycle_frames();
    size_t offset_frames;
    size_t remaining_frames;
    int16_t *segment_buf;
    float clamped_time = clampf_local(start_time, 0.0f, DEMO_SECONDS);

    *segment_pcm = NULL;
    *segment_frames = 0;

    if (!elevated_music_generate_pcm16(&full_pcm, &full_frames))
        return 0;

    if (full_frames < frames)
        frames = full_frames;

    offset_frames = (size_t)floorf(clamped_time * SAMPLE_RATE);
    if (offset_frames > frames)
        offset_frames = frames;

    remaining_frames = frames - offset_frames;
    if (remaining_frames == 0) {
        free(full_pcm);
        return 1;
    }

    segment_buf = (int16_t *)malloc(remaining_frames * 2u * sizeof(*segment_buf));
    if (!segment_buf) {
        free(full_pcm);
        return 0;
    }

    memcpy(segment_buf,
           full_pcm + offset_frames * 2u,
           remaining_frames * 2u * sizeof(*segment_buf));

    free(full_pcm);
    *segment_pcm = segment_buf;
    *segment_frames = remaining_frames;
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
    static const char template_path[] = "/tmp/elevated-audio-XXXXXX.wav";
    int fd;
    size_t n = sizeof(template_path);

    memcpy(audio->wav_path, template_path, n);
    fd = mkstemps(audio->wav_path, 4);
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
        execlp("aplay", "aplay", "-q", "-t", "wav", wav_path, (char *)NULL);
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
    if (waited == 0)
        return;

    audio->pid = -1;
    if (waited < 0) {
        audio->disabled = 1;
        return;
    }

    if (monotonic_elapsed(&audio->launched_at) < 1.0f) {
        audio->disabled = 1;
    }
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

static void fill_rect(int x0, int y0, int x1, int y1) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            px(x, y);
}

static void draw_hline(int x0, int x1, int y) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    for (int x = x0; x <= x1; x++)
        px(x, y);
}

static void draw_vline(int x, int y0, int y1) {
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        px(x, y);
}

static void draw_rect_outline(int x0, int y0, int x1, int y1) {
    draw_hline(x0, x1, y0);
    draw_hline(x0, x1, y1);
    draw_vline(x0, y0, y1);
    draw_vline(x1, y0, y1);
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

static float sample_track(uint8_t track_id, float scene_t) {
    float rowf = scene_t / TRACK_ROW_SECONDS;
    size_t idx = elevated_sync_track_offsets[track_id];
    size_t end = idx + elevated_sync_track_lengths[track_id] - 1u;
    uint16_t point;
    uint16_t next_point;
    uint16_t row;
    uint16_t next_row;
    uint8_t value;

    while (idx < end) {
        next_point = elevated_sync_points[idx + 1u];
        next_row = elevated_sync_rows[next_point & ELEVATED_SYNC_ROW_INDEX_MASK];
        if ((float)next_row > rowf)
            break;
        idx++;
    }

    point = elevated_sync_points[idx];
    value = (uint8_t)((point >> ELEVATED_SYNC_VALUE_SHIFT) & ELEVATED_SYNC_VALUE_MASK);
    if (!(point & ELEVATED_SYNC_INTERPOLATE_MASK) || idx >= end)
        return (float)value;

    row = elevated_sync_rows[point & ELEVATED_SYNC_ROW_INDEX_MASK];
    next_point = elevated_sync_points[idx + 1u];
    next_row = elevated_sync_rows[next_point & ELEVATED_SYNC_ROW_INDEX_MASK];
    if (next_row <= row)
        return (float)value;

    return mixf_local((float)value,
                      (float)((next_point >> ELEVATED_SYNC_VALUE_SHIFT) & ELEVATED_SYNC_VALUE_MASK),
                      clampf_local((rowf - (float)row) / (float)(next_row - row),
                                   0.0f, 1.0f));
}

static void sample_params(float scene_t, ElevatedParams *p) {
    float clamped_t = clampf_local(scene_t, 0.0f, DEMO_SECONDS);

    if (DEMO_SECONDS > 0.0f && clamped_t >= DEMO_SECONDS)
        clamped_t = nextafterf(DEMO_SECONDS, 0.0f);

    p->time = clamped_t;
    p->cam_seed_x = sample_track(ELEVATED_TRACK_CAM_SEED_X, clamped_t) / 256.0f;
    p->cam_seed_y = sample_track(ELEVATED_TRACK_CAM_SEED_Y, clamped_t) / 256.0f;
    p->cam_speed = sample_track(ELEVATED_TRACK_CAM_SPEED, clamped_t) / 4096.0f;
    p->cam_fov = sample_track(ELEVATED_TRACK_CAM_FOV, clamped_t) / 96.0f;
    p->cam_pos_y = sample_track(ELEVATED_TRACK_CAM_POS_Y, clamped_t) / 64.0f;
    p->cam_tar_y = (sample_track(ELEVATED_TRACK_CAM_TAR_Y, clamped_t) - 128.0f) / 4.0f;
    p->sun_angle = sample_track(ELEVATED_TRACK_SUN_ANGLE, clamped_t) / 32.0f;
    p->water_level = (sample_track(ELEVATED_TRACK_WATER_LEVEL, clamped_t) - 192.0f) / 128.0f;
    p->season = sample_track(ELEVATED_TRACK_SEASON, clamped_t) / 256.0f;
    p->brightness = (sample_track(ELEVATED_TRACK_BRIGHTNESS, clamped_t) - 128.0f) / 128.0f;
    p->contrast = sample_track(ELEVATED_TRACK_CONTRAST, clamped_t) / 128.0f;
    p->terrain_scale = (sample_track(ELEVATED_TRACK_TERRAIN_SCALE, clamped_t) - 128.0f) / 128.0f;
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
    float look_dist = 32.0f + p->cam_fov * 8.0f;
    float ahead_x = cam_x + 9.0f * sinf(wobble_t * 0.42f + seed_x * 1.5f);
    float ahead_z = cam_z + look_dist + 5.0f * cosf(wobble_t * 0.24f + seed_y * 1.8f);
    float ahead_ground = elevated_terrain_height(ahead_x, ahead_z, p);
    vec3_t delta;
    float horiz;

    *cam = v3(cam_x, ground + 2.8f + p->cam_pos_y * 1.35f, cam_z);
    *focus = v3(ahead_x,
                ahead_ground + 1.2f + p->cam_tar_y * 0.10f,
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
                    float face_rows = depth < 8.0f ? 14.0f : depth < 22.0f ? 9.0f : 7.0f;
                    float face_t = smoothstep_local(clampf_local((float)top_offset / face_rows, 0.0f, 1.0f));
                    float pixel_luma;

                    if (is_land) {
                        float sun = terrain_sunlight(slope_x, slope_z, p);
                        float ridge = clampf_local((fabsf(slope_x) + fabsf(slope_z) - 0.55f) / 0.80f, 0.0f, 1.0f);
                        float snowline = mixf_local(7.5f, -0.5f, p->snow);
                        float snow_cover = p->snow * smoothstep_local((h - snowline) / 3.5f);
                        float contour = fabsf(fractf_local((h + depth * 0.16f) * 0.33f) - 0.5f) < 0.10f ? 1.0f : 0.0f;
                        float base_luma = depth < 8.0f ? 0.70f : depth < 18.0f ? 0.55f : depth < 38.0f ? 0.41f : 0.28f;
                        float surface_luma = mixf_local(base_luma * 0.72f, 0.18f + sun * 0.70f, 0.58f);
                        float interior_luma = base_luma * 0.22f + 0.05f + sun * 0.14f + (1.0f - ridge) * 0.03f;
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
                            pixel_luma -= (depth < 10.0f ? 0.03f : 0.06f) * smoothstep_local(clampf_local((float)(top_offset - 4) / 5.0f, 0.0f, 1.0f));
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

static void draw_progress_bar(float scene_t) {
    static const float chapter_marks[] = { 0.18f, 0.39f, 0.58f, 0.77f, 0.91f };
    int x0 = 4;
    int y0 = 11;
    int x1 = WIDTH - 5;
    int y1 = 15;
    int inner_x0 = x0 + 1;
    int inner_x1 = x1 - 1;
    int inner_y0 = y0 + 1;
    int inner_y1 = y1 - 1;
    int rail_y = (y0 + y1) / 2;
    int playhead_x;
    int fill_x1;
    float progress = 0.0f;

    if (DEMO_SECONDS > 0.0f)
        progress = clampf_local(scene_t / DEMO_SECONDS, 0.0f, 1.0f);

    playhead_x = inner_x0 + (int)lroundf(progress * (float)(inner_x1 - inner_x0));
    if (playhead_x < inner_x0) playhead_x = inner_x0;
    if (playhead_x > inner_x1) playhead_x = inner_x1;

    fill_x1 = inner_x0 + (int)floorf(progress * (float)(inner_x1 - inner_x0 + 1)) - 1;

    draw_rect_outline(x0, y0, x1, y1);
    if (fill_x1 >= inner_x0)
        fill_rect(inner_x0, inner_y0, fill_x1, inner_y1);

    for (int x = inner_x0; x <= inner_x1; x += 4) {
        if (x > fill_x1)
            px(x, rail_y);
    }

    for (size_t i = 0; i < sizeof(chapter_marks) / sizeof(chapter_marks[0]); i++) {
        int tick_x = inner_x0 + (int)lroundf(chapter_marks[i] * (float)(inner_x1 - inner_x0));
        draw_vline(tick_x, y0 - 1, y0 + 1);
    }

    draw_vline(playhead_x, y0 - 1, y1 + 1);
    px(playhead_x - 1, y0);
    px(playhead_x + 1, y0);
    px(playhead_x - 1, y1);
    px(playhead_x + 1, y1);
}

static void draw_header(float scene_t) {
    draw_progress_bar(scene_t);
}

static void prepare_frame_cache(float scene_t, ElevatedFrameCache *cache) {
    cache->scene_t = scene_t;
    sample_params(scene_t, &cache->params);
    render_elevated_luma(cache);
}

static void draw_cached_frame(const ElevatedFrameCache *cache, unsigned phase, int full_refresh) {
    if (full_refresh) {
        memset(fb, 0, sizeof(fb));
        draw_header(cache->scene_t);
    } else {
        memset(fb + WIDTH * BLUE_START_PAGE, 0, BLUE_PLANE_BYTES);
    }

    draw_blue_luma_pdm(cache->blue_luma, phase);
    draw_elevated_overlays(cache);
}

int main(int argc, char *argv[]) {
    int disable_audio = 0;
    float start_time = 0.0f;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0) {
            disable_audio = 1;
        } else if (strcmp(argv[i], "-t") == 0) {
            if (++i >= argc || !parse_float_arg(argv[i], &start_time)) {
                usage(argv[0]);
                return 1;
            }
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
    init_oled_gray_lut();

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
        int16_t *segment_pcm = NULL;
        size_t segment_frames = 0;

        audio_playback.pid = -1;
        audio_playback.wav_path[0] = '\0';
        audio_playback.have_wav = 0;
        audio_playback.disabled = disable_audio ? 1 : 0;
        audio_playback.launched_at.tv_sec = 0;
        audio_playback.launched_at.tv_nsec = 0;

        if (!audio_playback.disabled) {
            if (build_audio_segment(&segment_pcm, &segment_frames, start_time)
                && create_audio_wav(&audio_playback, segment_pcm, segment_frames)) {
                if (!launch_audio_playback(&audio_playback)) {
                    audio_playback.disabled = 1;
                    fprintf(stderr, "warning: no audio: %s\n", strerror(errno));
                }
            } else {
                audio_playback.disabled = 1;
                fprintf(stderr, "warning: soundtrack generation failed\n");
            }
        }

        free(segment_pcm);
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
            int reached_end = 0;
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
            if (!audio_playback.disabled
                && (audio_playback.launched_at.tv_sec != 0 || audio_playback.launched_at.tv_nsec != 0)) {
                sim_t = start_time + monotonic_elapsed(&audio_playback.launched_at);
            } else {
                sim_t += dt;
            }

            if (sim_t >= DEMO_SECONDS) {
                sim_t = DEMO_SECONDS;
                reached_end = 1;
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

            if (reached_end)
                running = 0;

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
