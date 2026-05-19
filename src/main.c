#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/byteorder.h>

#include <string.h>

#include "osc.h"

/* change this to any other UART peripheral if desired */
#define UART_DEVICE_NODE DT_NODELABEL(arduino_serial)
#define USB_UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)

#define MSG_SIZE 36

/* queues to store up to 10 messages (aligned to 4-byte boundary) */
K_MSGQ_DEFINE(uart_msgq, MSG_SIZE, 10, 4);
K_MSGQ_DEFINE(osc_msgq, sizeof(osc_msg), 10, 4);

/* thread to parse OSC messages from raw UART data */
void parser_thread(void *, void *, void *);
K_THREAD_DEFINE(parser_thread_id, 2048, parser_thread, NULL, NULL, NULL, 5, 0, 0); // may need to adjust priority and stack size

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
static const struct device *const usb_uart_dev = DEVICE_DT_GET(USB_UART_DEVICE_NODE);

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

/*
 * Print a null-terminated string character by character to the UART interface
 */
void print_uart(char *buf)
{
	int msg_len = MSG_SIZE;

	for (int i = 0; i < msg_len; i++) {
		if (buf[i] == '\0') uart_poll_out(usb_uart_dev, '.');
		else uart_poll_out(usb_uart_dev, buf[i]);
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

	static osc_msg msg;

	/* indefinitely wait for input from UART peripheral */
	while (k_msgq_get(&osc_msgq, &msg, K_FOREVER) == 0)
	{
		printk("address pattern: %s\n", osc_addr_pattern(&msg));
		printk("type tag: %s\n", osc_type_tag(&msg));

		for (int i = 0; i < 3; i++)
		{
			uint32_t raw;
			memcpy(&raw, osc_args(&msg) + 4*i, 4);
			raw = sys_be32_to_cpu(raw); // convert from network big-endian to cpu byte order

			float testfloat;
			memcpy(&testfloat, &raw, 4);
			printf("argument: %f\n", testfloat); // must use printf instead of printk to print floats
		}
	}

	return 0;
}
