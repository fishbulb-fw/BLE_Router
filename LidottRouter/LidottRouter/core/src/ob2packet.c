#include "ob2packet.h"

uint16_t ob2packet_calculateCRC(const uint8_t * buffer, uint8_t bufferSize) {
	uint16_t temp, flag;
	temp = 0xffff;
	for (uint8_t i = 0; i < bufferSize; i++) {
		temp = temp ^ (uint16_t) buffer[i];
		for (uint8_t j = 1; j <= 8; j++) {
			flag = temp & 0x0001;
			temp >>= 1;
			if (flag) {
				temp ^= 0xA001;
			}
		}
	}
	return temp;
}

size_t ob2packet_makepacket(void * buf_ptr, size_t len) {
	/* First move len bytes to make space for the header */
	uint8_t * buf = buf_ptr;
	size_t i;
	for (i = (len + OB2PACKET_HEAD_SIZE); i >= OB2PACKET_HEAD_SIZE; i--) {
		buf[i] = buf[i - OB2PACKET_HEAD_SIZE];
	}
	buf[1] = OB2PACKET_PAYLOAD_JSON;
	len = len + OB2PACKET_HEAD_SIZE;
	return ob2packet_makepacket_inplace(buf_ptr, len);
}

size_t ob2packet_makepacket_inplace(void * buf_ptr, size_t len) {
	uint8_t * buf = buf_ptr;
	buf[0] = OB2PACKET_START_SENTINEL;
	buf[2] = (uint8_t)(len + OB2PACKET_TAIL_SIZE);

	/* calculate the CRC over the whole packet, including the type and size */
	uint16_t calc_crc;
	calc_crc = ob2packet_calculateCRC(buf, len);

	buf[len] = (calc_crc >> 8) & 0xff;
	len ++;
	buf[len] = calc_crc & 0xff;
	len ++;

	return len;
}

int ob2packet_validate(const void * buf_ptr, size_t buf_len) {
	uint8_t * buf = (uint8_t *)buf_ptr;
	uint16_t pkt_crc, calc_crc;
	if (buf[0] != OB2PACKET_START_SENTINEL) {
		return OB2PACKET_START_SENTINEL_NOT_FOUND;
	}
	if (!(buf[1] == OB2PACKET_PAYLOAD_JSON || buf[1] == OB2PACKET_PAYLOAD_BINARY)) {
		return OB2PACKET_UNKNOWN_HEADER;
	}
	if (buf[2] != buf_len) {
		return OB2PACKET_TOO_SMALL;
	}
	pkt_crc = buf[buf_len - 2] << 8;
	pkt_crc = pkt_crc | buf[buf_len - 1];
	calc_crc = ob2packet_calculateCRC(buf, buf_len - 2);
	if (pkt_crc != calc_crc) {
		return OB2PACKET_CRC_MISMATCH;
	}
	return buf[1];
}
