#include <stdint.h>

typedef struct osc_msg {
  uint8_t *idx_type_tag;
  uint8_t *idx_args;
  uint8_t *msg_buff;
  uint32_t length;
} osc_msg;

int osc_parse_message(osc_msg *msg, uint8_t *msg_buff, uint32_t buff_len);