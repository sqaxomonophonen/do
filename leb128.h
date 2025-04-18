#ifndef LEB128_H

#include <stdint.h>
#include <assert.h>
#include <stdarg.h>

#include "util.h"

ALWAYS_INLINE
static inline void leb128_encode_int(void(*write_fn)(uint8_t), int value)
{
	int more;
	if (value >= 0) {
		unsigned u = value;
		do {
			uint8_t b = u & 0x7f;
			u >>= 7;
			more = (b > 0x3f) || u;
			if (more) b |= 0x80;
			write_fn(b);
		} while (more);
	} else if (value < 0) {
		// convert negative value to two's complement. this is likely a no-op
		// cast but it's actually "implementation defined" (spec also allows
		// one's complement and sign+magnitude)
		unsigned u = ~(-(value+1));
		const unsigned neg = ~0u;
		const unsigned negpad = ~(neg >> 7);
		do {
			uint8_t b = u & 0x7f;
			u = (u >> 7) | negpad;
			more = ((b <= 0x3f) || (u != neg));
			if (more) b |= 0x80;
			write_fn(b);
		} while (more);
	} else {
		assert(!"unreachable");
	}
}

ALWAYS_INLINE
static inline int leb128_decode_int(uint8_t(*read_fn)(void))
{
	unsigned value = 0;
	int shift = 0;
	for (;;) {
		const uint8_t b = read_fn();
		value |= (b & 0x7f) << shift;
		shift += 7;
		if ((b & 0x80) == 0) {
			if (shift < 32 && (b & 0x40)) {
				value |= ((~0u) << shift);
			}
			break;
		}
	}

	// convert unsigned two's complement to signed value (should be equivalent
	// to a cast-to-int  on two's complement hardware)
	const unsigned m = 0x80000000;
	if (value < m) {
		return (int)value;
	} else {
		// XXX I think this is slightly "implementation defined" because the
		// minimum value is one lower for two's complement than compared to
		// one's complement and sign+magnitude
		return -0x7fffffff + (int)((value & ~m)-1);
	}
}

#define LEB128_H
#endif

#ifdef LEB128_UNIT_TEST

#include <stdio.h>
#include <stdlib.h>

static struct {
	uint8_t buffer[1<<4];
	int cursor;
} g;


static void write_fn(uint8_t v)
{
	assert(g.cursor < sizeof(g.buffer));
    g.buffer[g.cursor++] = v;
}

static uint8_t read_fn(void)
{
	assert(g.cursor < sizeof(g.buffer));
    return g.buffer[g.cursor++];
}

static void test(int value, int num_bytes, ...)
{
	int fail = 0;

    g.cursor = 0;
    leb128_encode_int(write_fn, value);
	const int num_written_bytes = g.cursor;
    if (num_written_bytes != num_bytes) {
		fail = 1;
    }

    va_list ap;

	if (!fail) {
		va_start(ap, num_bytes);
		for (int i=0; i<num_bytes; ++i) {
			const int expected_value = va_arg(ap, int);
			if (g.buffer[i] != expected_value) {
				fail = 1;
				break;
			}
		}
		va_end(ap);
	}

	if (fail) {
		fprintf(stderr, "LEB128 ENCODE FAIL: expected value %d to encode as byte sequence [", value);
		va_start(ap, num_bytes);
		for (int i=0; i<num_bytes; ++i) {
			fprintf(stderr, "%s%.2x", (i>0?" ":""), va_arg(ap, int));
		}
		va_end(ap);
		fprintf(stderr, "] but got [");
		for (int i=0; i<num_written_bytes; ++i) {
			fprintf(stderr, "%s%.2x", (i>0?" ":""), g.buffer[i]);
		}
		fprintf(stderr, "]\n");
	}

	g.cursor = 0;
	const int decoded_value = leb128_decode_int(read_fn);
	const int num_read_bytes = g.cursor;
	if (g.cursor != num_written_bytes) {
		fprintf(stderr, "LEB128 ROUNDTRIP FAIL: expected to read %d byte(s), but read %d\n", num_written_bytes, num_read_bytes);
	}
	if (decoded_value != value) {
		fprintf(stderr, "LEB128 ROUNDTRIP FAIL: expected to decode %d, but decoded %d\n", value, decoded_value);
	}
}

int main(int argc, char** argv)
{
	// test non-negative
    test(0x00        , 1, 0x00);
    test(0x01        , 1, 0x01);
    test(0x02        , 1, 0x02);
    test(0x3f        , 1, 0x3f);
    test(0x40        , 2, 0xc0, 0x00);
    test(0x41        , 2, 0xc1, 0x00);
    test(0x80        , 2, 0x80, 0x01);
    test(0x81        , 2, 0x81, 0x01);
    test(0x100       , 2, 0x80, 0x02);
    test(0x200       , 2, 0x80, 0x04);
    test(0x42424     , 3, 0xa4, 0xc8, 0x10);
    test(0x123abc    , 4, 0xbc, 0xf5, 0xc8, 0x00);
    test(0x234bcd    , 4, 0xcd, 0x97, 0x8d, 0x01);
    test(0x7ffffffe  , 5, 0xfe, 0xff, 0xff, 0xff, 0x07);
    test(0x7fffffff  , 5, 0xff, 0xff, 0xff, 0xff, 0x07); // maximum value

	// test negative
    test(-0x01       , 1, 0x7f);
    test(-0x02       , 1, 0x7e);
    test(-0x3f       , 1, 0x41);
    test(-0x40       , 1, 0x40);
    test(-0x41       , 2, 0xbf, 0x7f);
    test(-0x42424    , 3, 0xdc, 0xb7, 0x6f);
    test(-0x123abc   , 4, 0xc4, 0x8a, 0xb7, 0x7f);
    test(-0x234bcd   , 4, 0xb3, 0xe8, 0xf2, 0x7e);
    test(-0xfffffe   , 4, 0x82, 0x80, 0x80, 0x78);
    test(-0xffffff   , 4, 0x81, 0x80, 0x80, 0x78);
    test(-0xffffffe  , 5, 0x82, 0x80, 0x80, 0x80, 0x7f);
    test(-0xfffffff  , 5, 0x81, 0x80, 0x80, 0x80, 0x7f);
    test(-0x7fffffff , 5, 0x81, 0x80, 0x80, 0x80, 0x78);
    test(-0x80000000 , 5, 0x80, 0x80, 0x80, 0x80, 0x78); // minimum value

	// fuzz test
	uint32_t z = 654654;
	uint32_t w = 7653234;
	for (int i=0; i<1000000; ++i) {
		z = 36969 * (z & 0xffff) + (z>>16);
		w = 18000 * (w & 0xffff) + (w>>16);
		const int v0 = (z<<16) + w;
		g.cursor = 0;
		leb128_encode_int(write_fn, v0);
		const int num_written = g.cursor;
		g.cursor = 0;
		const int v1 = leb128_decode_int(read_fn);
		const int num_read = g.cursor;
		if ((num_read != num_written) || (v1 != v0)) {
			fprintf(stderr, "LEB128 FUZZ FAIL: input value %d (n=%d), output value %d (n=%d)\n", v0, num_written, v1, num_read);
		}
	}

    return 0;
}

#endif
