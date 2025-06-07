// cc -O0 -g -Wall test_utf8.c -o _test_utf8 && ./_test_utf8
// cc -O3 -g -Wall test_utf8.c -o _test_utf8 && ./_test_utf8
#define UTF8_UNIT_TEST
#define UTF8_IMPLEMENTATION
#include "utf8.h"
int main(int argc, char** argv)
{
	utf8_unit_test();
	printf("OK\n");
	return 0;
}
