#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/audio/codec.h>

#include "synth.h"
#include "workers.h"

/* peripheral DT nodes */
#define UART_DEVICE_NODE DT_NODELABEL(arduino_serial)
#define USB_UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)
#define I2S_CODEC_TX DT_ALIAS(i2s_codec_tx)

#define SAMPLE_FREQUENCY CONFIG_SAMPLE_FREQ
#define SAMPLE_BIT_WIDTH CONFIG_SAMPLE_WIDTH
#define BYTES_PER_SAMPLE CONFIG_BYTES_PER_SAMPLE
#define NUMBER_OF_CHANNELS (2U)
#define TIMEOUT           (2000U)

#define INITIAL_BLOCKS    CONFIG_I2S_INIT_BUFFERS
#define BLOCK_DURATION_MS 10
#define FRAMES_PER_BLOCK (SAMPLE_FREQUENCY * BLOCK_DURATION_MS / 1000)
#define SAMPLES_PER_BLOCK (FRAMES_PER_BLOCK * NUMBER_OF_CHANNELS)
#define BLOCK_SIZE  (BYTES_PER_SAMPLE * SAMPLES_PER_BLOCK)
#define BLOCK_COUNT (INITIAL_BLOCKS + CONFIG_EXTRA_BLOCKS)

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(mem_slab, __nocache, BLOCK_SIZE, BLOCK_COUNT, 4);

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
static const struct device *const usb_uart_dev = DEVICE_DT_GET(USB_UART_DEVICE_NODE);

static const struct device *const i2s_dev_codec = DEVICE_DT_GET(I2S_CODEC_TX);
static const struct device *const codec_dev = DEVICE_DT_GET(DT_NODELABEL(audio_codec));

static bool configure_tx_streams(const struct device *i2s_dev, struct i2s_config *config)
{
	int ret;

	ret = i2s_configure(i2s_dev, I2S_DIR_TX, config);
	if (ret < 0)
	{
		printk("Failed to configure codec stream: %d\n", ret);
		return false;
	}

	return true;
}

static bool trigger_command(const struct device *i2s_dev_codec, enum i2s_trigger_cmd cmd)
{
	int ret;

	ret = i2s_trigger(i2s_dev_codec, I2S_DIR_TX, cmd);
	if (ret < 0)
	{
		printk("Failed to trigger command %d on TX: %d\n", cmd, ret);
		return false;
	}

	return true;
}

int main(void)
{
	if (!device_is_ready(uart_dev))
	{
		printk("UART device not found!");
		return 0;
	}

	if (!device_is_ready(usb_uart_dev))
	{
		printk("UART device not found!");
		return 0;
	}

	if (!device_is_ready(i2s_dev_codec))
	{
		printk("%s is not ready\n", i2s_dev_codec->name);
		return 0;
	}

	if (!device_is_ready(codec_dev))
	{
		printk("%s is not ready", codec_dev->name);
		return 0;
	}

	struct i2s_config config;
	struct audio_codec_cfg audio_cfg;

	int ret;

	audio_cfg.dai_route = AUDIO_ROUTE_PLAYBACK;
	audio_cfg.dai_type = AUDIO_DAI_TYPE_I2S;
	audio_cfg.dai_cfg.i2s.word_size = SAMPLE_BIT_WIDTH;
	audio_cfg.dai_cfg.i2s.channels = 2;
	audio_cfg.dai_cfg.i2s.format = I2S_FMT_DATA_FORMAT_I2S;
#ifdef CONFIG_USE_CODEC_CLOCK
	audio_cfg.dai_cfg.i2s.options = I2S_OPT_FRAME_CLK_CONTROLLER | I2S_OPT_BIT_CLK_CONTROLLER;
#else
	audio_cfg.dai_cfg.i2s.options = I2S_OPT_FRAME_CLK_TARGET | I2S_OPT_BIT_CLK_TARGET;
#endif
	audio_cfg.dai_cfg.i2s.frame_clk_freq = SAMPLE_FREQUENCY;
	audio_cfg.dai_cfg.i2s.mem_slab = &mem_slab;
	audio_cfg.dai_cfg.i2s.block_size = BLOCK_SIZE;
	audio_codec_configure(codec_dev, &audio_cfg);
	k_msleep(1000);

	config.word_size = SAMPLE_BIT_WIDTH;
	config.channels = NUMBER_OF_CHANNELS;
	config.format = I2S_FMT_DATA_FORMAT_I2S;
#ifdef CONFIG_USE_CODEC_CLOCK
	config.options = I2S_OPT_BIT_CLK_TARGET | I2S_OPT_FRAME_CLK_TARGET;
#else
	config.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER;
#endif
	config.frame_clk_freq = SAMPLE_FREQUENCY;
	config.mem_slab = &mem_slab;
	config.block_size = BLOCK_SIZE;
	config.timeout = TIMEOUT;

	if (!configure_tx_streams(i2s_dev_codec, &config))
	{
		printk("failure to config streams\n");
		return 0;
	}

	if (!workers_init(uart_dev))
	{
		printk("failed to initialize workers\n");
		return 0;
	}

	for (;;)
	{
		bool started = false;

		while (1)
		{
			int i;

			static int16_t sample_buff[SAMPLES_PER_BLOCK]; // generated samples, important to not be on stack to avoid stack overflow

			for (i = 0; i < CONFIG_I2S_INIT_BUFFERS; i++)
			{
				struct synth_evt evt;

				/* drain the synth event queue to get the latest message */
				while (k_msgq_get(&synth_evt_msgq, &evt, K_NO_WAIT) == 0)
				{
					synth_update(&evt);
				}

				/* silence timed out voices */
				prune_voices();

				generate_sine(sample_buff, FRAMES_PER_BLOCK, SAMPLE_FREQUENCY, NUMBER_OF_CHANNELS);

				ret = i2s_buf_write(i2s_dev_codec, sample_buff, BLOCK_SIZE);
				if (ret < 0)
				{
					printk("Failed to write data: %d\n", ret);
					break;
				}
			}

			if (ret < 0)
			{
				printk("error %d\n", ret);
				break;
			}

			if (!started)
			{
				i2s_trigger(i2s_dev_codec, I2S_DIR_TX, I2S_TRIGGER_START);
				started = true;
			}
		}

		if (!trigger_command(i2s_dev_codec, I2S_TRIGGER_DROP))
		{
			printk("Send I2S trigger DRAIN failed: %d", ret);
			return 0;
		}

		printk("Streams stopped\n");

		return 0;
	}

	return 0;
}
