// cc -O0 -g -Wall test_path.c -o _test_path && ./_test_path
#define PATH_UNIT_TEST
#define PATH_IMPLEMENTATION
#include <stdio.h>
#include "path.h"
int main(int argc, char** argv)
{
	path_unit_test();
	printf("OK\n");
	return 0;
}
