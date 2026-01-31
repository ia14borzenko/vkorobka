#include "netpack.h"

cmdcode_t is_pack(msg_t rx_msg, msglen_t rx_msg_len, msg_t* payload_begin, msglen_t* payload_len)
{
    if (rx_msg_len < CMD_HEADER_LEN)
    {
        return CMD_RESERVED;
    }

    // Читаем заголовок байт за байтом в little-endian порядке
    // CMD Code: 2 байта (little-endian)
    cmdcode_t cmd_code = (cmdcode_t)(rx_msg[0] | (rx_msg[1] << 8));
    
    // Payload length: 4 байта (little-endian)
    msglen_t payload_len_val = (msglen_t)(rx_msg[2] | (rx_msg[3] << 8) | (rx_msg[4] << 16) | (rx_msg[5] << 24));

    if ((cmd_code == CMD_WIN) || (cmd_code == CMD_ESP) || (cmd_code == CMD_STM) 
        || (cmd_code == CMD_DPL) || (cmd_code == CMD_MIC) || (cmd_code == CMD_SPK))
    {
        *payload_begin = rx_msg + CMD_HEADER_LEN;
        *payload_len = payload_len_val;
        return cmd_code;
    }

    return CMD_RESERVED;
}

msglen_t pack_packet(cmdcode_t cmd_code, const void* payload, msglen_t payload_len, char* out_buffer)
{
    if (out_buffer == NULL)
    {
        return 0;
    }

    // Записываем заголовок байт за байтом в little-endian порядке
    // CMD Code: 2 байта (little-endian)
    out_buffer[0] = (char)(cmd_code & 0xFF);
    out_buffer[1] = (char)((cmd_code >> 8) & 0xFF);
    
    // Payload length: 4 байта (little-endian)
    out_buffer[2] = (char)(payload_len & 0xFF);
    out_buffer[3] = (char)((payload_len >> 8) & 0xFF);
    out_buffer[4] = (char)((payload_len >> 16) & 0xFF);
    out_buffer[5] = (char)((payload_len >> 24) & 0xFF);

    // Copy payload if present
    if (payload != NULL && payload_len > 0)
    {
        memcpy(out_buffer + CMD_HEADER_LEN, payload, payload_len);
    }

    return CMD_HEADER_LEN + payload_len;
}