// cc -O0 -g -Wall allocator.c stb_ds.c test_allocator.c -o _test_allocator -lm && ./_test_allocator
// cc -O2 -g -Wall allocator.c stb_ds.c test_allocator.c -o _test_allocator -lm && ./_test_allocator

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <assert.h>

#include "allocator.h"
#include "stb_ds.h"

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
#define REALLOC(PTR,SZ) (A.fn_realloc(A.allocator_context, (PTR), (SZ)))
#endif

#define VERBOSE (1)

static int depth;
static struct allocator A;
static void begin(int size_log2)
{
	init_scratch_allocator(&A, size_log2);
	begin_scratch_allocator(&A);
	++depth;
}

static void end(void)
{
	end_scratch_allocator(&A);
	--depth;
}

static void test_scratch_allocator(void)
{
	#ifndef USE_REAL_REALLOC
	{
		// test simple allocation and abort longjmp
		begin(1<<8);
		struct scratch_context_header* h = get_scratch_context_header(&A);
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
			A.fn_realloc(A.allocator_context, NULL, 1<<8);
			assert(!"unreachable");
		} else {
			assert(jmpval == 1);
		}
		end();
		if (VERBOSE) printf("simple test: OK\n");
	}
	#endif

	#ifndef USE_REAL_REALLOC
	{
		// test that allocations can be downsized and upsized within their
		// capacity without the overall allocation counter going up, and
		// without disturbing the data.
		begin(1<<15);
		struct scratch_context_header* h = get_scratch_context_header(&A);
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
		end();
		if (VERBOSE) printf("shrink test: OK\n");
	}
	#endif

	{
		// test growing allocations
		begin(1<<20);
		struct scratch_context_header* h = get_scratch_context_header(&A);
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
		end();
		if (VERBOSE) printf("grow test: OK\n");
	}

	{
		begin(1<<28);
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
		// double size of every 3rd allocation
		for (int i=0; i<N; i+=3) {
			void* newptr = REALLOC(pointers[i],sizes[i]*2);
			assert((newptr != pointers[i]) && "expected moved ptr");
			pointers[i] = newptr;
		}
		for (int i=0; i<N; ++i)  validate_pattern(pointers[i],sizes[i],i);
		#undef N
		end();
		if (VERBOSE) printf("random test: OK\n");
	}

	{
		// test of stb_ds.dynamic array hashmap with custom scratch allocator
		// (perhaps roll back into stbds_unit_tests() in stb_ds.h?)

		begin(1<<20);

		int* xs = NULL;
		int* ys = NULL;
		int* zs = NULL;
		arrinit(xs, &A);
		arrinit(ys, &A);
		arrinit(zs, &A);
		for (int i=0; i<1000; ++i) {
			arrput(xs, i);
			arrput(ys, 2*i);
			arrput(ys, 2*i+1);
			arrput(zs, 3*i);
			arrput(zs, 3*i-1);
			arrput(zs, 3*i-2);
		}
		for (int i=0; i<1000; ++i) {
			assert(xs[i] == i);
			assert(ys[2*i] == 2*i);
			assert(ys[2*i+1] == 2*i+1);
			assert(zs[3*i] == 3*i);
			assert(zs[3*i+1] == 3*i-1);
			assert(zs[3*i+2] == 3*i-2);
		}

		end();

		if (VERBOSE) printf("stb_ds.h array + scratch allocator test: OK\n");
	}

	{
		// test of stb_ds.h hashmap with custom scratch allocator
		// (perhaps roll back into stbds_unit_tests() in stb_ds.h?)

		begin(1<<24);

		struct value {
			int b1;
			double f;
			int b2;
		};

		struct {
			char* key;
			struct value value;
		}* lut;

		for (int pass1=0; pass1<3; ++pass1) {

			sh_new_strdup_with_context(lut, &A);

			for (int pass2=0; pass2<4; ++pass2) {

				const int N=1000;
				const int SEED=10101 + pass1*33 + pass2*555;
				char keybuf[256];

				{
					struct rng rng = make_rng(SEED);
					for (int i=0; i<N; ++i) {
						uint32_t r0 = rng_uint32(&rng);
						uint32_t r1 = rng_uint32(&rng);
						snprintf(keybuf, sizeof keybuf, "key%u", r0);
						assert(i == shputi(lut, keybuf, ((struct value){
							.f = (double)r1 * 0.0135,
							.b1=1,
							.b2=2,
						})));
					}
				}

				{
					struct rng rng = make_rng(SEED);
					for (int i=0; i<N; ++i) {
						uint32_t r0 = rng_uint32(&rng);
						uint32_t r1 = rng_uint32(&rng);
						snprintf(keybuf, sizeof keybuf, "key%u", r0);
						size_t si = shgeti(lut, keybuf);
						assert(si >= 0);
						struct value value = lut[si].value;
						assert(value.b1 == 1);
						assert(value.b2 == 2);
						assert(value.f == (double)r1 * 0.0135);
					}
				}

				{
					struct rng rng = make_rng(SEED);
					for (int i=0; i<N; ++i) {
						uint32_t r0 = rng_uint32(&rng);
						uint32_t r1 = rng_uint32(&rng);
						snprintf(keybuf, sizeof keybuf, "key%u", r0);
						size_t si = shgeti(lut, keybuf);
						assert(si >= 0);
						struct value value = lut[si].value;
						assert(value.b1 == 1);
						assert(value.b2 == 2);
						assert(value.f == (double)r1 * 0.0135);
					}
				}

				{
					struct rng rng = make_rng(SEED);
					for (int i=0; i<N; ++i) {
						uint32_t r0 = rng_uint32(&rng);
						rng_uint32(&rng);
						if ((i%3)!=1) continue;
						snprintf(keybuf, sizeof keybuf, "key%u", r0);
						assert(shdel(lut, keybuf) == 1);
					}
				}

				{
					struct rng rng = make_rng(SEED);
					for (int i=0; i<N; ++i) {
						uint32_t r0 = rng_uint32(&rng);
						uint32_t r1 = rng_uint32(&rng);
						snprintf(keybuf, sizeof keybuf, "key%u", r0);
						size_t si = shgeti(lut, keybuf);
						if ((i%3)==1) {
							assert(si == -1);
							continue;
						} else {
							assert(si >= 0);
							struct value value = lut[si].value;
							assert(value.b1 == 1);
							assert(value.b2 == 2);
							assert(value.f == (double)r1 * 0.0135);
						}
					}
				}

				const int N2=5555;
				const int SEED2=16161+pass1*91919+pass2*1919191;
				{
					struct rng rng = make_rng(SEED2);
					for (int i=0; i<N2; ++i) {
						uint32_t r0 = rng_uint32(&rng);
						uint32_t r1 = rng_uint32(&rng);
						snprintf(keybuf, sizeof keybuf, "okey%u", r0);
						(void)shputi(lut, keybuf, ((struct value){
							.f = (double)r1 * 0.321,
							.b1=11,
							.b2=22,
						}));
					}
				}

				{
					struct rng rng = make_rng(SEED);
					for (int i=0; i<N; ++i) {
						uint32_t r0 = rng_uint32(&rng);
						uint32_t r1 = rng_uint32(&rng);
						snprintf(keybuf, sizeof keybuf, "key%u", r0);
						size_t si = shgeti(lut, keybuf);
						if ((i%3)==1) {
							assert(si == -1);
							continue;
						} else {
							assert(si >= 0);
							struct value value = lut[si].value;
							assert(value.b1 == 1);
							assert(value.b2 == 2);
							assert(value.f == (double)r1 * 0.0135);
						}
					}
				}

				{
					struct rng rng = make_rng(SEED2);
					for (int i=0; i<N2; ++i) {
						uint32_t r0 = rng_uint32(&rng);
						uint32_t r1 = rng_uint32(&rng);
						snprintf(keybuf, sizeof keybuf, "okey%u", r0);
						size_t si = shgeti(lut, keybuf);
						assert(si >= 0);
						struct value value = lut[si].value;
						assert(value.b1 == 11);
						assert(value.b2 == 22);
						assert(value.f == (double)r1 * 0.321);
					}
				}

				{
					// clear array
					const int n = shlen(lut);
					for (int i=0; i<n; ++i) {
						char* key = lut[i].key;
						assert(shdel(lut, key) == 1);
					}
					assert(shlen(lut) == 0);
				}
			}

			shfree(lut);
			assert(lut == NULL);
		}

		end();

		if (VERBOSE) printf("stb_ds.h hashmap + scratch allocator test: OK\n");
	}
	assert((depth == 0) && "end() forgotten?");
}

int main(int argc, char** argv)
{
	test_scratch_allocator();
	return EXIT_SUCCESS;
}
