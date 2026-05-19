#include "osc.h"

#include <string.h>

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
        return -1; // address pattern not null terminated
    }

    i = osc_move4_up(i + 1); // move up to next multiple of 4 after null char

    if (i >= raw_len)
    {
        return -1; // address pattern padding overflows the raw buffer (should be impossible, malformed message)
    }

    if (raw_buff[i] != ',')
    {
        return -1; // missing/malformed type tag string
    }

    msg->idx_type_tag = i + 1; // type tag string starts after ','

    // iterate through type tag string until null char
    while (i < raw_len && raw_buff[i] != '\0')
    {
        i++;
    }
    if (i >= raw_len)
    {
        return -1; // type tag string not null terminated
    }

    // validate floats
    for (uint32_t j = msg->idx_type_tag; j < i; j++)
    {
        if (raw_buff[j] != 'f')
        {
            return -1; // unsopported argument type, only floats supported for now
        }
    }

    i = osc_move4_up(i + 1); // move up to next multiple of 4 after null char

    if (i > raw_len)
    {
        return -1; // type tag string padding overflows the raw buffer (should be impossible)
    }

    msg->idx_args = i; // arguments start after type tag string padding

    i += 4 * (strlen((char *)msg->msg_data + msg->idx_type_tag)); // move i to the end of the arguments, each float is 4 bytes long
    if (i > raw_len)
    {
        return -1; // arguments overflow the raw buffer (should be impossible, malformed message)
    }

    msg->msg_size = i; // now we know the actual size of the message, from start to end of arguments

    return msg->msg_size;
}