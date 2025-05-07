#ifndef MII_H

#include <stdint.h>

struct thicchar {
	int32_t codepoint;
	uint8_t color[4];
};

struct vmii {
	const char* compile_error;
};

void mii_init(void);
void mii_compile_thicc(struct vmii*, const struct thicchar*, int num_chars);
void mii_compile_graycode(struct vmii*, const char* utf8src, int num_bytes);

#define MII_H
#endif
