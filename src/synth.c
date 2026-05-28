#include "synth.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(synth, LOG_LEVEL_DBG);

static struct synth_voice synth_voices[NUM_VOICES_MAX];

void synth_update(const struct synth_evt *evt)
{
    switch (evt->type)
    {
        case EVT_TOUCH:
        {
            if (evt->touch.finger_idx >= NUM_VOICES_MAX)
            {
                LOG_WRN("Invalid finger index %u (max %d)", evt->touch.finger_idx, NUM_VOICES_MAX - 1);
                return;
            }

            struct synth_voice *voice = &synth_voices[evt->touch.finger_idx];

            // if voice is not active, activate it and initialize phase to 0
            if (!voice->active)
            {
                voice->active = true;
                voice->phase = 0.0f;
                voice->current_frequency = evt->touch.frequency; // start at the target frequency
                voice->current_amplitude = 0.0f; // ramp up from 0 to avoid clicks

				LOG_DBG("voice %d activated f=%.1f a=%.1f", evt->touch.finger_idx, (double)evt->touch.frequency, (double)evt->touch.amplitude);
            }

            voice->released = false;
            voice->target_frequency = evt->touch.frequency;
            voice->target_amplitude = evt->touch.amplitude;
            voice->last_update_time_ms = k_uptime_get();

            break;
        }

        case EVT_TOUCH_RELEASE:
        {
            if (evt->touch.finger_idx >= NUM_VOICES_MAX)
            {
                LOG_WRN("Invalid finger index %u (max %d)", evt->touch.finger_idx, NUM_VOICES_MAX - 1);
                return;
            }

            struct synth_voice *voice = &synth_voices[evt->touch.finger_idx];

            voice->released = true;
            voice->target_amplitude = 0.0f; // ramp down to 0 to avoid clicks

			LOG_DBG("voice %d released", evt->touch.finger_idx);

            break;
        }

        default:
        {
            LOG_WRN("Unsupported event type! Event ignored.");
            break;
        }
    }
}

void generate_sine(int16_t *buff, size_t num_frames, uint32_t sample_frequency, uint8_t num_channels)
{
    for (size_t i = 0; i < num_frames; i++)
    {
        float sample_val_sum = 0;

        /* sum/mix every active voice */
        for (int j = 0; j < NUM_VOICES_MAX; j++)
        {
            struct synth_voice *voice = &synth_voices[j];

            if (voice->active)
            {
                /* smoothing transitions towards target freq and amp */
                voice->current_frequency += FREQ_SMOOTHING_FACTOR * (voice->target_frequency - voice->current_frequency);
                voice->current_amplitude += AMP_SMOOTHING_FACTOR * (voice->target_amplitude - voice->current_amplitude);

                sample_val_sum += voice->current_amplitude * sinf(voice->phase);
                voice->phase += SYNTH_TWOPI * voice->current_frequency / sample_frequency;

                /* wrap phase in range 0 to 2*pi */
                if (voice->phase >= SYNTH_TWOPI)
                {
                    voice->phase -= SYNTH_TWOPI;
                }
            }
        }

        /* clamp to max amplitude, causes clipping */
        if (sample_val_sum > INT16_MAX)
        {
            sample_val_sum = INT16_MAX;
        }
        else if (sample_val_sum < INT16_MIN)
        {
            sample_val_sum = INT16_MIN;
        }

        buff[i * num_channels] = (int16_t)sample_val_sum; // left channel
        buff[i * num_channels + 1] = (int16_t)sample_val_sum; // right channel
    }
}

void prune_voices(void)
{
    int64_t max_last_update_time_ms = 0;
    bool any_active = false;

    for (int i = 0; i < NUM_VOICES_MAX; i++)
    {
        struct synth_voice *voice = &synth_voices[i];

        if (voice->active && !voice->released)
        {
            if (!any_active || voice->last_update_time_ms > max_last_update_time_ms)
            {
                max_last_update_time_ms = voice->last_update_time_ms;
            }

            any_active = true;
        }
    }

    for (int i = 0; i < NUM_VOICES_MAX; i++)
    {
        struct synth_voice *voice = &synth_voices[i];

        if (!voice->active)
        {
            continue;
        }

        /* if a voice lags behind the most recently updated active voice by more than the timeout, it is probably hanging due to a missed release event */
        if (!voice->released && any_active && (max_last_update_time_ms - voice->last_update_time_ms > VOICE_GROUP_TIMEOUT_MS))
        {
            voice->target_amplitude = 0.0f; // to avoid clicks set target amp to zero
            voice->released = true;

			LOG_WRN("Voice %d pruned due to time-out", i);
        }

        /* if a voice has been released and its amplitude is below the prune threshold, it is safe to deactivate it */
        if (voice->released && voice->current_amplitude < VOICE_PRUNE_AMP_THRESHOLD)
        {
            voice->target_amplitude = 0.0f; // to avoid clicks set target amp to zero
            voice->active = false;
            voice->released = false;
			LOG_DBG("Voice %d deactivated", i);
        }
    }
}
