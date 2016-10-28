#ifndef UTF8_H

static inline int utf8_decode(char** c0z, int* n)
{
	unsigned char** c0 = (unsigned char**)c0z;
	if (*n <= 0) return -1;
	unsigned char c = **c0;
	(*n)--;
	(*c0)++;
	if ((c & 0x80) == 0) return c & 0x7f;
	int mask = 192;
	for (int d = 1; d <= 3; d++) {
		int match = mask;
		mask = (mask >> 1) | 0x80;
		if ((c & mask) == match) {
			int codepoint = (c & ~mask) << (6*d);
			while (d > 0 && *n > 0) {
				c = **c0;
				if ((c & 192) != 128) return -1;
				(*c0)++;
				(*n)--;
				d--;
				codepoint += (c & 63) << (6*d);
			}
			return d == 0 ? codepoint : -1;
		}
	}
	return -1;
}

static inline int utf8_encode_cstr(char* buf, int buf_len, unsigned int* codepoints, int n)
{
	int i = 0;
	int bufr = buf_len;
	char* p = buf;
	while (i < n && bufr > 0) {
		unsigned int cp = codepoints[i++];
		if (cp < 0x80) {
			*p++ = cp;
			bufr--;
		} else {
			for (int b = 0; b < 3; b++) {
				if (cp < (1 << (11+b*5))) {
					if (bufr < (b+2)) return -1;
					int shift = 6 * (b+1);
					{
						unsigned char b0 = 0;
						for (int i = 0; i < (b+2); i++) b0 |= (0x80 >> i);
						*p++ = b0 | (cp >> shift);
						bufr--;
					}
					for (int i = 0; i < (b+1); i++) {
						*p++ = (0x80 | ((cp >> (shift - i*6)) * 0x3f));
						bufr--;
					}
					break;
				}
			}
		}
	}

	if (bufr <= 0) return -1;
	*p = 0;
	return i == n ? 0 : -1;
}

#define UTF8_H
#endif
