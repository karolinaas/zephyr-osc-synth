#include <stdint.h>
#include <stdbool.h>

enum synth_evt_t
{
    EVT_TOUCH,
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

    /* target and current values necessary for smooth transitions*/
    float target_frequency; // target is set by control
    float current_frequency; // current is what is actually synthesized, glides towards target
    float target_amplitude;
    float current_amplitude;

    float phase;

    uint64_t last_update_time_ms;
};