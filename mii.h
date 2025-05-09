#ifndef MII_H

#include <stdint.h>

#include "gig.h"

struct vmii {
	int has_compile_error;
	const char* compile_error;
	struct location compile_error_location;
};

void mii_init(void);
void mii_compile_thicc(struct vmii*, const struct thicchar*, int num_chars);
void mii_compile_graycode(struct vmii*, const char* utf8src, int num_bytes);

#define MII_H
#endif
