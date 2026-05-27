#include "i2s_codec.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/audio/codec.h>

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(mem_slab, __nocache, BLOCK_SIZE, BLOCK_COUNT, 4);

bool configure_tx_streams(const struct device *i2s_dev, struct i2s_config *config)
{
    config->word_size = SAMPLE_BIT_WIDTH;
    config->channels = NUMBER_OF_CHANNELS;
    config->format = I2S_FMT_DATA_FORMAT_I2S;
#ifdef CONFIG_USE_CODEC_CLOCK
    config->options = I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET;
#else
    config->options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER;
#endif
    config->frame_clk_freq = SAMPLE_FREQUENCY;
    config->mem_slab = &mem_slab;
    config->block_size = BLOCK_SIZE;
    config->timeout = I2S_TX_TIMEOUT;

    int ret;

    ret = i2s_configure(i2s_dev, I2S_DIR_TX, config);

    if (ret < 0)
    {
        printk("Failed to configure codec stream: %d\n", ret);
        return false;
    }

    return true;
}

bool trigger_command(const struct device *i2s_dev, enum i2s_trigger_cmd cmd)
{
    int ret;

    ret = i2s_trigger(i2s_dev, I2S_DIR_TX, cmd);
    if (ret < 0)
    {
        printk("Failed to trigger command %d on TX: %d\n", cmd, ret);
        return false;
    }

    return true;
}

bool codec_config(const struct device *codec_dev, struct audio_codec_cfg *config)
{
    config->dai_route = AUDIO_ROUTE_PLAYBACK;
    config->dai_type = AUDIO_DAI_TYPE_I2S;
    config->dai_cfg.i2s.word_size = SAMPLE_BIT_WIDTH;
    config->dai_cfg.i2s.channels = 2;
    config->dai_cfg.i2s.format = I2S_FMT_DATA_FORMAT_I2S;
#ifdef CONFIG_USE_CODEC_CLOCK
    config->dai_cfg.i2s.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
#else
    config->dai_cfg.i2s.options = I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET;
#endif
    config->dai_cfg.i2s.frame_clk_freq = SAMPLE_FREQUENCY;
    config->dai_cfg.i2s.mem_slab = &mem_slab;
    config->dai_cfg.i2s.block_size = BLOCK_SIZE;


    int ret;

    ret = audio_codec_configure(codec_dev, config);

    if (ret < 0)
    {
        printk("Failed to configure codec: %d\n", ret);
        return false;
    }

    k_msleep(1000);

    return true;
}