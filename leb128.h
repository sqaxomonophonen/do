#ifndef LEB128_H

// "little endian base 128" variable-length integer encoding. see LEB128_UNIT_TEST.

#if (INTPTR_MAX < INT32_MAX)
#error "this code has not been tested on anything smaller than 32-bit system"
#endif

#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include <stdarg.h>

#include "util.h"

#define LEB128_DEFINE_FOR_TYPE(TYPE,UTYPE,NAME) \
\
ALWAYS_INLINE \
static inline void leb128_encode_##NAME(void(*write_fn)(uint8_t,void*), TYPE value, void* userdata) \
{ \
	int more; \
	if (value >= 0) { \
		UTYPE u = value; \
		do { \
			uint8_t b = u & 0x7f; \
			u >>= 7; \
			more = (b > 0x3f) || u; \
			if (more) b |= 0x80; \
			write_fn(b, userdata); \
		} while (more); \
	} else if (value < 0) { \
		/* convert negative value to two's complement. this is likely a no-op */ \
		/* cast but it's actually "implementation defined" (spec also allows */ \
		/* one's complement and sign+magnitude) */ \
		UTYPE u = ~(-(value+1)); \
		const UTYPE neg = ~((UTYPE)0); \
		const UTYPE negpad = ~(neg >> 7); \
		do { \
			uint8_t b = u & 0x7f; \
			u = (u >> 7) | negpad; \
			more = ((b <= 0x3f) || (u != neg)); \
			if (more) b |= 0x80; \
			write_fn(b, userdata); \
		} while (more); \
	} else { \
		assert(!"unreachable"); \
	} \
} \
\
ALWAYS_INLINE \
static inline TYPE leb128_decode_##NAME(uint8_t(*read_fn)(void*), void* userdata) \
{ \
	UTYPE uvalue = 0; \
	int shift = 0; \
	const UTYPE neg = ~((UTYPE)0); \
	for (;;) { \
		const uint8_t b = read_fn(userdata); \
		uvalue |= (UTYPE)(b & 0x7f) << shift; \
		shift += 7; \
		if ((b & 0x80) == 0) { \
			if (shift < (8*sizeof(TYPE)) && (b & 0x40)) { \
				uvalue |= (neg << shift); \
			} \
			break; \
		} \
	} \
\
	/* convert unsigned two's complement to signed value (should be equivalent */ \
	/* to a cast-to-int  on two's complement hardware) */ \
	const UTYPE m = (UTYPE)1 << (8*sizeof(UTYPE)-1); \
	if (uvalue < m) { \
		return (TYPE)uvalue; \
	} else { \
		/* XXX I think this is slightly "implementation defined" because the */ \
		/* minimum value is one lower for two's complement than compared to */ \
		/* one's complement and sign+magnitude. nevertheless it does the right */ \
		/* thing on two's complement. */ \
		return -((1ULL << (8*sizeof(UTYPE)-1)) - 1) + (TYPE)((uvalue & ~m)-(TYPE)1); \
	} \
}

// TYPE: signed integer type to encode/decode
// UTYPE: unsigned integer type; must have same width as TYPE
// NAME: type name used in function name, e.g. `int64` instead of `int64_t`

// define leb128_encode_int() and leb128_decode_int()
LEB128_DEFINE_FOR_TYPE(
	/*TYPE=*/int,
	/*UTYPE=*/unsigned,
	/*NAME=*/int)

// define leb128_encode_int64() and leb128_decode_int64()
LEB128_DEFINE_FOR_TYPE(
	/*TYPE=*/int64_t,
	/*UTYPE=*/uint64_t,
	/*NAME=*/int64)


#define LEB128_H
#endif

#ifdef LEB128_UNIT_TEST

#ifndef LEB128_NUM_FUZZ_ITERATIONS
#define LEB128_NUM_FUZZ_ITERATIONS (1000)
#endif

// XXX is this a good way to detect whether `int64_t` is `long` or `long long`
// for the purpose of printf?
#if (INTPTR_MAX == INT32_MAX)
#define INT64FMT "%lld"
#elif (INTPTR_MAX == INT64_MAX)
#define INT64FMT "%ld"
#else
#error "missing platform support"
#endif

#include <stdio.h>
#include <stdlib.h>

static struct {
	uint8_t buffer[1<<4];
	int cursor;
} _g_l128u;

static void write_fn(uint8_t v, void* userdata)
{
	assert(userdata == (void*)42);
	assert(_g_l128u.cursor < sizeof(_g_l128u.buffer));
    _g_l128u.buffer[_g_l128u.cursor++] = v;
}

static uint8_t read_fn(void* userdata)
{
	assert(userdata == (void*)42);
	assert(_g_l128u.cursor < sizeof(_g_l128u.buffer));
    return _g_l128u.buffer[_g_l128u.cursor++];
}

static void _test_l128u(int64_t value, int num_bytes, ...)
{
	for (int pass=0; pass<2; ++pass) {
		if (pass == 0 && !((INT_MIN <= value) && (value <= INT_MAX))) {
			// skip 32-bit tests of 64-bit values (a little icky because a
			// passed test is silent, but as always, to test the test, break
			// the test:)
			continue;
		}

		int fail = 0;

		_g_l128u.cursor = 0;
		if (pass == 0) {
			assert((INT_MIN <= value) && (value <= INT_MAX));
			leb128_encode_int(write_fn, (int)value, (void*)42);
		} else if (pass == 1) {
			leb128_encode_int64(write_fn, value, (void*)42);
		} else {
			assert(!"bad state");
		}

		const int num_written_bytes = _g_l128u.cursor;
		if (num_written_bytes != num_bytes) fail = 1;

		va_list ap;

		if (!fail) {
			va_start(ap, num_bytes);
			for (int i=0; i<num_bytes; ++i) {
				const int expected_value = va_arg(ap, int);
				if (_g_l128u.buffer[i] != expected_value) {
					fail = 1;
					break;
				}
			}
			va_end(ap);
		}

		if (fail) {
			fprintf(stderr, "LEB128 ENCODE FAIL: expected value " INT64FMT " to encode as byte sequence [", value);
			va_start(ap, num_bytes);
			for (int i=0; i<num_bytes; ++i) {
				fprintf(stderr, "%s%.2x", (i>0?" ":""), va_arg(ap, int));
			}
			va_end(ap);
			fprintf(stderr, "] but got [");
			for (int i=0; i<num_written_bytes; ++i) {
				fprintf(stderr, "%s%.2x", (i>0?" ":""), _g_l128u.buffer[i]);
			}
			fprintf(stderr, "]\n");
			abort();
		}

		_g_l128u.cursor = 0;
		int64_t decoded_value;
		if (pass == 0) {
			decoded_value = leb128_decode_int(read_fn, (void*)42);
		} else if (pass == 1) {
			decoded_value = leb128_decode_int64(read_fn, (void*)42);
		} else {
			assert(!"bad state");
		}
		const int num_read_bytes = _g_l128u.cursor;
		if (_g_l128u.cursor != num_written_bytes) {
			fprintf(stderr, "LEB128 ROUNDTRIP FAIL: expected to read %d byte(s), but read %d\n", num_written_bytes, num_read_bytes);
			abort();
		}
		if (decoded_value != value) {
			fprintf(stderr, "LEB128 ROUNDTRIP FAIL: expected to decode " INT64FMT " but decoded " INT64FMT, value, decoded_value);
			abort();
		}
	}
}

static void leb128_unit_test(void)
{
	// test non-negative 32/64bit
    _test_l128u(0x00LL       , 1, 0x00);
    _test_l128u(0x01LL       , 1, 0x01);
    _test_l128u(0x02LL       , 1, 0x02);
    _test_l128u(0x3fLL       , 1, 0x3f);
    _test_l128u(0x40LL       , 2, 0xc0, 0x00);
    _test_l128u(0x41LL       , 2, 0xc1, 0x00);
    _test_l128u(0x80LL       , 2, 0x80, 0x01);
    _test_l128u(0x81LL       , 2, 0x81, 0x01);
    _test_l128u(0x100LL      , 2, 0x80, 0x02);
    _test_l128u(0x200LL      , 2, 0x80, 0x04);
    _test_l128u(0x42424LL    , 3, 0xa4, 0xc8, 0x10);
    _test_l128u(0x123abcLL   , 4, 0xbc, 0xf5, 0xc8, 0x00);
    _test_l128u(0x234bcdLL   , 4, 0xcd, 0x97, 0x8d, 0x01);
    _test_l128u(0x7ffffffeLL , 5, 0xfe, 0xff, 0xff, 0xff, 0x07);
    _test_l128u(0x7fffffffLL , 5, 0xff, 0xff, 0xff, 0xff, 0x07); // maximum int value

	// test non-negative 64bit
    _test_l128u(0x424242424LL        ,  6, 0xa4, 0xc8, 0x90, 0xa1, 0xc2, 0x00);
    _test_l128u(0x123456bcdef0LL     ,  7, 0xf0, 0xbd, 0xf3, 0xb5, 0xc5, 0xc6, 0x04);
    _test_l128u(0x1234567abcdef0LL   ,  8, 0xf0, 0xbd, 0xf3, 0xd5, 0xe7, 0x8a, 0x8d, 0x09);
    _test_l128u(0x123456789abcdef0LL ,  9, 0xf0, 0xbd, 0xf3, 0xd5, 0x89, 0xcf, 0x95, 0x9a, 0x12);
    _test_l128u(0x7ffffffffffffffeLL , 10, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00);
    _test_l128u(0x7fffffffffffffffLL , 10, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00);

	// test negative
    _test_l128u(-0x01LL        , 1, 0x7f);
    _test_l128u(-0x02LL        , 1, 0x7e);
    _test_l128u(-0x3fLL        , 1, 0x41);
    _test_l128u(-0x40LL        , 1, 0x40);
    _test_l128u(-0x41LL        , 2, 0xbf, 0x7f);
    _test_l128u(-0x42424LL     , 3, 0xdc, 0xb7, 0x6f);
    _test_l128u(-0x123abcLL    , 4, 0xc4, 0x8a, 0xb7, 0x7f);
    _test_l128u(-0x234bcdLL    , 4, 0xb3, 0xe8, 0xf2, 0x7e);
    _test_l128u(-0xfffffeLL    , 4, 0x82, 0x80, 0x80, 0x78);
    _test_l128u(-0xffffffLL    , 4, 0x81, 0x80, 0x80, 0x78);
    _test_l128u(-0xffffffeLL   , 5, 0x82, 0x80, 0x80, 0x80, 0x7f);
    _test_l128u(-0xfffffffLL   , 5, 0x81, 0x80, 0x80, 0x80, 0x7f);
    _test_l128u(-0x7fffffffLL  , 5, 0x81, 0x80, 0x80, 0x80, 0x78); // minimum safe int value
    _test_l128u(-0x80000000LL  , 5, 0x80, 0x80, 0x80, 0x80, 0x78); // minimum int value

	// test negative 64bit
    _test_l128u(-0x424242424LL        ,  6, 0xdc, 0xb7, 0xef, 0xde, 0xbd, 0x7f);
    _test_l128u(-0x123456bcdef0LL     ,  7, 0x90, 0xc2, 0x8c, 0xca, 0xba, 0xb9, 0x7b);
    _test_l128u(-0x1234567abcdef0LL   ,  8, 0x90, 0xc2, 0x8c, 0xaa, 0x98, 0xf5, 0xf2, 0x76);
    _test_l128u(-0x123456789abcdef0LL ,  9, 0x90, 0xc2, 0x8c, 0xaa, 0xf6, 0xb0, 0xea, 0xe5, 0x6d);
    _test_l128u(-0x7fffffffffffffffLL , 10, 0x81, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7f);
    _test_l128u(-0x8000000000000000LL , 10, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7f);

	// uncomment for FAIL TEST:
    //_test_l128u(-0x8000000000000000LL , 10, 0x81, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x7f);

	// fuzz test
	uint32_t z = 654654;
	uint32_t w = 7653234;
	for (int i=0; i<LEB128_NUM_FUZZ_ITERATIONS; ++i) {
		z = 36969 * (z & 0xffff) + (z>>16);
		w = 18000 * (w & 0xffff) + (w>>16);
		const int v0 = (z<<16) + w;
		_g_l128u.cursor = 0;
		leb128_encode_int(write_fn, v0, (void*)42);
		const int num_written = _g_l128u.cursor;
		_g_l128u.cursor = 0;
		const int v1 = leb128_decode_int(read_fn, (void*)42);
		const int num_read = _g_l128u.cursor;
		if ((num_read != num_written) || (v1 != v0)) {
			fprintf(stderr, "LEB128 FUZZ FAIL: input value %d (n=%d), output value %d (n=%d)\n", v0, num_written, v1, num_read);
			abort();
		}
	}
}

#endif
