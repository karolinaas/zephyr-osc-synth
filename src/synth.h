#pragma once

#include <zephyr/kernel.h>
#include <math.h>
#include <stdint.h>
#include <stdbool.h>

#define NUM_VOICES_MAX 3
#define VOICE_PRUNE_AMP_THRESHOLD 1.0f // below this amp voice is pruned
#define VOICE_GROUP_TIMEOUT_MS 50 // if a voice lags this much behind the rest of voices, it is pruned, helps prevent hanging voices when events are missed

#define FREQUENCY_MAX_HZ 1000.0f
#define AMPLITUDE_MAX ((float)INT16_MAX / NUM_VOICES_MAX)

#define FREQ_SMOOTHING_FACTOR 0.01f // between 0 and 1, higher smoothing converges faster, shouldn't be much higher than 0,02
#define AMP_SMOOTHING_FACTOR 0.01f

#ifndef SYNTH_PI
#define SYNTH_PI 3.14159265358979323846f // float because cortex M33 can only do float, has no double hw support
#endif
#ifndef SYNTH_TWOPI
#define SYNTH_TWOPI (SYNTH_PI * 2.0f)
#endif

enum synth_evt_t
{
    EVT_TOUCH,
    EVT_TOUCH_RELEASE,
};

struct synth_evt_touch
{
    uint32_t finger_idx; // probably overkill since humans only have 10 fingers
    float frequency;
    float amplitude;
};

struct synth_evt
{
    enum synth_evt_t type;

    union
    {
        struct synth_evt_touch touch;
    };
};

struct synth_voice
{
    bool active;
    bool released;

    int64_t last_update_time_ms;

    /* target and current values necessary for smooth transitions*/
    float target_frequency; // target is set by control
    float current_frequency; // current is what is actually synthesized, glides towards target
    float target_amplitude;
    float current_amplitude;

    float phase;
};

void synth_update(const struct synth_evt *evt);
void generate_sine(int16_t *buff, size_t num_frames, uint32_t sample_frequency, uint8_t num_channels);
void prune_voices(void);