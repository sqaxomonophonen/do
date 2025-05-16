#ifndef MIE_H

#include <stdint.h>

#include "gig.h"

#define LIST_OF_VAL_TYPES \
	X(VAL_NIL) \
	X(VAL_INT) \
	X(VAL_FLOAT) \
	X(VAL_STR) \
	X(VAL_ARR) \
	X(VAL_MAP) \
	X(VAL_I32ARR) \
	X(VAL_F32ARR) \
	X(VAL_MIE)

enum val_type {
	#define X(ENUM) ENUM,
	LIST_OF_VAL_TYPES
	#undef X
};

#define LIST_OF_STR_COMPONENTS \
	X(CODEPOINT) \
	X(R) \
	X(G) \
	X(B) \
	X(X)
// XXX "Y" in codepoint upper bits?

enum str_component {
	#define X(ENUM) STRCOMP_##ENUM,
	LIST_OF_STR_COMPONENTS
	#undef X
};

struct val {
	int32_t type;
	// don't change `int32_t type` into `enum val_type type` even though it may
	// seem appropriate; compilers may choose any integer type that can
	// represent all the enum values, but mie code depends on this being a
	// normal "i32"
	union {
		int32_t  i32;
		float    f32;
	};
};

static inline struct val typeval(int32_t type, int32_t value) {
	return ((struct val) {
		.type = type,
		.i32 = value,
	});
}

static inline struct val intval(int32_t i) {
	return typeval(VAL_INT, i);
}

static inline struct val floatval(float f) {
	return ((struct val) {
		.type = VAL_FLOAT,
		.f32 = f,
	});
}


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
