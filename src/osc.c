#include "osc.h"

#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(osc, LOG_LEVEL_DBG);

// helper function
static inline uint32_t osc_move4_up(uint32_t idx)
{
    return (idx + 3) & ~0x3; // move up to next multiple of 4
}

int osc_parse_message(osc_msg *msg, uint8_t *raw_buff, uint32_t raw_len)
{
    // initial bound check
    if (raw_len > OSC_MSG_MAX_SIZE)
    {
        return -1; // raw buffer too long
    }

    memcpy(msg->msg_data, raw_buff, raw_len);
    msg->idx_addr_pattern = 0; // rn we hope that the osc message is not prepended with its size (some non-standard implementations)

    uint32_t i = 0;

    // iterate through address pattern until null char
    while (i < raw_len && raw_buff[i] != '\0')
    {
        i++;
    }

    if (i >= raw_len)
    {
        LOG_WRN("Address pattern not null-terminated! Message ignored.");
        return -1;
    }

    i = osc_move4_up(i + 1); // move up to next multiple of 4 after null char

    if (i >= raw_len)
    {
        LOG_WRN("Address pattern padding overflows the raw buffer! Should be impossible, malformed message. Message ignored.");
        return -1;
    }

    if (raw_buff[i] != ',')
    {
        LOG_WRN("Type tag string does not start with ','! Missing or malformed message. Message ignored.");
        return -1;
    }

    msg->idx_type_tag = i + 1; // type tag string starts after ','

    // iterate through type tag string until null char
    while (i < raw_len && raw_buff[i] != '\0')
    {
        i++;
    }

    if (i >= raw_len)
    {
        LOG_WRN("Type tag string not null-terminated! Missing or malformed message. Message ignored.");
        return -1; // type tag string not null terminated
    }

    // validate floats
    for (uint32_t j = msg->idx_type_tag; j < i; j++)
    {
        if (raw_buff[j] != 'f')
        {
            LOG_WRN("Unsupported argument type '%c'! Only floats supported for now. Message ignored.", raw_buff[j]);
            return -1;
        }
    }

    i = osc_move4_up(i + 1); // move up to next multiple of 4 after null char

    if (i > raw_len)
    {
        LOG_WRN("Type tag string padding overflows the raw buffer! Should be impossible, malformed message. Message ignored.");
        return -1;
    }

    msg->idx_args = i; // arguments start after type tag string padding

    i += 4 * osc_num_args(msg); // move i to the end of the arguments, each float is 4 bytes long
    
    if (i > raw_len)
    {
        LOG_WRN("Arguments overflow the raw buffer! Should be impossible, malformed message. Message ignored.");
        return -1;
    }

    msg->msg_size = i; // now we know the actual size of the message, from start to end of arguments

    return msg->msg_size;
}

float osc_get_arg_float(osc_msg *msg, uint32_t arg_idx)
{
    if (arg_idx >= osc_num_args(msg))
    {
        return 0.0f; // out of bounds, maybe should handle differently?
    }

    uint32_t raw;
    memcpy(&raw, osc_args(msg) + 4 * arg_idx, 4); // each float is 4 bytes long
    raw = sys_be32_to_cpu(raw); // convert from network big-endian to cpu endian

    float value;
    memcpy(&value, &raw, 4);

    return value;
}