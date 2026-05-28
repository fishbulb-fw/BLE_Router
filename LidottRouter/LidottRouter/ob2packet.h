#ifndef OB2PACKET_H
#define OB2PACKET_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Packet Format:
 * [ 1 byte:   Sentinel indicates new packet, always 0x02           ]
 * [ 1 byte:   Packet type (0x01 or 0x02)                           ]
 * [ 1 byte:   Length of packet including Header, Payload and CRC   ]
 * [ Length - 5 bytes: Payload                                      ]
 * [ 2 bytes:  CRC16 (matching modbus specification but big endian) ]
 */

#define OB2PACKET_START_SENTINEL (2)
#define OB2PACKET_HEAD_SIZE (3)
#define OB2PACKET_TAIL_SIZE (2)
#define OB2PACKET_HEADANDTAIL_SIZE (OB2PACKET_HEAD_SIZE + OB2PACKET_TAIL_SIZE)

enum ob2packet_payloadtypes {
	OB2PACKET_PAYLOAD_JSON = 1,
	OB2PACKET_PAYLOAD_BINARY = 2
};

uint16_t ob2packet_calculateCRC(const uint8_t * buffer, uint8_t bufferSize);

enum ob2packet_cbexrecv_error_codes {
	OB2PACKET_START_SENTINEL_NOT_FOUND = -1,
	OB2PACKET_TOO_SMALL = -2,
	OB2PACKET_UNKNOWN_HEADER = -3,
	OB2PACKET_HEADERLEN_SAYS_TOO_SMALL = -4,
	OB2PACKET_BUFFER_TOO_SMALL = -5,
	OB2PACKET_CRC_MISMATCH = -6
};

/*
 * Mutate buffer at buf_ptr and create a packet that conforms
 * to the "Packet Format" described earlier.
 * buf_ptr should be big enough to hold the payload, header
 * and CRC.
 * len bytes at buf_ptr will be moved down to make space for
 * the header.
 * len is the size of the payload.
 */
size_t ob2packet_makepacket(void * buf_ptr, size_t len);

/*
 * Mutate buffer at buf_ptr and create a packet that conforms
 * to the "Packet Format" described earlier.
 * Assume that the type is set and there is space for the header
 * at the start of the buffer before the payload.
 */
size_t ob2packet_makepacket_inplace(void * buf_ptr, size_t len);

/*
 * buf_ptr is a complete packet, header and checksum included.
 */
int ob2packet_validate(const void * buf_ptr, size_t buf_len);

#ifdef __cplusplus
}
#endif

#endif /* OB2PACKET_H */
