// cc -O0 -g -Wall stb_divide.c stb_ds.c stb_sprintf.c utf8.c path.c mie.c io.c arg.c allocator.c gig.c test_gig.c -o _test_gig -lm
#include <stdlib.h>
#include <assert.h>
#include <time.h>

#include "main.h"
#include "gig.h"
#include "arg.h"
#include "stb_ds.h" // XXX?

int64_t get_nanoseconds_epoch(void)
{
	// XXX for testing purposes maybe make this "settable"?
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

int64_t get_nanoseconds(void)
{
	return get_nanoseconds_epoch();
}

void sleep_nanoseconds(int64_t ns)
{
	assert(!"TODO");
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <dir>\n", argv[0]);
		fprintf(stderr, "(it creates test files inside that dir)\n");
		exit(EXIT_FAILURE);
	}
	arg_dir = argv[1];
	io_init(16);
	mie_thread_init();
	gig_init();

	gig_host(argv[1]);

	begin_mim(1);
	mimex("newbook 1 mie-ordlyd -");
	mimex("newdoc 1 50 art.mie");
	mimex("setdoc 1 50");
	mimf("0,1,1c");
	mimi(0,"hello");
	end_mim();

	struct mim_state* ms = NULL;
	struct document* doc = NULL;
	get_state_and_doc(1, &ms, &doc);

	printf("ms %d/%d\n", ms->book_id, ms->doc_id);
	printf("doc %d/%d\n", doc->book_id, doc->doc_id);
	printf("doc %zd\n", arrlen(doc->fat_char_arr));

	return EXIT_SUCCESS;
}
