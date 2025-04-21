#ifndef LEB128_H

// "little endian base 128" variable-length integer encoding. see LEB128_UNIT_TEST.

#include <stdint.h>
#include <assert.h>
#include <limits.h>
#include <stdarg.h>

#include "util.h"

#define LEB128_DEFINE_FOR_TYPE(TYPE,UTYPE,U1,NUMBITS,UHALF,NEG0,NAME) \
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
		const UTYPE neg = U1; \
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
	UTYPE value = 0; \
	TYPE shift = 0; \
	for (;;) { \
		const uint8_t b = read_fn(userdata); \
		value |= (b & 0x7f) << shift; \
		shift += 7; \
		if ((b & 0x80) == 0) { \
			if (shift < NUMBITS && (b & 0x40)) { \
				value |= (U1 << shift); \
			} \
			break; \
		} \
	} \
 \
	/* convert unsigned two's complement to signed value (should be equivalent */ \
	/* to a cast-to-int  on two's complement hardware) */ \
	const UTYPE m = UHALF; \
	if (value < m) { \
		return (TYPE)value; \
	} else { \
		/* XXX I think this is slightly "implementation defined" because the */ \
		/* minimum value is one lower for two's complement than compared to */ \
		/* one's complement and sign+magnitude. nevertheless it does the right */ \
		/* thing on two's complement. */ \
		return NEG0 + (TYPE)((value & ~m)-1); \
	} \
}

// TYPE: signed integer type to encode/decode
// UTYPE: unsigned integer type, must have same width as TYPE
// U1: UTYPE value that is "all ones" (~0)
// NUMBITS: 16 for int16_t and so on
// UHALF: half the unsigned max size + 1, e.g. 0x8000 for int16_t
// NEG0: minimum safe signed value (NOTE: should be -0x7fff for int16_t to deal
//       with the remote possibility of non two's complement hardware)
// NAME: used in function name, e.g. `int16` for `int16_t` or `int` for `int`.

LEB128_DEFINE_FOR_TYPE(
	/*TYPE=*/int,
	/*UTYPE=*/unsigned,
	/*U1=*/(~0u),
	/*NUMBITS=*/(32),
	/*UHALF=*/(0x80000000),
	/*NEG0=*/(-0x7fffffff),
	/*NAME=*/int)

LEB128_DEFINE_FOR_TYPE(
	/*TYPE=*/int64_t,
	/*UTYPE=*/uint64_t,
	/*U1=*/(~0ull),
	/*NUMBITS=*/(64),
	/*UHALF=*/(0x8000000000000000LL),
	/*NEG0=*/(-0x7fffffffffffffffLL),
	/*NAME=*/int64)


#define LEB128_H
#endif

#ifdef LEB128_UNIT_TEST

#ifndef LEB128_NUM_FUZZ_ITERATIONS
#define LEB128_NUM_FUZZ_ITERATIONS (1000)
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
			fprintf(stderr, "LEB128 ENCODE FAIL: expected value %ld to encode as byte sequence [", value);
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
		}

		_g_l128u.cursor = 0;
		int decoded_value;
		if (pass == 0) {
			decoded_value = leb128_decode_int(read_fn, (void*)42);
		} else if (pass == 1) {
			const int64_t decoded_value64 = leb128_decode_int64(read_fn, (void*)42);
			assert((INT_MIN <= decoded_value64) && (decoded_value64 <= INT_MAX));
			decoded_value = (int)decoded_value64;
		} else {
			assert(!"bad state");
		}
		const int num_read_bytes = _g_l128u.cursor;
		if (_g_l128u.cursor != num_written_bytes) {
			fprintf(stderr, "LEB128 ROUNDTRIP FAIL: expected to read %d byte(s), but read %d\n", num_written_bytes, num_read_bytes);
		}
		if (decoded_value != value) {
			fprintf(stderr, "LEB128 ROUNDTRIP FAIL: expected to decode %ld, but decoded %d\n", value, decoded_value);
		}
	}
}

static void leb128_unit_test(void)
{
	// test non-negative
    _test_l128u(0x00l        , 1, 0x00);
    _test_l128u(0x01l        , 1, 0x01);
    _test_l128u(0x02l        , 1, 0x02);
    _test_l128u(0x3fl        , 1, 0x3f);
    _test_l128u(0x40l        , 2, 0xc0, 0x00);
    _test_l128u(0x41l        , 2, 0xc1, 0x00);
    _test_l128u(0x80l        , 2, 0x80, 0x01);
    _test_l128u(0x81l        , 2, 0x81, 0x01);
    _test_l128u(0x100l       , 2, 0x80, 0x02);
    _test_l128u(0x200l       , 2, 0x80, 0x04);
    _test_l128u(0x42424l     , 3, 0xa4, 0xc8, 0x10);
    _test_l128u(0x123abcl    , 4, 0xbc, 0xf5, 0xc8, 0x00);
    _test_l128u(0x234bcdl    , 4, 0xcd, 0x97, 0x8d, 0x01);
    _test_l128u(0x7ffffffel  , 5, 0xfe, 0xff, 0xff, 0xff, 0x07);
    _test_l128u(0x7fffffffl  , 5, 0xff, 0xff, 0xff, 0xff, 0x07); // maximum value

	// test negative
    _test_l128u(-0x01l       , 1, 0x7f);
    _test_l128u(-0x02l       , 1, 0x7e);
    _test_l128u(-0x3fl       , 1, 0x41);
    _test_l128u(-0x40l       , 1, 0x40);
    _test_l128u(-0x41l       , 2, 0xbf, 0x7f);
    _test_l128u(-0x42424l    , 3, 0xdc, 0xb7, 0x6f);
    _test_l128u(-0x123abcl   , 4, 0xc4, 0x8a, 0xb7, 0x7f);
    _test_l128u(-0x234bcdl   , 4, 0xb3, 0xe8, 0xf2, 0x7e);
    _test_l128u(-0xfffffel   , 4, 0x82, 0x80, 0x80, 0x78);
    _test_l128u(-0xffffffl   , 4, 0x81, 0x80, 0x80, 0x78);
    _test_l128u(-0xffffffel  , 5, 0x82, 0x80, 0x80, 0x80, 0x7f);
    _test_l128u(-0xfffffffl  , 5, 0x81, 0x80, 0x80, 0x80, 0x7f);
    _test_l128u(-0x7fffffffl , 5, 0x81, 0x80, 0x80, 0x80, 0x78);
    _test_l128u(-0x80000000l , 5, 0x80, 0x80, 0x80, 0x80, 0x78); // minimum value

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
		}
	}
}

#endif
