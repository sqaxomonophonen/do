#include <stdlib.h>
#include <assert.h>

#include "base64.h"
#include "util.h"

#define BASE64_DIGITS "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
//                     0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF

static inline char encode_base64_digit(unsigned v)
{
	if (v >= 64) assert(!"bad digit");
	return (BASE64_DIGITS)[v];
}

char* base64_encode(char* output, uint8_t* input, int n_bytes)
{
	uint8_t* rp = input;
	char* wp = output;
	int n_bytes_remaining = n_bytes;
	while (n_bytes_remaining >= 3) {
		uint8_t b0 = *(rp)++;
		uint8_t b1 = *(rp)++;
		uint8_t b2 = *(rp)++;
		*(wp++) = encode_base64_digit((b0 >> 2) & 0x3f);
		*(wp++) = encode_base64_digit(((b0 & 0x3) << 4) | ((b1 >> 4) & 0xf));
		*(wp++) = encode_base64_digit(((b1 & 0xf) << 2) | ((b2 >> 6) & 0x3));
		*(wp++) = encode_base64_digit(b2 & 0x3f);
		n_bytes_remaining -= 3;
	}
	switch (n_bytes_remaining) {
	case 0: break;
	case 1: {
		uint8_t b0 = *(rp)++;
		*(wp++) = encode_base64_digit((b0 >> 2) & 0x3f);
		*(wp++) = encode_base64_digit((b0 & 0x3) << 4);
		*(wp++) = '=';
		*(wp++) = '=';
	} break;
	case 2: {
		uint8_t b0 = *(rp)++;
		uint8_t b1 = *(rp)++;
		*(wp++) = encode_base64_digit((b0 >> 2) & 0x3f);
		*(wp++) = encode_base64_digit(((b0 & 0x3) << 4) | ((b1 >> 4) & 0xf));
		*(wp++) = encode_base64_digit((b1 & 0xf) << 2);
		*(wp++) = '=';
	} break;
	default: assert(!"bad padding state");
	}
	return wp;
}

static inline int decode_base64_digit(char digit)
{
	if ('A' <= digit && digit <= 'Z') {
		return digit - 'A';
	} else if ('a' <= digit && digit <= 'z') {
		return (digit - 'a') + 26;
	} else if ('0' <= digit && digit <= '9') {
		return (digit - '0') + 52;
	} else if (digit == '+') {
		return 62;
	} else if (digit == '/') {
		return 63;
	}
	return -1;
}

uint8_t* base64_decode_line(uint8_t* output, char* line)
{
	char* rp = line;
	uint8_t* wp = output;
	for (;;) {
		int digits[4] = {0};
		int padding = 0;
		int eol = 0;
		for (int i = 0; i < 4; i++) {
			char c = *(rp++);
			int d = decode_base64_digit(c);
			if (padding == 0 && d != -1) {
				digits[i] = d;
			} else {
				if (padding == 0 && ((c == 0) || (c == '\n') || (c == '\r'))) {
					if (i == 0) {
						eol = 1;
						break;
					} else {
						return NULL;
					}
				} else if (c == '=') {
					// padding char can only occur in position 2,3
					if ((i == 0) || (i == 1)) return NULL;
					padding++;
				} else {
					return NULL;
				}
			}
		}
		if (eol) break;
		switch (padding) {
		case 0: {
			*(wp++) = (digits[0] << 2) | (digits[1] >> 4);
			*(wp++) = ((digits[1] & 0xf) << 4) | (digits[2] >> 2);
			*(wp++) = ((digits[2] & 0x3) << 6) | digits[3];
		} break;
		case 1: {
			*(wp++) = (digits[0] << 2) | (digits[1] >> 4);
			*(wp++) = ((digits[1] & 0xf) << 4) | (digits[2] >> 2);
		} break;
		case 2: {
			*(wp++) = (digits[0] << 2) | (digits[1] >> 4);
		} break;
		default: assert(!"bad state");
		}
	}
	return wp;
}
