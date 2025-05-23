#ifndef MIE_H

#include <stdint.h>
#include <setjmp.h> // (!)

#include "gig.h"
#include "allocator.h"

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

int mie_compile_colorcode(const struct colorchar*, int num_chars);
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

void mie_begin_scrallox(void);
void mie_end_scrallox(void);
jmp_buf* mie_prep_scrallox_jmp_buf_for_out_of_memory(void);
struct allocator* mie_borrow_scrallox(void);
void mie_scrallox_save(int);
void mie_scrallox_restore(int);
void mie_scrallox_stats(size_t* out_allocated, size_t* out_capacity);
// functions for interfacing with mie.c's scratch allocator. notes:
//  - the compiler and vm (vmie) uses this scratch allocator a lot. see mie.c /
//    allocator.h / grep for more discussion.
//  - you can only use the allocator, and by extension the compiler/vm, between
//    mie_begin_scrallox() and mie_end_scrallox() calls (mie.c never calls
//    these on its own)
//  - between begin/end you can "borrow" the allocator with
//    mie_borrow_scrallox() and make your own allocations with it. these
//    allocations are only valid between begin/end. it's most likely a lot
//    safer to call mie_borrow_scrallox() just-in-time when you need it instead
//    of holding on to the allocator.
//  - mie_prep_scrallox_jmp_buf_for_out_of_memory() is used together with
//    setjmp() (look it up!) to handle out-of-memory errors that can happen
//    because the scratch allocator is initialized with a fixed amount of
//    memory, which is all it can give you, and it never allocates more on
//    demand. calling this function also instructs the allocator to actually
//    use the jmp_buf (otherwise it just panics on out-of-memory) so only call
//    it if you're using it. suggested/example usage:
//      mie_begin_scrallox();
//      if (0 == setjmp(*mie_prep_scrallox_jmp_buf_for_out_of_memory())) {
//        // compile, use VM, mie_borrow_scrallox(), etc..
//      } else {
//        // out of memory! setjmp() continues from here, so do any clean-up /
//        // error handling here
//      }
//      mie_end_scrallox();
//    NOTE that mie_end_scrallox() must be called after mie_begin_scrallox()
//    even in the case of errors. also, mie_begin_scrallox() always clears the
//    jmp_buf behavior, so there's no need for a "stop using jmp_buf" function.
//  - TODO save/restore docs?
//  - all these precautions and rules are here to prevent memory corruption
//    bugs from hell, while the scratch allocator in turn facilitates simpler
//    (GC-like) memory management in the compiler/vm, and also speeds things up
//    considerably. you know, traaadeofffs!

#define MIE_H
#endif
