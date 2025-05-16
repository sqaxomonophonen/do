// cc -O0 -g -Wall utf8.c allocator.c stb_sprintf.c stb_ds.c stb_divide.c mie.c test_mie.c -o _test_mie -lm && ./_test_mie
// cc -O0 -g -fsanitize=undefined -fsanitize=address -Wall utf8.c allocator.c stb_sprintf.c stb_ds.c stb_divide.c mie.c test_mie.c -o _test_mie -lm && ./_test_mie
// cc -DMIE_ONLY_SYSALLOC -O3 -g -Wall utf8.c allocator.c stb_sprintf.c stb_ds.c stb_divide.c mie.c test_mie.c -o _test_mie -lm && ./_test_mie
// cc -DMIE_ONLY_SYSALLOC -O0 -g -Wall utf8.c allocator.c stb_sprintf.c stb_ds.c stb_divide.c mie.c test_mie.c -o _test_mie -lm && ./_test_mie
// cc -O3 -g -Wall utf8.c allocator.c stb_sprintf.c stb_ds.c stb_divide.c mie.c test_mie.c -o _test_mie -lm && ./_test_mie

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "mie.h"

static char src[1<<20];

int main(int argc, char** argv)
{
	mie_selftest();
	if (argc < 2) {
		printf("mie selftest OK! (I can also run .mie programs via argv)\n");
		return EXIT_SUCCESS;
	}
	for (int i=1; i<argc; ++i) {
		const char* path = argv[i];
		FILE* f = fopen(path, "r");
		if (f == NULL) {
			fprintf(stderr, "%s: %s\n", path, strerror(errno));
			exit(EXIT_FAILURE);
		}
		assert(fseek(f, 0, SEEK_END) != -1);
		const long sz = ftell(f);
		assert(fseek(f, 0, SEEK_SET) != -1);
		assert(fread(src, sz, 1, f) == 1);
		assert(fclose(f) != -1);

		const int prg = mie_compile_graycode(src, sz);
		if (prg == -1) {
			fprintf(stderr, "%s: compile error: %s\n", path, mie_error());
			exit(EXIT_FAILURE);
		}

		vmie_reset(prg);
		if (vmie_run() == -1) {
			fprintf(stderr, "%s: run error: %s\n", path, mie_error());
			exit(EXIT_FAILURE);
		}

		vmie_dump_stack();
	}
	return EXIT_SUCCESS;
}
