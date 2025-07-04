// run_selftest() is intended to run various automated tests before the program
// starts up. This may or may not be a good idea :-) Scattered thoughts on it:

//  - Some environments are less convenient to test in than others. E.g. for
//    the emscripten build you need to test it in a browser. In C it's
//    dangerous to assume that a unit test passing on one platform means it'll
//    pass on all platforms (yes I've stubbed my toe on "implementation defined
//    behavior" before). By including the test in your program you get the test
//    without thinking about it.

//  - Self testing is one way to test a compilation unit that depends on other
//    compilation units. I don't think dynamic dependency injection is a good
//    solution; you end up mocking every single little thing just so you can
//    write a test. Some standalone tests are hybrids though (e.g. test_gig.c)

//  - These tests cannot be expensive. They probably shouldn't have significant
//    static memory requirements, and no dynamic memory requirements (no
//    malloc). However, you can still do standalone tests that can waste almost
//    as many resources as you want. See e.g. LEB128_NUM_FUZZ_ITERATIONS: it
//    defines how expensive the "fuzz test" is. You can also use defines to
//    include/exclude expensive tests.

//  - There can be some namespacing annoyances when writing tests. See e.g.
//    `_g_l128u` in leb128.h which used to be just `g`.

//  - Sometimes the compiler will notice that your program crashes early
//    (failed test assertion) and produce a smaller binary.

//  - Maybe support a define that replaces run_selftest() with an empty
//    function, just in case it makes a difference on constrained platforms.

//  - Growing problem: all binaries want to run_selftest(), but some tests are
//    only relevant for some binaries. e.g. web/emscripten doesn't want to run
//    webserv_selftest(). possible solution: in the Makefile (or wherever)
//    the decision is made to depend on webserv.o; the same place could add
//    a define we use here, e.g. `-DHAS_WEBSERV`. webserv.c itself could
//    throw an #error if HAS_WEBSERV is undefined

#include "main.h"

#define UTF8_UNIT_TEST
#include "utf8.h"

#define LEB128_UNIT_TEST
#include "leb128.h"

#define PATH_UNIT_TEST
#include "path.h"

#ifndef __EMSCRIPTEN__
#include "webserv.h"
#endif

void run_selftest(void)
{
	const int64_t t0 = get_nanoseconds_monotonic();
	utf8_unit_test();
	leb128_unit_test();
	path_unit_test();
	mie_selftest();
	#ifndef __EMSCRIPTEN__
	webserv_selftest();
	#endif
	const int64_t dt = get_nanoseconds_monotonic() - t0;
	printf("selftest took %.5fs\n", (double)dt * 1e-9);
}
