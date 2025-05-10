// cc -O0 -g -Wall utf8.c allocator.c stb_sprintf.c stb_ds.c mii.c test_mii.c -o _test_mii -lm && ./_test_mii
// cc -O3 -g -Wall utf8.c allocator.c stb_sprintf.c stb_ds.c mii.c test_mii.c -o _test_mii -lm && ./_test_mii

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "mii.h"

int main(int argc, char** argv)
{
	mii_init();

	//struct vmii* vm = vmii_new();
	//const char* src = "2 3 (lelelele) + . :foo : bar baz; baz ;";
	//const char* src = ": square dup * ; 42 square";
	const char* src = ": bsquare 0 : hmm 69 ; pick F* ; -42 bsquare";
	int prg = mii_compile_graycode(src, strlen(src));

	if (prg == -1) {
		fprintf(stderr, "compile error: %s\n", mii_error());
		exit(EXIT_FAILURE);
	}

	vmii_reset(prg);
	int r = vmii_run();
	if (r == -1) {
		fprintf(stderr, "vmii error: %s\n", mii_error());
		exit(EXIT_FAILURE);
	} else {
		printf("r=%d\n", r);
		for (int i=0; i<r; ++i) {
			printf(" %d:%f\n", i, vmii_floatval(i));
		}
	}

	return EXIT_SUCCESS;
}
