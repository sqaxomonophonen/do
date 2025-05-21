#ifndef BINARY_H

// little-endian uint64 decode
static inline uint64_t leu64_decode(const uint8_t* data)
{
	uint64_t v=0;
	for (int i=0; i<8; ++i) v += ((uint64_t)data[i] << (i*8));
	return v;
}

// little-endian uint64 encode
static inline void leu64_encode(uint8_t* data, uint64_t value)
{
	for (int i=0; i<8; ++i) data[i] = ((value >> (i*8)) & 0xff);
}

#define BINARY_H
#endif
