// cc -O0 -g -Wall utf8.c allocator.c stb_sprintf.c stb_ds.c mii.c test_mii.c -o _test_mii && ./_test_mii

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "mii.h"

int main(int argc, char** argv)
{
	struct vmii vm = {0};
	const char* src = "2 3 (lelelele) + . :foo : bar baz; baz ;";
	mii_compile_graycode(&vm, src, strlen(src));
	if (vm.compile_error) {
		fprintf(stderr, "compile error: %s\n", vm.compile_error);
		exit(EXIT_FAILURE);
	}
	return EXIT_SUCCESS;
}
