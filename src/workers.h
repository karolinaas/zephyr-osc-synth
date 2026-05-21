#pragma once

#include <zephyr/device.h>
#include <stdbool.h>

#define MSG_SIZE 512

extern struct k_msgq synth_evt_msgq;

bool workers_init(const struct device *uart_dev);
void parser_thread(void *, void *, void *);
void osc_handler_thread(void *, void *, void *);
void diagnostic_thread(void *, void *, void *);
