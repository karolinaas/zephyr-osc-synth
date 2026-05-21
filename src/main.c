#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/audio/codec.h>

#include <string.h>
#include <math.h>

#include "osc.h"
#include "synth.h"

#define MSG_SIZE 512

#define NUM_VOICES_MAX 3
#define VOICE_PRUNE_AMP_THRESHOLD 1.0f // below this amp voice is pruned

#define FREQUENCY_MAX_HZ 1000.0f
#define AMPLITUDE_MAX ((float)INT16_MAX / NUM_VOICES_MAX)

#define FREQ_SMOOTHING_FACTOR 0.001f // between 0 and 1, higher smoothing converges faster, shouldn't be much higher than 0,02
#define AMP_SMOOTHING_FACTOR 0.005f

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
K_MSGQ_DEFINE(synth_evt_msgq, sizeof(struct synth_evt), 10, 4);

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

static struct synth_voice synth_voices[NUM_VOICES_MAX];

static void synth_update(const struct synth_evt *evt)
{
	switch (evt->type)
	{
		case EVT_TOUCH:
		{
			if (evt->touch.finger_idx >= NUM_VOICES_MAX)
			{
				printk("invalid finger index\n");
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
			}

			voice->released = false;
			voice->target_frequency = evt->touch.frequency;
			voice->target_amplitude = evt->touch.amplitude;
			
			break;
		}

		case EVT_TOUCH_RELEASE:
		{
			if (evt->touch.finger_idx >= NUM_VOICES_MAX)
			{
				printk("invalid finger index\n");
				return;
			}

			struct synth_voice *voice = &synth_voices[evt->touch.finger_idx];

			voice->released = true;
			voice->target_amplitude = 0.0f; // ramp down to 0 to avoid clicks

			break;
		}

		default:
		{
			printk("unsupported event type\n");
			break;
		}
	}
}

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
			printf("\t\targument: %f\n", osc_get_arg_float(&msg, i)); // must use printf instead of printk to print floats
		}

		if (!strcmp((char *)osc_addr_pattern(&msg), "/touch"))
		{
			if (num_args != 3)
			{
				printk("invalid number of args for /touch, expected 3, instead got %d\n", num_args);
				continue;
			}

			float width = osc_get_arg_float(&msg, 1);
			float height = osc_get_arg_float(&msg, 2);

			struct synth_evt evt;
			evt.touch.finger_idx = (uint32_t)osc_get_arg_float(&msg, 0);

			if (height < 0.0f || width < 0.0f)
			{	
				/* S2O sends negative values for touch release */
				evt.type = EVT_TOUCH_RELEASE;
			}
			else
			{
				/* value clamp just in case */
				width = width > 1.0f ? 1.0f : width;
				height = height > 1.0f ? 1.0f : height;

				evt.type = EVT_TOUCH;
				
				evt.touch.amplitude = width * AMPLITUDE_MAX;
				evt.touch.frequency = height * FREQUENCY_MAX_HZ;
			}

			/* drop when full */
			k_msgq_put(&synth_evt_msgq, &evt, K_NO_WAIT);
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

static void generate_sine(int16_t *buff, size_t num_frames)
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
				voice->phase += M_TWOPI * voice->current_frequency / SAMPLE_FREQUENCY;

				/* wrap phase in range 0 to 2*pi */
				if (voice->phase >= M_TWOPI)
				{
					voice->phase -= M_TWOPI;
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

		buff[i * NUMBER_OF_CHANNELS] = (int16_t)sample_val_sum; // left channel
		buff[i * NUMBER_OF_CHANNELS + 1] = (int16_t)sample_val_sum; // right channel
	}
}

void prune_voices(void)
{
	for (int i = 0; i < NUM_VOICES_MAX; i++)
	{
		struct synth_voice *voice = &synth_voices[i];

		if (voice->active && voice->released && voice->current_amplitude < VOICE_PRUNE_AMP_THRESHOLD)
		{
			voice->target_amplitude = 0.0f; // to avoid clicks set target amp to zero
			voice->active = false;
			voice->released = false;
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
				struct synth_evt evt;

				/* drain the synth event queue to get the latest message */
				while (k_msgq_get(&synth_evt_msgq, &evt, K_NO_WAIT) == 0)
				{
					synth_update(&evt);
				}

				/* silence timed out voices */
				prune_voices();

				generate_sine(sample_buff, FRAMES_PER_BLOCK);

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
