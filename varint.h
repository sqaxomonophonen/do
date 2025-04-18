#ifndef VARINT_H

#include <stdint.h>
#include <assert.h>
#include <stdarg.h>

// XXX FIXME TODO do LEB128 instead

static inline void varint_encode_int(void(*write)(uint8_t), int value)
{
    assert(!"TODO");
}

static inline int varint_decode_int(uint8_t(*read)(void))
{
    assert(!"TODO");
}

#define VARINT_H
#endif

#if 0

#ifdef VARINT_UNIT_TEST

#include <stdio.h>
#include <stdlib.h>

static uint8_t buffer[1<<10];
int buffer_cursor = 0;
static void write(uint8_t v)
{
    buffer[buffer_cursor++] = v;
}

static void test_encode(int value, int num_bytes, ...)
{
    buffer_cursor = 0;
    varint_encode_int(write, value);
    if (buffer_cursor != num_bytes) {
        fprintf(stderr, "FAIL: expected int %d to encode as %d bytes, but got %d\n", value, num_bytes, buffer_cursor);
        exit(EXIT_FAILURE);
    }

    va_list ap;
    va_start(ap, num_bytes);
    for (int i=0; i<num_bytes; ++i) {
        const int expected_value = va_arg(ap, int);
        if (buffer[i] != expected_value) {
            fprintf(stderr, "FAIL: expected value %d at index %d, but got %d\n", expected_value, i, (int)buffer[i]);
            exit(EXIT_FAILURE);
        }
    }
    va_end(ap);
}

int main(int argc, char** argv)
{
    test_encode(0,1,0);
    test_encode(1,1,1);
    test_encode(2,1,2);
    test_encode(-1,1,0);
    test_encode(-2,1,0);

    return 0;
}

#endif
#endif
