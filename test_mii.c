// cc -O0 -g -Wall utf8.c allocator.c stb_sprintf.c stb_ds.c stb_divide.c mii.c test_mii.c -o _test_mii -lm && ./_test_mii
// cc -O3 -g -Wall utf8.c allocator.c stb_sprintf.c stb_ds.c stb_divide.c mii.c test_mii.c -o _test_mii -lm && ./_test_mii

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "mii.h"

int main(int argc, char** argv)
{
	mii_selftest();
	return EXIT_SUCCESS;
}
