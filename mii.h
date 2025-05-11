#ifndef MII_H

#include <stdint.h>

#include "gig.h"

enum val_type {
	VAL_INT=1,
	VAL_FLOAT,
	VAL_GRAY_STRING,
	VAL_COLOR_STRING,
	VAL_ARR,
	VAL_MAP,
	VAL_I32ARR,
	VAL_F32ARR,
	_VAL_FIRST_DERIVED_TYPE_,
};

struct val {
	int type;
	union {
		int32_t  i32;
		float    f32;
	};
};

void mii_thread_init(void);
// must be called at least once from any thread wishing to use this API.
// subsequent calls are no-ops.

int mii_compile_thicc(const struct thicchar*, int num_chars);
int mii_compile_graycode(const char* utf8src, int num_bytes);
// these return program id, or -1 on failure (see mii_error())

void mii_program_free(int program_index);

//struct location mii_error_location(void); // XXX or range?

void vmii_reset(int program_index);
int vmii_run(void);
int vmii_get_stack_height(void);
struct val vmii_val(int i);

const char* mii_error(void);

void mii_selftest(void);

#define MII_H
#endif
