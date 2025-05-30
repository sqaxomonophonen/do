// cc -O0 -g base64.c test_base64.c -o _test_base64 && ./_test_base64
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>

#include "base64.h"
#include "util.h"

void enc(char* output, uint8_t* input, int n_bytes)
{
	char* w = base64_encode(output, input, n_bytes);
	*(w++) = 0;
	#if 0
	printf("enc -> [%s]\n", output);
	#endif
}

int dec(uint8_t* output, char* input)
{
	uint8_t* p = base64_decode_line(output, input);
	assert((p != NULL) && "base64_decode_line() failed");
	const int n = p - output;
	#if 0
	printf("dec -> [");
	for (int i = 0; i < n; i++) {
		printf("%s%.2x", i>0?", ":"", output[i]);
	}
	printf("]\n");
	#endif
	return n;
}

static void test0(int d, uint8_t* xs, size_t nxs, const char* expected_base64)
{
	uint8_t ys[1<<10];
	char line[1<<10];
	const int n = nxs - d;
	enc(line, xs, n);
	assert(strcmp(line, expected_base64) == 0);
	assert(strlen(line) == ((n+2)/3)*4);
	assert(dec(ys, line) == n);
	assert(memcmp(xs, ys, n) == 0);
}

static void testfuzz(void)
{
	uint8_t xs[3000];
	uint8_t ys[ARRAY_LENGTH(xs)];
	char line[4100];
	for (int i0 = 0; i0 < 100; i0++) {
		const int n = ARRAY_LENGTH(xs) - (rand() & 255);
		for (int i1 = 0; i1 < n; i1++) xs[i1] = rand() & 0xff;
		enc(line, xs, n);
		assert(strlen(line) == ((n+2)/3)*4);
		assert(dec(ys, line) == n);
		assert(memcmp(xs, ys, n) == 0);
	}
}

int main(int argc, char** argv)
{
	{
		uint8_t xs[] = {
			0x10, 0x20, 0x30,
			0x60, 0x70, 0x90,
			0xB0, 0xC0, 0xD0,
			0xFF, 0xEE, 0xDD,
			0x01, 0x02, 0x03,
			0x3c, 0x3b, 0x3a,
			0x77, 0x66, 0x55,
		};
		const size_t nxs = ARRAY_LENGTH(xs);
		test0(0, xs, nxs, "ECAwYHCQsMDQ/+7dAQIDPDs6d2ZV");
		test0(1, xs, nxs, "ECAwYHCQsMDQ/+7dAQIDPDs6d2Y=");
		test0(2, xs, nxs, "ECAwYHCQsMDQ/+7dAQIDPDs6dw==");
		test0(3, xs, nxs, "ECAwYHCQsMDQ/+7dAQIDPDs6");
	}

	testfuzz();

	printf("OK\n");

	return EXIT_SUCCESS;
}
