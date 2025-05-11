// cc -O0 -g -Wall allocator.c test_allocator.c -o _test_allocator -lm && ./_test_allocator

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include "allocator.h"

struct rng {
	uint32_t z;
	uint32_t w;
};

static inline uint32_t rng_uint32(struct rng* rng)
{
	rng->z = 36969 * (rng->z & 65535) + (rng->z>>16);
	rng->w = 18000 * (rng->w & 65535) + (rng->w>>16);
	return (rng->z<<16) + rng->w;
}

static inline struct rng make_rng(uint32_t seed)
{
	return ((struct rng){
		.z = 654654 + seed,
		.w = 7653234 + seed * 69069,
	});
}

static void fill_pattern(void* mem, size_t n, uint32_t seed)
{
	assert(((n&3)==0) && "we only support multiples-of-4 sizes");
	struct rng rng = make_rng(seed);
	uint32_t* p = mem;
	for (size_t i=0; i<(n>>2); ++i) *(p++) = rng_uint32(&rng);
}

static void validate_pattern(void* mem, size_t n, uint32_t seed)
{
	assert(((n&3)==0) && "we only support multiples-of-4 sizes");
	struct rng rng = make_rng(seed);
	uint32_t* p = mem;
	for (size_t i=0; i<(n>>2); ++i) {
		const uint32_t actual = *(p++);
		const uint32_t expected = rng_uint32(&rng);
		if (actual != expected) {
			printf("at pos %zd/%zd, expected %.8x but found %.8x\n", i*4, n, expected, actual);
			abort();
		}
	}
}

//#define USE_REAL_REALLOC
// define USE_REAL_REALLOC to use real realloc(). not all tests support this
// (#ifndef them out). useful for determining if a bug is in your test code or
// in the code we're supposed to test (allocator.c) :-)

#ifdef USE_REAL_REALLOC
#define REALLOC(PTR,SZ) realloc(PTR,SZ)
#else
#define REALLOC(PTR,SZ) (a.fn_realloc(a.allocator_context, (PTR), (SZ)))
#endif

#define VERBOSE (1)

static void test_scratch_allocator(void)
{
	#ifndef USE_REAL_REALLOC
	{
		// test simple allocation and abort longjmp
		struct allocator a={0};
		struct scratch_context_header* h = init_scratch_allocator(&a, 1<<8);
		assert(h->allocated == 0);
		assert(h->capacity == 1<<8);
		h->has_abort_jmp_buf = 1;
		const int jmpval = setjmp(h->abort_jmp_buf);
		if (jmpval == 0) {
			const size_t s0 = 1<<7;
			void* m0 = REALLOC(NULL, s0);
			assert(m0 != NULL);
			fill_pattern(m0, s0, 42);
			validate_pattern(m0, s0, 42);
			assert(h->allocated == (sizeof(size_t) + s0)); // NOTE includes size_t allocation header
			// this allocation fails, and falls back to setjmp() above
			a.fn_realloc(a.allocator_context, NULL, 1<<8);
			assert(!"unreachable");
		} else {
			assert(jmpval == 1);
		}
		if (VERBOSE) printf("simple test: OK\n");
	}
	#endif

	#ifndef USE_REAL_REALLOC
	{
		// test that allocations can be downsized and upsized within their
		// capacity without the overall allocation counter going up, and
		// without disturbing the data.
		struct allocator a={0};
		struct scratch_context_header* h = init_scratch_allocator(&a, 1<<15);
		const size_t s0 = 1234*4;
		for (int pass=0; pass<5; ++pass) {
			void* m0 = REALLOC(NULL, s0);
			const int seed = 101+pass;
			fill_pattern(m0, s0, seed);
			validate_pattern(m0, s0, seed);
			size_t s1 = s0;
			const int N0 = h->allocated;
			for (int i=0; i<500; ++i) {
				m0 = REALLOC(m0, s1);
				validate_pattern(m0, s1, seed);
				s1 -= 8;
			}
			const int N1 = h->allocated;
			assert(N1==N0);
			for (int i=0; i<500; ++i) {
				m0 = REALLOC(m0, s1);
				validate_pattern(m0, s1, seed);
				s1 += 8;
			}
			const int N2 = h->allocated;
			assert(N2==N1);
		}
		if (VERBOSE) printf("shrink test: OK\n");
	}
	#endif

	{
		// test growing allocations
		struct allocator a={0};
		struct scratch_context_header* h = init_scratch_allocator(&a, 1<<20);
		const size_t s0 = 3333*4;
		for (int pass=0; pass<3; ++pass) {
			const int seed = 1001+pass;
			void* m0 = NULL;
			for (int m=0; m<4; ++m) {
				const size_t s1 = (1+m)*s0;
				m0 = REALLOC(m0, s1);
				if (VERBOSE>=2) printf("pass=%d m=%d a=%zd\n",pass,m,h->allocated);
				fill_pattern(m0+(m*s0), s0, seed+m*303);
				for (int r=0; r<=m; ++r) {
					if (VERBOSE>=2) printf("validate %d/%d ... ", r,m);
					validate_pattern(m0+(r*s0), s0, seed+r*303);
					if (VERBOSE>=2) printf("OK\n");
				}
			}
		}
		if (VERBOSE) printf("grow test: OK\n");
	}

	{
		struct allocator a={0};
		struct scratch_context_header* h = init_scratch_allocator(&a, 1<<28);
		(void)h;
		struct rng rng = make_rng(999);

		#define N (1<<8)
		static void* pointers[N];
		static size_t sizes[N];
		for (int i=0; i<N; ++i) {
			const size_t s = 32 + (rng_uint32(&rng) & 0xfffc);
			void* m = REALLOC(NULL,s);
			sizes[i] = s;
			pointers[i] = m;
			fill_pattern(m,s,i);
		}
		for (int i=0; i<N; i+=3) {
			void* newptr = REALLOC(pointers[i],sizes[i]*2);
			assert((newptr != pointers[i]) && "expected moved ptr");
			pointers[i] = newptr;
		}
		for (int i=0; i<N; ++i)  validate_pattern(pointers[i],sizes[i],i);
		#undef N
		if (VERBOSE) printf("random test: OK\n");
	}
}

int main(int argc, char** argv)
{
	test_scratch_allocator();
	return EXIT_SUCCESS;
}
