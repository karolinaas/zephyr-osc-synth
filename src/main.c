#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#include "synth.h"
#include "workers.h"
#include "i2s_codec.h"

/* peripheral DT nodes */
#define UART_DEVICE_NODE DT_NODELABEL(arduino_serial)
#define USB_UART_DEVICE_NODE DT_CHOSEN(zephyr_shell_uart)
#define I2S_CODEC_TX DT_ALIAS(i2s_codec_tx)

static const struct device *const uart_dev = DEVICE_DT_GET(UART_DEVICE_NODE);
static const struct device *const usb_uart_dev = DEVICE_DT_GET(USB_UART_DEVICE_NODE);

static const struct device *const i2s_dev_codec = DEVICE_DT_GET(I2S_CODEC_TX);
static const struct device *const codec_dev = DEVICE_DT_GET(DT_NODELABEL(audio_codec));

int main(void)
{
    struct i2s_config config;
    struct audio_codec_cfg audio_cfg;

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

    // has to be ran before configure configure_tx_streams
    if (!codec_config(codec_dev, &audio_cfg))
    {
        printk("failure to config codec\n");
        return 0;
    }

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

    bool started = false;

    while (1)
    {
        int ret;
        static int16_t sample_buff[SAMPLES_PER_BLOCK]; // generated samples, important to not be on stack to avoid stack overflow

        for (int i = 0; i < CONFIG_I2S_INIT_BUFFERS; i++)
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
        printk("Send I2S trigger DRAIN failed");
        return 0;
    }

    printk("Streams stopped\n");

    return 0;
}
