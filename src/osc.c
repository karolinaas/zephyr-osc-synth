#include "osc.h"

uint8_t test_buff[] = {'/', 'm', 'e', 's', 's', 'a', 'g', 'e', '\0', ',', 'i', '\0', '\0', 69, '\0', '\0', '\0', '\0'};
//                               ^1^2  ^3
int osc_parse_message(osc_msg *msg, uint8_t *msg_buff, uint32_t buff_len)
{
    msg->msg_buff = msg_buff;

    int32_t i = 0;

    // iterace přes buffer, dokud nenajdeme ',', což značí začátek OSC type tag stringu ^1
    for (i; msg_buff[i] != ','; i++)
    {
        if (i >= buff_len)
        {
            return -1; //chybí type tag string
        }
    }

    msg->idx_type_tag = msg_buff + i + 1;

    //iterace zkkrz type targ string dokud nenajdeme null char ^2
    for (i; msg_buff[i] != '\0'; i++)
    {
        if (i >= buff_len)
        {
            return -1; //type tag string not null terminated
        }
    }

    //iterace zkrz type type tag string padding null chary dokud nenajdeme argument ^3
    for (i; msg_buff[i] == '\0'; i++)
    {
        if (i >= buff_len)
        {
            return -1; //chybi argumenty
        }
    }

    msg->idx_args = msg_buff + i;

    return 0;
}