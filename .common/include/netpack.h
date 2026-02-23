#ifndef NETPACK_H
#define NETPACK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "my_types.h"

#define CMD_CODE_LEN 2
#define CMD_LEN_LEN 4
#define CMD_HEADER_LEN (CMD_CODE_LEN + CMD_LEN_LEN)

typedef enum
{
    CMD_RESERVED = 0x0000,
    
	CMD_WIN = 0x0041,
	CMD_ESP = 0x0048,
	CMD_STM = 0x0049,
	
	// one-direction-data devices
	CMD_DPL = 0x00A1, // display
	CMD_MIC = 0x00A2, // microphone
	CMD_SPK = 0x00A3, // speaker
	
} cmdcode_t;

typedef const char* msg_t;
typedef u32 msglen_t;

typedef struct
{
    cmdcode_t cmd_code;
    msglen_t payload_len;
} cmd_header_t;

/**
    Check if msg CMD format is matched.
    \param rx_msg byte array received from link
    \param rx_msg_len length of rxed array
    \param payload_len container with read payload length (from header field)
    \param payload_begin container for start byte of msg payload
    \return CMD_RESERVED if not CMD format or cmd code not found else cmd code value
*/
cmdcode_t is_pack(msg_t rx_msg, msglen_t rx_msg_len, msg_t* payload_begin, msglen_t* payload_len);

/**
    Pack data into CMD format packet.
    \param cmd_code command code
    \param payload data to pack
    \param payload_len length of payload data
    \param out_buffer output buffer (must be at least CMD_HEADER_LEN + payload_len bytes)
    \return total packet size (CMD_HEADER_LEN + payload_len) or 0 on error
*/
msglen_t pack_packet(cmdcode_t cmd_code, const void* payload, msglen_t payload_len, char* out_buffer);

#ifdef __cplusplus
}
#endif

#endif