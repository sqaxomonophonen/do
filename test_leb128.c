// cc -O0 -g -Wall test_leb128.c -o _test_leb128 && ./_test_leb128
#define LEB128_UNIT_TEST
#define LEB128_NUM_FUZZ_ITERATIONS (1000000)
#include "leb128.h"
int main(int argc, char** argv)
{
	leb128_unit_test();
	return 0;
}
