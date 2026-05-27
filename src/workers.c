#include "workers.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>

#include <string.h>

#include "osc.h"
#include "synth.h"

/* queues to store up to 10 messages (aligned to 4-byte boundary) */
K_MSGQ_DEFINE(uart_msgq, MSG_SIZE, 10, 4);
K_MSGQ_DEFINE(osc_msgq, sizeof(osc_msg), 10, 4);
K_MSGQ_DEFINE(synth_evt_msgq, sizeof(struct synth_evt), 64, 4);

/* thread to parse OSC messages from raw UART data */
K_THREAD_DEFINE(parser_thread_id, 2048, parser_thread, NULL, NULL, NULL, 5, 0, 0); // may need to adjust priority and stack size
K_THREAD_DEFINE(osc_handler_thread_id, 2048, osc_handler_thread, NULL, NULL, NULL, 6, 0, 0);
K_THREAD_DEFINE(diagnostic_thread_id, 1024, diagnostic_thread, NULL, NULL, NULL, 7, 0, 0);

/* counters for dropped messages for diagnostics */
static volatile uint32_t uart_msgq_dropped;
static volatile uint32_t osc_msgq_dropped;
static volatile uint32_t synth_msgq_dropped;

/* receive buffer used in UART ISR callback */
static char rx_buf[MSG_SIZE];
static int rx_buf_pos;

/*
 * Read individual bytes from UART until packet delimiter 0xDEADBEEF is detected.
 * Afterwards push the data to the message queue.
 */
static void uart_cb(const struct device *uart_dev, void *user_data)
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

			/* if queue is full, message is dropped, increment the dropped counter */
			if (k_msgq_put(&uart_msgq, &rx_buf, K_NO_WAIT) != 0)
			{
				uart_msgq_dropped++;
			}

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

bool workers_init(const struct device *uart_dev)
{
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

		return false;
	}

	uart_irq_rx_enable(uart_dev);
	return true;
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

		/* if queue is full, message is dropped, increment the dropped counter */
		if (k_msgq_put(&osc_msgq, &msg, K_NO_WAIT) != 0)
		{
			osc_msgq_dropped++;
		}
	}
}

void osc_handler_thread(void *, void *, void *)
{
	static osc_msg msg;

	/* indefinitely wait for input from UART peripheral */
	while (k_msgq_get(&osc_msgq, &msg, K_FOREVER) == 0)
	{
		//printk("address pattern: %s\n", osc_addr_pattern(&msg));
		//printk("\ttype tag: %s\n", osc_type_tag(&msg));

		int num_args = osc_num_args(&msg);

		for (int i = 0; i < num_args; i++)
		{
			//printf("\t\targument: %f\n", osc_get_arg_float(&msg, i)); // must use printf instead of printk to print floats
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

			k_timeout_t timeout = evt.type == EVT_TOUCH_RELEASE ? K_MSEC(200) : K_NO_WAIT;

			/*  drop when full */
			if (k_msgq_put(&synth_evt_msgq, &evt, timeout) != 0)
			{
				synth_msgq_dropped++;
			}

			if (evt.type == EVT_TOUCH_RELEASE)
			{
				printk("RELEASE idx=%u\n", evt.touch.finger_idx);
			}
			else
			{
				printk("TOUCH   idx=%u\n", evt.touch.finger_idx);
			}
		}
	}
}

void diagnostic_thread(void *, void *, void *)
{
	while (1)
	{
		printk("Dropped queue messages: UART: %u, OSC: %u, Synth: %u\n", uart_msgq_dropped, osc_msgq_dropped, synth_msgq_dropped);
		printk("Used queue slots: UART: %u, OSC: %u, Synth: %u\n", k_msgq_num_used_get(&uart_msgq), k_msgq_num_used_get(&osc_msgq), k_msgq_num_used_get(&synth_evt_msgq));
		k_sleep(K_SECONDS(5));
	}
}
