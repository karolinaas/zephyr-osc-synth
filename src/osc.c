#include "osc.h"

#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(osc, LOG_LEVEL_DBG);

// helper function
static inline uint32_t osc_move4_up(uint32_t idx)
{
    return (idx + 3) & ~0x3; // move up to next multiple of 4
}

static size_t osc_arg_offset(osc_msg *msg, uint32_t arg_idx)
{
    size_t offset = 0; // relative to osc_args(msg)
    const uint8_t *type_tag_str = osc_type_tag_str(msg);

    for (uint32_t i = 0; i < arg_idx; i++)
    {
        uint8_t type_tag = type_tag_str[i];

        if (!osc_arg_type_valid(type_tag))
        {
            LOG_WRN("Invalid argument type '%c' at index %d!", type_tag, i);
            return 0; // should be impossible since we validate this during parsing
        }

        switch (type_tag)
        {
            case 'f':
            {
                offset += 4;

                break;
            }
            case 'i':
            {
                offset += 4;

                break;
            }
            case 's':
            {
                offset += strlen((char *)osc_args(msg) + offset) + 1; // string length + null terminator
                offset = osc_move4_up(offset); // strings are padded to 4 bytes

                break;
            }
            case 'b':
            {
                if (msg->idx_args + offset + 4 > msg->msg_size)
                {
                    LOG_WRN("Blob size field overflows message buffer! Malformed message.");
                    return 0;
                }

                int32_t blob_size; // int32 defined in osc spec, but should always be positive since it's a size
                memcpy(&blob_size, osc_args(msg) + offset, 4);
                blob_size = sys_be32_to_cpu(blob_size); // convert from network big-endian to cpu endian

                offset += 4 + blob_size; // size field + blob data
                offset = osc_move4_up(offset); // blobs are padded to 4 bytes

                break;
            }
        }
    }

    return offset;
}

int osc_parse_message(osc_msg *msg, uint8_t *raw_buff, size_t raw_len)
{
    // initial bound check
    if (raw_len > OSC_MSG_MAX_SIZE)
    {
        return -1; // raw buffer too long
    }

    /* check for message size prefix */
    if (raw_len >= 4 && raw_buff[0] != '/')
    {
        int32_t prefix_size;
        memcpy(&prefix_size, raw_buff, 4);
        prefix_size = sys_be32_to_cpu(prefix_size);

        if (prefix_size < 0 || (size_t)prefix_size > raw_len - 4)
        {
            LOG_WRN("Invalid prefix size %d! Message ignored.", prefix_size);
            return -1;
        }

        raw_buff += 4; // skip prefix size field
        raw_len = (size_t)prefix_size; // now raw_len is the actual size of the OSC message without the prefix
    }

    memcpy(msg->msg_data, raw_buff, raw_len);
    msg->idx_addr_pattern = 0;

    uint32_t i = 0;

    if (raw_buff[0] != '/')
    {
        LOG_WRN("Address pattern does not start with '/'! Malformed message. Message ignored.");
        return -1;
    }

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
        if (!osc_arg_type_valid(raw_buff[j]))
        {
            LOG_WRN("Unsupported argument type '%c'! Message ignored.", raw_buff[j]);
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

    for (uint32_t j = 0; j < osc_num_args(msg); j++)
    {
        uint8_t type_tag = osc_type_tag_str(msg)[j];

        switch (type_tag)
        {
            case 'f':
            {
                i += 4;

                break;
            }
            case 'i':
            {
                i += 4;

                break;
            }
            case 's':
            {
                i += strnlen((char *)osc_args(msg) + i, raw_len - i) + 1;// string length + null terminator
                i = osc_move4_up(i); // strings are padded to 4 bytes

                if (i > raw_len)
                {
                    LOG_WRN("String argument overflows the raw buffer! Malformed message. Message ignored.");
                    return -1;
                }

                break;
            }
            case 'b':
            {
                if (i + 4 > raw_len)
                {
                    LOG_WRN("Blob size field overflows message buffer! Malformed message.");
                    return -1;
                }

                int32_t blob_size; // int32 defined in osc spec, but should always be positive since it's a size
                memcpy(&blob_size, osc_args(msg) + i, 4);
                blob_size = sys_be32_to_cpu(blob_size); // convert from network big-endian to cpu endian

                if (blob_size < 0 || (uint32_t)blob_size > raw_len - i - 4)
                {
                    LOG_WRN("Blob size %d invalid! Malformed message. Message ignored.", blob_size);
                    return -1;
                }

                i += 4 + blob_size; // size field + blob data
                i = osc_move4_up(i); // blobs are padded to 4 bytes

                break;
            }
        }
    }
    
    if (i > raw_len)
    {
        LOG_WRN("Arguments overflow the raw buffer! Should be impossible, malformed message. Message ignored.");
        return -1;
    }

    msg->msg_size = i; // now we know the actual size of the message, from start to end of arguments

    return msg->msg_size;
}

uint8_t osc_get_arg_type(osc_msg *msg, uint32_t arg_idx)
{
    if (arg_idx >= osc_num_args(msg))
    {
        LOG_WRN("Argument index %d out of bounds (num args: %d)!", arg_idx, osc_num_args(msg));
        return 0;
    }

    return osc_type_tag_str(msg)[arg_idx];
}

float osc_get_arg_float32(osc_msg *msg, uint32_t arg_idx)
{
    if (arg_idx >= osc_num_args(msg))
    {
        LOG_WRN("Argument index %d out of bounds (num args: %d)!", arg_idx, osc_num_args(msg));
        return 0.0f; // out of bounds, maybe should handle differently?
    }

    if (osc_get_arg_type(msg, arg_idx) != 'f')
    {
        LOG_WRN("Argument index %d is not of type float!", arg_idx);
        return 0.0f;
    }

    uint32_t raw;
    float value;
    size_t offset = osc_arg_offset(msg, arg_idx);

    memcpy(&raw, osc_args(msg) + offset, 4);
    raw = sys_be32_to_cpu(raw); // convert from network big-endian to cpu endian

    memcpy(&value, &raw, 4);

    return value;
}

int32_t osc_get_arg_int32(osc_msg *msg, uint32_t arg_idx)
{
    if (arg_idx >= osc_num_args(msg))
    {
        LOG_WRN("Argument index %d out of bounds (num args: %d)!", arg_idx, osc_num_args(msg));
        return 0;
    }

    if (osc_get_arg_type(msg, arg_idx) != 'i')
    {
        LOG_WRN("Argument index %d is not of type int32!", arg_idx);
        return 0;
    }

    uint32_t raw;
    int32_t value;
    size_t offset = osc_arg_offset(msg, arg_idx);

    memcpy(&raw, osc_args(msg) + offset, 4);
    value = sys_be32_to_cpu(raw); // convert from network big-endian to cpu endian

    return value;
}

const char *osc_get_arg_string(osc_msg *msg, uint32_t arg_idx)
{
    if (arg_idx >= osc_num_args(msg))
    {
        LOG_WRN("Argument index %d out of bounds (num args: %d)!", arg_idx, osc_num_args(msg));
        return 0;
    }

    if (osc_get_arg_type(msg, arg_idx) != 's')
    {
        LOG_WRN("Argument index %d is not of type string!", arg_idx);
        return 0;
    }

    size_t offset = osc_arg_offset(msg, arg_idx);

    if (msg->idx_args + offset >= msg->msg_size)
    {
        LOG_WRN("String argument offset overflows message buffer! Malformed message.");
        return 0;
    }

    return (char *)osc_args(msg) + offset;
}

const uint8_t *osc_get_arg_blob(osc_msg *msg, uint32_t arg_idx, size_t *out_blob_size)
{
    if (arg_idx >= osc_num_args(msg))
    {
        LOG_WRN("Argument index %d out of bounds (num args: %d)!", arg_idx, osc_num_args(msg));
        return 0;
    }

    if (osc_get_arg_type(msg, arg_idx) != 'b')
    {
        LOG_WRN("Argument index %d is not of type blob!", arg_idx);
        return 0;
    }

    size_t offset = osc_arg_offset(msg, arg_idx);
    int32_t blob_size;

    memcpy(&blob_size, osc_args(msg) + offset, 4);
    blob_size = sys_be32_to_cpu(blob_size);

    if (out_blob_size)
    {
        *out_blob_size = blob_size;
    }

    return osc_args(msg) + offset + 4; // data starts after the 4 byte size prefix
}