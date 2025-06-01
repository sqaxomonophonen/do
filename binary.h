#ifndef BINARY_H

#include <stdint.h>

// little-endian uint64 decode
static inline uint64_t leu64_decode(const uint8_t* data)
{
	uint64_t value=0;
	for (int i=0; i<8; ++i) value += ((uint64_t)data[i] << (i*8));
	return value;
}

// little-endian uint64 encode
static inline void leu64_encode(uint8_t* data, uint64_t value)
{
	for (int i=0; i<8; ++i) data[i] = ((value >> (i*8)) & 0xff);
}

static inline uint32_t leu32_pdecode(const uint8_t** pp)
{
	uint32_t val =
		  (*pp[0])
		+ (*pp[1] << 8)
		+ (*pp[2] << 16)
		+ (*pp[3] << 24);
	(*pp) += 4;
	return val;
}

static inline void leu16_pencode(uint8_t** pp, uint16_t value)
{
	(*pp)[0] =  value     & 0xff;
	(*pp)[1] = (value>>8) & 0xff;
	(*pp) += 2;
}

static inline uint16_t leu16_pdecode(const uint8_t** pp)
{
	uint16_t val =
		  ((*pp)[0])
		+ ((*pp)[1] << 8);
	(*pp) += 2;
	return val;
}

static inline void leu64_pencode(uint8_t** pp, uint64_t value)
{
	for (int i=0; i<8; ++i) {
		*((*pp)++) = ((value >> (i*8)) & 0xff);
	}
}

static inline uint64_t leu64_pdecode(const uint8_t** pp)
{
	uint64_t value = leu64_decode(*pp);
	*(pp) += 8;
	return value;
}

#define BINARY_H
#endif
