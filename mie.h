#ifndef MIE_H

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

void mie_thread_init(void);
// must be called at least once from any thread wishing to use this API.
// subsequent calls are no-ops.

int mie_compile_thicc(const struct thicchar*, int num_chars);
int mie_compile_graycode(const char* utf8src, int num_bytes);
// these return program id, or -1 on failure (see mie_error())

void mie_program_free(int program_index);

//struct location mie_error_location(void); // XXX or range?

void vmie_reset(int program_index);
int vmie_run(void);
int vmie_get_stack_height(void);
struct val vmie_val(int i);
void vmie_dump_stack(void);
void vmie_dump_val(struct val);

const char* mie_error(void);

void mie_selftest(void);

#define MIE_H
#endif
