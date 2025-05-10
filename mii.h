#ifndef MII_H

#include <stdint.h>

#include "gig.h"

void mii_init(void); // TODO mii_thread_init() instead? (thread-local stuff)

int mii_compile_thicc(const struct thicchar*, int num_chars);
int mii_compile_graycode(const char* utf8src, int num_bytes);
// these return program id, or -1 on failure (see mii_error())

void mii_program_free(int program_index);

//struct location mii_error_location(void); // XXX or range?

void vmii_reset(int program_index);
int vmii_run(void);
float vmii_floatval(int i);

const char* mii_error(void);

#define MII_H
#endif
