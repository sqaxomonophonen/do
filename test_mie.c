// cc -O0 -g -Wall utf8.c allocator.c stb_sprintf.c stb_ds.c stb_divide.c mie.c test_mie.c -o _test_mie -lm && ./_test_mie
// cc -O3 -g -Wall utf8.c allocator.c stb_sprintf.c stb_ds.c stb_divide.c mie.c test_mie.c -o _test_mie -lm && ./_test_mie

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "mie.h"

int main(int argc, char** argv)
{
	mie_selftest();
	return EXIT_SUCCESS;
}
