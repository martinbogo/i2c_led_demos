/*
 * Author  : Martin Bogomolni
 * Date    : 2026-04-09
 * License : CC BY-NC 4.0 (https://creativecommons.org/licenses/by-nc/4.0/)
 *
 * anim_v_animator.c - Scripted OLED recreation of a classic animator-versus-stick-figure short
 *
 * Compile:  make anim_v_animator or ./build.sh pi
 * Run:      sudo ./anim_v_animator
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
#define TARGET_FPS  12

#define WIN_X       1
#define WIN_Y       1
#define WIN_W       126
#define WIN_H       62
#define TOPBAR_H    10
#define CANVAS_X    4
#define CANVAS_Y    13
#define CANVAS_W    120
#define CANVAS_H    47

#define TOOL_PENCIL_X  5
#define TOOL_SELECT_X  14
#define TOOL_SHAPE_X   23
#define TOOL_FILL_X    32
#define TOOL_ERASE_X   41
#define TOOL_Y         3
#define TOOL_W         7
#define TOOL_H         7

#define PLAY_X      96
#define STOP_X      105
#define TRASH_X     115
#define BTN_Y       3
#define BTN_W       7
#define BTN_H       7

#define ACT_COUNT   7
#define TRANSITION_BLEND_SECONDS 0.9f

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
} stick_pose_t;

typedef enum {
    TOOL_NONE,
    TOOL_PENCIL,
    TOOL_SELECT,
    TOOL_SHAPE,
    TOOL_FILL,
    TOOL_ERASE
} tool_t;

typedef struct {
    tool_t tool;
    int play_down;
    int stop_down;
    int trash_hot;
    int canvas_damage;
    int glitch;
} ui_state_t;

typedef struct {
    float scene_t;
    int play_down;
    int stop_down;
    int glitch;
    float shake;
} act6_control_state_t;

typedef struct {
    vec2_t fig_pos;
    stick_pose_t pose;
    float scale;
    float angle;
    vec2_t cursor_pos;
    vec2_t fig_vel;
    vec2_t cursor_vel;
    int cursor_pressed;
} continuity_state_t;

typedef struct {
    continuity_state_t source;
    int active;
    int to_act;
} transition_state_t;

typedef struct {
    vec2_t left_tip;
    vec2_t right_tip;
    vec2_t left_hand;
    vec2_t right_hand;
} projected_staff_t;

static const float act_durations[ACT_COUNT] = {
    18.0f, 14.0f, 23.0f, 23.0f, 24.0f, 12.0f, 15.55f
};

static int i2c_fd;
static uint8_t fb[WIDTH * PAGES];
static volatile int running = 1;

static const stick_pose_t pose_stand = {
    { 0.0f, -8.0f },
    {{ -2.5f, -5.0f }, { 2.5f, -5.0f }},
    {{ -3.5f, -1.5f }, { 3.5f, -1.5f }},
    {{ -1.8f, 4.0f }, { 1.8f, 4.0f }},
    {{ -2.3f, 10.0f }, { 2.3f, 10.0f }},
    2.8f
};

static const stick_pose_t __attribute__((unused)) pose_twitch = {
    { 0.6f, -8.2f },
    {{ -2.0f, -5.2f }, { 2.6f, -4.0f }},
    {{ -1.4f, -1.2f }, { 4.6f, -2.4f }},
    {{ -1.2f, 4.0f }, { 2.3f, 3.8f }},
    {{ -2.1f, 10.0f }, { 2.7f, 9.2f }},
    2.8f
};

static const stick_pose_t __attribute__((unused)) pose_reach = {
    { 0.8f, -8.0f },
    {{ -1.5f, -5.0f }, { 1.5f, -8.0f }},
    {{ -1.5f, -1.0f }, { 2.0f, -14.0f }},
    {{ -1.8f, 4.0f }, { 1.4f, 4.0f }},
    {{ -2.3f, 10.0f }, { 2.0f, 10.0f }},
    2.8f
};

static const stick_pose_t pose_pull = {
    { -0.6f, -7.8f },
    {{ -3.4f, -6.0f }, { 1.4f, -8.0f }},
    {{ -6.8f, -4.4f }, { 2.0f, -13.5f }},
    {{ -0.8f, 4.0f }, { 2.6f, 4.6f }},
    {{ -0.6f, 10.0f }, { 4.0f, 10.0f }},
    2.8f
};

static const stick_pose_t pose_stumble = {
    { -2.4f, -7.0f },
    {{ -3.0f, -5.0f }, { 1.0f, -3.5f }},
    {{ -5.2f, -1.5f }, { 4.2f, 0.8f }},
    {{ -0.8f, 4.8f }, { 4.2f, 4.0f }},
    {{ 1.0f, 10.0f }, { 6.0f, 9.2f }},
    2.8f
};

static const stick_pose_t pose_punch = {
    { 1.2f, -8.0f },
    {{ -1.2f, -5.0f }, { 3.0f, -6.0f }},
    {{ -1.2f, -1.0f }, { 7.5f, -6.5f }},
    {{ -1.8f, 4.5f }, { 1.0f, 4.0f }},
    {{ -2.2f, 10.0f }, { 1.6f, 10.0f }},
    2.8f
};

static const stick_pose_t pose_kick = {
    { 0.8f, -8.0f },
    {{ -2.0f, -5.0f }, { 2.6f, -5.2f }},
    {{ -2.2f, -1.4f }, { 4.2f, -2.0f }},
    {{ -1.6f, 4.0f }, { 2.6f, 1.0f }},
    {{ -1.8f, 10.0f }, { 7.8f, 3.6f }},
    2.8f
};

static const stick_pose_t pose_hang = {
    { 0.0f, -6.8f },
    {{ -1.5f, -9.0f }, { 1.5f, -9.0f }},
    {{ -1.5f, -13.0f }, { 1.5f, -13.0f }},
    {{ -1.0f, 2.0f }, { 1.0f, 2.0f }},
    {{ -1.8f, 8.5f }, { 1.8f, 8.5f }},
    2.7f
};

static const stick_pose_t pose_gymnast_hollow = {
    { 0.6f, -6.6f },
    {{ -1.8f, -9.4f }, { 2.0f, -8.8f }},
    {{ -2.0f, -13.2f }, { 2.4f, -12.6f }},
    {{ 1.3f, 1.5f }, { 3.8f, 1.0f }},
    {{ 5.1f, 2.1f }, { 7.5f, 1.5f }},
    2.7f
};

static const stick_pose_t pose_gymnast_invert = {
    { 0.0f, -6.8f },
    {{ -1.4f, -9.1f }, { 1.4f, -9.1f }},
    {{ -1.6f, -13.1f }, { 1.6f, -13.1f }},
    {{ -2.6f, 0.8f }, { 2.6f, 0.8f }},
    {{ -0.9f, 6.6f }, { 0.9f, 6.6f }},
    2.7f
};

static const stick_pose_t pose_land_crouch = {
    { 0.5f, -6.3f },
    {{ -3.0f, -4.8f }, { 3.0f, -4.8f }},
    {{ -5.0f, -1.8f }, { 5.0f, -1.8f }},
    {{ -3.3f, 1.8f }, { 3.3f, 1.8f }},
    {{ -4.6f, 5.0f }, { 4.6f, 5.0f }},
    2.8f
};

static const stick_pose_t pose_land_balance = {
    { 0.2f, -7.2f },
    {{ -3.1f, -5.8f }, { 3.2f, -5.2f }},
    {{ -5.3f, -2.6f }, { 5.4f, -1.8f }},
    {{ -2.4f, 3.2f }, { 2.5f, 3.1f }},
    {{ -3.0f, 8.3f }, { 3.2f, 8.1f }},
    2.8f
};

static const stick_pose_t pose_staff = {
    { 1.0f, -8.0f },
    {{ -2.5f, -5.0f }, { 3.5f, -7.0f }},
    {{ -2.5f, -1.8f }, { 7.0f, -8.0f }},
    {{ -1.4f, 4.0f }, { 1.8f, 4.0f }},
    {{ -2.0f, 10.0f }, { 2.2f, 10.0f }},
    2.8f
};

static const stick_pose_t pose_staff_swing = {
    { -0.8f, -8.0f },
    {{ -3.0f, -7.0f }, { 2.0f, -4.5f }},
    {{ -7.2f, -8.0f }, { 5.8f, -1.0f }},
    {{ -1.2f, 4.2f }, { 2.5f, 3.8f }},
    {{ -1.6f, 10.0f }, { 3.8f, 10.0f }},
    2.8f
};

static const stick_pose_t pose_climb = {
    { 1.2f, -6.5f },
    {{ 1.0f, -10.0f }, { 4.5f, -7.0f }},
    {{ 0.8f, -14.0f }, { 7.6f, -8.0f }},
    {{ 2.6f, -0.5f }, { 0.0f, 3.5f }},
    {{ 3.8f, 5.0f }, { 1.4f, 8.5f }},
    2.6f
};

static const stick_pose_t pose_climb_alt = {
    { 0.6f, -6.8f },
    {{ 4.2f, -9.5f }, { 1.2f, -6.6f }},
    {{ 7.2f, -13.0f }, { 1.0f, -8.0f }},
    {{ 0.2f, 2.0f }, { 2.8f, -0.8f }},
    {{ 1.4f, 7.8f }, { 3.8f, 4.8f }},
    2.6f
};

static const stick_pose_t pose_reach_edge = {
    { -0.4f, -7.8f },
    {{ -4.2f, -6.5f }, { 1.8f, -5.5f }},
    {{ -8.4f, -4.2f }, { 4.8f, -2.5f }},
    {{ -1.0f, 4.0f }, { 2.0f, 4.0f }},
    {{ -1.8f, 10.0f }, { 2.8f, 10.0f }},
    2.8f
};

static const stick_pose_t run_cycle[2] = {
    {
        { 0.5f, -8.0f },
        {{ 2.2f, -5.5f }, { -2.2f, -5.0f }},
        {{ 5.0f, -1.8f }, { -5.2f, -2.0f }},
        {{ -4.4f, 4.2f }, { 4.0f, 3.8f }},
        {{ -7.0f, 10.0f }, { 6.5f, 9.0f }},
        2.7f
    },
    {
        { -0.5f, -8.0f },
        {{ -2.2f, -5.0f }, { 2.2f, -5.5f }},
        {{ -5.2f, -2.0f }, { 5.0f, -1.8f }},
        {{ -4.0f, 3.8f }, { 4.4f, 4.2f }},
        {{ -6.5f, 9.0f }, { 7.0f, 10.0f }},
        2.7f
    }
};

static continuity_state_t g_last_scene_state;
static transition_state_t g_transition;
static int g_prev_act_idx = -1;

static stick_pose_t pose_mix(stick_pose_t a, stick_pose_t b, float t);

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

static float seg_u(float t, float t0, float t1) {
    if (t1 <= t0)
        return 1.0f;
    return clampf_local((t - t0) / (t1 - t0), 0.0f, 1.0f);
}

static float fractf_local(float v) {
    return v - floorf(v);
}

static float __attribute__((unused)) hold_after(float t, float at, float hold) {
    if (t <= at) return t;
    if (t < at + hold) return at;
    return t - hold;
}

static int flash_window(float t, float at, float dur) {
    return t >= at && t <= at + dur;
}

static float __attribute__((unused)) hash01(int n) {
    float s = sinf((float)n * 12.9898f + 78.233f) * 43758.5453f;
    return fractf_local(s);
}

static float total_animation_duration(void) {
    float total = 0.0f;

    for (int i = 0; i < ACT_COUNT; i++)
        total += act_durations[i];
    return total;
}

static vec2_t v2(float x, float y) {
    vec2_t v = {x, y};
    return v;
}

static vec2_t v2_add(vec2_t a, vec2_t b) {
    return v2(a.x + b.x, a.y + b.y);
}

static vec2_t v2_sub(vec2_t a, vec2_t b) {
    return v2(a.x - b.x, a.y - b.y);
}

static vec2_t v2_scale(vec2_t a, float s) {
    return v2(a.x * s, a.y * s);
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

static vec2_t v2_rot(vec2_t p, float a) {
    float c = cosf(a);
    float s = sinf(a);
    return v2(p.x * c - p.y * s, p.x * s + p.y * c);
}

static vec2_t __attribute__((unused)) recoil_offset(float t, float at, float dx, float dy, float dur) {
    float u = (t - at) / dur;
    if (u < 0.0f || u > 1.0f) return v2(0.0f, 0.0f);
    u = smoothstep_local(u);
    return v2(dx * sinf(u * (float)M_PI) * (1.0f - u),
              dy * sinf(u * (float)M_PI) * (1.0f - u));
}

static stick_pose_t pose_angry(stick_pose_t pose, float intensity) {
    pose.neck.x += 0.8f * intensity;
    pose.neck.y += 0.5f * intensity;
    pose.elbows[0].x -= 0.9f * intensity;
    pose.elbows[1].x += 1.1f * intensity;
    pose.hands[0].x -= 1.8f * intensity;
    pose.hands[1].x += 2.6f * intensity;
    pose.hands[0].y -= 0.8f * intensity;
    pose.hands[1].y -= 1.2f * intensity;
    pose.knees[0].y += 0.7f * intensity;
    pose.knees[1].y += 0.5f * intensity;
    return pose;
}

static stick_pose_t pose_strain(stick_pose_t pose, float dir, float intensity) {
    pose.neck.x += dir * 1.4f * intensity;
    pose.neck.y += 0.9f * intensity;
    pose.elbows[0].x -= dir * 1.1f * intensity;
    pose.elbows[1].x += dir * 0.7f * intensity;
    pose.hands[0].x -= dir * 2.6f * intensity;
    pose.hands[1].x += dir * 2.2f * intensity;
    pose.hands[0].y -= 1.2f * intensity;
    pose.hands[1].y -= 1.0f * intensity;
    pose.knees[0].y += 0.8f * intensity;
    pose.knees[1].y += 0.8f * intensity;
    pose.feet[0].x -= dir * 0.9f * intensity;
    pose.feet[1].x -= dir * 0.5f * intensity;
    return pose;
}

static stick_pose_t pose_hand_target(stick_pose_t pose, int hand, vec2_t target_rel_neck, float bend) {
    float hand_sign = hand == 0 ? -1.0f : 1.0f;
    vec2_t elbow = v2(target_rel_neck.x * 0.55f - hand_sign * bend,
                      target_rel_neck.y * 0.45f - 2.2f * bend);
    pose.elbows[hand] = v2_add(pose.neck, elbow);
    pose.hands[hand] = v2_add(pose.neck, target_rel_neck);
    return pose;
}

static stick_pose_t pose_both_hands_target(stick_pose_t pose, vec2_t left_rel_neck, vec2_t right_rel_neck, float bend) {
    pose = pose_hand_target(pose, 0, left_rel_neck, bend);
    pose = pose_hand_target(pose, 1, right_rel_neck, bend);
    return pose;
}

static stick_pose_t pose_stretched(stick_pose_t pose, float stretch) {
    stretch = clampf_local(stretch, 0.0f, 8.0f);
    pose.neck.x += stretch * 0.55f;
    for (int i = 0; i < 2; i++) {
        float arm_gain = i == 0 ? 0.45f : 0.9f;
        pose.elbows[i].x += stretch * arm_gain * 0.65f;
        pose.hands[i].x += stretch * arm_gain;
        pose.knees[i].x += stretch * 0.18f;
        pose.feet[i].x += stretch * 0.26f;
    }
    return pose;
}

static float bounce_offset(float t, float t0, float t1, float drop) {
    float u = seg_u(t, t0, t1);
    if (u <= 0.5f)
        return drop * (u * 2.0f);
    return drop * (1.0f - (u - 0.5f) * 2.0f);
}

static act6_control_state_t eval_act6_control(float raw_t) {
    act6_control_state_t state = { raw_t, 0, 0, 0, 0.0f };

    if (raw_t < 2.0f) {
        state.scene_t = raw_t;
    } else if (raw_t < 2.65f) {
        state.scene_t = 2.0f;
        state.stop_down = 1;
    } else if (raw_t < 4.15f) {
        float u = smoothstep_local((raw_t - 2.65f) / 1.5f);
        state.scene_t = mixf_local(2.0f, 6.0f, u);
        state.play_down = raw_t < 3.0f;
    } else if (raw_t < 7.35f) {
        float u = raw_t - 4.15f;
        state.scene_t = 6.0f + fmodf(u * 2.8f, 2.0f);
        state.play_down = (((int)(u * 11.0f)) & 1) == 0;
        state.shake = 0.9f;
        state.glitch = raw_t > 5.4f;
    } else if (raw_t < 9.35f) {
        float u = smoothstep_local((raw_t - 7.35f) / 2.0f);
        float stutter = (floorf((raw_t - 7.35f) * 12.0f) / 12.0f) * 0.18f;
        state.scene_t = mixf_local(8.0f, 10.0f, u) - stutter;
        state.play_down = (((int)((raw_t - 7.35f) * 14.0f)) & 1) == 0;
        state.glitch = 1;
        state.shake = 1.5f;
    } else {
        float u = smoothstep_local((raw_t - 9.35f) / 2.65f);
        state.scene_t = mixf_local(10.0f, 12.0f, u);
        state.glitch = raw_t < 10.2f;
        state.shake = raw_t < 10.0f ? 0.9f : 0.0f;
    }

    return state;
}

static void begin_act_transition(int act_idx) {
    if (g_prev_act_idx != -1 && act_idx != g_prev_act_idx) {
        g_transition.active = 1;
        g_transition.to_act = act_idx;
        g_transition.source = g_last_scene_state;
    }
    g_prev_act_idx = act_idx;
}

static void apply_transition_blend(int act_idx, float act_t,
                                   float *fig_x, float *fig_y, stick_pose_t *pose,
                                   float *scale, float *angle,
                                   float *cur_x, float *cur_y, int *cursor_pressed) {
    if (!g_transition.active || g_transition.to_act != act_idx)
        return;

    float u = smoothstep_local(smoothstep_local(clampf_local(act_t / TRANSITION_BLEND_SECONDS, 0.0f, 1.0f)));

    *fig_x = mixf_local(g_transition.source.fig_pos.x, *fig_x, u);
    *fig_y = mixf_local(g_transition.source.fig_pos.y, *fig_y, u);
    *pose = pose_mix(g_transition.source.pose, *pose, u);
    *scale = mixf_local(g_transition.source.scale > 0.0f ? g_transition.source.scale : *scale, *scale, u);
    *angle = mixf_local(g_transition.source.angle, *angle, u);
    *cur_x = mixf_local(g_transition.source.cursor_pos.x, *cur_x, u);
    *cur_y = mixf_local(g_transition.source.cursor_pos.y, *cur_y, u);
    if (u < 0.4f)
        *cursor_pressed = g_transition.source.cursor_pressed;

    if (u >= 0.999f)
        g_transition.active = 0;
}

static void remember_scene_state(float fig_x, float fig_y, const stick_pose_t *pose,
                                 float scale, float angle,
                                 float cur_x, float cur_y, int cursor_pressed) {
    vec2_t new_fig = v2(fig_x, fig_y);
    vec2_t new_cur = v2(cur_x, cur_y);
    g_last_scene_state.fig_vel = v2_sub(new_fig, g_last_scene_state.fig_pos);
    g_last_scene_state.cursor_vel = v2_sub(new_cur, g_last_scene_state.cursor_pos);
    g_last_scene_state.fig_pos = new_fig;
    g_last_scene_state.pose = *pose;
    g_last_scene_state.scale = scale;
    g_last_scene_state.angle = angle;
    g_last_scene_state.cursor_pos = new_cur;
    g_last_scene_state.cursor_pressed = cursor_pressed;
}

static int transition_source_for_act(int act_idx, continuity_state_t *state) {
    if (g_transition.active && g_transition.to_act == act_idx) {
        if (state)
            *state = g_transition.source;
        return 1;
    }
    return 0;
}

static stick_pose_t pose_mix(stick_pose_t a, stick_pose_t b, float t) {
    stick_pose_t pose;
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

static stick_pose_t sample_run_cycle(float phase) {
    float f = fractf_local(phase) * 2.0f;
    int i0 = (int)floorf(f);
    int i1 = (i0 + 1) & 1;
    return pose_mix(run_cycle[i0], run_cycle[i1], f - (float)i0);
}

static stick_pose_t sample_walk_cycle(float phase) {
    stick_pose_t walk = sample_run_cycle(phase * 0.6f);
    return pose_mix(pose_stand, walk, 0.5f);
}

static stick_pose_t sample_gymnast_swing_pose(float u) {
    u = clampf_local(u, 0.0f, 1.0f);

    if (u < 0.45f) {
        float su = smoothstep_local(u / 0.45f);
        return pose_mix(pose_hang, pose_gymnast_hollow, su);
    }

    {
        float su = smoothstep_local((u - 0.45f) / 0.55f);
        return pose_mix(pose_gymnast_hollow, pose_gymnast_invert, su);
    }
}

static vec2_t locked_hands_hip(const stick_pose_t *pose, vec2_t handle, float angle) {
    vec2_t avg_hands = v2((pose->hands[0].x + pose->hands[1].x) * 0.5f,
                          (pose->hands[0].y + pose->hands[1].y) * 0.5f);
    vec2_t rotated = v2_rot(avg_hands, angle);

    return v2(handle.x - rotated.x, handle.y - rotated.y);
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

static void clear_px(int x, int y) {
    if (x >= 0 && x < WIDTH && y >= 0 && y < HEIGHT)
        fb[x + (y / 8) * WIDTH] &= (uint8_t)~(1 << (y & 7));
}

static void any_px(int x, int y, int canvas_space) {
    if (canvas_space) {
        if (x >= 0 && x < CANVAS_W && y >= 0 && y < CANVAS_H)
            px(CANVAS_X + x, CANVAS_Y + y);
    } else {
        px(x, y);
    }
}

static void any_clear_px(int x, int y, int canvas_space) {
    if (canvas_space) {
        if (x >= 0 && x < CANVAS_W && y >= 0 && y < CANVAS_H)
            clear_px(CANVAS_X + x, CANVAS_Y + y);
    } else {
        clear_px(x, y);
    }
}

static void draw_line_any(int x0, int y0, int x1, int y1, int canvas_space) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    for (;;) {
        any_px(x0, y0, canvas_space);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_thick_line_any(int x0, int y0, int x1, int y1, int thickness, int canvas_space) {
    float nx = (float)(-(y1 - y0));
    float ny = (float)(x1 - x0);
    float len = sqrtf(nx * nx + ny * ny);

    if (thickness <= 1 || len <= 1e-6f) {
        draw_line_any(x0, y0, x1, y1, canvas_space);
        return;
    }

    nx /= len;
    ny /= len;

    for (int i = 0; i < thickness; i++) {
        float off = (float)i - 0.5f * (float)(thickness - 1);
        draw_line_any((int)lroundf((float)x0 + nx * off),
                      (int)lroundf((float)y0 + ny * off),
                      (int)lroundf((float)x1 + nx * off),
                      (int)lroundf((float)y1 + ny * off),
                      canvas_space);
    }
}

static projected_staff_t project_staff_shadow(vec2_t center, float half_len, float yaw, float roll,
                                              float grip_ratio, float hand_offset) {
    projected_staff_t staff;
    float dx = cosf(yaw) * cosf(roll) * half_len;
    float dy = sinf(roll) * half_len;
    vec2_t axis = v2(dx, dy);
    float axis_len = v2_len(axis);
    vec2_t dir = axis_len > 1e-6f ? v2_scale(axis, 1.0f / axis_len) : v2(1.0f, 0.0f);
    vec2_t normal = v2(-dir.y, dir.x);
    float grip = axis_len * grip_ratio;

    staff.left_tip = v2(center.x - dx, center.y - dy);
    staff.right_tip = v2(center.x + dx, center.y + dy);
    staff.left_hand = v2(center.x - dir.x * grip + normal.x * hand_offset,
                         center.y - dir.y * grip + normal.y * hand_offset);
    staff.right_hand = v2(center.x + dir.x * grip - normal.x * hand_offset,
                          center.y + dir.y * grip - normal.y * hand_offset);
    return staff;
}

static void draw_projected_staff(const projected_staff_t *staff, int thickness) {
    draw_thick_line_any((int)lroundf(staff->left_tip.x), (int)lroundf(staff->left_tip.y),
                        (int)lroundf(staff->right_tip.x), (int)lroundf(staff->right_tip.y),
                        thickness, 1);
}

static void draw_line_pattern_any(int x0, int y0, int x1, int y1, int on, int off, int canvas_space) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    int step = 0;
    int period = on + off;

    if (period < 1) period = 1;
    for (;;) {
        if ((step % period) < on)
            any_px(x0, y0, canvas_space);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
        step++;
    }
}

static void fill_rect_any(int x0, int y0, int x1, int y1, int canvas_space) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            any_px(x, y, canvas_space);
}

static void clear_rect_any(int x0, int y0, int x1, int y1, int canvas_space) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            any_clear_px(x, y, canvas_space);
}

static void outline_rect_any(int x0, int y0, int x1, int y1, int canvas_space) {
    draw_line_any(x0, y0, x1, y0, canvas_space);
    draw_line_any(x0, y1, x1, y1, canvas_space);
    draw_line_any(x0, y0, x0, y1, canvas_space);
    draw_line_any(x1, y0, x1, y1, canvas_space);
}

static void __attribute__((unused)) fill_circle_any(int cx, int cy, int r, int canvas_space) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r)
                any_px(cx + x, cy + y, canvas_space);
}

static void outline_circle_any(int cx, int cy, int r, int canvas_space) {
    const int segs = 18;
    for (int i = 0; i < segs; i++) {
        float a0 = (float)i * 2.0f * (float)M_PI / (float)segs;
        float a1 = (float)(i + 1) * 2.0f * (float)M_PI / (float)segs;
        int x0 = cx + (int)lroundf(cosf(a0) * r);
        int y0 = cy + (int)lroundf(sinf(a0) * r);
        int x1 = cx + (int)lroundf(cosf(a1) * r);
        int y1 = cy + (int)lroundf(sinf(a1) * r);
        draw_line_any(x0, y0, x1, y1, canvas_space);
    }
}

static void draw_partial_circle_any(int cx, int cy, int r, float u, int canvas_space) {
    const int segs = 18;
    int count = (int)lroundf(clampf_local(u, 0.0f, 1.0f) * (float)segs);
    if (count < 1) count = 1;
    for (int i = 0; i < count; i++) {
        float a0 = -0.4f + (float)i * 2.0f * (float)M_PI / (float)segs;
        float a1 = -0.4f + (float)(i + 1) * 2.0f * (float)M_PI / (float)segs;
        int x0 = cx + (int)lroundf(cosf(a0) * r);
        int y0 = cy + (int)lroundf(sinf(a0) * r);
        int x1 = cx + (int)lroundf(cosf(a1) * r);
        int y1 = cy + (int)lroundf(sinf(a1) * r);
        draw_line_any(x0, y0, x1, y1, canvas_space);
    }
}

static void draw_tool_button(int x, tool_t active_tool, tool_t this_tool) {
    outline_rect_any(x, TOOL_Y, x + TOOL_W - 1, TOOL_Y + TOOL_H - 1, 0);
    if (active_tool == this_tool)
        fill_rect_any(x + 1, TOOL_Y + 1, x + TOOL_W - 2, TOOL_Y + TOOL_H - 2, 0);
}

static void draw_toolbar_icon(tool_t tool, int x, int active) {
    int fg = active ? 0 : 1;
    int bx = x;
    int by = TOOL_Y;
    draw_tool_button(x, active ? tool : TOOL_NONE, tool);
    if (!fg) {
        outline_rect_any(bx + 1, by + 1, bx + TOOL_W - 2, by + TOOL_H - 2, 0);
        return;
    }

    switch (tool) {
        case TOOL_PENCIL:
            draw_line_any(bx + 1, by + 5, bx + 5, by + 1, 0);
            draw_line_any(bx + 4, by + 1, bx + 5, by + 2, 0);
            any_px(bx + 1, by + 5, 0);
            break;
        case TOOL_SELECT:
            draw_line_pattern_any(bx + 1, by + 1, bx + 5, by + 1, 1, 1, 0);
            draw_line_pattern_any(bx + 5, by + 1, bx + 5, by + 5, 1, 1, 0);
            draw_line_pattern_any(bx + 5, by + 5, bx + 1, by + 5, 1, 1, 0);
            draw_line_pattern_any(bx + 1, by + 5, bx + 1, by + 1, 1, 1, 0);
            break;
        case TOOL_SHAPE:
            outline_rect_any(bx + 1, by + 1, bx + 3, by + 3, 0);
            any_px(bx + 5, by + 5, 0);
            any_px(bx + 4, by + 4, 0);
            break;
        case TOOL_FILL:
            draw_line_any(bx + 2, by + 1, bx + 4, by + 3, 0);
            draw_line_any(bx + 4, by + 3, bx + 2, by + 5, 0);
            draw_line_any(bx + 2, by + 5, bx + 1, by + 4, 0);
            draw_line_any(bx + 1, by + 4, bx + 2, by + 1, 0);
            any_px(bx + 5, by + 5, 0);
            break;
        case TOOL_ERASE:
            draw_line_any(bx + 1, by + 5, bx + 3, by + 1, 0);
            draw_line_any(bx + 3, by + 1, bx + 5, by + 3, 0);
            draw_line_any(bx + 5, by + 3, bx + 3, by + 5, 0);
            draw_line_any(bx + 3, by + 5, bx + 1, by + 5, 0);
            break;
        default:
            break;
    }
}

static void draw_play_button(int down) {
    outline_rect_any(PLAY_X, BTN_Y, PLAY_X + BTN_W - 1, BTN_Y + BTN_H - 1, 0);
    if (down)
        fill_rect_any(PLAY_X + 1, BTN_Y + 1, PLAY_X + BTN_W - 2, BTN_Y + BTN_H - 2, 0);
    draw_line_any(PLAY_X + 2, BTN_Y + 1, PLAY_X + 5, BTN_Y + 3, 0);
    draw_line_any(PLAY_X + 5, BTN_Y + 3, PLAY_X + 2, BTN_Y + 5, 0);
    draw_line_any(PLAY_X + 2, BTN_Y + 5, PLAY_X + 2, BTN_Y + 1, 0);
}

static void draw_stop_button(int down) {
    outline_rect_any(STOP_X, BTN_Y, STOP_X + BTN_W - 1, BTN_Y + BTN_H - 1, 0);
    if (down)
        fill_rect_any(STOP_X + 1, BTN_Y + 1, STOP_X + BTN_W - 2, BTN_Y + BTN_H - 2, 0);
    outline_rect_any(STOP_X + 2, BTN_Y + 2, STOP_X + 4, BTN_Y + 4, 0);
}

static void draw_trash_button(int hot) {
    outline_rect_any(TRASH_X, BTN_Y, TRASH_X + BTN_W - 1, BTN_Y + BTN_H - 1, 0);
    if (hot)
        fill_rect_any(TRASH_X + 1, BTN_Y + 1, TRASH_X + BTN_W - 2, BTN_Y + BTN_H - 2, 0);
    draw_line_any(TRASH_X + 2, BTN_Y + 2, TRASH_X + 4, BTN_Y + 2, 0);
    draw_line_any(TRASH_X + 2, BTN_Y + 2, TRASH_X + 2, BTN_Y + 5, 0);
    draw_line_any(TRASH_X + 4, BTN_Y + 2, TRASH_X + 4, BTN_Y + 5, 0);
    draw_line_any(TRASH_X + 2, BTN_Y + 5, TRASH_X + 4, BTN_Y + 5, 0);
    any_px(TRASH_X + 3, BTN_Y + 1, 0);
}

static void draw_canvas_frame(int damage_stage) {
    if (damage_stage <= 0) {
        outline_rect_any(CANVAS_X, CANVAS_Y, CANVAS_X + CANVAS_W - 1, CANVAS_Y + CANVAS_H - 1, 0);
        return;
    }

    draw_line_any(CANVAS_X, CANVAS_Y, CANVAS_X + CANVAS_W - 20, CANVAS_Y, 0);
    draw_line_any(CANVAS_X, CANVAS_Y, CANVAS_X, CANVAS_Y + CANVAS_H - 1, 0);
    draw_line_any(CANVAS_X, CANVAS_Y + CANVAS_H - 1, CANVAS_X + CANVAS_W - 1, CANVAS_Y + CANVAS_H - 1, 0);
    draw_line_any(CANVAS_X + CANVAS_W - 1, CANVAS_Y + 18, CANVAS_X + CANVAS_W - 1, CANVAS_Y + CANVAS_H - 1, 0);

    if (damage_stage >= 2) {
        draw_line_any(CANVAS_X + CANVAS_W - 18, CANVAS_Y + 2, CANVAS_X + CANVAS_W - 10, CANVAS_Y + 7, 0);
        draw_line_any(CANVAS_X + CANVAS_W - 14, CANVAS_Y, CANVAS_X + CANVAS_W - 7, CANVAS_Y + 5, 0);
        draw_line_any(CANVAS_X + CANVAS_W - 5, CANVAS_Y + 8, CANVAS_X + CANVAS_W + 1, CANVAS_Y + 3, 0);
    }
}

static void draw_ui(const ui_state_t *ui, float sim_t) {
    outline_rect_any(WIN_X, WIN_Y, WIN_X + WIN_W - 1, WIN_Y + WIN_H - 1, 0);
    draw_line_any(WIN_X + 1, WIN_Y + TOPBAR_H, WIN_X + WIN_W - 2, WIN_Y + TOPBAR_H, 0);

    draw_toolbar_icon(TOOL_PENCIL, TOOL_PENCIL_X, ui->tool == TOOL_PENCIL);
    draw_toolbar_icon(TOOL_SELECT, TOOL_SELECT_X, ui->tool == TOOL_SELECT);
    draw_toolbar_icon(TOOL_SHAPE, TOOL_SHAPE_X, ui->tool == TOOL_SHAPE);
    draw_toolbar_icon(TOOL_FILL, TOOL_FILL_X, ui->tool == TOOL_FILL);
    draw_toolbar_icon(TOOL_ERASE, TOOL_ERASE_X, ui->tool == TOOL_ERASE);

    draw_play_button(ui->play_down);
    draw_stop_button(ui->stop_down);
    draw_trash_button(ui->trash_hot);

    for (int x = 55; x < 90; x += 4) {
        int yy = 7 + (int)lroundf(sinf(sim_t * 1.7f + x * 0.25f) * 1.0f);
        any_px(x, yy, 0);
    }

    draw_canvas_frame(ui->canvas_damage);

    if (ui->glitch) {
        for (int i = 0; i < 6; i++) {
            int y = CANVAS_Y + 4 + ((i * 7 + (int)(sim_t * 23.0f)) % (CANVAS_H - 8));
            draw_line_any(CANVAS_X + 1, y, CANVAS_X + CANVAS_W - 2, y, 0);
        }
    }
}

static vec2_t transform_rel(vec2_t p, float scale, float angle) {
    return v2_scale(v2_rot(p, angle), scale);
}

#define BASE_TORSO_LEN       8.0f
#define BASE_UPPER_ARM_LEN   3.905125f
#define BASE_LOWER_ARM_LEN   3.640055f
#define BASE_UPPER_LEG_LEN   4.386342f
#define BASE_LOWER_LEG_LEN   6.020797f

#define TARGET_TORSO_LEN     6.0f
#define TARGET_UPPER_ARM_LEN 3.47f
#define TARGET_LOWER_ARM_LEN 3.24f
#define TARGET_UPPER_LEG_LEN 3.39f
#define TARGET_LOWER_LEG_LEN 4.67f
#define TARGET_HEAD_RADIUS   4.0f
#define TARGET_HEAD_OFFSET   8.0f

typedef struct {
    vec2_t neck;
    vec2_t elbows[2];
    vec2_t hands[2];
    vec2_t knees[2];
    vec2_t feet[2];
    vec2_t head_center;
    float head_r;
} resolved_pose_t;

static vec2_t unit_or(vec2_t v, vec2_t fallback) {
    float len = v2_len(v);
    if (len <= 1e-6f)
        return v2_norm(fallback);
    return v2_scale(v, 1.0f / len);
}

static float scaled_limb_length(float raw_len, float base_len, float target_len) {
    if (raw_len <= 1e-6f || base_len <= 1e-6f)
        return target_len;
    return target_len * (raw_len / base_len);
}

static void resolve_pose_local(const stick_pose_t *pose, resolved_pose_t *resolved) {
    vec2_t torso_dir = unit_or(pose->neck, v2(0.0f, -1.0f));
    float torso_len = scaled_limb_length(v2_len(pose->neck), BASE_TORSO_LEN, TARGET_TORSO_LEN);

    resolved->neck = v2_scale(torso_dir, torso_len);

    for (int i = 0; i < 2; i++) {
        vec2_t arm_fallback = v2(i == 0 ? -1.0f : 1.0f, 0.0f);
        vec2_t raw_upper_arm = v2_sub(pose->elbows[i], pose->neck);
        vec2_t upper_arm_dir = unit_or(raw_upper_arm, arm_fallback);
        float upper_arm_len = scaled_limb_length(v2_len(raw_upper_arm), BASE_UPPER_ARM_LEN, TARGET_UPPER_ARM_LEN);

        resolved->elbows[i] = v2_add(resolved->neck, v2_scale(upper_arm_dir, upper_arm_len));

        vec2_t raw_lower_arm = v2_sub(pose->hands[i], pose->elbows[i]);
        vec2_t lower_arm_dir = unit_or(raw_lower_arm, raw_upper_arm);
        float lower_arm_len = scaled_limb_length(v2_len(raw_lower_arm), BASE_LOWER_ARM_LEN, TARGET_LOWER_ARM_LEN);

        resolved->hands[i] = v2_add(resolved->elbows[i], v2_scale(lower_arm_dir, lower_arm_len));

        vec2_t leg_fallback = v2(i == 0 ? -0.45f : 0.45f, 1.0f);
        vec2_t raw_upper_leg = pose->knees[i];
        vec2_t upper_leg_dir = unit_or(raw_upper_leg, leg_fallback);
        float upper_leg_len = scaled_limb_length(v2_len(raw_upper_leg), BASE_UPPER_LEG_LEN, TARGET_UPPER_LEG_LEN);

        resolved->knees[i] = v2_scale(upper_leg_dir, upper_leg_len);

        vec2_t raw_lower_leg = v2_sub(pose->feet[i], pose->knees[i]);
        vec2_t lower_leg_dir = unit_or(raw_lower_leg, raw_upper_leg);
        float lower_leg_len = scaled_limb_length(v2_len(raw_lower_leg), BASE_LOWER_LEG_LEN, TARGET_LOWER_LEG_LEN);

        resolved->feet[i] = v2_add(resolved->knees[i], v2_scale(lower_leg_dir, lower_leg_len));
    }

    resolved->head_center = v2_add(resolved->neck, v2_scale(unit_or(resolved->neck, v2(0.0f, -1.0f)), TARGET_HEAD_OFFSET));
    resolved->head_r = TARGET_HEAD_RADIUS;
}

static void draw_stick_pose(float x, float hip_y, const stick_pose_t *pose, float scale, float angle) {
    vec2_t hip = v2(x, hip_y);
    resolved_pose_t resolved;
    resolve_pose_local(pose, &resolved);

    vec2_t neck = v2_add(hip, transform_rel(resolved.neck, scale, angle));
    vec2_t elbows[2], hands[2], knees[2], feet[2];
    for (int i = 0; i < 2; i++) {
        elbows[i] = v2_add(hip, transform_rel(resolved.elbows[i], scale, angle));
        hands[i] = v2_add(hip, transform_rel(resolved.hands[i], scale, angle));
        knees[i] = v2_add(hip, transform_rel(resolved.knees[i], scale, angle));
        feet[i] = v2_add(hip, transform_rel(resolved.feet[i], scale, angle));
    }

    vec2_t head_center = v2_add(hip, transform_rel(resolved.head_center, scale, angle));
    int hr = (int)lroundf(resolved.head_r * scale);
    if (hr < 2) hr = 2;

    draw_line_any((int)lroundf(hip.x), (int)lroundf(hip.y), (int)lroundf(neck.x), (int)lroundf(neck.y), 1);
    for (int i = 0; i < 2; i++) {
        draw_line_any((int)lroundf(neck.x), (int)lroundf(neck.y), (int)lroundf(elbows[i].x), (int)lroundf(elbows[i].y), 1);
        draw_line_any((int)lroundf(elbows[i].x), (int)lroundf(elbows[i].y), (int)lroundf(hands[i].x), (int)lroundf(hands[i].y), 1);
        draw_line_any((int)lroundf(hip.x), (int)lroundf(hip.y), (int)lroundf(knees[i].x), (int)lroundf(knees[i].y), 1);
        draw_line_any((int)lroundf(knees[i].x), (int)lroundf(knees[i].y), (int)lroundf(feet[i].x), (int)lroundf(feet[i].y), 1);
        any_px((int)lroundf(hands[i].x), (int)lroundf(hands[i].y), 1);
        any_px((int)lroundf(feet[i].x), (int)lroundf(feet[i].y), 1);
    }

    outline_circle_any((int)lroundf(head_center.x), (int)lroundf(head_center.y), hr, 1);
}

static void local_figure_bounds(const stick_pose_t *pose, int *min_x, int *min_y, int *max_x, int *max_y) {
    vec2_t hip = v2(0.0f, 0.0f);
    resolved_pose_t resolved;
    vec2_t neck;
    vec2_t pts[11];

    resolve_pose_local(pose, &resolved);
    neck = resolved.neck;

    pts[0] = hip;
    pts[1] = neck;
    for (int i = 0; i < 2; i++) {
        pts[2 + i] = resolved.elbows[i];
        pts[4 + i] = resolved.hands[i];
        pts[6 + i] = resolved.knees[i];
        pts[8 + i] = resolved.feet[i];
    }

    pts[10] = resolved.head_center;

    int hr = (int)lroundf(resolved.head_r);
    if (hr < 2) hr = 2;

    *min_x = 999;
    *min_y = 999;
    *max_x = -999;
    *max_y = -999;

    for (int i = 0; i < 11; i++) {
        int px0 = (int)lroundf(pts[i].x);
        int py0 = (int)lroundf(pts[i].y);
        if (i == 10) {
            if (px0 - hr < *min_x) *min_x = px0 - hr;
            if (px0 + hr > *max_x) *max_x = px0 + hr;
            if (py0 - hr < *min_y) *min_y = py0 - hr;
            if (py0 + hr > *max_y) *max_y = py0 + hr;
        } else {
            if (px0 < *min_x) *min_x = px0;
            if (px0 > *max_x) *max_x = px0;
            if (py0 < *min_y) *min_y = py0;
            if (py0 > *max_y) *max_y = py0;
        }
    }

    *min_x -= 2;
    *min_y -= 2;
    *max_x += 2;
    *max_y += 2;
}

static void __attribute__((unused)) figure_bounds(const stick_pose_t *pose, float x, float hip_y, float scale, float angle,
                                                  int *min_x, int *min_y, int *max_x, int *max_y) {
    vec2_t hip = v2(x, hip_y);
    resolved_pose_t resolved;
    vec2_t neck;
    vec2_t pts[11];

    resolve_pose_local(pose, &resolved);
    neck = v2_add(hip, transform_rel(resolved.neck, scale, angle));

    pts[0] = hip;
    pts[1] = neck;
    for (int i = 0; i < 2; i++) {
        pts[2 + i] = v2_add(hip, transform_rel(resolved.elbows[i], scale, angle));
        pts[4 + i] = v2_add(hip, transform_rel(resolved.hands[i], scale, angle));
        pts[6 + i] = v2_add(hip, transform_rel(resolved.knees[i], scale, angle));
        pts[8 + i] = v2_add(hip, transform_rel(resolved.feet[i], scale, angle));
    }
    pts[10] = v2_add(hip, transform_rel(resolved.head_center, scale, angle));
    int hr = (int)lroundf(resolved.head_r * scale);
    if (hr < 2) hr = 2;

    *min_x = 999;
    *min_y = 999;
    *max_x = -999;
    *max_y = -999;

    for (int i = 0; i < 11; i++) {
        int px0 = (int)lroundf(pts[i].x);
        int py0 = (int)lroundf(pts[i].y);
        if (i == 10) {
            if (px0 - hr < *min_x) *min_x = px0 - hr;
            if (px0 + hr > *max_x) *max_x = px0 + hr;
            if (py0 - hr < *min_y) *min_y = py0 - hr;
            if (py0 + hr > *max_y) *max_y = py0 + hr;
        } else {
            if (px0 < *min_x) *min_x = px0;
            if (px0 > *max_x) *max_x = px0;
            if (py0 < *min_y) *min_y = py0;
            if (py0 > *max_y) *max_y = py0;
        }
    }
    *min_x -= 2;
    *min_y -= 2;
    *max_x += 2;
    *max_y += 2;
}

static void draw_selection_box(const stick_pose_t *pose, float x, float hip_y, float scale, float angle, int rot_handle) {
    int min_x, min_y, max_x, max_y;
    vec2_t hip = v2(x, hip_y);
    vec2_t corners[4];
    vec2_t handles[8];

    local_figure_bounds(pose, &min_x, &min_y, &max_x, &max_y);

    vec2_t local_corners[4] = {
        v2((float)min_x, (float)min_y),
        v2((float)max_x, (float)min_y),
        v2((float)max_x, (float)max_y),
        v2((float)min_x, (float)max_y)
    };

    for (int i = 0; i < 4; i++)
        corners[i] = v2_add(hip, transform_rel(local_corners[i], scale, angle));

    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        draw_line_pattern_any((int)lroundf(corners[i].x), (int)lroundf(corners[i].y),
                              (int)lroundf(corners[j].x), (int)lroundf(corners[j].y), 1, 1, 1);
    }

    handles[0] = corners[0];
    handles[1] = corners[1];
    handles[2] = corners[2];
    handles[3] = corners[3];
    handles[4] = v2_lerp(corners[0], corners[1], 0.5f);
    handles[5] = v2_lerp(corners[1], corners[2], 0.5f);
    handles[6] = v2_lerp(corners[2], corners[3], 0.5f);
    handles[7] = v2_lerp(corners[3], corners[0], 0.5f);

    for (int i = 0; i < 8; i++) {
        int hx = (int)lroundf(handles[i].x);
        int hy = (int)lroundf(handles[i].y);
        fill_rect_any(hx - 1, hy - 1, hx + 1, hy + 1, 1);
    }

    if (rot_handle) {
        vec2_t top_mid = handles[4];
        vec2_t up = v2_norm(transform_rel(v2(0.0f, -1.0f), 1.0f, angle));
        vec2_t tangent = v2_norm(v2_sub(corners[1], corners[0]));
        vec2_t stem_end = v2_add(top_mid, v2_scale(up, 4.0f));
        vec2_t knob = v2_add(top_mid, v2_scale(up, 6.0f));
        int sx = (int)lroundf(stem_end.x);
        int sy = (int)lroundf(stem_end.y);
        int kx = (int)lroundf(knob.x);
        int ky = (int)lroundf(knob.y);

        draw_line_any((int)lroundf(top_mid.x), (int)lroundf(top_mid.y), sx, sy, 1);
        outline_circle_any(kx, ky, 1, 1);
        draw_line_any(kx + (int)lroundf(tangent.x * 2.0f), ky + (int)lroundf(tangent.y * 2.0f),
                      kx + (int)lroundf(tangent.x * 4.0f + up.x), ky + (int)lroundf(tangent.y * 4.0f + up.y), 1);
        draw_line_any(kx + (int)lroundf(tangent.x * 3.0f - up.x), ky + (int)lroundf(tangent.y * 3.0f - up.y),
                      kx + (int)lroundf(tangent.x * 4.0f + up.x), ky + (int)lroundf(tangent.y * 4.0f + up.y), 1);
    }
}

static void draw_cursor_oriented(float x, float y, int pressed, float angle) {
    static const vec2_t points[] = {
        { 0.0f, 0.0f },
        { 0.0f, 8.0f },
        { 6.0f, 5.0f },
        { 3.0f, 5.0f },
        { 5.0f, 10.0f },
        { 4.0f, 10.0f },
        { 2.0f, 6.0f }
    };
    static const int segments[][2] = {
        { 0, 1 },
        { 0, 2 },
        { 2, 3 },
        { 3, 4 },
        { 4, 5 },
        { 5, 6 },
        { 6, 1 }
    };
    vec2_t base = v2(x, y);

    for (size_t i = 0; i < sizeof(segments) / sizeof(segments[0]); i++) {
        vec2_t p0 = v2_add(base, v2_rot(points[segments[i][0]], angle));
        vec2_t p1 = v2_add(base, v2_rot(points[segments[i][1]], angle));
        draw_line_any((int)lroundf(p0.x), (int)lroundf(p0.y),
                      (int)lroundf(p1.x), (int)lroundf(p1.y), 0);
    }

    if (pressed) {
        vec2_t click0 = v2_add(base, v2_rot(v2(6.0f, 1.0f), angle));
        vec2_t click1 = v2_add(base, v2_rot(v2(7.0f, 2.0f), angle));
        any_px((int)lroundf(click0.x), (int)lroundf(click0.y), 0);
        any_px((int)lroundf(click1.x), (int)lroundf(click1.y), 0);
    }
}

static void draw_cursor(float x, float y, int pressed) {
    draw_cursor_oriented(x, y, pressed, 0.0f);
}

static void __attribute__((unused)) draw_cursor_stage(float x, float y, int pressed, int stage) {
    int ix = (int)lroundf(x);
    int iy = (int)lroundf(y);

    switch (stage) {
        case 0:
            draw_cursor(x, y, pressed);
            break;
        case 1:
            draw_line_any(ix, iy, ix, iy + 5, 0);
            draw_line_any(ix, iy, ix + 4, iy + 3, 0);
            draw_line_any(ix + 4, iy + 3, ix + 2, iy + 3, 0);
            draw_line_any(ix + 2, iy + 3, ix + 3, iy + 6, 0);
            draw_line_any(ix + 3, iy + 6, ix + 2, iy + 6, 0);
            draw_line_any(ix + 2, iy + 6, ix + 1, iy + 4, 0);
            draw_line_any(ix + 1, iy + 4, ix, iy + 5, 0);
            if (pressed)
                any_px(ix + 4, iy + 1, 0);
            break;
        case 2:
            draw_line_any(ix, iy, ix, iy + 2, 0);
            draw_line_any(ix, iy, ix + 1, iy + 2, 0);
            any_px(ix + 1, iy + 1, 0);
            break;
        case 3:
            any_px(ix, iy, 0);
            break;
        default:
            break;
    }
}

static void draw_impact_burst(float x, float y, int r, int canvas_space) {
    int ix = (int)lroundf(x);
    int iy = (int)lroundf(y);
    for (int i = 0; i < 6; i++) {
        float a = (float)i * (float)M_PI / 3.0f;
        int x0 = ix + (int)lroundf(cosf(a) * 1.0f);
        int y0 = iy + (int)lroundf(sinf(a) * 1.0f);
        int x1 = ix + (int)lroundf(cosf(a) * (float)r);
        int y1 = iy + (int)lroundf(sinf(a) * (float)r);
        draw_line_any(x0, y0, x1, y1, canvas_space);
    }
}

static void draw_confetti_pop(float x, float y, float u) {
    static const vec2_t base_offsets[] = {
        {-12.0f, -1.0f}, {-8.0f, -3.0f}, {-3.0f, -2.0f}, {3.0f, -1.0f},
        {8.0f, -2.0f}, {12.0f, 0.0f}, {-7.0f, 2.0f}, {6.0f, 3.0f}
    };
    static const vec2_t velocities[] = {
        {-2.0f, -3.0f}, {-1.5f, -4.0f}, {-0.5f, -3.0f}, {0.8f, -3.5f},
        {1.5f, -3.0f}, {2.4f, -2.2f}, {-1.4f, 1.8f}, {1.8f, 1.4f}
    };

    u = clampf_local(u, 0.0f, 1.0f);
    for (int i = 0; i < 8; i++) {
        vec2_t p = v2(x + base_offsets[i].x + velocities[i].x * u * 4.0f,
                      y + base_offsets[i].y + velocities[i].y * u * 4.0f);
        int px0 = (int)lroundf(p.x);
        int py0 = (int)lroundf(p.y);
        int px1 = px0 + (i & 1 ? 1 : -1);
        int py1 = py0 + ((i / 2) & 1 ? 1 : 0);
        draw_line_any(px0, py0, px1, py1, 1);
    }
}

static void __attribute__((unused)) draw_impact_rays(float x, float y, int r, int canvas_space) {
    int ix = (int)lroundf(x);
    int iy = (int)lroundf(y);
    draw_line_any(ix - r - 1, iy - 1, ix + r + 1, iy + 1, canvas_space);
    draw_line_any(ix - r - 1, iy + 1, ix + r + 1, iy - 1, canvas_space);
    draw_line_any(ix - 1, iy - r - 1, ix + 1, iy + r + 1, canvas_space);
    draw_line_any(ix + 1, iy - r - 1, ix - 1, iy + r + 1, canvas_space);
}

static void draw_canvas_flash(void) {
    fill_rect_any(0, 0, CANVAS_W - 1, CANVAS_H - 1, 1);
}

static void draw_strain_lines(float x, float y, float intensity) {
    int rays = 4 + (int)lroundf(intensity * 4.0f);
    int len = 3 + (int)lroundf(intensity * 3.0f);
    for (int i = 0; i < rays; i++) {
        float a = ((float)i / (float)rays) * 2.0f * (float)M_PI + intensity * 0.8f;
        int x0 = (int)lroundf(x + cosf(a) * 4.0f);
        int y0 = (int)lroundf(y + sinf(a) * 4.0f);
        int x1 = (int)lroundf(x + cosf(a) * (4.0f + len));
        int y1 = (int)lroundf(y + sinf(a) * (4.0f + len));
        draw_line_any(x0, y0, x1, y1, 1);
    }
}

static void draw_eraser_outline(float x, float y, int w, int h, int canvas_space) {
    int ix = (int)lroundf(x);
    int iy = (int)lroundf(y);
    outline_rect_any(ix, iy, ix + w, iy + h, canvas_space);
    draw_line_any(ix + 1, iy + h, ix + w - 2, iy + 1, canvas_space);
}

static void draw_skid_marks(float x, float y, float fade) {
    int len = 4 - (int)lroundf(clampf_local(fade, 0.0f, 1.0f) * 2.0f);
    if (len < 1)
        len = 1;
    int ix = (int)lroundf(x);
    int iy = (int)lroundf(y);
    draw_line_any(ix - len, iy, ix - 1, iy, 1);
    if (fade < 0.7f)
        draw_line_any(ix - len - 1, iy + 1, ix - 2, iy + 1, 1);
}

static void draw_timeline_overlay(float playhead_x, int play_down, int stop_down) {
    int y = 38;
    draw_line_any(20, y, 110, y, 1);
    outline_rect_any(23, y - 2, 27, y + 2, 1);
    if (stop_down)
        fill_rect_any(24, y - 1, 26, y + 1, 1);
    outline_rect_any(29, y - 2, 33, y + 2, 1);
    draw_line_any(30, y - 1, 32, y, 1);
    draw_line_any(32, y, 30, y + 1, 1);
    draw_line_any(30, y + 1, 30, y - 1, 1);
    if (play_down)
        fill_rect_any(30, y - 1, 31, y + 1, 1);
    int px0 = (int)lroundf(playhead_x);
    draw_line_any(px0, y - 3, px0 - 2, y - 6, 1);
    draw_line_any(px0, y - 3, px0 + 2, y - 6, 1);
    draw_line_any(px0 - 2, y - 6, px0 + 2, y - 6, 1);
}

static void draw_timeline_overlay_progress(float u, float playhead_x, int play_down, int stop_down) {
    int y = 38;
    u = clampf_local(u, 0.0f, 1.0f);
    if (u <= 0.0f)
        return;

    draw_line_any(20, y, (int)lroundf(mixf_local(20.0f, 110.0f, u)), y, 1);

    if (u >= 0.2f) {
        outline_rect_any(23, y - 2, 27, y + 2, 1);
        if (stop_down)
            fill_rect_any(24, y - 1, 26, y + 1, 1);
    }

    if (u >= 0.35f) {
        outline_rect_any(29, y - 2, 33, y + 2, 1);
        draw_line_any(30, y - 1, 32, y, 1);
        draw_line_any(32, y, 30, y + 1, 1);
        draw_line_any(30, y + 1, 30, y - 1, 1);
        if (play_down)
            fill_rect_any(30, y - 1, 31, y + 1, 1);
    }

    if (u >= 0.55f) {
        float pu = seg_u(u, 0.55f, 1.0f);
        int px0 = (int)lroundf(mixf_local(60.0f, playhead_x, pu));
        draw_line_any(px0, y - 3, px0 - 2, y - 6, 1);
        draw_line_any(px0, y - 3, px0 + 2, y - 6, 1);
        draw_line_any(px0 - 2, y - 6, px0 + 2, y - 6, 1);
    }
}

static void clear_top_transport_region(void) {
    clear_rect_any(52, 2, 123, 9, 0);
}

static void draw_falling_transport_controls(float x0, float x1, float y, float playhead_u) {
    int ix0 = (int)lroundf(x0);
    int ix1 = (int)lroundf(x1);
    int iy = (int)lroundf(y);
    int btn_y0 = iy - 3;
    int btn_y1 = iy + 2;
    int rewind_x = ix0;
    int play_x = ix0 + 8;
    int ff_x = ix0 + 16;
    int track_x0 = ix0 + 26;
    int track_x1 = ix1;
    int px0;

    if (track_x1 <= track_x0)
        track_x1 = track_x0 + 1;

    outline_rect_any(rewind_x, btn_y0, rewind_x + 5, btn_y1, 0);
    draw_line_any(rewind_x + 2, iy - 2, rewind_x, iy, 0);
    draw_line_any(rewind_x, iy, rewind_x + 2, iy + 2, 0);
    draw_line_any(rewind_x + 5, iy - 2, rewind_x + 3, iy, 0);
    draw_line_any(rewind_x + 3, iy, rewind_x + 5, iy + 2, 0);

    outline_rect_any(play_x, btn_y0, play_x + 5, btn_y1, 0);
    draw_line_any(play_x + 2, iy - 2, play_x + 4, iy, 0);
    draw_line_any(play_x + 4, iy, play_x + 2, iy + 2, 0);
    draw_line_any(play_x + 2, iy + 2, play_x + 2, iy - 2, 0);

    outline_rect_any(ff_x, btn_y0, ff_x + 5, btn_y1, 0);
    draw_line_any(ff_x + 1, iy - 2, ff_x + 3, iy, 0);
    draw_line_any(ff_x + 3, iy, ff_x + 1, iy + 2, 0);
    draw_line_any(ff_x + 3, iy - 2, ff_x + 5, iy, 0);
    draw_line_any(ff_x + 5, iy, ff_x + 3, iy + 2, 0);

    draw_line_any(track_x0, iy, track_x1, iy, 0);
    px0 = (int)lroundf(mixf_local((float)(track_x0 + 6), (float)(track_x1 - 2), clampf_local(playhead_u, 0.0f, 1.0f)));
    draw_line_any(px0, iy - 3, px0 - 2, iy - 6, 0);
    draw_line_any(px0, iy - 3, px0 + 2, iy - 6, 0);
    draw_line_any(px0 - 2, iy - 6, px0 + 2, iy - 6, 0);
}

static void draw_delete_zone_local(int x0, int y0, int x1, int y1) {
    outline_rect_any(x0, y0, x1, y1, 1);
    draw_line_any(x0 + 1, y0 + 1, x1 - 1, y1 - 1, 1);
    draw_line_any(x0 + 1, y1 - 1, x1 - 1, y0 + 1, 1);
}

typedef struct {
    int x0, y0;
    int x1, y1;
} stroke_segment_t;

static const stroke_segment_t the_end_strokes[] = {
    {38, 16, 44, 16}, {41, 16, 41, 24},
    {48, 16, 48, 24}, {48, 20, 52, 20}, {52, 20, 52, 24},
    {56, 16, 60, 16}, {56, 20, 60, 20}, {56, 20, 56, 24}, {56, 24, 60, 24},
    {67, 16, 67, 24}, {67, 16, 72, 16}, {67, 20, 71, 20}, {67, 24, 72, 24},
    {76, 20, 76, 24}, {76, 20, 80, 20}, {80, 20, 80, 24},
    {84, 16, 84, 24}, {84, 16, 87, 18}, {87, 18, 87, 22}, {87, 22, 84, 24}
};

static void draw_the_end_progress(float progress, float *cursor_x, float *cursor_y) {
    const int stroke_count = (int)(sizeof(the_end_strokes) / sizeof(the_end_strokes[0]));
    int full_count;
    float partial_u;

    progress = clampf_local(progress, 0.0f, (float)stroke_count);
    full_count = (int)floorf(progress);
    partial_u = progress - (float)full_count;

    if (cursor_x)
        *cursor_x = (float)the_end_strokes[0].x0;
    if (cursor_y)
        *cursor_y = (float)the_end_strokes[0].y0;

    for (int i = 0; i < full_count && i < stroke_count; i++) {
        draw_line_any(the_end_strokes[i].x0, the_end_strokes[i].y0,
                      the_end_strokes[i].x1, the_end_strokes[i].y1, 1);
        if (cursor_x)
            *cursor_x = (float)the_end_strokes[i].x1;
        if (cursor_y)
            *cursor_y = (float)the_end_strokes[i].y1;
    }

    if (full_count < stroke_count && partial_u > 0.0f) {
        int px = (int)lroundf(mixf_local((float)the_end_strokes[full_count].x0,
                                         (float)the_end_strokes[full_count].x1, partial_u));
        int py = (int)lroundf(mixf_local((float)the_end_strokes[full_count].y0,
                                         (float)the_end_strokes[full_count].y1, partial_u));

        draw_line_any(the_end_strokes[full_count].x0, the_end_strokes[full_count].y0, px, py, 1);
        if (cursor_x)
            *cursor_x = (float)px;
        if (cursor_y)
            *cursor_y = (float)py;
    } else if (full_count >= stroke_count && stroke_count > 0) {
        if (cursor_x)
            *cursor_x = (float)the_end_strokes[stroke_count - 1].x1;
        if (cursor_y)
            *cursor_y = (float)the_end_strokes[stroke_count - 1].y1;
    }
}

static void apply_random_screen_fade(float u) {
    u = clampf_local(u, 0.0f, 1.0f);
    if (u <= 0.0f)
        return;
    if (u >= 1.0f) {
        memset(fb, 0, sizeof(fb));
        return;
    }

    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            if (hash01(x + y * WIDTH) < u)
                clear_px(x, y);
        }
    }
}

static void __attribute__((unused)) draw_partial_stick_build(float x, float hip_y, float u, vec2_t *cursor_out) {
    vec2_t cursor = v2(x, hip_y);
    vec2_t neck = v2(x, hip_y - 8.0f);
    vec2_t head = v2(x, hip_y - 13.0f);

    if (u <= 0.28f) {
        float p = u / 0.28f;
        draw_partial_circle_any((int)head.x, (int)head.y, 3, p, 1);
        float a = -0.4f + p * 2.0f * (float)M_PI;
        cursor = v2(head.x + cosf(a) * 3.0f, head.y + sinf(a) * 3.0f);
    } else {
        outline_circle_any((int)head.x, (int)head.y, 3, 1);
    }

    if (u > 0.28f) {
        float p = clampf_local((u - 0.28f) / 0.18f, 0.0f, 1.0f);
        float y1 = mixf_local(head.y + 3.0f, neck.y, p);
        draw_line_any((int)x, (int)(head.y + 3.0f), (int)x, (int)lroundf(y1), 1);
        cursor = v2(x, y1);
    }

    if (u > 0.46f) {
        float p = clampf_local((u - 0.46f) / 0.20f, 0.0f, 1.0f);
        float ax = mixf_local(x, x + 4.0f, p);
        float ay = mixf_local(neck.y, neck.y - 1.0f, p);
        draw_line_any((int)x, (int)neck.y, (int)lroundf(x - 4.0f * p), (int)lroundf(neck.y - 1.0f * p), 1);
        draw_line_any((int)x, (int)neck.y, (int)lroundf(ax), (int)lroundf(ay), 1);
        cursor = v2(ax, ay);
    }

    if (u > 0.66f) {
        float p = clampf_local((u - 0.66f) / 0.34f, 0.0f, 1.0f);
        float lx = mixf_local(x, x - 2.0f, p);
        float ly = mixf_local(hip_y, hip_y + 10.0f, p);
        float rx = mixf_local(x, x + 2.0f, p);
        float ry = mixf_local(hip_y, hip_y + 10.0f, p);
        draw_line_any((int)x, (int)hip_y, (int)lroundf(lx), (int)lroundf(ly), 1);
        draw_line_any((int)x, (int)hip_y, (int)lroundf(rx), (int)lroundf(ry), 1);
        cursor = v2(rx, ry);
    }

    *cursor_out = cursor;
}

static void draw_marquee(int x0, int y0, int x1, int y1) {
    draw_line_pattern_any(x0, y0, x1, y0, 1, 1, 1);
    draw_line_pattern_any(x1, y0, x1, y1, 1, 1, 1);
    draw_line_pattern_any(x1, y1, x0, y1, 1, 1, 1);
    draw_line_pattern_any(x0, y1, x0, y0, 1, 1, 1);
}

static void draw_rect_progress_any(int x0, int y0, int x1, int y1, float u, int canvas_space) {
    float progress = clampf_local(u, 0.0f, 1.0f) * 4.0f;

    if (progress <= 0.0f)
        return;

    if (progress <= 1.0f) {
        int xt = (int)lroundf(mixf_local((float)x0, (float)x1, progress));
        draw_line_any(x0, y0, xt, y0, canvas_space);
        return;
    }

    draw_line_any(x0, y0, x1, y0, canvas_space);
    progress -= 1.0f;
    if (progress <= 1.0f) {
        int yt = (int)lroundf(mixf_local((float)y0, (float)y1, progress));
        draw_line_any(x1, y0, x1, yt, canvas_space);
        return;
    }

    draw_line_any(x1, y0, x1, y1, canvas_space);
    progress -= 1.0f;
    if (progress <= 1.0f) {
        int xt = (int)lroundf(mixf_local((float)x1, (float)x0, progress));
        draw_line_any(x1, y1, xt, y1, canvas_space);
        return;
    }

    draw_line_any(x1, y1, x0, y1, canvas_space);
    progress -= 1.0f;
    if (progress <= 1.0f) {
        int yt = (int)lroundf(mixf_local((float)y1, (float)y0, progress));
        draw_line_any(x0, y1, x0, yt, canvas_space);
        return;
    }

    draw_line_any(x0, y1, x0, y0, canvas_space);
}

static vec2_t rect_progress_point(int x0, int y0, int x1, int y1, float u) {
    float progress = clampf_local(u, 0.0f, 1.0f) * 4.0f;

    if (progress <= 1.0f)
        return v2(mixf_local((float)x0, (float)x1, progress), (float)y0);

    progress -= 1.0f;
    if (progress <= 1.0f)
        return v2((float)x1, mixf_local((float)y0, (float)y1, progress));

    progress -= 1.0f;
    if (progress <= 1.0f)
        return v2(mixf_local((float)x1, (float)x0, progress), (float)y1);

    progress -= 1.0f;
    if (progress <= 1.0f)
        return v2((float)x0, mixf_local((float)y1, (float)y0, progress));

    return v2((float)x0, (float)y0);
}

static void draw_bucket_icon(float x, float y) {
    int ix = (int)lroundf(x);
    int iy = (int)lroundf(y);

    draw_line_any(ix + 5, iy + 4, ix + 7, iy + 2, 0);
    draw_line_any(ix + 7, iy + 2, ix + 9, iy + 4, 0);
    draw_line_any(ix + 9, iy + 4, ix + 7, iy + 6, 0);
    draw_line_any(ix + 7, iy + 6, ix + 5, iy + 4, 0);
    any_px(ix + 10, iy + 6, 0);
}

static void draw_fake_left_tool_icons(float u) {
    static const int icon_y[] = {8, 18, 28, 38};
    int top_y;

    u = clampf_local(u, 0.0f, 1.0f);
    top_y = (int)lroundf(mixf_local(-24.0f, 8.0f, smoothstep_local(u)));

    if (u <= 0.0f)
        return;

    draw_line_any(4, 0, 4, top_y - 2, 1);
    draw_line_any(9, 0, 9, top_y - 2, 1);
    outline_rect_any(2, top_y - 2, 11, top_y + 35, 1);

    if (top_y <= icon_y[0] + 2) {
        draw_line_any(4, 10, 7, 7, 1);
        draw_line_any(6, 7, 7, 8, 1);
        any_px(4, 10, 1);
    }

    if (top_y <= icon_y[1] + 2)
        fill_rect_any(2, 17, 6, 20, 1);

    if (top_y <= icon_y[2] + 2) {
        draw_line_any(4, 28, 4, 31, 1);
        draw_line_any(4, 28, 7, 30, 1);
        draw_line_any(7, 30, 5, 30, 1);
        draw_line_any(5, 30, 6, 33, 1);
        draw_line_any(6, 33, 5, 33, 1);
        draw_line_any(5, 33, 4, 31, 1);
    }

    if (top_y <= icon_y[3] + 2) {
        draw_line_any(4, 38, 6, 36, 1);
        draw_line_any(6, 36, 8, 38, 1);
        draw_line_any(8, 38, 6, 40, 1);
        draw_line_any(6, 40, 4, 38, 1);
        any_px(9, 40, 1);
    }
}

static void draw_checker_fill(int x0, int y0, int x1, int y1, int phase) {
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (((x + y + phase) & 1) == 0)
                any_px(x, y, 1);
        }
    }
}

static void resolve_act_time(float sim_t, int *act_idx, float *act_t, float *act_duration) {
    float t = sim_t;

    if (t < 0.0f)
        t = 0.0f;

    for (int i = 0; i < ACT_COUNT; i++) {
        if (t < act_durations[i]) {
            *act_idx = i;
            *act_t = t;
            *act_duration = act_durations[i];
            return;
        }
        t -= act_durations[i];
    }

    *act_idx = ACT_COUNT - 1;
    *act_t = act_durations[ACT_COUNT - 1];
    *act_duration = act_durations[ACT_COUNT - 1];
}

static void default_ui_for_act(int act_idx, float act_t, ui_state_t *ui) {
    ui->tool = TOOL_NONE;
    ui->play_down = 0;
    ui->stop_down = 0;
    ui->trash_hot = 0;
    ui->canvas_damage = 0;
    ui->glitch = 0;

    switch (act_idx) {
        case 0:
            ui->tool = act_t < 8.0f ? TOOL_PENCIL : TOOL_SELECT;
            break;
        case 1:
            ui->tool = TOOL_SELECT;
            break;
        case 2:
            if (act_t < 10.0f) ui->tool = TOOL_SELECT;
            else if (act_t < 14.0f) ui->tool = TOOL_FILL;
            else ui->tool = TOOL_SELECT;
            break;
        case 3:
            if (act_t < 12.0f) ui->tool = TOOL_SHAPE;
            else if (act_t < 21.0f) ui->tool = TOOL_PENCIL;
            else ui->tool = TOOL_ERASE;
            break;
        case 4:
            ui->tool = act_t < 9.0f ? TOOL_PENCIL : TOOL_ERASE;
            if (act_t > 13.5f) ui->canvas_damage = act_t > 16.0f ? 2 : 1;
            break;
        case 5:
        {
            act6_control_state_t ctrl = eval_act6_control(act_t);
            ui->play_down = ctrl.play_down;
            ui->stop_down = ctrl.stop_down;
            ui->glitch = ctrl.glitch;
            if (ui->glitch) ui->canvas_damage = 2;
            break;
        }
        case 6:
            ui->tool = act_t >= 6.0f ? TOOL_PENCIL : TOOL_SELECT;
            ui->trash_hot = act_t > 1.5f && act_t < 6.0f;
            break;
        default:
            break;
    }
}

static void draw_scene_creation(float t, float sim_t) {
    float fig_x = 64.0f;
    float fig_y = 32.0f;
    float scale = 1.0f;
    float angle = 0.0f;
    float cur_x = CANVAS_X - 12.0f;
    float cur_y = CANVAS_Y - 12.0f;
    int cursor_pressed = 0;
    int selection_visible = 0;
    int rotation_handle_visible = 0;
    stick_pose_t pose = pose_stand;
    vec2_t cursor = v2(64.0f, 32.0f);
    (void)sim_t;

    if (t < 1.0f) {
        cursor = v2(-12.0f, -12.0f);
    } else if (t < 2.0f) {
        float u = smoothstep_local(seg_u(t, 1.0f, 2.0f));
        cursor = v2(mixf_local(4.0f, 64.0f, u), mixf_local(4.0f, 18.0f, u));
    } else {
        float head_center_y = 18.0f;
        float shoulder_y = 26.0f;
        float hip_x = 64.0f;
        cursor = v2(64.0f, 18.0f);

        if (t < 6.0f) {
            if (t < 2.5f) {
                cursor = v2(64.0f, 18.0f);
            } else {
                if (t < 3.333f) {
                    float u = seg_u(t, 2.5f, 3.333f);
                    draw_partial_circle_any((int)hip_x, (int)head_center_y, 4, u, 1);
                    cursor = v2(hip_x + cosf((-0.4f + u * 2.0f * (float)M_PI)) * 4.0f,
                                head_center_y + sinf((-0.4f + u * 2.0f * (float)M_PI)) * 4.0f);
                } else {
                    outline_circle_any((int)hip_x, (int)head_center_y, 4, 1);
                }

                if (t >= 3.333f) {
                    if (t < 4.0f) {
                        float u = seg_u(t, 3.333f, 4.0f);
                        float y1 = mixf_local(22.0f, 32.0f, u);
                        draw_line_any((int)hip_x, 22, (int)hip_x, (int)lroundf(y1), 1);
                        cursor = v2(hip_x, y1);
                    } else {
                        draw_line_any((int)hip_x, 22, (int)hip_x, 32, 1);
                    }
                }

                if (t >= 4.0f) {
                    if (t < 4.417f) {
                        float u = seg_u(t, 4.0f, 4.417f);
                        float ax = mixf_local(64.0f, 58.0f, u);
                        float ay = mixf_local(shoulder_y, 29.0f, u);
                        draw_line_any(64, (int)shoulder_y, (int)lroundf(ax), (int)lroundf(ay), 1);
                        cursor = v2(ax, ay);
                    } else {
                        draw_line_any(64, (int)shoulder_y, 58, 29, 1);
                        if (t < 4.583f) {
                            float u = seg_u(t, 4.417f, 4.583f);
                            cursor = v2(mixf_local(58.0f, 64.0f, u), mixf_local(29.0f, shoulder_y, u));
                        } else if (t < 5.0f) {
                            float u = seg_u(t, 4.583f, 5.0f);
                            float ax = mixf_local(64.0f, 70.0f, u);
                            float ay = mixf_local(shoulder_y, 29.0f, u);
                            draw_line_any(64, (int)shoulder_y, (int)lroundf(ax), (int)lroundf(ay), 1);
                            cursor = v2(ax, ay);
                        } else {
                            draw_line_any(64, (int)shoulder_y, 70, 29, 1);
                        }
                    }
                }

                if (t >= 5.0f) {
                    if (t < 5.417f) {
                        float u = seg_u(t, 5.0f, 5.417f);
                        float lx = mixf_local(64.0f, 60.0f, u);
                        float ly = mixf_local(32.0f, 39.0f, u);
                        draw_line_any(64, 32, (int)lroundf(lx), (int)lroundf(ly), 1);
                        cursor = v2(lx, ly);
                    } else {
                        draw_line_any(64, 32, 60, 39, 1);
                        if (t < 5.583f) {
                            float u = seg_u(t, 5.417f, 5.583f);
                            cursor = v2(mixf_local(60.0f, 64.0f, u), mixf_local(39.0f, 32.0f, u));
                        } else if (t < 6.0f) {
                            float u = seg_u(t, 5.583f, 6.0f);
                            float rx = mixf_local(64.0f, 68.0f, u);
                            float ry = mixf_local(32.0f, 39.0f, u);
                            draw_line_any(64, 32, (int)lroundf(rx), (int)lroundf(ry), 1);
                            cursor = v2(rx, ry);
                        }
                    }
                }
            }
        }
        if (t >= 8.0f && t < 8.667f) {
            float u = smoothstep_local(seg_u(t, 8.0f, 8.667f));
            cursor = v2(mixf_local(68.0f, 84.0f, u), mixf_local(39.0f, 32.0f, u));
        } else if (t >= 8.667f && t < 9.0f) {
            float u = smoothstep_local(seg_u(t, 8.667f, 9.0f));
            cursor = v2(mixf_local(84.0f, 53.0f, u), mixf_local(32.0f, 10.0f, u));
        }

        if (t >= 9.0f && t < 10.0f) {
            cursor = v2(53.0f, 10.0f);
            selection_visible = 1;
        }

        if (t >= 10.0f && t < 12.0f) {
            float u = smoothstep_local(seg_u(t, 10.0f, 12.0f));
            fig_x = mixf_local(64.0f, 96.0f, u);
            cursor = v2(mixf_local(60.0f, 92.0f, u), 25.0f);
            cursor_pressed = 1;
            selection_visible = 1;
        } else if (t >= 12.0f && t < 13.0f) {
            float u = smoothstep_local(seg_u(t, 12.0f, 13.0f));
            fig_x = mixf_local(96.0f, 64.0f, u);
            cursor = v2(mixf_local(92.0f, 60.0f, u), 25.0f);
            cursor_pressed = 1;
            selection_visible = 1;
        } else if (t >= 13.0f && t < 14.0f) {
            float u = smoothstep_local(seg_u(t, 13.0f, 14.0f));
            cursor = v2(mixf_local(60.0f, 64.0f, u), mixf_local(25.0f, 8.0f, u));
            cursor_pressed = 1;
            selection_visible = 1;
            rotation_handle_visible = 1;
        } else if (t >= 14.0f && t < 16.0f) {
            float u = smoothstep_local(seg_u(t, 14.0f, 16.0f));
            angle = u * 2.0f * (float)M_PI;
            cursor = v2(64.0f, 8.0f);
            cursor_pressed = 1;
            selection_visible = 1;
            rotation_handle_visible = 1;
        } else if (t >= 16.0f && t < 17.0f) {
            float u = smoothstep_local(seg_u(t, 16.0f, 17.0f));
            cursor = v2(mixf_local(64.0f, 108.0f, u), 8.0f);
        } else if (t >= 17.0f) {
            cursor = v2(108.0f, 8.0f);
        }
    }

    cur_x = CANVAS_X + cursor.x;
    cur_y = CANVAS_Y + cursor.y;
    apply_transition_blend(0, t, &fig_x, &fig_y, &pose, &scale, &angle, &cur_x, &cur_y, &cursor_pressed);
    if (t >= 6.0f)
        draw_stick_pose(fig_x, fig_y, &pose, scale, angle);
    if (selection_visible)
        draw_selection_box(&pose, fig_x, fig_y, scale, angle, rotation_handle_visible);
    if (t >= 1.0f)
        draw_cursor(cur_x, cur_y, cursor_pressed);

    remember_scene_state(fig_x, fig_y, &pose, scale, angle, cur_x, cur_y, cursor_pressed);
}

static void draw_scene_awakening(float t, float sim_t) {
    float raw_t = t;
    float fig_x = 64.0f;
    float fig_y = 32.0f;
    float cur_x = CANVAS_X + 108.0f;
    float cur_y = CANVAS_Y + 8.0f;
    stick_pose_t pose = pose_stand;
    stick_pose_t lift_pose = pose_stand;
    float scale = 1.0f;
    float angle = 0.0f;
    float cursor_angle = 0.0f;
    int cursor_pressed = 0;
    continuity_state_t incoming;
    int has_incoming = transition_source_for_act(1, &incoming);
    (void)sim_t;

    lift_pose.neck.x += 0.25f;
    lift_pose.elbows[1] = v2(3.8f, -8.6f);
    lift_pose.hands[1] = v2(9.0f, -11.0f);

    if (t < 0.5f) {
        float u = seg_u(t, 0.0f, 0.5f);
        float intro = smoothstep_local(seg_u(t, 0.0f, 0.35f));
        float head_shift;

        if (u < 0.33f)
            head_shift = mixf_local(0.0f, -3.0f, u / 0.33f);
        else if (u < 0.66f)
            head_shift = mixf_local(-3.0f, 3.0f, (u - 0.33f) / 0.33f);
        else
            head_shift = mixf_local(3.0f, 0.0f, (u - 0.66f) / 0.34f);

        pose = pose_stand;
        pose.neck.x += head_shift * 0.58f;
        if (has_incoming) {
            fig_x = mixf_local(incoming.fig_pos.x, 64.0f, intro);
            fig_y = mixf_local(incoming.fig_pos.y, 32.0f, intro);
            pose = pose_mix(incoming.pose, pose, intro);
            cur_x = mixf_local(incoming.cursor_pos.x, CANVAS_X + 108.0f, intro);
            cur_y = mixf_local(incoming.cursor_pos.y, CANVAS_Y + 8.0f, intro);
        } else {
            cur_x = CANVAS_X + 108.0f;
            cur_y = CANVAS_Y + 8.0f;
        }
    } else if (t < 2.0f) {
        pose = pose_stand;
        cur_x = CANVAS_X + 108.0f;
        cur_y = CANVAS_Y + 8.0f;
    } else if (t < 3.0f) {
        float u = seg_u(t, 2.0f, 3.0f);
        float lift_u = smoothstep_local(u);
        vec2_t cursor_local;

        if (u < 0.22f) {
            float su = smoothstep_local(u / 0.22f);
            cursor_local = v2_lerp(v2(108.0f, 8.0f), v2(98.0f, 12.0f), su);
        } else if (u < 0.38f) {
            float su = smoothstep_local((u - 0.22f) / 0.16f);
            cursor_local = v2_lerp(v2(98.0f, 12.0f), v2(100.0f, 11.0f), su);
            cursor_local.y += sinf(su * (float)M_PI) * 0.8f;
        } else if (u < 0.72f) {
            float su = smoothstep_local((u - 0.38f) / 0.34f);
            cursor_local = v2_lerp(v2(100.0f, 11.0f), v2(79.0f, 20.0f), su);
        } else if (u < 0.86f) {
            float su = smoothstep_local((u - 0.72f) / 0.14f);
            cursor_local = v2_lerp(v2(79.0f, 20.0f), v2(82.0f, 21.0f), su);
            cursor_local.y += sinf(su * (float)M_PI) * 0.7f;
        } else {
            float su = smoothstep_local((u - 0.86f) / 0.14f);
            cursor_local = v2_lerp(v2(82.0f, 21.0f), v2(72.0f, 22.0f), su);
        }

        pose = pose_mix(pose_stand, lift_pose, lift_u);
        cursor_angle = 0.06f * sinf(u * 8.0f * (float)M_PI) * (1.0f - 0.25f * lift_u);
        cur_x = CANVAS_X + cursor_local.x;
        cur_y = CANVAS_Y + cursor_local.y;
    } else if (t < 3.333f) {
        pose = lift_pose;
        cur_x = CANVAS_X + 72.0f;
        cur_y = CANVAS_Y + 22.0f;
        cursor_pressed = 1;
        cursor_angle = 15.0f * (float)M_PI / 180.0f;
    } else if (t < 4.0f) {
        stick_pose_t grab_pose = lift_pose;

        if (t < 3.417f) {
            float u = seg_u(t, 3.333f, 3.417f);
            fig_x = mixf_local(64.0f, 67.0f, smoothstep_local(u));
            pose = pose_mix(grab_pose, pose_stumble, u * 0.35f);
            cur_x = CANVAS_X + mixf_local(72.0f, 84.0f, u);
            cur_y = CANVAS_Y + 22.0f;
            cursor_pressed = 1;
            cursor_angle = mixf_local(15.0f * (float)M_PI / 180.0f, 0.0f, smoothstep_local(u));
            draw_impact_burst(72.0f, 22.0f, 2, 1);
        } else if (t < 3.667f) {
            float u = smoothstep_local(seg_u(t, 3.417f, 3.667f));
            fig_x = 67.0f;
            pose = pose_mix(grab_pose, pose_stumble, u);
            cur_x = CANVAS_X + 84.0f;
            cur_y = CANVAS_Y + 22.0f;
        } else {
            float u = smoothstep_local(seg_u(t, 3.667f, 4.0f));
            fig_x = mixf_local(67.0f, 64.0f, u);
            pose = pose_mix(pose_stumble, pose_stand, u);
            cur_x = CANVAS_X + 84.0f;
            cur_y = CANVAS_Y + 22.0f;
        }
    } else if (t < 5.0f) {
        fig_x = 64.0f;
        fig_y = 32.0f;
        pose = pose_angry(pose_stand, 0.15f);
        cur_x = CANVAS_X + 84.0f;
        cur_y = CANVAS_Y + 22.0f;
    } else if (t < 6.0f) {
        float u = smoothstep_local(seg_u(t, 5.0f, 6.0f));
        fig_x = mixf_local(64.0f, 72.0f, u);
        fig_y = 32.0f;
        pose = sample_run_cycle(t * 1.0f);
        cur_x = CANVAS_X + 84.0f;
        cur_y = CANVAS_Y + 22.0f;
    } else if (t < 7.0f) {
        float phase = (t - 6.0f) * 3.0f * (float)M_PI;
        float pull = sinf(phase) * 2.0f;
        float cursor_local_x = 82.0f + pull;
        float cursor_local_y = 22.0f;
        float neck_x = pose_pull.neck.x;
        float neck_y = pose_pull.neck.y;
        fig_x = 72.0f - pull * 0.75f;
        fig_y = 32.0f;
        pose = pose_both_hands_target(
            pose_strain(pose_pull, pull >= 0.0f ? 1.0f : -1.0f, 0.55f),
            v2(cursor_local_x - 3.0f - fig_x - neck_x, cursor_local_y - fig_y - neck_y),
            v2(cursor_local_x + 1.0f - fig_x - neck_x, cursor_local_y - fig_y - neck_y),
            0.75f);
        cur_x = CANVAS_X + cursor_local_x;
        cur_y = CANVAS_Y + cursor_local_y;
        cursor_pressed = 1;
    } else if (t < 8.0f) {
        float u = seg_u(t, 7.0f, 8.0f);
        if (u < 0.35f) {
            float ku = smoothstep_local(u / 0.35f);
            fig_x = 72.0f;
            fig_y = mixf_local(32.0f, 26.0f, ku);
            cur_x = CANVAS_X + 80.0f;
            cur_y = CANVAS_Y + mixf_local(22.0f, 8.0f, ku);
            pose = pose_both_hands_target(pose_pull, v2(5.0f, -6.0f), v2(8.0f, -8.0f), 0.8f);
            cursor_pressed = 1;
        } else {
            float du = smoothstep_local(seg_u(u, 0.35f, 1.0f));
            fig_x = 72.0f;
            fig_y = mixf_local(26.0f, 32.0f, du) + bounce_offset(du, 0.75f, 1.0f, 2.0f);
            cur_x = CANVAS_X + 80.0f;
            cur_y = CANVAS_Y + 8.0f;
            pose = pose_mix(pose_stumble, pose_stand, du);
        }
    } else if (t < 10.0f) {
        float u = smoothstep_local(seg_u(t, 8.0f, 10.0f));
        fig_x = 72.0f;
        fig_y = 32.0f;
        pose = pose_stand;
        pose.neck.x -= 0.5f;
        cur_x = CANVAS_X + mixf_local(80.0f, 90.0f, u);
        cur_y = CANVAS_Y + mixf_local(8.0f, 28.0f, u);
    } else if (t < 12.0f) {
        float phase = (t - 10.0f) * 3.0f * (float)M_PI;
        float pull = sinf(phase) * 4.0f;
        float cursor_local_x = 86.0f + pull;
        float cursor_local_y = 28.0f;
        float neck_x = pose_pull.neck.x;
        float neck_y = pose_pull.neck.y;
        fig_x = 76.0f - pull * 0.6f;
        fig_y = 32.0f;
        pose = pose_both_hands_target(
            pose_strain(pose_pull, pull >= 0.0f ? 1.0f : -1.0f, 0.8f),
            v2(cursor_local_x - 3.0f - fig_x - neck_x, cursor_local_y - fig_y - neck_y),
            v2(cursor_local_x + 1.0f - fig_x - neck_x, cursor_local_y - fig_y - neck_y),
            0.85f);
        draw_strain_lines(fig_x + 1.0f, fig_y - 8.0f, 0.7f);
        cur_x = CANVAS_X + cursor_local_x;
        cur_y = CANVAS_Y + cursor_local_y;
        cursor_pressed = 1;
    } else {
        if (t < 12.333f) {
            float u = smoothstep_local(seg_u(t, 12.0f, 12.333f));
            fig_x = mixf_local(76.0f, 88.0f, u);
            fig_y = 32.0f;
            pose = pose_stumble;
            cur_x = CANVAS_X + mixf_local(88.0f, 104.0f, u);
            cur_y = CANVAS_Y + 28.0f;
        } else if (t < 12.667f) {
            float u = smoothstep_local(seg_u(t, 12.333f, 12.667f));
            fig_x = mixf_local(88.0f, 30.0f, u);
            fig_y = 32.0f;
            pose = pose_stumble;
            cur_x = CANVAS_X + mixf_local(104.0f, 90.0f, u);
            cur_y = CANVAS_Y + 28.0f;
        } else {
            fig_x = 30.0f;
            fig_y = 32.0f;
            pose = pose_stumble;
            cur_x = CANVAS_X + 90.0f;
            cur_y = CANVAS_Y + 28.0f;
            draw_skid_marks(fig_x + 2.0f, fig_y + 7.0f, seg_u(t, 12.667f, 14.0f));
        }
        cursor_pressed = 1;
    }

    apply_transition_blend(1, raw_t, &fig_x, &fig_y, &pose, &scale, &angle, &cur_x, &cur_y, &cursor_pressed);
    draw_stick_pose(fig_x, fig_y, &pose, scale, angle);
    draw_cursor_oriented(cur_x, cur_y, cursor_pressed, cursor_angle);
    if (flash_window(raw_t, 3.34f, 0.04f) || flash_window(raw_t, 7.05f, 0.05f))
        draw_canvas_flash();
    remember_scene_state(fig_x, fig_y, &pose, scale, angle, cur_x, cur_y, cursor_pressed);
}

static void draw_scene_escalation(float t, float sim_t) {
    float raw_t = t;
    float fig_x = 30.0f;
    float fig_y = 32.0f;
    float cur_x = CANVAS_X + 90.0f;
    float cur_y = CANVAS_Y + 28.0f;
    float scale = 1.0f;
    float angle = 0.0f;
    int cursor_pressed = 0;
    int bucket_visible = 0;
    stick_pose_t pose = pose_stand;
    continuity_state_t incoming;
    int has_incoming = transition_source_for_act(2, &incoming);
    (void)sim_t;

    if (t < 2.0f) {
        float u = smoothstep_local(seg_u(t, 0.0f, 2.0f));
        float intro = smoothstep_local(seg_u(t, 0.0f, 0.8f));
        fig_x = has_incoming ? mixf_local(incoming.fig_pos.x, 30.0f, intro) : 30.0f;
        fig_y = has_incoming ? mixf_local(incoming.fig_pos.y, 32.0f, intro) : 32.0f;
        pose = has_incoming ? pose_mix(incoming.pose, pose_stand, intro) : pose_stand;
        pose.neck.x -= 0.45f;
        cur_x = has_incoming ? mixf_local(incoming.cursor_pos.x, CANVAS_X + 70.0f, intro) : CANVAS_X + mixf_local(90.0f, 70.0f, u);
        cur_y = has_incoming ? mixf_local(incoming.cursor_pos.y, CANVAS_Y + 22.0f, intro) : CANVAS_Y + mixf_local(28.0f, 22.0f, u);
    } else if (t < 3.0f) {
        float u = smoothstep_local(seg_u(t, 2.0f, 3.0f));
        float trace_u = seg_u(u, 0.18f, 1.0f);
        draw_rect_progress_any(18, 20, 44, 42, trace_u, 1);
        fig_x = 30.0f;
        fig_y = 32.0f;
        pose = pose_stand;
        pose.neck.x -= 0.45f;
        cursor_pressed = 1;
        if (u < 0.18f) {
            float cu = smoothstep_local(u / 0.18f);
            cur_x = CANVAS_X + mixf_local(70.0f, 18.0f, cu);
            cur_y = CANVAS_Y + mixf_local(22.0f, 20.0f, cu);
        } else {
            vec2_t cursor = rect_progress_point(18, 20, 44, 42, trace_u);
            cur_x = CANVAS_X + cursor.x;
            cur_y = CANVAS_Y + cursor.y;
        }
    } else if (t < 5.0f) {
        float u = seg_u(t, 3.0f, 5.0f);
        float box_u = smoothstep_local(u);
        float box_x0 = mixf_local(18.0f, 92.0f, box_u);
        float box_x1 = mixf_local(44.0f, 118.0f, box_u);

        if (t < 3.333f) {
            float exit_u = smoothstep_local(seg_u(t, 3.0f, 3.333f));
            fig_x = mixf_local(30.0f, 18.0f, exit_u);
            pose = sample_walk_cycle(t * 1.05f);
        } else {
            fig_x = 18.0f;
            pose = pose_stand;
            pose.neck.x -= 0.45f;
        }
        fig_y = 32.0f;
        draw_marquee((int)lroundf(box_x0), 20, (int)lroundf(box_x1), 42);
        cur_x = CANVAS_X + box_x1;
        cur_y = CANVAS_Y + 20.0f;
        cursor_pressed = 1;
    } else if (t < 5.667f) {
        float u = seg_u(t, 5.0f, 5.667f);
        fig_x = mixf_local(18.0f, 40.0f, u);
        fig_y = mixf_local(32.0f, 36.0f, u);
        pose = sample_run_cycle(t * 1.2f);
        if (u < 0.45f) {
            float cu = smoothstep_local(u / 0.45f);
            cur_x = CANVAS_X + mixf_local(118.0f, 18.0f, cu);
            cur_y = CANVAS_Y + mixf_local(20.0f, 32.0f, cu);
        } else {
            float cu = smoothstep_local((u - 0.45f) / 0.55f);
            cur_x = CANVAS_X + mixf_local(18.0f, 40.0f, cu);
            cur_y = CANVAS_Y + mixf_local(32.0f, 22.0f, cu);
        }
    } else if (t < 6.0f) {
        float u = seg_u(t, 5.667f, 6.0f);
        float jump_u;
        stick_pose_t crouch_takeoff = pose_mix(pose_stand, pose_land_crouch, 0.82f);
        stick_pose_t reach_takeoff = pose_both_hands_target(pose_mix(pose_land_crouch, pose_hang, 0.55f),
                                                            v2(-1.4f, -13.0f), v2(1.4f, -13.0f), 0.28f);

        fig_x = 40.0f;
        if (u < 0.22f) {
            float cu = smoothstep_local(u / 0.22f);
            fig_y = mixf_local(36.0f, 37.5f, cu);
            pose = pose_mix(pose_stand, crouch_takeoff, cu);
        } else {
            jump_u = smoothstep_local((u - 0.22f) / 0.78f);
            fig_y = mixf_local(37.5f, 22.0f, jump_u);
            pose = pose_mix(crouch_takeoff, reach_takeoff, jump_u);
        }
        draw_marquee(28, 20, 52, 44);
        fill_rect_any(39, 21, 41, 23, 1);
        cur_x = CANVAS_X + 40.0f;
        cur_y = CANVAS_Y + 22.0f;
    } else if (t < 8.0f) {
        float u = seg_u(t, 6.0f, 8.0f);
        vec2_t handle = v2(40.0f, 22.0f);
        vec2_t hip;

        pose = sample_gymnast_swing_pose(u);
        angle = -(float)M_PI * u;
        hip = locked_hands_hip(&pose, handle, angle);
        fig_x = hip.x;
        fig_y = hip.y;
        draw_marquee(28, 20, 52, 44);
        fill_rect_any(39, 21, 41, 23, 1);
        cur_x = CANVAS_X + 40.0f;
        cur_y = CANVAS_Y + 22.0f;
    } else if (t < 10.0f) {
        float u = seg_u(t, 8.0f, 10.0f);
        vec2_t handle = v2(40.0f, 22.0f);
        stick_pose_t swing_end_pose = sample_gymnast_swing_pose(1.0f);
        stick_pose_t quarter_turn_pose = pose_mix(pose_gymnast_invert, pose_gymnast_hollow, 0.68f);
        vec2_t swing_end_hip = locked_hands_hip(&swing_end_pose, handle, -(float)M_PI);
        vec2_t quarter_turn_hip = locked_hands_hip(&quarter_turn_pose, handle, -1.5f * (float)M_PI);

        if (u < 0.2f) {
            float du = smoothstep_local(u / 0.2f);
            fig_x = mixf_local(swing_end_hip.x, quarter_turn_hip.x, du);
            fig_y = mixf_local(swing_end_hip.y, quarter_turn_hip.y, du);
            pose = pose_mix(swing_end_pose, quarter_turn_pose, du);
            angle = mixf_local(-(float)M_PI, -1.5f * (float)M_PI, du);
        } else if (u < 0.46f) {
            float lu = smoothstep_local((u - 0.2f) / 0.26f);
            fig_x = mixf_local(quarter_turn_hip.x, 40.0f, lu);
            fig_y = mixf_local(quarter_turn_hip.y, 42.0f, lu);
            pose = pose_mix(quarter_turn_pose, pose_land_crouch, lu);
            angle = mixf_local(-1.5f * (float)M_PI, -2.0f * (float)M_PI, lu);
        } else if (u < 0.58f) {
            float lu = smoothstep_local((u - 0.46f) / 0.12f);
            fig_x = 40.0f;
            fig_y = mixf_local(42.0f, 45.0f, lu);
            pose = pose_land_crouch;
            angle = -2.0f * (float)M_PI;
        } else if (u < 0.74f) {
            float lu = smoothstep_local((u - 0.58f) / 0.16f);
            fig_x = 40.0f;
            fig_y = mixf_local(45.0f, 39.0f, lu);
            pose = pose_mix(pose_land_crouch, pose_land_balance, lu);
            angle = -2.0f * (float)M_PI;
        } else {
            float bu = smoothstep_local((u - 0.74f) / 0.26f);
            fig_x = 40.0f;
            fig_y = 39.0f;
            pose = pose_mix(pose_land_balance, pose_stand, bu);
            pose.hands[0].x -= (1.0f - bu) * 0.8f;
            pose.hands[1].x += (1.0f - bu) * 1.0f;
            angle = -2.0f * (float)M_PI;
        }
        cur_x = CANVAS_X + 40.0f;
        cur_y = CANVAS_Y + mixf_local(22.0f, 39.0f, smoothstep_local(seg_u(u, 0.34f, 1.0f)));
    } else if (t < 11.0f) {
        float u = seg_u(t, 10.0f, 11.0f);
        fig_x = 40.0f;
        fig_y = 39.0f;
        pose = pose_stand;
        bucket_visible = 1;
        if (u < 0.45f) {
            float cu = smoothstep_local(u / 0.45f);
            cur_x = CANVAS_X + mixf_local(40.0f, 6.0f, cu);
            cur_y = CANVAS_Y + mixf_local(12.0f, 36.0f, cu);
        } else {
            float cu = smoothstep_local((u - 0.45f) / 0.55f);
            cur_x = CANVAS_X + mixf_local(6.0f, 44.0f, cu);
            cur_y = CANVAS_Y + mixf_local(36.0f, 39.0f, cu);
        }
    } else if (t < 11.167f) {
        fig_x = 40.0f;
        fig_y = 39.0f;
        pose = pose_stand;
        cur_x = CANVAS_X + 44.0f;
        cur_y = CANVAS_Y + 39.0f;
        cursor_pressed = 1;
        bucket_visible = 1;
        draw_checker_fill(34, 24, 46, 41, (int)(t * 24.0f));
    } else if (t < 12.0f) {
        float u = seg_u(t, 11.167f, 12.0f);
        fig_y = 39.0f;
        cur_x = CANVAS_X + 44.0f;
        cur_y = CANVAS_Y + 39.0f;
        if (u < 0.55f) {
            float shake = (((int)floorf((t - 11.167f) * 12.0f)) & 1) == 0 ? -3.0f : 3.0f;
            fig_x = 40.0f + shake;
            pose = pose_angry(pose_stand, 0.28f);
        } else {
            fig_x = 40.0f;
            pose = pose_angry(pose_stand, 0.35f);
        }
    } else if (t < 13.0f) {
        fig_x = 40.0f;
        fig_y = 39.0f;
        pose = pose_angry(pose_stand, 0.45f);
        cur_x = CANVAS_X + 44.0f;
        cur_y = CANVAS_Y + 39.0f;
    } else if (t < 13.333f) {
        float u = seg_u(t, 13.0f, 13.333f);
        fig_x = 40.0f;
        fig_y = 39.0f;
        cur_x = CANVAS_X + 52.0f;
        cur_y = CANVAS_Y + 39.0f;
        if (u < 0.25f) {
            pose = pose_angry(pose_punch, 0.55f);
            draw_impact_burst(52.0f, 33.0f, 2, 1);
        } else {
            float ru = smoothstep_local((u - 0.25f) / 0.75f);
            pose = pose_mix(pose_angry(pose_punch, 0.55f), pose_angry(pose_stand, 0.45f), ru);
        }
    } else if (t < 17.0f) {
        float u = seg_u(t, 13.333f, 17.0f);
        if (u < 0.125f) {
            float chase_u = smoothstep_local(u / 0.125f);
            fig_x = mixf_local(40.0f, 48.0f, chase_u);
            fig_y = 39.0f;
            cur_x = CANVAS_X + mixf_local(52.0f, 68.0f, chase_u);
            cur_y = CANVAS_Y + 39.0f;
        } else {
            float chase_u = smoothstep_local((u - 0.125f) / 0.875f);
            fig_x = mixf_local(48.0f, 80.0f, chase_u);
            fig_y = mixf_local(39.0f, 32.0f, clampf_local(chase_u * 1.15f, 0.0f, 1.0f));
            cur_x = CANVAS_X + mixf_local(68.0f, 112.0f, chase_u);
            cur_y = CANVAS_Y + mixf_local(39.0f, 32.0f, clampf_local(chase_u * 1.2f, 0.0f, 1.0f));
        }
        pose = sample_run_cycle(t * 1.15f);
    } else if (t < 18.667f) {
        float u = seg_u(t, 17.0f, 18.667f);
        float arc = sinf(u * (float)M_PI);
        fig_x = 80.0f;
        fig_y = 32.0f;
        pose = sample_run_cycle(0.35f + u * 0.35f);
        cur_x = CANVAS_X + mixf_local(112.0f, 75.0f, u) - arc * 13.5f;
        cur_y = CANVAS_Y + 32.0f - arc * 14.0f;
    } else if (t < 19.5f) {
        float u = smoothstep_local(seg_u(t, 18.667f, 19.5f));
        fig_x = mixf_local(80.0f, 104.0f, u);
        fig_y = mixf_local(32.0f, 22.0f, u) - sinf(u * (float)M_PI) * 6.0f;
        pose = pose_stumble;
        cur_x = CANVAS_X + mixf_local(75.0f, 104.0f, u);
        cur_y = CANVAS_Y + mixf_local(32.0f, 22.0f, u) - 4.0f;
        cursor_pressed = 1;
    } else {
        fig_x = 104.0f;
        cur_x = CANVAS_X + 108.0f;
        cur_y = CANVAS_Y + 12.0f;
        if (t < 20.0f) {
            float u = smoothstep_local(seg_u(t, 19.5f, 20.0f));
            fig_y = mixf_local(22.0f, 42.0f, u);
            pose = pose_stumble;
        } else if (t < 20.333f) {
            float u = smoothstep_local(seg_u(t, 20.0f, 20.333f));
            fig_y = mixf_local(42.0f, 45.0f, u);
            pose = pose_stumble;
        } else if (t < 20.667f) {
            float u = smoothstep_local(seg_u(t, 20.333f, 20.667f));
            fig_y = mixf_local(45.0f, 39.0f, u);
            pose = pose_stumble;
        } else if (t < 21.5f) {
            float u = smoothstep_local(seg_u(t, 20.667f, 21.5f));
            fig_y = mixf_local(39.0f, 42.0f, u);
            pose = pose_mix(pose_stumble, pose_stand, u);
        } else {
            fig_y = 42.0f;
            pose = pose_stand;
        }
    }

    apply_transition_blend(2, raw_t, &fig_x, &fig_y, &pose, &scale, &angle, &cur_x, &cur_y, &cursor_pressed);
    draw_stick_pose(fig_x, fig_y, &pose, scale, angle);
    if (bucket_visible)
        draw_bucket_icon(cur_x, cur_y);
    if (t >= 18.667f && t < 19.5f)
        draw_selection_box(&pose, fig_x, fig_y, scale, angle, 0);
    draw_cursor(cur_x, cur_y, cursor_pressed);
    if (flash_window(raw_t, 11.0f, 0.05f) || flash_window(raw_t, 18.7f, 0.05f))
        draw_canvas_flash();
    remember_scene_state(fig_x, fig_y, &pose, scale, angle, cur_x, cur_y, cursor_pressed);
}

static void draw_scene_weapons(float t, float sim_t) {
    float raw_t = t;
    float fig_x = 104.0f;
    float fig_y = 42.0f;
    float cur_x = CANVAS_X + 75.0f;
    float cur_y = CANVAS_Y + 32.0f;
    float scale = 1.0f;
    float angle = 0.0f;
    int cursor_pressed = 1;
    int show_large_eraser = 0;
    float eraser_icon_x = 0.0f;
    float eraser_icon_y = 0.0f;
    float confetti_u = -1.0f;
    stick_pose_t pose = pose_stand;
    continuity_state_t incoming;
    int has_incoming = transition_source_for_act(3, &incoming);
    (void)sim_t;

    if (t < 0.833f) {
        float u = smoothstep_local(seg_u(t, 0.0f, 0.833f));
        fig_x = has_incoming ? mixf_local(incoming.fig_pos.x, 104.0f, u) : 104.0f;
        fig_y = has_incoming ? mixf_local(incoming.fig_pos.y, 42.0f, u) : 42.0f;
        pose = has_incoming ? pose_mix(incoming.pose, pose_stand, u) : pose_stand;
        cur_x = has_incoming ? mixf_local(incoming.cursor_pos.x, 8.0f, u) : mixf_local(CANVAS_X + 75.0f, 8.0f, u);
        cur_y = has_incoming ? mixf_local(incoming.cursor_pos.y, 6.0f, u) : mixf_local(CANVAS_Y + 32.0f, 6.0f, u);
    } else if (t < 3.0f) {
        fig_x = 104.0f;
        fig_y = 42.0f;
        pose = pose_stand;
        if (t < 1.333f) {
            float travel_u = smoothstep_local(seg_u(t, 0.833f, 1.333f));
            cur_x = mixf_local(8.0f, CANVAS_X + 86.0f, travel_u);
            cur_y = mixf_local(6.0f, CANVAS_Y + 14.0f, travel_u);
        } else {
            float u = smoothstep_local(seg_u(t, 1.333f, 3.0f));
            draw_rect_progress_any(86, 14, 118, 46, u, 1);
            if (u < 0.25f) {
                cur_x = CANVAS_X + mixf_local(86.0f, 118.0f, u / 0.25f);
                cur_y = CANVAS_Y + 14.0f;
            } else if (u < 0.5f) {
                cur_x = CANVAS_X + 118.0f;
                cur_y = CANVAS_Y + mixf_local(14.0f, 46.0f, (u - 0.25f) / 0.25f);
            } else if (u < 0.75f) {
                cur_x = CANVAS_X + mixf_local(118.0f, 86.0f, (u - 0.5f) / 0.25f);
                cur_y = CANVAS_Y + 46.0f;
            } else {
                cur_x = CANVAS_X + 86.0f;
                cur_y = CANVAS_Y + mixf_local(46.0f, 14.0f, (u - 0.75f) / 0.25f);
            }
        }
    } else if (t < 4.0f) {
        float u = smoothstep_local(seg_u(t, 3.0f, 4.0f));
        fig_x = mixf_local(104.0f, 101.0f, clampf_local(u * 1.2f, 0.0f, 1.0f));
        fig_y = 42.0f;
        pose = pose_angry(pose_stand, 0.35f);
        draw_rect_progress_any(86, 14, 118, 46, 1.0f, 1);
        cur_x = CANVAS_X + 92.0f;
        cur_y = CANVAS_Y + 20.0f;
    } else if (t < 5.0f) {
        float u = smoothstep_local(seg_u(t, 4.0f, 5.0f));
        fig_x = mixf_local(101.0f, 110.0f, clampf_local(u * 1.5f, 0.0f, 1.0f));
        fig_y = 42.0f;
        pose = sample_walk_cycle(t * 0.9f);
        draw_rect_progress_any(86, 14, 118, 46, 1.0f, 1);
        if (u > 0.55f) {
            pose = pose_punch;
            clear_rect_any(117, 28, 119, 44, 1);
            draw_impact_burst(118.0f, 36.0f, 2, 1);
        }
        cur_x = CANVAS_X + 92.0f;
        cur_y = CANVAS_Y + 20.0f;
    } else if (t < 6.0f) {
        float u = smoothstep_local(seg_u(t, 5.0f, 6.0f));
        fig_x = mixf_local(110.0f, 122.0f, u);
        fig_y = 42.0f;
        pose = sample_walk_cycle(t * 0.9f);
        draw_rect_progress_any(86, 14, 118, 46, 1.0f, 1);
        clear_rect_any(117, 28, 119, 44, 1);
        cur_x = CANVAS_X + 118.0f;
        cur_y = CANVAS_Y + 14.0f;
    } else if (t < 8.0f) {
        float u = smoothstep_local(seg_u(t, 6.0f, 8.0f));
        fig_x = mixf_local(122.0f, 90.0f, u);
        fig_y = mixf_local(42.0f, 32.0f, u);
        pose = sample_run_cycle(t * 1.15f);
        draw_partial_circle_any(50, 32, 18, u, 1);
        cur_x = CANVAS_X + (50.0f + cosf((-0.4f + u * 2.0f * (float)M_PI)) * 18.0f);
        cur_y = CANVAS_Y + (32.0f + sinf((-0.4f + u * 2.0f * (float)M_PI)) * 18.0f);
    } else if (t < 10.0f) {
        float u = smoothstep_local(seg_u(t, 8.0f, 10.0f));
        fig_x = 90.0f;
        fig_y = 32.0f;
        pose = pose_stand;
        outline_circle_any((int)lroundf(mixf_local(50.0f, 90.0f, u)), 32, 18, 1);
        cur_x = CANVAS_X + mixf_local(50.0f, 90.0f, u);
        cur_y = CANVAS_Y + 14.0f;
    } else if (t < 11.0f) {
        float u = seg_u(t, 10.0f, 11.0f);
        fig_y = 32.0f;
        outline_circle_any(90, 32, 18, 1);
        clear_rect_any(105, 24, 110, 40, 1);
        if (u < (2.0f / 12.0f)) {
            fig_x = 90.0f;
            pose = pose_punch;
            draw_impact_burst(108.0f, 32.0f, 3, 1);
        } else {
            float su = smoothstep_local(seg_u(u, 2.0f / 12.0f, 1.0f));
            fig_x = mixf_local(90.0f, 112.0f, su);
            pose = sample_walk_cycle(0.2f + su * 0.5f);
        }
        cur_x = CANVAS_X + 90.0f;
        cur_y = CANVAS_Y + 14.0f;
    } else if (t < 12.0f) {
        float u = smoothstep_local(seg_u(t, 11.0f, 12.0f));
        fig_x = mixf_local(112.0f, 64.0f, u);
        fig_y = 32.0f;
        pose = sample_run_cycle(t * 1.2f);
        outline_circle_any(90, 32, 18, 1);
        clear_rect_any(105, 24, 110, 40, 1);
        cur_x = CANVAS_X + mixf_local(90.0f, 0.0f, u);
        cur_y = CANVAS_Y + mixf_local(14.0f, 45.0f, u);
    } else if (t < 13.0f) {
        float u = seg_u(t, 12.0f, 13.0f);
        float draw_u = u < 0.5f ? smoothstep_local(u / 0.5f) : 1.0f;
        fig_x = 64.0f;
        fig_y = 32.0f;
        pose = pose_stand;
        draw_thick_line_any(0, 45, (int)lroundf(mixf_local(0.0f, (float)(CANVAS_W - 1), draw_u)), 45, 5, 1);
        cur_x = CANVAS_X + mixf_local(0.0f, (float)(CANVAS_W - 1), draw_u);
        cur_y = CANVAS_Y + 45.0f;
    } else if (t < 15.0f) {
        float u = seg_u(t, 13.0f, 15.0f);
        draw_thick_line_any(0, 45, CANVAS_W - 1, 45, 5, 1);
        if (u < 0.22f) {
            float ru = smoothstep_local(u / 0.22f);
            fig_x = mixf_local(64.0f, 124.0f, ru);
            fig_y = 32.0f;
            pose = sample_run_cycle(t * 1.3f);
        } else if (u < 0.28f) {
            float bu = smoothstep_local((u - 0.22f) / 0.06f);
            fig_x = mixf_local(124.0f, 120.0f, bu);
            fig_y = 32.0f + bounce_offset(bu, 0.0f, 1.0f, 2.0f);
            pose = pose_stumble;
        } else if (u < 0.52f) {
            float lu = smoothstep_local((u - 0.28f) / 0.24f);
            fig_x = mixf_local(120.0f, 4.0f, lu);
            fig_y = 32.0f;
            pose = sample_run_cycle(t * 1.3f + 0.5f);
        } else if (u < 0.58f) {
            float bu = smoothstep_local((u - 0.52f) / 0.06f);
            fig_x = mixf_local(4.0f, 8.0f, bu);
            fig_y = 32.0f + bounce_offset(bu, 0.0f, 1.0f, 2.0f);
            pose = pose_stumble;
        } else if (u < 0.72f) {
            float ru = smoothstep_local((u - 0.58f) / 0.14f);
            fig_x = mixf_local(8.0f, 64.0f, ru);
            fig_y = 32.0f;
            pose = sample_run_cycle(t * 1.3f + 0.2f);
        } else {
            float ju = seg_u(u, 0.72f, 1.0f);
            fig_x = 64.0f;
            if (ju < 0.4f) {
                float au = smoothstep_local(ju / 0.4f);
                fig_y = mixf_local(32.0f, 18.0f, au);
                pose = pose_mix(sample_run_cycle(0.2f), pose_stumble, au * 0.6f);
            } else if (ju < 0.82f) {
                float du = smoothstep_local((ju - 0.4f) / 0.42f);
                fig_y = mixf_local(18.0f, 38.0f, du);
                pose = pose_mix(pose_stumble, pose_land_balance, du * 0.6f);
            } else {
                float lu = seg_u(ju, 0.82f, 1.0f);
                fig_y = 38.0f + bounce_offset(lu, 0.0f, 1.0f, 3.0f);
                pose = pose_mix(pose_land_crouch, pose_land_balance, smoothstep_local(lu));
            }
        }
        cur_x = CANVAS_X + (CANVAS_W - 1);
        cur_y = CANVAS_Y + 45.0f;
    } else if (t < 17.0f) {
        float u = seg_u(t, 15.0f, 17.0f);
        float cursor_u = smoothstep_local(u);
        float reach_u = smoothstep_local(seg_u(u, 0.0f, 0.18f));
        float lift_u = smoothstep_local(seg_u(u, 0.18f, 0.42f));
        float wind_u = smoothstep_local(seg_u(u, 0.42f, 1.0f));
        float staff_center_y = mixf_local(45.0f, 43.0f, lift_u);
        float staff_yaw = mixf_local(0.859f, 1.02f, wind_u);
        float staff_roll = mixf_local(0.0f, -0.11f, wind_u);
        stick_pose_t base_pose = pose_mix(pose_land_balance, pose_staff, 0.18f + wind_u * 0.35f);
        projected_staff_t staff = project_staff_shadow(v2(64.0f, staff_center_y), 23.0f,
                                                       staff_yaw, staff_roll, 0.54f, 0.45f);
        float neck_x;
        float neck_y;

        fig_x = 64.0f;
        fig_y = mixf_local(38.2f, 39.1f, reach_u * (1.0f - lift_u * 0.7f));
        draw_thick_line_any(0, 45, CANVAS_W - 1, 45, 5, 1);
        if (u >= 0.18f)
            clear_rect_any(49, 43, 79, 47, 1);
        if (u >= 0.18f)
            draw_projected_staff(&staff, 5);

        neck_x = base_pose.neck.x;
        neck_y = base_pose.neck.y;
        if (u < 0.18f) {
            pose = pose_both_hands_target(
                pose_mix(pose_land_balance, pose_stand, reach_u * 0.18f),
                v2(54.0f - fig_x - neck_x, 45.0f - fig_y - neck_y),
                v2(74.0f - fig_x - neck_x, 45.0f - fig_y - neck_y),
                1.0f);
        } else {
            pose = pose_both_hands_target(
                base_pose,
                v2(staff.left_hand.x - fig_x - neck_x, staff.left_hand.y - fig_y - neck_y),
                v2(staff.right_hand.x - fig_x - neck_x, staff.right_hand.y - fig_y - neck_y),
                0.78f);
        }

        cur_x = CANVAS_X + mixf_local((float)(CANVAS_W - 1), 86.0f, cursor_u);
        cur_y = CANVAS_Y + mixf_local(45.0f, 36.0f, cursor_u);
    } else if (t < 19.0f) {
        float u = smoothstep_local(seg_u(t, 17.0f, 19.0f));
        float yaw = u < 0.52f
            ? mixf_local(1.02f, 1.18f, smoothstep_local(u / 0.52f))
            : mixf_local(1.18f, 0.30f, smoothstep_local((u - 0.52f) / 0.48f));
        float roll = mixf_local(-0.11f, -0.33f, u);
        projected_staff_t staff = project_staff_shadow(v2(64.0f, 43.0f), 23.0f,
                                                       yaw, roll, 0.55f, 0.35f);
        stick_pose_t base_pose = pose_mix(pose_staff, pose_staff_swing, 0.25f + u * 0.65f);
        float neck_x;
        float neck_y;

        fig_x = 64.0f;
        fig_y = 38.0f;
        draw_thick_line_any(0, 45, CANVAS_W - 1, 45, 5, 1);
        clear_rect_any(49, 43, 79, 47, 1);
        draw_projected_staff(&staff, 5);

        neck_x = base_pose.neck.x;
        neck_y = base_pose.neck.y;
        pose = pose_both_hands_target(
            base_pose,
            v2(staff.left_hand.x - fig_x - neck_x, staff.left_hand.y - fig_y - neck_y),
            v2(staff.right_hand.x - fig_x - neck_x, staff.right_hand.y - fig_y - neck_y),
            0.68f);
        cur_x = CANVAS_X + 86.0f;
        cur_y = CANVAS_Y + 36.0f;
    } else if (t < 20.0f) {
        float u = smoothstep_local(seg_u(t, 19.0f, 20.0f));
        float yaw = mixf_local(0.30f, 0.45f, u * 0.7f);
        float roll = mixf_local(-0.33f, -0.47f, u);
        projected_staff_t staff = project_staff_shadow(v2(64.0f, 43.0f), 23.0f,
                                                       yaw, roll, 0.55f, 0.28f);
        stick_pose_t base_pose = pose_mix(pose_staff_swing, pose_strain(pose_staff_swing, 1.0f, 0.18f), u * 0.6f);
        float dodge_u = smoothstep_local(seg_u(u, 0.0f, 0.28f));
        float neck_x;
        float neck_y;

        fig_x = 64.0f;
        fig_y = 38.0f;
        draw_thick_line_any(0, 45, CANVAS_W - 1, 45, 5, 1);
        clear_rect_any(49, 43, 79, 47, 1);
        draw_projected_staff(&staff, 5);

        neck_x = base_pose.neck.x;
        neck_y = base_pose.neck.y;
        pose = pose_both_hands_target(
            base_pose,
            v2(staff.left_hand.x - fig_x - neck_x, staff.left_hand.y - fig_y - neck_y),
            v2(staff.right_hand.x - fig_x - neck_x, staff.right_hand.y - fig_y - neck_y),
            0.62f);
        cur_x = CANVAS_X + 86.0f;
        cur_y = CANVAS_Y + (u < 0.28f ? mixf_local(36.0f, 24.0f, dodge_u) : 24.0f);
    } else {
        projected_staff_t aimed_staff = project_staff_shadow(v2(64.0f, 43.0f), 23.0f,
                                                             0.30f, -0.31f, 0.55f, 0.28f);
        stick_pose_t aimed_pose = pose_both_hands_target(
            pose_staff_swing,
            v2(aimed_staff.left_hand.x - 64.0f - pose_staff_swing.neck.x,
               aimed_staff.left_hand.y - 38.0f - pose_staff_swing.neck.y),
            v2(aimed_staff.right_hand.x - 64.0f - pose_staff_swing.neck.x,
               aimed_staff.right_hand.y - 38.0f - pose_staff_swing.neck.y),
            0.62f);

        if (t < 20.833f) {
            float u = smoothstep_local(seg_u(t, 20.0f, 20.833f));
            fig_x = 64.0f;
            fig_y = 38.0f;
            pose = aimed_pose;
            draw_thick_line_any(0, 45, CANVAS_W - 1, 45, 5, 1);
            clear_rect_any(49, 43, 79, 47, 1);
            draw_projected_staff(&aimed_staff, 5);

            eraser_icon_x = mixf_local(CANVAS_X + 86.0f, 6.0f, u);
            eraser_icon_y = mixf_local(CANVAS_Y + 24.0f, 16.0f, u);
            cur_x = eraser_icon_x;
            cur_y = eraser_icon_y;
            show_large_eraser = 1;
            cursor_pressed = 0;
        } else if (t < 21.0f) {
            float u = seg_u(t, 20.833f, 21.0f);
            fig_x = 64.0f;
            fig_y = 38.0f;
            pose = pose_mix(aimed_pose, pose_stumble, smoothstep_local(u) * 0.25f);
            draw_thick_line_any(0, 45, CANVAS_W - 1, 45, 5, 1);
            clear_rect_any(49, 43, 79, 47, 1);
            confetti_u = u;

            eraser_icon_x = 6.0f;
            eraser_icon_y = 16.0f;
            cur_x = eraser_icon_x;
            cur_y = eraser_icon_y;
            show_large_eraser = 1;
            cursor_pressed = 0;
        } else if (t < 21.25f) {
            float u = smoothstep_local(seg_u(t, 21.0f, 21.25f));
            fig_x = mixf_local(64.0f, 68.0f, u);
            fig_y = 38.0f;
            pose = pose_mix(pose_stumble, pose_angry(pose_stand, 0.2f), u * 0.45f);
            cur_x = 6.0f;
            cur_y = 16.0f;
            show_large_eraser = 1;
            cursor_pressed = 0;
        } else if (t < 21.5f) {
            float u = smoothstep_local(seg_u(t, 21.25f, 21.5f));
            fig_x = 68.0f;
            fig_y = mixf_local(38.0f, 45.0f, u);
            pose = pose_mix(pose_stumble, pose_stand, u);
            cur_x = 6.0f;
            cur_y = 16.0f;
            cursor_pressed = 0;
        } else {
            fig_x = 68.0f;
            fig_y = 45.0f;
            pose = pose_stand;
            cur_x = 6.0f;
            cur_y = 16.0f;
            cursor_pressed = 0;
        }
    }

    apply_transition_blend(3, raw_t, &fig_x, &fig_y, &pose, &scale, &angle, &cur_x, &cur_y, &cursor_pressed);
    draw_stick_pose(fig_x, fig_y, &pose, scale, angle);
    if (confetti_u >= 0.0f)
        draw_confetti_pop(64.0f, 43.0f, confetti_u);
    if (show_large_eraser)
        draw_eraser_outline((int)lroundf(cur_x) - 4, (int)lroundf(cur_y) - 4, 7, 7, 0);
    else
        draw_cursor(cur_x, cur_y, cursor_pressed);
    if (flash_window(raw_t, 6.15f, 0.05f) || flash_window(raw_t, 10.05f, 0.05f) || flash_window(raw_t, 20.15f, 0.05f))
        draw_canvas_flash();
    remember_scene_state(fig_x, fig_y, &pose, scale, angle, cur_x, cur_y, cursor_pressed);
}

static void draw_scene_battlefield(float t, float sim_t) {
    float raw_t = t;
    float fig_x = 40.0f;
    float fig_y = 32.0f;
    float cur_x = CANVAS_X + 86.0f;
    float cur_y = CANVAS_Y + 24.0f;
    float scale = 1.0f;
    float angle = 0.0f;
    int cursor_pressed = 1;
    stick_pose_t pose = pose_stand;
    continuity_state_t incoming;
    int has_incoming = transition_source_for_act(4, &incoming);
    float overlay_u = 0.0f;
    float fake_icons_u = -1.0f;
    int show_eraser = 0;
    float eraser_x = 0.0f;
    float eraser_y = 0.0f;
    float eraser_half_w = 3.0f;
    float eraser_half_h = 3.0f;
    int erase_leg_overlap = 0;
    int draw_left_stump = 0;
    int right_border_gap = 0;
    float right_border_repair_u = -1.0f;
    int redraw_first_wall_on_top = 0;
    int hide_top_transport = 0;
    int show_falling_transport = 0;
    float falling_transport_x0 = 0.0f;
    float falling_transport_x1 = 0.0f;
    float falling_transport_y = 0.0f;
    float falling_transport_playhead_u = 0.45f;
    (void)sim_t;

    if (t < 2.0f) {
        float u = smoothstep_local(seg_u(t, 0.0f, 2.0f));
        float intro = smoothstep_local(seg_u(t, 0.0f, 0.35f));
        fig_x = has_incoming ? mixf_local(incoming.fig_pos.x, 40.0f, intro) : mixf_local(68.0f, 40.0f, u);
        fig_y = has_incoming ? mixf_local(incoming.fig_pos.y, 32.0f, intro) : mixf_local(45.0f, 32.0f, u);
        pose = sample_walk_cycle(t * 0.8f);
        cur_x = has_incoming ? mixf_local(incoming.cursor_pos.x, 8.0f, intro) : mixf_local(CANVAS_X + 86.0f, 8.0f, u);
        cur_y = has_incoming ? mixf_local(incoming.cursor_pos.y, 6.0f, intro) : mixf_local(CANVAS_Y + 24.0f, 6.0f, u);
    } else if (t < 3.0f) {
        float u = seg_u(t, 2.0f, 3.0f);
        fig_x = 40.0f;
        fig_y = 32.0f;
        pose = pose_stand;
        if (t < 2.3f) {
            float travel_u = smoothstep_local(seg_u(t, 2.0f, 2.3f));
            cur_x = mixf_local(8.0f, CANVAS_X + 80.0f, travel_u);
            cur_y = mixf_local(6.0f, CANVAS_Y + 0.0f, travel_u);
        } else if (u < 0.6f) {
            float wu = smoothstep_local(seg_u(t, 2.3f, 2.6f));
            draw_line_any(80, 0, 80, (int)lroundf(mixf_local(0.0f, 46.0f, wu)), 1);
            cur_x = CANVAS_X + 80.0f;
            cur_y = CANVAS_Y + mixf_local(0.0f, 46.0f, wu);
        } else {
            float pu = smoothstep_local((u - 0.6f) / 0.4f);
            draw_line_any(80, 0, 80, 46, 1);
            draw_line_any(40, 45, (int)lroundf(mixf_local(40.0f, 70.0f, pu)), 45, 1);
            cur_x = CANVAS_X + mixf_local(40.0f, 70.0f, pu);
            cur_y = CANVAS_Y + 45.0f;
        }
    } else if (t < 4.0f) {
        float u = seg_u(t, 3.0f, 4.0f);
        draw_line_any(80, 0, 80, 46, 1);
        draw_line_any(40, 45, 70, 45, 1);
        if (u < 0.75f) {
            float ru = smoothstep_local(u / 0.75f);
            fig_x = mixf_local(40.0f, 76.0f, ru);
            fig_y = 32.0f;
            pose = sample_run_cycle(t * 1.2f);
        } else {
            float bu = smoothstep_local((u - 0.75f) / 0.25f);
            fig_x = mixf_local(76.0f, 72.0f, bu);
            fig_y = 32.0f;
            pose = pose_stumble;
        }
        cur_x = CANVAS_X + 80.0f;
        cur_y = CANVAS_Y + 26.0f;
    } else if (t < 5.0f) {
        float u = seg_u(t, 4.0f, 5.0f);
        float neck_x;
        float neck_y;
        stick_pose_t base_pose;

        draw_line_any(80, 0, 80, 46, 1);
        draw_line_any(40, 45, 70, 45, 1);
        clear_rect_any(79, 20, 81, 32, 1);
        redraw_first_wall_on_top = 1;
        if (u < 0.16f) {
            fig_x = 72.0f;
            fig_y = 32.0f;
            pose = pose_stumble;
        } else if (u < 0.32f) {
            fig_x = 72.0f;
            fig_y = 32.0f;
            pose = pose_punch;
            draw_impact_burst(80.0f, 26.0f, 2, 1);
        } else if (u < 0.56f) {
            float su = smoothstep_local(seg_u(u, 0.32f, 0.56f));

            fig_x = mixf_local(72.0f, 76.5f, su);
            fig_y = mixf_local(32.0f, 31.1f, su);
            angle = mixf_local(0.0f, -0.12f, su);

            base_pose = pose_stretched(pose_mix(pose_pull, pose_stumble, 0.28f), mixf_local(1.6f, 3.1f, su));
            base_pose.neck.x += mixf_local(0.8f, 1.8f, su);
            base_pose.neck.y += mixf_local(0.6f, 1.0f, su);
            base_pose.knees[0] = v2(-0.4f, 3.6f);
            base_pose.knees[1] = v2(2.7f, 4.8f);
            base_pose.feet[0] = v2(0.8f, 6.0f);
            base_pose.feet[1] = v2(4.9f, 7.1f);
            neck_x = base_pose.neck.x;
            neck_y = base_pose.neck.y;
            pose = pose_both_hands_target(
                base_pose,
                v2(78.0f - fig_x - neck_x, 29.0f - fig_y - neck_y),
                v2(mixf_local(81.5f, 83.0f, su) - fig_x - neck_x,
                   mixf_local(27.0f, 24.5f, su) - fig_y - neck_y),
                0.24f);
        } else if (u < 0.82f) {
            float su = smoothstep_local(seg_u(u, 0.56f, 0.82f));

            fig_x = mixf_local(76.5f, 82.5f, su);
            fig_y = mixf_local(31.1f, 31.7f, su);
            angle = mixf_local(-0.12f, -0.04f, su);

            base_pose = pose_stretched(pose_mix(pose_pull, pose_stand, 0.12f), mixf_local(3.2f, 4.8f, su));
            base_pose.neck.x += 2.0f;
            base_pose.neck.y += 1.0f;
            base_pose.knees[0] = v2(0.3f, 2.8f);
            base_pose.knees[1] = v2(3.7f, 4.1f);
            base_pose.feet[0] = v2(2.2f, 5.1f);
            base_pose.feet[1] = v2(6.0f, 6.8f);
            neck_x = base_pose.neck.x;
            neck_y = base_pose.neck.y;
            pose = pose_both_hands_target(
                base_pose,
                v2(79.0f - fig_x - neck_x, 30.5f - fig_y - neck_y),
                v2(84.0f - fig_x - neck_x, 25.5f - fig_y - neck_y),
                0.18f);
        } else {
            float su = smoothstep_local(seg_u(u, 0.82f, 1.0f));

            fig_x = mixf_local(82.5f, 84.0f, su);
            fig_y = mixf_local(31.7f, 32.0f, su);
            angle = mixf_local(-0.04f, 0.0f, su);

            base_pose = pose_stretched(pose_mix(pose_pull, pose_stumble, 0.32f + su * 0.28f), mixf_local(2.0f, 0.8f, su));
            base_pose.neck.x += 1.2f;
            base_pose.neck.y += 0.5f;
            base_pose.knees[0] = v2(0.5f, 3.1f);
            base_pose.knees[1] = v2(4.0f, 4.4f);
            base_pose.feet[0] = v2(2.2f, 6.0f);
            base_pose.feet[1] = v2(6.0f, 7.0f);
            neck_x = base_pose.neck.x;
            neck_y = base_pose.neck.y;
            pose = pose_both_hands_target(
                base_pose,
                v2(mixf_local(80.0f, 82.0f, su) - fig_x - neck_x,
                   mixf_local(31.0f, 29.0f, su) - fig_y - neck_y),
                v2(mixf_local(84.0f, 86.0f, su) - fig_x - neck_x,
                   mixf_local(27.5f, 28.5f, su) - fig_y - neck_y),
                0.22f);
        }
        cur_x = CANVAS_X + 80.0f;
        cur_y = CANVAS_Y + 26.0f;
    } else if (t < 6.0f) {
        float u = seg_u(t, 5.0f, 6.0f);
        draw_line_any(80, 0, 80, 46, 1);
        draw_line_any(40, 45, 70, 45, 1);
        clear_rect_any(79, 20, 81, 32, 1);
        if (u < 0.333f) {
            float wu = smoothstep_local(u / 0.333f);
            draw_line_any(100, 0, 100, (int)lroundf(mixf_local(0.0f, 46.0f, wu)), 1);
            fig_x = 84.0f;
            fig_y = 32.0f;
            pose = pose_stumble;
        } else {
            draw_line_any(100, 0, 100, 46, 1);
            if (u < 0.666f) {
                float wu = smoothstep_local((u - 0.333f) / 0.333f);
                fig_x = mixf_local(84.0f, 92.0f, wu);
                fig_y = 32.0f;
                pose = sample_walk_cycle(t * 1.1f);
            } else {
                fig_x = 92.0f;
                fig_y = 32.0f;
                pose = pose_stand;
            }
        }
        cur_x = CANVAS_X + 100.0f;
        cur_y = CANVAS_Y + 24.0f;
    } else if (t < 7.0f) {
        float u = seg_u(t, 6.0f, 7.0f);
        draw_line_any(80, 0, 80, 46, 1);
        draw_line_any(40, 45, 70, 45, 1);
        clear_rect_any(79, 20, 81, 32, 1);
        draw_line_any(100, 0, 100, 46, 1);
        if (u < 0.2f) {
            fig_x = 92.0f;
            fig_y = 32.0f;
            pose = pose_stand;
        } else if (u < 0.35f) {
            fig_x = 92.0f;
            fig_y = 32.0f;
            pose = pose_kick;
            clear_rect_any(99, 32, 101, 44, 1);
            draw_impact_burst(100.0f, 39.0f, 2, 1);
        } else {
            float ku = smoothstep_local((u - 0.35f) / 0.65f);
            fig_x = mixf_local(92.0f, 106.0f, ku);
            fig_y = 32.0f;
            pose = pose_kick;
            clear_rect_any(99, 32, 101, 44, 1);
            draw_impact_burst(100.0f, 39.0f, 2, 1);
        }
        cur_x = CANVAS_X + 100.0f;
        cur_y = CANVAS_Y + 24.0f;
    } else if (t < 8.0f) {
        float u = seg_u(t, 7.0f, 8.0f);
        draw_line_any(80, 0, 80, 46, 1);
        draw_line_any(40, 45, 70, 45, 1);
        draw_line_any(100, 0, 100, 46, 1);
        if (u < 0.5f) {
            cur_x = mixf_local(CANVAS_X + 100.0f, 44.0f, u / 0.5f);
            cur_y = mixf_local(CANVAS_Y + 24.0f, 6.0f, u / 0.5f);
        } else {
            cur_x = mixf_local(44.0f, CANVAS_X + 98.0f, (u - 0.5f) / 0.5f);
            cur_y = mixf_local(6.0f, CANVAS_Y + 32.0f, (u - 0.5f) / 0.5f);
        }
        fig_x = 106.0f;
        fig_y = 32.0f;
        pose = pose_angry(pose_stand, 0.25f);
    } else if (t < 9.0f) {
        float u = seg_u(t, 8.0f, 9.0f);
        draw_line_any(80, 0, 80, 46, 1);
        draw_line_any(40, 45, 70, 45, 1);
        draw_line_any(100, 0, 100, 46, 1);
        fig_x = 106.0f;
        eraser_x = mixf_local(98.0f, 104.0f, smoothstep_local(u));
        eraser_y = 32.0f;
        show_eraser = 1;
        erase_leg_overlap = 1;
        if (u < 0.45f) {
            fig_y = 32.0f;
            pose = pose_stumble;
        } else {
            float hu = smoothstep_local((u - 0.45f) / 0.55f);
            fig_y = 32.0f - sinf(hu * (float)M_PI) * 2.0f;
            pose = pose_angry(pose_stand, 0.2f);
            pose.knees[0] = v2(-0.4f, 1.0f);
            pose.feet[0] = v2(-0.4f, 1.0f);
            draw_left_stump = 1;
        }
        cur_x = CANVAS_X + eraser_x;
        cur_y = CANVAS_Y + eraser_y;
    } else if (t < 10.0f) {
        float u = smoothstep_local(seg_u(t, 9.0f, 10.0f));
        draw_line_any(80, 0, 80, 46, 1);
        draw_line_any(40, 45, 70, 45, 1);
        draw_line_any(100, 0, 100, 46, 1);
        fig_x = 106.0f;
        fig_y = 32.0f;
        pose = pose_angry(pose_mix(pose_stumble, pose_stand, u), 0.15f);
        draw_line_any(102, 32, 102, (int)lroundf(mixf_local(32.0f, 39.0f, u)), 1);
        cur_x = CANVAS_X + 98.0f;
        cur_y = CANVAS_Y + 32.0f;
    } else if (t < 12.0f) {
        const float push_duration = 8.0f / 12.0f;
        float push_u = clampf_local((t - 10.0f) / push_duration, 0.0f, 1.0f);
        float grip_u = smoothstep_local(seg_u(t, 10.0f, 10.25f));
        float settle_u = smoothstep_local(seg_u(t, 10.0f + push_duration, 11.25f));
        stick_pose_t brace_pose = pose_strain(pose_pull, 1.0f, 0.95f);
        float neck_x;
        float neck_y;

        draw_line_any(80, 0, 80, 46, 1);
        draw_line_any(40, 45, 70, 45, 1);
        draw_line_any(100, 0, 100, 46, 1);

        fig_x = mixf_local(106.0f, 116.0f, push_u);
        fig_y = 32.0f;
        eraser_x = mixf_local(100.0f, 120.0f, push_u);
        eraser_y = 32.0f;
        eraser_half_w = mixf_local(3.0f, 4.0f, grip_u);
        eraser_half_h = mixf_local(3.0f, 4.0f, grip_u);
        show_eraser = 1;

        brace_pose.neck.x += 1.1f;
        brace_pose.neck.y += 0.7f;
        brace_pose.knees[0] = v2(-1.8f, 4.4f);
        brace_pose.knees[1] = v2(2.8f, 4.9f);
        brace_pose.feet[0] = v2(-4.8f, 9.8f);
        brace_pose.feet[1] = v2(1.8f, 9.3f);
        brace_pose = pose_stretched(brace_pose, 0.7f + settle_u * 0.5f);

        pose = pose_mix(sample_run_cycle(t * 1.25f), brace_pose, grip_u);
        pose.neck.y += 0.2f * push_u;
        neck_x = pose.neck.x;
        neck_y = pose.neck.y;
        pose = pose_both_hands_target(
            pose,
            v2((eraser_x - eraser_half_w) - fig_x - neck_x,
               eraser_y - fig_y - neck_y),
            v2((eraser_x + eraser_half_w) - fig_x - neck_x,
               eraser_y - fig_y - neck_y),
            mixf_local(0.35f, 0.8f, grip_u));

        cur_x = CANVAS_X + eraser_x;
        cur_y = CANVAS_Y + eraser_y;
    } else if (t < 14.0f) {
        float u = seg_u(t, 12.0f, 14.0f);
        draw_line_any(80, 0, 80, 46, 1);
        draw_line_any(40, 45, 70, 45, 1);
        draw_line_any(100, 0, 100, 46, 1);
        right_border_gap = 1;
        if (u < 0.167f) {
            show_eraser = 1;
            eraser_x = 120.0f;
            eraser_y = 32.0f;
            eraser_half_w = 4.0f;
            eraser_half_h = 4.0f;
            cur_x = CANVAS_X + 120.0f;
            cur_y = CANVAS_Y + 32.0f;
        } else if (u < 0.333f) {
            float pu = smoothstep_local((u - 0.167f) / 0.166f);
            show_eraser = 1;
            eraser_x = 120.0f;
            eraser_y = mixf_local(32.0f, 16.0f, pu);
            eraser_half_w = 4.0f;
            eraser_half_h = 4.0f;
            cur_x = CANVAS_X + 120.0f;
            cur_y = CANVAS_Y + eraser_y;
        } else if (u < 0.5f) {
            cur_x = CANVAS_X + 120.0f;
            cur_y = CANVAS_Y + 16.0f;
        } else if (u < 0.625f) {
            float ru = smoothstep_local((u - 0.5f) / 0.125f);
            right_border_repair_u = ru;
            cur_x = CANVAS_X + (CANVAS_W - 1);
            cur_y = CANVAS_Y + mixf_local(27.0f, 37.0f, ru);
        } else {
            right_border_repair_u = 1.0f;
            cur_x = CANVAS_X + (CANVAS_W - 1);
            cur_y = CANVAS_Y + 37.0f;
        }
        fig_x = 116.0f;
        fig_y = 32.0f;
        pose = pose_stand;
    } else if (t < 16.0f) {
        float u = smoothstep_local(seg_u(t, 14.0f, 16.0f));
        fig_x = mixf_local(116.0f, 8.0f, u);
        fig_y = 32.0f;
        pose = sample_run_cycle(t * 1.1f);
        fake_icons_u = clampf_local(seg_u(t, 14.0f, 14.75f), 0.0f, 1.0f);
        cur_x = CANVAS_X + 120.0f;
        cur_y = CANVAS_Y + 16.0f;
    } else if (t < 18.0f) {
        float u = smoothstep_local(seg_u(t, 16.0f, 18.0f));
        int climb_phase = (((int)floorf((t - 16.0f) * 6.0f)) & 1);
        fig_x = mixf_local(8.0f, 4.0f, u);
        fig_y = mixf_local(32.0f, 8.0f, u);
        pose = climb_phase == 0 ? pose_climb : pose_climb_alt;
        fake_icons_u = 1.0f;
        cur_x = CANVAS_X + 120.0f;
        cur_y = CANVAS_Y + 16.0f;
    } else if (t < 19.0f) {
        float u = smoothstep_local(seg_u(t, 18.0f, 19.0f));
        fig_x = 4.0f;
        fig_y = 4.0f;
        fake_icons_u = 1.0f;
        if (u < 0.417f) {
            float su = smoothstep_local(u / 0.417f);
            pose = pose_mix(pose_stand, pose_reach_edge, 0.2f);
            cur_x = CANVAS_X + mixf_local(120.0f, 12.0f, su);
            cur_y = CANVAS_Y + mixf_local(16.0f, 4.0f, su);
        } else if (u < 0.583f) {
            pose = pose_kick;
            cur_x = CANVAS_X + 12.0f;
            cur_y = CANVAS_Y + 4.0f;
            draw_impact_burst(12.0f, 6.0f, 2, 1);
        } else if (u < 0.75f) {
            float du = smoothstep_local((u - 0.583f) / 0.167f);
            pose = pose_mix(pose_kick, pose_stand, du * 0.35f);
            cur_x = CANVAS_X + 12.0f;
            cur_y = CANVAS_Y + mixf_local(4.0f, 0.0f, du);
        } else {
            float bu = smoothstep_local((u - 0.75f) / 0.25f);
            pose = pose_mix(pose_kick, pose_stand, 0.35f + bu * 0.65f);
            cur_x = CANVAS_X + 12.0f;
            cur_y = CANVAS_Y + mixf_local(0.0f, 4.0f, bu);
        }
    } else if (t < 20.0f) {
        float u = seg_u(t, 19.0f, 20.0f);
        fig_x = 4.0f;
        if (u < 0.5f) {
            float fu = smoothstep_local(u / 0.5f);
            fig_y = mixf_local(4.0f, 32.0f, fu);
            pose = pose_stumble;
        } else if (u < 0.833f) {
            float bu = seg_u(u, 0.5f, 0.833f);
            fig_y = 32.0f + bounce_offset(bu, 0.0f, 1.0f, 2.0f);
            pose = pose_mix(pose_stumble, pose_stand, smoothstep_local(bu));
        } else {
            fig_y = 32.0f;
            pose = pose_stand;
        }
        cur_x = CANVAS_X + 12.0f;
        cur_y = CANVAS_Y + 14.0f;
    } else if (t < 21.0f) {
        float u = seg_u(t, 20.0f, 21.0f);
        float fall_u;
        fig_x = 4.0f;
        fig_y = 32.0f;
        pose = pose_stand;
        pose.neck.x += sinf(u * 4.0f * (float)M_PI) * 0.7f;
        cur_x = CANVAS_X + 12.0f;
        cur_y = CANVAS_Y + 14.0f;

        hide_top_transport = 1;
        show_falling_transport = 1;
        fall_u = smoothstep_local(seg_u(t, 20.1f, 20.85f));
        falling_transport_x0 = mixf_local(54.0f, 24.0f, fall_u);
        falling_transport_x1 = mixf_local(116.0f, 114.0f, fall_u);
        if (t < 20.1f) {
            falling_transport_y = 6.0f;
        } else if (fall_u < 0.82f) {
            falling_transport_y = mixf_local(6.0f, 51.0f, smoothstep_local(fall_u / 0.82f));
        } else {
            float bu = seg_u(fall_u, 0.82f, 1.0f);
            falling_transport_y = 51.0f + bounce_offset(bu, 0.0f, 1.0f, 3.0f);
        }
        falling_transport_playhead_u = mixf_local(0.68f, 0.42f, fall_u);
    } else if (t < 22.0f) {
        float u = smoothstep_local(seg_u(t, 21.0f, 22.0f));
        fig_x = mixf_local(4.0f, 20.0f, u);
        fig_y = 32.0f;
        pose = sample_walk_cycle(t * 0.8f);
        cur_x = CANVAS_X + 12.0f;
        cur_y = CANVAS_Y + 14.0f;
        overlay_u = clampf_local((t - 21.0f) / (8.0f / 12.0f), 0.0f, 1.0f);
        hide_top_transport = 1;
    } else if (t < 22.333f) {
        fig_x = 20.0f;
        fig_y = 32.0f;
        pose = pose_stand;
        pose.neck.x -= 0.4f;
        pose.neck.y += 0.4f;
        cur_x = CANVAS_X + 12.0f;
        cur_y = CANVAS_Y + 14.0f;
        overlay_u = 1.0f;
        hide_top_transport = 1;
    } else if (t < 22.833f) {
        float u = smoothstep_local(seg_u(t, 22.333f, 22.833f));
        fig_x = 20.0f;
        fig_y = mixf_local(32.0f, 32.0f, u);
        pose = pose_mix(pose_stand, pose_stumble, 0.35f + 0.25f * u);
        cur_x = CANVAS_X + 12.0f;
        cur_y = CANVAS_Y + 14.0f;
        overlay_u = 1.0f;
        hide_top_transport = 1;
    } else {
        fig_x = 20.0f;
        fig_y = 32.0f;
        pose = pose_mix(pose_stand, pose_stumble, 0.6f);
        cur_x = CANVAS_X + 12.0f;
        cur_y = CANVAS_Y + 14.0f;
        overlay_u = 1.0f;
        hide_top_transport = 1;
    }

    apply_transition_blend(4, raw_t, &fig_x, &fig_y, &pose, &scale, &angle, &cur_x, &cur_y, &cursor_pressed);
    if (hide_top_transport)
        clear_top_transport_region();
    if (overlay_u > 0.0f)
        draw_timeline_overlay_progress(overlay_u, 60.0f, 0, 0);
    if (fake_icons_u >= 0.0f)
        draw_fake_left_tool_icons(fake_icons_u);
    draw_stick_pose(fig_x, fig_y, &pose, scale, angle);
    if (redraw_first_wall_on_top) {
        draw_line_any(80, 0, 80, 19, 1);
        draw_line_any(80, 33, 80, 46, 1);
    }
    if (erase_leg_overlap) {
        clear_rect_any((int)lroundf(eraser_x - eraser_half_w), (int)lroundf(eraser_y - eraser_half_h),
                       (int)lroundf(eraser_x + eraser_half_w), (int)lroundf(eraser_y + eraser_half_h), 1);
    }
    if (draw_left_stump)
        draw_line_any(106, 32, 104, 34, 1);
    if (show_eraser)
        draw_eraser_outline(eraser_x - eraser_half_w, eraser_y - eraser_half_h,
                            (int)lroundf(eraser_half_w * 2.0f), (int)lroundf(eraser_half_h * 2.0f), 1);
    if (right_border_gap) {
        clear_rect_any(CANVAS_W - 1, 27, CANVAS_W - 1, 37, 1);
        if (right_border_repair_u >= 0.0f)
            draw_line_any(CANVAS_W - 1, 27, CANVAS_W - 1,
                          (int)lroundf(mixf_local(27.0f, 37.0f, right_border_repair_u)), 1);
    }
    if (show_falling_transport)
        draw_falling_transport_controls(falling_transport_x0, falling_transport_x1,
                                       falling_transport_y, falling_transport_playhead_u);
    draw_cursor(cur_x, cur_y, cursor_pressed);
    remember_scene_state(fig_x, fig_y, &pose, scale, angle, cur_x, cur_y, cursor_pressed);
}

static void draw_scene_controls(float t, float sim_t) {
    float raw_t = t;
    float cur_x = CANVAS_X + 12.0f;
    float cur_y = CANVAS_Y + 14.0f;
    float fig_x = 20.0f;
    float fig_y = 32.0f;
    float scale = 1.0f;
    float angle = 0.0f;
    int cursor_pressed = 0;
    int play_down = 0;
    int stop_down = 0;
    float playhead_x = 60.0f;
    stick_pose_t pose = pose_stand;
    continuity_state_t incoming;
    int has_incoming = transition_source_for_act(5, &incoming);
    stick_pose_t crouch_pose = pose_mix(pose_stand, pose_stumble, 0.62f);
    (void)sim_t;

    crouch_pose.neck.x += 0.45f;
    crouch_pose.neck.y += 0.2f;

    if (t < 2.0f) {
        float intro = smoothstep_local(seg_u(t, 0.0f, 0.4f));
        fig_x = has_incoming ? mixf_local(incoming.fig_pos.x, 20.0f, intro) : 20.0f;
        fig_y = has_incoming ? mixf_local(incoming.fig_pos.y, 32.0f, intro) : 32.0f;
        pose = has_incoming ? pose_mix(incoming.pose, crouch_pose, intro) : crouch_pose;
        pose.neck.x += sinf(seg_u(t, 0.0f, 2.0f) * 2.0f * (float)M_PI) * 0.45f;
        cur_x = has_incoming ? mixf_local(incoming.cursor_pos.x, CANVAS_X + 12.0f, intro) : CANVAS_X + 12.0f;
        cur_y = has_incoming ? mixf_local(incoming.cursor_pos.y, CANVAS_Y + 14.0f, intro) : CANVAS_Y + 14.0f;
    } else if (t < 4.0f) {
        fig_x = 24.0f;
        fig_y = 32.0f;
        pose = crouch_pose;
        if (t < 2.083f) {
            stop_down = 1;
            cur_x = CANVAS_X + 12.0f;
            cur_y = CANVAS_Y + 14.0f;
        } else if (t < 2.417f) {
            stop_down = 1;
            cur_x = CANVAS_X + 12.0f;
            cur_y = CANVAS_Y + 14.0f;
        } else if (t < 2.833f) {
            float u = smoothstep_local(seg_u(t, 2.417f, 2.833f));
            cur_x = CANVAS_X + mixf_local(12.0f, 30.0f, u);
            cur_y = CANVAS_Y + mixf_local(14.0f, 38.0f, u);
        } else if (t < 2.917f) {
            play_down = 1;
            cursor_pressed = 1;
            cur_x = CANVAS_X + 30.0f;
            cur_y = CANVAS_Y + 38.0f;
        } else {
            float u = smoothstep_local(seg_u(t, 2.917f, 4.0f));
            pose = pose_mix(crouch_pose, pose_stand, u * 0.35f);
            cur_x = CANVAS_X + 30.0f;
            cur_y = CANVAS_Y + 38.0f;
        }
    } else if (t < 5.0f) {
        fig_x = 24.0f;
        fig_y = 32.0f;
        pose = crouch_pose;
        if (t < 4.083f) {
            stop_down = 1;
            cur_x = CANVAS_X + 30.0f;
            cur_y = CANVAS_Y + 38.0f;
        } else if (t < 4.25f) {
            stop_down = 1;
            cur_x = CANVAS_X + 30.0f;
            cur_y = CANVAS_Y + 38.0f;
        } else if (t < 4.333f) {
            play_down = 1;
            cursor_pressed = 1;
            cur_x = CANVAS_X + 30.0f;
            cur_y = CANVAS_Y + 38.0f;
        } else if (t < 4.583f) {
            pose = pose_mix(crouch_pose, pose_stand, smoothstep_local(seg_u(t, 4.333f, 4.583f)) * 0.25f);
            cur_x = CANVAS_X + 30.0f;
            cur_y = CANVAS_Y + 38.0f;
        } else {
            cur_x = CANVAS_X + 30.0f;
            cur_y = CANVAS_Y + 38.0f;
        }
    } else if (t < 7.0f) {
        float step = floorf((t - 5.0f) * 6.0f);
        float hiccup = ((((int)floorf((t - 5.0f) * 12.0f)) & 1) == 0) ? -1.0f : 1.0f;
        stop_down = (((int)step) & 1) == 0;
        play_down = !stop_down;
        fig_x = (stop_down ? 24.0f : 30.0f) + hiccup;
        fig_y = 32.0f;
        pose = stop_down ? crouch_pose : pose_mix(crouch_pose, pose_kick, 0.22f);
        cur_x = CANVAS_X + (stop_down ? 24.0f : 30.0f);
        cur_y = CANVAS_Y + 38.0f;
        cursor_pressed = play_down;
        playhead_x = 60.0f;
    } else if (t < 8.0f) {
        fig_y = 32.0f;
        if (t < 7.1f) {
            float travel_u = smoothstep_local(seg_u(t, 7.0f, 7.1f));
            cur_x = CANVAS_X + mixf_local(30.0f, 60.0f, travel_u);
            cur_y = CANVAS_Y + 38.0f;
            fig_x = 24.0f;
            pose = crouch_pose;
            playhead_x = 60.0f;
        } else if (t < 7.35f) {
            float u = seg_u(t, 7.1f, 7.35f);
            playhead_x = mixf_local(60.0f, 20.0f, u);
            cur_x = CANVAS_X + playhead_x;
            cur_y = CANVAS_Y + 38.0f;
            cursor_pressed = 1;
            fig_x = 24.0f;
            pose = crouch_pose;
        } else if (t < 7.517f) {
            playhead_x = 20.0f;
            cur_x = CANVAS_X + 20.0f;
            cur_y = CANVAS_Y + 38.0f;
            cursor_pressed = 1;
            fig_x = 64.0f;
            fig_y = 32.0f;
            pose = pose_stand;
        } else if (t < 7.6f) {
            playhead_x = 20.0f;
            cur_x = CANVAS_X + 20.0f;
            cur_y = CANVAS_Y + 38.0f;
            fig_x = 24.0f;
            fig_y = 32.0f;
            pose = crouch_pose;
        } else {
            playhead_x = 20.0f;
            cur_x = CANVAS_X + 20.0f;
            cur_y = CANVAS_Y + 38.0f;
            fig_x = 24.0f;
            fig_y = 32.0f;
            pose = crouch_pose;
        }
    } else if (t < 9.0f) {
        if (t < 8.25f) {
            float u = seg_u(t, 8.0f, 8.25f);
            playhead_x = mixf_local(20.0f, 110.0f, u);
            cur_x = CANVAS_X + playhead_x;
            cur_y = CANVAS_Y + 38.0f;
            cursor_pressed = 1;
            fig_x = mixf_local(24.0f, 80.0f, u);
            fig_y = 32.0f;
            pose = sample_run_cycle(t * 6.0f);
        } else if (t < 8.5f) {
            float u = smoothstep_local(seg_u(t, 8.25f, 8.5f));
            playhead_x = mixf_local(110.0f, 90.0f, u);
            fig_x = 80.0f;
            fig_y = 32.0f;
            pose = pose_stand;
            pose.neck.x -= 0.5f;
            cur_x = CANVAS_X + mixf_local(110.0f, 90.0f, u);
            cur_y = CANVAS_Y + 38.0f;
        } else {
            playhead_x = 90.0f;
            fig_x = 80.0f;
            fig_y = 32.0f;
            pose = pose_stand;
            pose.neck.x -= 0.5f;
            cur_x = CANVAS_X + 90.0f;
            cur_y = CANVAS_Y + 38.0f;
        }
    } else if (t < 10.0f) {
        playhead_x = 90.0f;
        fig_x = 80.0f;
        fig_y = 32.0f;
        pose = pose_stand;
        pose.neck.x -= 0.5f;
        cur_x = CANVAS_X + 90.0f;
        cur_y = CANVAS_Y + 38.0f;
    } else if (t < 10.5f) {
        float offsets[6] = {2.0f, 2.0f, -2.0f, -2.0f, 1.0f, 0.0f};
        int idx = (int)clampf_local(floorf(seg_u(t, 10.0f, 10.5f) * 6.0f), 0.0f, 5.0f);
        float shake = offsets[idx];
        playhead_x = 90.0f;
        fig_x = 80.0f + shake;
        fig_y = 32.0f;
        pose = pose_stand;
        pose.neck.x -= 0.5f;
        cur_x = CANVAS_X + 90.0f + shake;
        cur_y = CANVAS_Y + 38.0f;
        for (int i = 0; i < 4; i++) {
            int y = 10 + i * 8;
            draw_line_any(6 + (int)shake, y, 114 + (int)shake, y, 1);
        }
    } else if (t < 10.917f) {
        float u = smoothstep_local(seg_u(t, 10.5f, 10.917f));
        playhead_x = 90.0f;
        fig_x = 80.0f;
        fig_y = 32.0f;
        pose = pose_stand;
        pose.neck.x -= 0.5f;
        cur_x = CANVAS_X + 90.0f;
        cur_y = CANVAS_Y + mixf_local(38.0f, 32.0f, u);
    } else if (t < 11.167f) {
        float u = smoothstep_local(seg_u(t, 10.917f, 11.167f));
        playhead_x = 90.0f;
        fig_x = 80.0f;
        fig_y = 32.0f;
        pose = pose_stand;
        pose.neck.x -= 0.5f;
        cur_x = CANVAS_X + mixf_local(90.0f, 80.0f, u);
        cur_y = CANVAS_Y + 32.0f;
    } else {
        playhead_x = 90.0f;
        fig_x = 80.0f;
        fig_y = 32.0f;
        pose = pose_stand;
        pose.neck.x -= 0.5f;
        cur_x = CANVAS_X + 80.0f;
        cur_y = CANVAS_Y + 32.0f;
    }

    apply_transition_blend(5, raw_t, &fig_x, &fig_y, &pose, &scale, &angle, &cur_x, &cur_y, &cursor_pressed);
    clear_top_transport_region();
    draw_timeline_overlay(playhead_x, play_down, stop_down);
    draw_stick_pose(fig_x, fig_y, &pose, scale, angle);
    draw_cursor(cur_x, cur_y, cursor_pressed);
    if (flash_window(raw_t, 2.0f, 1.0f / 12.0f) || flash_window(raw_t, 4.0f, 1.0f / 12.0f) ||
        ((((int)floorf((raw_t - 5.0f) * 12.0f)) & 1) == 0 && raw_t >= 5.0f && raw_t < 6.0f))
        draw_canvas_flash();
    remember_scene_state(fig_x, fig_y, &pose, scale, angle, cur_x, cur_y, cursor_pressed);
}

static void draw_scene_resolution(float t, float sim_t) {
    float raw_t = t;
    float cur_x = CANVAS_X + 80.0f;
    float cur_y = CANVAS_Y + 32.0f;
    float fig_x = 80.0f;
    float fig_y = 32.0f;
    float scale = 1.0f;
    float angle = 0.0f;
    int cursor_pressed = 1;
    stick_pose_t pose = pose_stand;
    float delete_x0 = 104.0f;
    float delete_x1 = 112.0f;
    float delete_y0 = 26.0f;
    float delete_y1 = 38.0f;
    continuity_state_t incoming;
    int has_incoming = transition_source_for_act(6, &incoming);
    float text_progress = -1.0f;
    float fade_u = -1.0f;
    (void)sim_t;

    if (t < 1.5f) {
        fig_x = 80.0f;
        fig_y = 32.0f;
        draw_selection_box(&pose_stand, fig_x, fig_y, scale, angle, 0);
        if (has_incoming) {
            float intro = smoothstep_local(seg_u(t, 0.0f, 0.35f));
            cur_x = mixf_local(incoming.cursor_pos.x, CANVAS_X + 87.0f, intro);
            cur_y = mixf_local(incoming.cursor_pos.y, CANVAS_Y + 24.0f, intro);
        } else {
            cur_x = CANVAS_X + 87.0f;
            cur_y = CANVAS_Y + 24.0f;
        }
        {
            int phase = (int)clampf_local(floorf(t / 0.5f), 0.0f, 2.0f);
            float local = seg_u(t, phase * 0.5f, phase * 0.5f + 0.5f);
            int marquee_right_x = 86;

            if (local < 0.45f) {
                pose = pose_mix(pose_stand, pose_angry(pose_punch, 0.55f), smoothstep_local(local / 0.45f));
                clear_rect_any(marquee_right_x, 18, marquee_right_x, 42, 1);
            } else {
                pose = pose_stand;
                draw_line_any(marquee_right_x, 18, marquee_right_x, 42, 1);
            }
        }
    } else if (t < 2.0f) {
        float u = smoothstep_local(seg_u(t, 1.5f, 2.0f));
        fig_x = 80.0f;
        fig_y = 32.0f;
        pose = pose_stand;
        cur_x = CANVAS_X + mixf_local(87.0f, 104.0f, u);
        cur_y = CANVAS_Y + mixf_local(24.0f, 26.0f, u);
        outline_rect_any((int)delete_x0, (int)delete_y0, (int)mixf_local(delete_x0, delete_x1, u), (int)delete_y1, 1);
        if (u > 0.7f)
            draw_delete_zone_local((int)delete_x0, (int)delete_y0, (int)delete_x1, (int)delete_y1);
    } else if (t < 3.0f) {
        float u = smoothstep_local(seg_u(t, 2.0f, 3.0f));
        float stretch = mixf_local(0.0f, 7.0f, u);
        fig_x = mixf_local(80.0f, 104.0f, u);
        fig_y = 32.0f;
        pose = pose_stretched(pose_strain(pose_stand, 1.0f, 0.35f + u * 0.25f), stretch);
        cur_x = CANVAS_X + mixf_local(80.0f, 106.0f, u);
        cur_y = CANVAS_Y + 32.0f;
        draw_delete_zone_local((int)delete_x0, (int)delete_y0, (int)delete_x1, (int)delete_y1);
        draw_selection_box(&pose, fig_x, fig_y, scale, angle, 0);
    } else if (t < 4.0f) {
        float u = smoothstep_local(seg_u(t, 3.0f, 4.0f));
        float stretch = mixf_local(7.0f, 8.0f, u);
        float neck_x = pose_stand.neck.x;
        float neck_y = pose_stand.neck.y;
        fig_x = mixf_local(104.0f, 108.0f, u);
        fig_y = 32.0f;
        pose = pose_stretched(pose_strain(pose_stand, 1.0f, 0.95f), stretch);
        pose = pose_hand_target(pose, 1, v2((CANVAS_W - 1) - fig_x - neck_x, 26.0f - fig_y - neck_y), 0.15f);
        cur_x = CANVAS_X + 108.0f;
        cur_y = CANVAS_Y + 32.0f;
        draw_delete_zone_local((int)delete_x0, (int)delete_y0, (int)delete_x1, (int)delete_y1);
        draw_selection_box(&pose, fig_x, fig_y, scale, angle, 0);
        draw_strain_lines(fig_x + 1.0f, fig_y - 8.0f, 0.9f);
    } else if (t < 4.667f) {
        float u = seg_u(t, 4.0f, 4.667f);
        fig_x = 108.0f;
        fig_y = 32.0f;
        pose = pose_stretched(pose_strain(pose_stand, 1.0f, 1.0f), 8.0f);
        draw_delete_zone_local((int)delete_x0, (int)delete_y0, (int)delete_x1, (int)delete_y1);
        draw_stick_pose(fig_x, fig_y, &pose, scale, angle);
        if (u > 0.1f)
            clear_rect_any(99, 35, 104, 44, 1);
        if (u > 0.3f)
            clear_rect_any(98, 20, 104, 32, 1);
        if (u > 0.5f)
            clear_rect_any(103, 22, 108, 40, 1);
        if (u > 0.7f)
            clear_rect_any(108, 18, 116, 40, 1);
        if (u > 0.88f)
            clear_rect_any(108, 10, 116, 18, 1);
        cur_x = CANVAS_X + 108.0f;
        cur_y = CANVAS_Y + 32.0f;
        cursor_pressed = u < 0.35f;
    } else if (t < 5.0f) {
        cur_x = CANVAS_X + 108.0f;
        cur_y = CANVAS_Y + 32.0f;
        cursor_pressed = 1;
    } else if (t < 6.0f) {
        float u = smoothstep_local(seg_u(t, 5.0f, 6.0f));
        cur_x = CANVAS_X + mixf_local(108.0f, 64.0f, u);
        cur_y = CANVAS_Y + 32.0f;
        cursor_pressed = 1;
    } else if (t < 6.35f) {
        float u = smoothstep_local(seg_u(t, 6.0f, 6.35f));
        cur_x = CANVAS_X + mixf_local(64.0f, 38.0f, u);
        cur_y = CANVAS_Y + mixf_local(32.0f, 16.0f, u);
        cursor_pressed = 1;
    } else if (t < 8.55f) {
        float local_x;
        float local_y;

        text_progress = seg_u(t, 6.35f, 8.55f) * (float)(sizeof(the_end_strokes) / sizeof(the_end_strokes[0]));
        draw_the_end_progress(text_progress, &local_x, &local_y);
        cur_x = CANVAS_X + local_x;
        cur_y = CANVAS_Y + local_y;
        cursor_pressed = 1;
    } else if (t < 10.55f) {
        float local_x;
        float local_y;

        text_progress = (float)(sizeof(the_end_strokes) / sizeof(the_end_strokes[0]));
        draw_the_end_progress(text_progress, &local_x, &local_y);
        cur_x = CANVAS_X + local_x;
        cur_y = CANVAS_Y + local_y;
        cursor_pressed = 0;
    } else if (t < 15.55f) {
        float local_x;
        float local_y;

        text_progress = (float)(sizeof(the_end_strokes) / sizeof(the_end_strokes[0]));
        draw_the_end_progress(text_progress, &local_x, &local_y);
        cur_x = CANVAS_X + local_x;
        cur_y = CANVAS_Y + local_y;
        cursor_pressed = 0;
        fade_u = smoothstep_local(seg_u(t, 10.55f, 15.55f));
    } else {
        fade_u = 1.0f;
        cursor_pressed = 0;
    }

    apply_transition_blend(6, raw_t, &fig_x, &fig_y, &pose, &scale, &angle, &cur_x, &cur_y, &cursor_pressed);
    if (t < 4.0f)
        draw_stick_pose(fig_x, fig_y, &pose, scale, angle);
    if (t >= 2.0f && t < 4.667f)
        draw_delete_zone_local((int)delete_x0, (int)delete_y0, (int)delete_x1, (int)delete_y1);
    if (t >= 4.667f && t < 4.833f)
        draw_delete_zone_local((int)delete_x0, (int)delete_y0, (int)delete_x1, (int)delete_y1);
    if (text_progress >= 0.0f)
        draw_the_end_progress(text_progress, NULL, NULL);
    if (t < 15.55f)
        draw_cursor(cur_x, cur_y, cursor_pressed);
    if (fade_u >= 0.0f)
        apply_random_screen_fade(fade_u);
    if (flash_window(raw_t, 4.62f, 0.05f))
        draw_canvas_flash();
    remember_scene_state(fig_x, fig_y, &pose, scale, angle, cur_x, cur_y, cursor_pressed);
}

static void draw_scene(int act_idx, float act_t, float act_duration, float sim_t) {
    (void)act_duration;
    switch (act_idx) {
        case 0: draw_scene_creation(act_t, sim_t); break;
        case 1: draw_scene_awakening(act_t, sim_t); break;
        case 2: draw_scene_escalation(act_t, sim_t); break;
        case 3: draw_scene_weapons(act_t, sim_t); break;
        case 4: draw_scene_battlefield(act_t, sim_t); break;
        case 5: draw_scene_controls(act_t, sim_t); break;
        case 6: draw_scene_resolution(act_t, sim_t); break;
        default: break;
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
    signal(SIGINT, stop);
    signal(SIGTERM, stop);

    long frame_ns = 1000000000L / TARGET_FPS;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);

    struct timespec prev;
    clock_gettime(CLOCK_MONOTONIC, &prev);

    float sim_t = 0.0f;
    float total_duration = total_animation_duration();

    while (running) {
        struct timespec curr;
        clock_gettime(CLOCK_MONOTONIC, &curr);
        float dt = (curr.tv_sec - prev.tv_sec) + (curr.tv_nsec - prev.tv_nsec) / 1e9f;
        if (dt > 1.0f) dt = 1.0f;
        prev = curr;
        sim_t += dt;
        if (sim_t > total_duration)
            sim_t = total_duration;

        int act_idx = 0;
        float act_t = 0.0f;
        float act_duration = act_durations[0];
        resolve_act_time(sim_t, &act_idx, &act_t, &act_duration);
        begin_act_transition(act_idx);

        ui_state_t ui;
        default_ui_for_act(act_idx, act_t, &ui);

        memset(fb, 0, sizeof(fb));
        draw_ui(&ui, sim_t);
        draw_scene(act_idx, act_t, act_duration, sim_t);
        flush();

        if (sim_t >= total_duration)
            break;

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
