#ifndef ELEVATED_MUSIC_H
#define ELEVATED_MUSIC_H

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "elevated_music_data.h"

#define ELEVATED_MUSIC_SAMPLE_RATE 44100u
#define ELEVATED_MUSIC_NOTES_PER_ROW 16u
#define ELEVATED_MUSIC_NUM_ROWS 114u
#define ELEVATED_MUSIC_MAX_NOTE_SAMPLES 5210u
#define ELEVATED_MUSIC_CONTENT_SAMPLES ((size_t)ELEVATED_MUSIC_NUM_ROWS * (size_t)ELEVATED_MUSIC_NOTES_PER_ROW * (size_t)ELEVATED_MUSIC_MAX_NOTE_SAMPLES)
#define ELEVATED_MUSIC_TOTAL_SAMPLES ((ELEVATED_MUSIC_CONTENT_SAMPLES + 65535u) & ~(size_t)65535u)
#define ELEVATED_MUSIC_MAX_STACK_HEIGHT 4u
#define ELEVATED_MUSIC_MAX_DELAY_SAMPLES 65536u
#define ELEVATED_MUSIC_TOTAL_PARAM_WORDS 1114112u

/* Match the original synth_core.nh float literals exactly. */
#define ELEVATED_MUSIC_FILTER_CUTOFF_SCALE 3.55243682861328125e-5f
#define ELEVATED_MUSIC_NOTE_FREQ_START 1.749869973e-4f
#define ELEVATED_MUSIC_NOTE_FREQ_STEP_SQUARED (1.029302237f * 1.029302237f)

typedef struct {
    uint8_t type;
    uint8_t op;
    float phase_shift;
    float detune;
} ElevatedMusicOscillator;

typedef struct {
    int32_t envelope[4];
    float noise_mix;
    float freq_exp;
    float base_freq;
    float volume;
    float stereo;
    ElevatedMusicOscillator oscillators[3];
} ElevatedMusicInstrument;

typedef struct {
    float *stack;
    int level;
    size_t note_offset;
    uint32_t random_seed;
} ElevatedMusicContext;

static int32_t elevated_music_read_i32(const uint8_t *ptr) {
    int32_t value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static float elevated_music_read_f32(const uint8_t *ptr) {
    float value;
    memcpy(&value, ptr, sizeof(value));
    return value;
}

static void elevated_music_write_i32(uint8_t *ptr, int32_t value) {
    memcpy(ptr, &value, sizeof(value));
}

static void elevated_music_write_f32(uint8_t *ptr, float value) {
    memcpy(ptr, &value, sizeof(value));
}

static float elevated_music_note_frequency(uint8_t note) {
    static int initialized = 0;
    static float note_table[128];

    if (!initialized) {
        note_table[0] = ELEVATED_MUSIC_NOTE_FREQ_START;
        for (int i = 1; i < 128; i++)
            note_table[i] = note_table[i - 1] * ELEVATED_MUSIC_NOTE_FREQ_STEP_SQUARED;
        initialized = 1;
    }

    return note_table[note & 127u];
}

static float elevated_music_random(ElevatedMusicContext *ctx) {
    uint32_t value = ctx->random_seed;
    value = value * 16307u + 17u;
    ctx->random_seed = value;
    return (float)(int16_t)(value >> 14) * (1.0f / 32768.0f);
}

static float elevated_music_oscillator(float phase, uint8_t type, float phase_shift) {
    float wrapped = phase + phase_shift;
    wrapped = 2.0f * (wrapped - nearbyintf(wrapped));

    switch (type) {
    case 1:
        return sinf(wrapped * 3.14159265358979323846f);
    case 2:
        return wrapped > 0.0f ? 1.0f : -1.0f;
    default:
        return wrapped;
    }
}

static float *elevated_music_level_buffer(const ElevatedMusicContext *ctx, int level) {
    return ctx->stack + (size_t)level * ELEVATED_MUSIC_TOTAL_SAMPLES * 2u;
}

static float *elevated_music_current_buffer(const ElevatedMusicContext *ctx) {
    return elevated_music_level_buffer(ctx, ctx->level);
}

static void elevated_music_parse_instrument(const uint8_t *params, ElevatedMusicInstrument *instrument) {
    memset(instrument, 0, sizeof(*instrument));

    for (int i = 0; i < 4; i++)
        instrument->envelope[i] = elevated_music_read_i32(params + i * 4);

    instrument->noise_mix = elevated_music_read_f32(params + 4 * 4);
    instrument->freq_exp = elevated_music_read_f32(params + 5 * 4);
    instrument->base_freq = elevated_music_read_f32(params + 6 * 4);
    instrument->volume = elevated_music_read_f32(params + 7 * 4);
    instrument->stereo = elevated_music_read_f32(params + 8 * 4);

    for (int i = 0; i < 3; i++) {
        size_t offset = 9u * 4u + (size_t)i * 12u;
        instrument->oscillators[i].type = params[offset + 0u];
        instrument->oscillators[i].op = params[offset + 1u];
        instrument->oscillators[i].phase_shift = elevated_music_read_f32(params + offset + 4u);
        instrument->oscillators[i].detune = elevated_music_read_f32(params + offset + 8u);
    }
}

static void elevated_music_apply_operator(float *accumulator, float wave, uint8_t op) {
    switch (op) {
    case 2:
        *accumulator += wave;
        break;
    case 3:
        *accumulator -= wave;
        break;
    case 4:
        *accumulator *= wave;
        break;
    default:
        break;
    }
}

static void elevated_music_synth(ElevatedMusicContext *ctx, const uint8_t *params) {
    static const float envelope_scales[4] = { 1.0f, -0.5f, 0.0f, -0.5f };
    ElevatedMusicInstrument instrument;
    float *stream;
    size_t note_base = ctx->note_offset;

    elevated_music_parse_instrument(params, &instrument);

    ctx->level += 1;
    if (ctx->level >= (int)ELEVATED_MUSIC_MAX_STACK_HEIGHT)
        ctx->level = (int)ELEVATED_MUSIC_MAX_STACK_HEIGHT - 1;

    stream = elevated_music_current_buffer(ctx);
    memset(stream, 0, ELEVATED_MUSIC_TOTAL_SAMPLES * 2u * sizeof(*stream));

    for (size_t event = 0; event < (size_t)ELEVATED_MUSIC_NUM_ROWS * (size_t)ELEVATED_MUSIC_NOTES_PER_ROW; event++) {
        size_t event_index = note_base + event;
        uint8_t pattern = elevated_sequence_data[event_index >> 4];
        uint8_t note = elevated_pattern_data[(size_t)pattern * 16u + (event_index & 15u)];
        float *note_dst = stream + event * (size_t)ELEVATED_MUSIC_MAX_NOTE_SAMPLES * 2u;
        float phase = 0.0f;
        float envelope = 0.0f;
        float frequency;
        size_t frame_index = 0;
        int note_stop = 0;

        if (note == 0)
            continue;

        note_stop = note == 127u;
        frequency = elevated_music_note_frequency(note) - instrument.base_freq;

        for (int segment = 0; segment < 4; segment++) {
            int count = instrument.envelope[segment];
            float env_step;

            if (count <= 0)
                continue;

            env_step = note_stop ? 0.0f : envelope_scales[segment] / (float)count;
            for (int sample = 0; sample < count && frame_index < ELEVATED_MUSIC_MAX_NOTE_SAMPLES; sample++, frame_index++) {
                float accumulator = 0.0f;
                float out;

                envelope += env_step;
                frequency *= instrument.freq_exp;
                phase += frequency + instrument.base_freq;

                for (int osc = 0; osc < 3; osc++) {
                    const ElevatedMusicOscillator *oscillator = &instrument.oscillators[osc];
                    float detune = oscillator->detune;
                    float wave = elevated_music_oscillator(phase * (2.0f - detune), oscillator->type, oscillator->phase_shift)
                               + elevated_music_oscillator(phase * detune, oscillator->type, oscillator->phase_shift);
                    elevated_music_apply_operator(&accumulator, wave, oscillator->op);
                }

                out = (accumulator + elevated_music_random(ctx) * instrument.noise_mix) * envelope * instrument.volume;
                note_dst[frame_index * 2u + 0u] = out;
                note_dst[frame_index * 2u + 1u] = out * instrument.stereo;
            }
        }
    }

    ctx->note_offset += (size_t)ELEVATED_MUSIC_NUM_ROWS * (size_t)ELEVATED_MUSIC_NOTES_PER_ROW;
}

static void elevated_music_filter(ElevatedMusicContext *ctx, uint8_t *params, float *state) {
    float *stream = elevated_music_current_buffer(ctx);
    float cutoff = elevated_music_read_f32(params + 0u * 4u);
    float resonance = elevated_music_read_f32(params + 1u * 4u);
    float lfo1_step = elevated_music_read_f32(params + 2u * 4u);
    float cos1 = elevated_music_read_f32(params + 3u * 4u);
    float lfo2_step = elevated_music_read_f32(params + 4u * 4u);
    float cos2 = elevated_music_read_f32(params + 5u * 4u);
    float dry = elevated_music_read_f32(params + 6u * 4u);
    int32_t filter_type = elevated_music_read_i32(params + 7u * 4u);
    float sin1 = state[0];
    float sin2 = state[1];

    if (filter_type < 0)
        filter_type = 0;
    if (filter_type > 2)
        filter_type = 2;

    for (size_t frame = 0; frame < ELEVATED_MUSIC_TOTAL_SAMPLES; frame++) {
        float f;

        cos1 -= sin1 * lfo1_step;
        sin1 += cos1 * lfo1_step;
        cos2 -= sin2 * lfo2_step;
        sin2 += cos2 * lfo2_step;
        f = (sin1 + sin2 + cutoff) * ELEVATED_MUSIC_FILTER_CUTOFF_SCALE;

        for (int ch = 0; ch < 2; ch++) {
            float *low = &state[2 + ch * 3 + 0];
            float *high = &state[2 + ch * 3 + 1];
            float *band = &state[2 + ch * 3 + 2];
            float input = stream[frame * 2u + (size_t)ch];
            float wet;

            *low += f * *band;
            *high = resonance * (input - *band) - *low;
            *band = *high * f + *band;

            switch (filter_type) {
            case 0:
                wet = *low;
                break;
            case 1:
                wet = *high;
                break;
            default:
                wet = *band;
                break;
            }

            stream[frame * 2u + (size_t)ch] = input * dry + wet;
        }
    }

    state[0] = sin1;
    state[1] = sin2;
    elevated_music_write_f32(params + 3u * 4u, cos1);
    elevated_music_write_f32(params + 5u * 4u, cos2);
}

static void elevated_music_delay(ElevatedMusicContext *ctx, uint8_t *params, float *delay_buffer) {
    float *stream = elevated_music_current_buffer(ctx);
    int32_t delay_pos = elevated_music_read_i32(params + 0u);
    int32_t delay_length = elevated_music_read_i32(params + 4u);
    float feedback = elevated_music_read_f32(params + 8u);

    if (delay_length <= 0)
        return;

    for (size_t frame = 0; frame < ELEVATED_MUSIC_TOTAL_SAMPLES; frame++) {
        size_t index;
        float wet_l;
        float wet_r;
        float out_l;
        float out_r;

        delay_pos--;
        if (delay_pos < 0)
            delay_pos += delay_length;

        index = (size_t)delay_pos * 2u;
        wet_l = delay_buffer[index + 0u];
        wet_r = delay_buffer[index + 1u];
        out_l = stream[frame * 2u + 0u] + wet_l * feedback;
        out_r = stream[frame * 2u + 1u] + wet_r * feedback;

        stream[frame * 2u + 0u] = out_l;
        stream[frame * 2u + 1u] = out_r;
        delay_buffer[index + 0u] = out_l;
        delay_buffer[index + 1u] = out_r;
    }

    elevated_music_write_i32(params + 0u, delay_pos);
}

static void elevated_music_allpass(ElevatedMusicContext *ctx, uint8_t *params, float *delay_buffer) {
    float *stream = elevated_music_current_buffer(ctx);
    int32_t delay_pos = elevated_music_read_i32(params + 0u);
    int32_t delay_length = elevated_music_read_i32(params + 4u);
    float feedback = elevated_music_read_f32(params + 8u);

    if (delay_length <= 0)
        return;

    for (size_t frame = 0; frame < ELEVATED_MUSIC_TOTAL_SAMPLES; frame++) {
        size_t index;
        float wet_l;
        float wet_r;
        float next_l;
        float next_r;

        delay_pos--;
        if (delay_pos < 0)
            delay_pos += delay_length;

        index = (size_t)delay_pos * 2u;
        wet_l = delay_buffer[index + 0u];
        wet_r = delay_buffer[index + 1u];
        next_l = stream[frame * 2u + 0u] + wet_r * feedback;
        next_r = stream[frame * 2u + 1u] + wet_l * feedback;

        delay_buffer[index + 0u] = next_l;
        delay_buffer[index + 1u] = next_r;
        stream[frame * 2u + 0u] = wet_r - next_l * feedback;
        stream[frame * 2u + 1u] = wet_l - next_r * feedback;
    }

    elevated_music_write_i32(params + 0u, delay_pos);
}

static void elevated_music_mixer(ElevatedMusicContext *ctx, const uint8_t *params) {
    float *src = elevated_music_current_buffer(ctx);
    float *dst = elevated_music_level_buffer(ctx, ctx->level - 1);
    float src_gain = elevated_music_read_f32(params + 0u);
    float dst_gain = elevated_music_read_f32(params + 4u);

    for (size_t i = 0; i < ELEVATED_MUSIC_TOTAL_SAMPLES * 2u; i++)
        dst[i] = src[i] * src_gain + dst[i] * dst_gain;

    ctx->level -= 1;
}

static void elevated_music_distortion2(ElevatedMusicContext *ctx, const uint8_t *params) {
    float *stream = elevated_music_current_buffer(ctx);
    float a = elevated_music_read_f32(params + 0u);
    float b = elevated_music_read_f32(params + 4u);

    for (size_t i = 0; i < ELEVATED_MUSIC_TOTAL_SAMPLES * 2u; i++)
        stream[i] = sinf(stream[i] * a) * b;
}

static void elevated_music_compressor(ElevatedMusicContext *ctx, const uint8_t *params) {
    float *stream = elevated_music_current_buffer(ctx);
    float threshold = elevated_music_read_f32(params + 0u);
    float ratio = elevated_music_read_f32(params + 4u);
    float postadd = elevated_music_read_f32(params + 8u);

    for (size_t i = 0; i < ELEVATED_MUSIC_TOTAL_SAMPLES * 2u; i++) {
        float sample = stream[i];
        float magnitude = fabsf(sample) - threshold;

        if (magnitude >= 0.0f)
            magnitude = magnitude * ratio + postadd;

        magnitude += threshold;
        stream[i] = copysignf(magnitude, sample);
    }
}

static int elevated_music_generate_pcm16(int16_t **out_pcm, size_t *out_frames) {
    static const size_t synth_param_size = 72u;
    static const size_t machine_param_sizes[] = { 72u, 12u, 32u, 12u, 8u, 8u, 12u };

    ElevatedMusicContext ctx;
    uint8_t *machine_tree;
    float *param_memory;
    int16_t *pcm;
    size_t param_cursor = 0u;
    size_t tree_cursor = 0u;

    *out_pcm = NULL;
    *out_frames = 0u;

    ctx.stack = (float *)calloc(ELEVATED_MUSIC_MAX_STACK_HEIGHT * ELEVATED_MUSIC_TOTAL_SAMPLES * 2u, sizeof(float));
    machine_tree = (uint8_t *)malloc(sizeof(elevated_machine_tree));
    param_memory = (float *)calloc(ELEVATED_MUSIC_TOTAL_PARAM_WORDS, sizeof(float));
    pcm = (int16_t *)malloc(ELEVATED_MUSIC_TOTAL_SAMPLES * 2u * sizeof(int16_t));
    if (!ctx.stack || !machine_tree || !param_memory || !pcm) {
        free(ctx.stack);
        free(machine_tree);
        free(param_memory);
        free(pcm);
        return 0;
    }

    memcpy(machine_tree, elevated_machine_tree, sizeof(elevated_machine_tree));
    ctx.level = -1;
    ctx.note_offset = 0u;
    ctx.random_seed = 0u;

    elevated_music_synth(&ctx, machine_tree + tree_cursor);
    tree_cursor += synth_param_size;

    while (tree_cursor < sizeof(elevated_machine_tree)) {
        uint8_t type = machine_tree[tree_cursor++];
        uint8_t *params = machine_tree + tree_cursor;

        if (type == 255u)
            break;

        switch (type) {
        case 0:
            elevated_music_synth(&ctx, params);
            break;
        case 1:
            elevated_music_delay(&ctx, params, param_memory + param_cursor);
            param_cursor += ELEVATED_MUSIC_MAX_DELAY_SAMPLES * 2u;
            break;
        case 2:
            elevated_music_filter(&ctx, params, param_memory + param_cursor);
            param_cursor += 8u;
            break;
        case 3:
            elevated_music_compressor(&ctx, params);
            break;
        case 4:
            elevated_music_mixer(&ctx, params);
            break;
        case 5:
            elevated_music_distortion2(&ctx, params);
            break;
        case 6:
            elevated_music_allpass(&ctx, params, param_memory + param_cursor);
            param_cursor += ELEVATED_MUSIC_MAX_DELAY_SAMPLES * 2u;
            break;
        default:
            free(ctx.stack);
            free(machine_tree);
            free(param_memory);
            free(pcm);
            return 0;
        }

        tree_cursor += machine_param_sizes[type];
    }

    if (ctx.level != 0) {
        free(ctx.stack);
        free(machine_tree);
        free(param_memory);
        free(pcm);
        return 0;
    }

    {
        float *mix = elevated_music_current_buffer(&ctx);
        for (size_t i = 0; i < ELEVATED_MUSIC_TOTAL_SAMPLES * 2u; i++) {
            long sample = lrintf(mix[i] * 32767.0f);
            if (sample < -32768L)
                sample = -32768L;
            else if (sample > 32767L)
                sample = 32767L;
            pcm[i] = (int16_t)sample;
        }
    }

    free(ctx.stack);
    free(machine_tree);
    free(param_memory);

    *out_pcm = pcm;
    *out_frames = ELEVATED_MUSIC_TOTAL_SAMPLES;
    return 1;
}

#endif
