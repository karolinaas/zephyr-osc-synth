#include <stdint.h>
#include <stdbool.h>

enum synth_evt_t
{
    EVT_TOUCH_SET,
};

struct evt_touch_set
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
        struct evt_touch_set touch_set;
    };
};

struct synth_voice
{
    bool active;

    float frequency;
    float amplitude;
    float phase;

    uint64_t last_update_time_ms;
};