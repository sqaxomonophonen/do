#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "util.h"
#include "allocator.h"

static void* my_realloc(void* context, void *ptr, size_t size)
{
	assert((context != NULL) && "realloc used without context; perhaps see get_system_allocator()?");
	struct allocator* a = context;
	assert((a->fn_realloc != NULL) && "allocator not initialized?");
	return a->fn_realloc(a->allocator_context, ptr, size);
}

static void my_free(void* context, void *ptr)
{
	assert((context != NULL) && "realloc used without context; perhaps see get_system_allocator()?");
	struct allocator* a = context;
	if (a->fn_free == NULL) return; // no-free allocators are allowed; just do nothing
	a->fn_free(a->allocator_context, ptr);
}

#define STBDS_ASSERT(x)      assert(x);
#define STBDS_REALLOC(c,p,s) my_realloc(c,p,s)
#define STBDS_FREE(c,p)      my_free(c,p)

#define STB_DS_IMPLEMENTATION
#include "stb_ds.h"
