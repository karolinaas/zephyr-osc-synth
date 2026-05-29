#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#define OSC_MSG_MAX_SIZE 512 // bytes

#define OSC_BUNDLE_PREFIX "#bundle"
#define OSC_TIME_TAG_IMMEDIATELY 0x0000000000000001ULL // The time tag value consisting of 63 zero bits followed by a one in the least signifigant bit is a special case meaning “immediately.”

typedef struct osc_msg
{
    uint8_t msg_data[OSC_MSG_MAX_SIZE];

    uint32_t idx_addr_pattern;
    uint32_t idx_type_tag;
    uint32_t idx_args;

    size_t msg_size;
} osc_msg;

typedef void (*osc_bundle_handler)(osc_msg *msg, void *cb_data);

int osc_parse_message(osc_msg *msg, uint8_t *raw_buff, size_t raw_len);
int osc_parse_bundle(uint8_t *raw_buff, size_t raw_len, osc_bundle_handler handler, void *cb_data);

uint8_t osc_get_arg_type(osc_msg *msg, uint32_t arg_idx);
float osc_get_arg_float32(osc_msg *msg, uint32_t arg_idx);
int32_t osc_get_arg_int32(osc_msg *msg, uint32_t arg_idx);
const char *osc_get_arg_string(osc_msg *msg, uint32_t arg_idx);
const uint8_t *osc_get_arg_blob(osc_msg *msg, uint32_t arg_idx, size_t *out_blob_size);

uint64_t osc_bundle_get_time_tag(uint8_t *raw_buff);

// accessors for pointer convenience
static inline uint8_t *osc_addr_pattern(osc_msg *msg)
{
    return &msg->msg_data[msg->idx_addr_pattern];
}

static inline uint8_t *osc_type_tag_str(osc_msg *msg)
{
    return &msg->msg_data[msg->idx_type_tag];
}

static inline uint8_t *osc_args(osc_msg *msg)
{
    return &msg->msg_data[msg->idx_args];
}

static inline uint32_t osc_num_args(osc_msg *msg)
{
    return strlen((char *)osc_type_tag_str(msg));
}

static inline bool osc_arg_type_valid(uint8_t type_tag)
{
    return type_tag == 'f' || type_tag == 'i' || type_tag == 's' || type_tag == 'b';
}
