#pragma once

#include <zephyr/drivers/i2s.h>
#include <zephyr/audio/codec.h>
#include <stdbool.h>

#define SAMPLE_FREQUENCY      CONFIG_SAMPLE_FREQ
#define SAMPLE_BIT_WIDTH      CONFIG_SAMPLE_WIDTH
#define BYTES_PER_SAMPLE      CONFIG_BYTES_PER_SAMPLE
#define NUMBER_OF_CHANNELS    (2U)
#define I2S_TX_TIMEOUT        (2000U)

#define INITIAL_BLOCKS        CONFIG_I2S_INIT_BUFFERS
#define BLOCK_DURATION_MS     10
#define FRAMES_PER_BLOCK      (SAMPLE_FREQUENCY * BLOCK_DURATION_MS / 1000)
#define SAMPLES_PER_BLOCK     (FRAMES_PER_BLOCK * NUMBER_OF_CHANNELS)
#define BLOCK_SIZE            (BYTES_PER_SAMPLE * SAMPLES_PER_BLOCK)
#define BLOCK_COUNT           (INITIAL_BLOCKS + CONFIG_EXTRA_BLOCKS)

bool configure_tx_streams(const struct device *i2s_dev, struct i2s_config *config);
bool trigger_command(const struct device *i2s_dev, enum i2s_trigger_cmd cmd);
bool codec_config(const struct device *codec_dev, struct audio_codec_cfg *config);