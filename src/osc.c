#include "osc.h"

#include <string.h>

uint8_t test_buff[] = {'/', 'm', 'e', 's', 's', 'a', 'g', 'e', '\0', ',', 'i', '\0', '\0', 69, '\0', '\0', '\0', '\0'};
//                               ^1^2  ^3
int osc_parse_message(osc_msg *msg, uint8_t *raw_buff, uint32_t raw_len)
{
    // initial bound check
    if (raw_len > OSC_MSG_MAX_SIZE)
    {
        return -1; // message too long
    }

    memcpy(msg->msg_data, raw_buff, raw_len);
    msg->msg_size = raw_len; // wrong, idk the size yet, i'd need to parse the message properly. this is a problem for future me
    msg->idx_addr_pattern = 0; // rn we hope that the osc message is not prepended with its size lol, pray

    int32_t i = 0;

    // iterace přes buffer, dokud nenajdeme ',', což značí začátek OSC type tag stringu ^1
    for (; raw_buff[i] != ','; i++)
    {
        if (i >= raw_len)
        {
            return -1; // chybí type tag string
        }
    }

    msg->idx_type_tag = i + 1;

    // iterace zkkrz type targ string dokud nenajdeme null char ^2
    for (; raw_buff[i] != '\0'; i++)
    {
        if (i >= raw_len)
        {
            return -1; // type tag string not null terminated
        }
    }

    // iterace zkrz type type tag string padding null chary dokud nenajdeme argument ^3
    for (; raw_buff[i] == '\0'; i++)
    {
        if (i >= raw_len)
        {
            return -1; // chybi argumenty
        }
    }

    msg->idx_args = i;

    return msg->msg_size;
}