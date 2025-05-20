#ifndef ALLOCATOR_H

#include <setjmp.h>
#include <assert.h>

#include "util.h"

struct allocator {
	void* allocator_context;
	void*(*fn_realloc)(void*,void*,size_t);
	void(*fn_free)(void*,void*);
};

extern struct allocator system_allocator;

// scratch (aka arena) allocator: it doesn't support freeing individual
// allocations (fn_free is a no-op); you can only free everything at once
// (free_all_scratch_allocations()). this makes it really simple and fast, and
// useful in cases where you need to do a lot of "work" and throw it away again
// (like a compiler). it has a fixed capacity that you choose on init, and you
// can decide whether you prefer an "abort longjmp" when allocation fails, or
// if a panic is better. allocations are size_t-aligned (e.g. 8B on x86-64)

struct scratch_context_header {
	unsigned in_scope :1;
	int has_abort_jmp_buf;
	jmp_buf abort_jmp_buf; // (200B on my end; linux/x86-64)
	size_t allocated, capacity;
};

static inline struct scratch_context_header* get_scratch_context_header(struct allocator* a)
{
	return a->allocator_context;
}

void init_scratch_allocator(struct allocator*, size_t capacity);
// the returned scratch_context_header can be modified:
//   use free_all_scratch_allocations() to free everything and start over
//   if has_abort_jmp_buf==0 it panics on allocation failure
//   or set has_abort_jmp_buf=1 and setjmp(abort_jmp_buf) from a recovery point


static inline void begin_scratch_allocator(struct allocator* a)
{
	struct scratch_context_header* h = get_scratch_context_header(a);
	assert((!h->in_scope) && "bad scratch allocation sequence: begin called again inside begin/end scope?");
	assert(h->allocated == 0);
	h->in_scope = 1;
}

static inline void end_scratch_allocator(struct allocator* a)
{
	struct scratch_context_header* h = get_scratch_context_header(a);
	assert((h->in_scope) && "bad scratch allocation sequence: end called outside of begin/end scope?");
	h->in_scope = 0;
	h->allocated = 0;
}

#if 0
static inline void free_all_scratch_allocations(struct scratch_context_header* h)
{
	h->allocated = 0;
}
#endif

void* get_scratch_memory_ptr(struct scratch_context_header*);

#define ALLOCATOR_H
#endif
