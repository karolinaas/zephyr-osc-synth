#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/audio/codec.h>

#include <string.h>
#include <math.h>

#include "osc.h"

#define MSG_SIZE 512

#define SINE_FREQUENCY 230.6f
#define AMPLITUDE 16383 // 32767/2 for 16-bit audio

/* peripheral DT nodes */
#define UART_DEVICE_NODE DT_NODELABEL(arduino_serial)
#define USB_UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)
#define I2S_CODEC_TX DT_ALIAS(i2s_codec_tx)

#define SAMPLE_FREQUENCY CONFIG_SAMPLE_FREQ
#define SAMPLE_BIT_WIDTH CONFIG_SAMPLE_WIDTH
#define BYTES_PER_SAMPLE CONFIG_BYTES_PER_SAMPLE
#define NUMBER_OF_CHANNELS (2U)
#define SAMPLES_PER_BLOCK ((SAMPLE_FREQUENCY / 10) * NUMBER_OF_CHANNELS)
#define INITIAL_BLOCKS    CONFIG_I2S_INIT_BUFFERS
#define TIMEOUT           (2000U)

#define BLOCK_SIZE  (BYTES_PER_SAMPLE * SAMPLES_PER_BLOCK)
#define BLOCK_COUNT (INITIAL_BLOCKS + CONFIG_EXTRA_BLOCKS)
#define FRAMES_PER_BLOCK (SAMPLES_PER_BLOCK / NUMBER_OF_CHANNELS)

#ifndef M_PI
#define M_PI 3.14159265358979323846f // float because cortex M33 can only do float, has no double hw support
#endif
#ifndef M_TWOPI
#define M_TWOPI (M_PI * 2.0f)
#endif

/* queues to store up to 10 messages (aligned to 4-byte boundary) */
K_MSGQ_DEFINE(uart_msgq, MSG_SIZE, 10, 4);
K_MSGQ_DEFINE(osc_msgq, sizeof(osc_msg), 10, 4);

/* thread to parse OSC messages from raw UART data */
void parser_thread(void *, void *, void *);
K_THREAD_DEFINE(parser_thread_id, 2048, parser_thread, NULL, NULL, NULL, 5, 0, 0); // may need to adjust priority and stack size
void osc_handler_thread(void *, void *, void *);
K_THREAD_DEFINE(osc_handler_thread_id, 2048, osc_handler_thread, NULL, NULL, NULL, 6, 0, 0);

K_MEM_SLAB_DEFINE_IN_SECT_STATIC(mem_slab, __nocache, BLOCK_SIZE, BLOCK_COUNT, 4);

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
static const struct device *const usb_uart_dev = DEVICE_DT_GET(USB_UART_DEVICE_NODE);

static const struct device *const i2s_dev_codec = DEVICE_DT_GET(I2S_CODEC_TX);
static const struct device *const codec_dev = DEVICE_DT_GET(DT_NODELABEL(audio_codec));

/* receive buffer used in UART ISR callback */
static char rx_buf[MSG_SIZE];
static int rx_buf_pos;

/*
 * Read individual bytes from UART until packet delimiter 0xDEADBEEF is detected.
 * Afterwards push the data to the message queue.
 */
void uart_cb(const struct device *dev, void *user_data)
{
	static uint32_t shift_reg = 0;
	uint8_t byte;

	if (!uart_irq_update(uart_dev))
    {
		return;
	}

	if (!uart_irq_rx_ready(uart_dev))
    {
		return;
	}

	/* read until FIFO empty */
	while (uart_fifo_read(uart_dev, &byte, 1) == 1)
    {
		/* shift existing bytes towards LSB by one byte, then write new byte */
		shift_reg = (shift_reg >> 8) | ((uint32_t)byte << 24);

		if (shift_reg == 0xDEADBEEF)
        {
			//printk("%x\n", shift_reg);

			/* if queue is full, message is silently dropped */
			k_msgq_put(&uart_msgq, &rx_buf, K_NO_WAIT);

			/* reset the buffer position and shift register */
			rx_buf_pos = 0;
			shift_reg = 0;
		}
        else if (rx_buf_pos < sizeof(rx_buf))
        {
            rx_buf[rx_buf_pos++] = byte;
		}
		/* else: characters beyond buffer size are dropped */
	}
}

/*
 * Parse raw OSC message from UART and push it to the message queue.
 */
void parser_thread(void *, void *, void *)
{
	char raw_buf[MSG_SIZE];
	static osc_msg msg;

	/* indefinitely wait until uart_msgq has data, should use no cpu while waiting */
	while (k_msgq_get(&uart_msgq, &raw_buf, K_FOREVER) == 0)
	{
		if (osc_parse_message(&msg, (uint8_t *)raw_buf, MSG_SIZE) < 0)
		{
			printk("OSC message parse error.\n");
			continue;
		}

		/* if queue is full, message is silently dropped */
		k_msgq_put(&osc_msgq, &msg, K_NO_WAIT); // maybe it would be more reasonable to wait for space in queue?
	}
}

void osc_handler_thread(void *, void *, void *)
{
	static osc_msg msg;

	/* indefinitely wait for input from UART peripheral */
	while (k_msgq_get(&osc_msgq, &msg, K_FOREVER) == 0)
	{
		printk("address pattern: %s\n", osc_addr_pattern(&msg));
		printk("\ttype tag: %s\n", osc_type_tag(&msg));

		int num_args = osc_num_args(&msg);

		for (int i = 0; i < num_args; i++)
		{
			uint32_t raw;
			memcpy(&raw, osc_args(&msg) + 4*i, 4);
			raw = sys_be32_to_cpu(raw); // convert from network big-endian to cpu endian

			float testfloat;
			memcpy(&testfloat, &raw, 4);
			printf("\t\targument: %f\n", testfloat); // must use printf instead of printk to print floats
		}
	}
}

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

static void generate_sine(int16_t *buff, size_t num_frames, float freq_hz, float amplitude)
{
	static float phase = 0.0f; // accumulates phase, static to persist between calls
	const float phase_increment = M_TWOPI * freq_hz / SAMPLE_FREQUENCY;

	for (size_t i = 0; i < num_frames; i++)
	{
		int16_t sample_value = (int16_t)(amplitude * sinf(phase));

		buff[i * NUMBER_OF_CHANNELS] = sample_value; // left channel
		buff[i * NUMBER_OF_CHANNELS + 1] = sample_value; // right channel

		phase += phase_increment;

		if (phase >= M_TWOPI)
		{
			phase -= M_TWOPI; // wrap phase in range 0 to 2*pi
		}
	}
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

	/* configure interrupt and callback to receive data */
	int ret = uart_irq_callback_user_data_set(uart_dev, uart_cb, NULL);

	if (ret < 0)
	{
		if (ret == -ENOTSUP)
		{
			printk("Interrupt-driven UART API support not enabled\n");
		}
		else if (ret == -ENOSYS)
		{
			printk("UART device does not support interrupt-driven API\n");
		}
		else
		{
			printk("Error setting UART callback: %d\n", ret);
		}
		return 0;
	}
	uart_irq_rx_enable(uart_dev);

	for (;;)
	{
		bool started = false;

		while (1)
		{
			int i;

			static int16_t sample_buff[SAMPLES_PER_BLOCK]; // generated samples, important to not be on stack to avoid stack overflow

			for (i = 0; i < CONFIG_I2S_INIT_BUFFERS; i++)
			{
				generate_sine(sample_buff, FRAMES_PER_BLOCK, SINE_FREQUENCY, AMPLITUDE);

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
