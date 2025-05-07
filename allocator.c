#include <assert.h>
#include <stdio.h> // XXX
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "allocator.h"

static void* realloc_wrapper(void* allocator_context, void* ptr, size_t sz)
{
	(void)allocator_context;
	return realloc(ptr, sz);
}

static void free_wrapper(void* allocator_context, void* ptr)
{
	(void)allocator_context;
	free(ptr);
}

static struct allocator system_allocator = {
	.fn_realloc = realloc_wrapper,
	.fn_free    = free_wrapper,
};

struct allocator* get_system_allocator(void)
{
	assert((system_allocator.fn_realloc!=NULL) && (system_allocator.fn_free != NULL));
	return &system_allocator;
}

#define SCRATCH_HEADER_SIZE CACHE_ALIGN(sizeof(struct scratch_context_header))

struct scratch_allocation_header {
	size_t capacity;
};

static struct scratch_allocation_header* get_scratch_header(void* ptr)
{
	return ((struct scratch_allocation_header*)ptr) - 1;
}

static void* scratch_malloc(void* allocator_context, size_t sz)
{
	struct scratch_context_header* h = allocator_context;
	const size_t hsz = sizeof(struct scratch_allocation_header);
	assert(IS_POWER_OF_TWO(hsz) && "the following round-up code only works if size is a power-of-two");
	h->allocated = (h->allocated+hsz-1) & ~(hsz-1);
	const size_t required = h->allocated + hsz + sz;

	if (required > h->capacity) {
		// handle allocation failure
		if (h->has_abort_jmp_buf) {
			longjmp(h->abort_jmp_buf, 0);
		} else {
			assert(!"scratch_malloc(): allocation request exceeds capacity! (maybe increase capacity, or use abort_jmp_buf");
		}
		assert(!"unreachable");
	}

	void* ptr = (void*)h + SCRATCH_HEADER_SIZE + h->allocated;
	h->allocated = required;
	return ptr;
}

static void* scratch_realloc(void* allocator_context, void* ptr, size_t sz)
{
	if (ptr == NULL) {
		return scratch_malloc(allocator_context, sz);
	} else {
		const size_t cap = get_scratch_header(ptr)->capacity;
		if (sz <= cap) {
			return ptr;
		} else {
			void* ptr2 = scratch_malloc(allocator_context, sz);
			memcpy(ptr2, ptr, cap);
			return ptr2;
		}
	}
}

static void scratch_free(void* allocator_context, void* ptr)
{
	(void)allocator_context; (void)ptr;
	// scratch allocators ignore frees
}

struct scratch_context_header* init_scratch_allocator(struct allocator* a, size_t capacity)
{
	memset(a, 0, sizeof *a);
	a->allocator_context = malloc(SCRATCH_HEADER_SIZE + capacity);
	memset(a->allocator_context, 0, SCRATCH_HEADER_SIZE);
	((struct scratch_context_header*)a->allocator_context)->capacity = capacity;
	a->fn_realloc = scratch_realloc;
	a->fn_free = scratch_free;
	return a->allocator_context;
}
