// cc -O0 -g -Wall stb_divide.c stb_ds.c stb_sprintf.c utf8.c path.c mie.c jio.c arg.c allocator.c gig.c test_gig.c -o _test_gig -lm
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <threads.h>
#include <unistd.h>
#include <sys/stat.h>

#include "main.h"
#include "jio.h"
#include "gig.h"
#include "arg.h"
#include "path.h"
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
	const int64_t one_billion = 1000000000LL;
	const struct timespec ts = {
		.tv_nsec = ns % one_billion,
		.tv_sec  = ns / one_billion,
	};
	nanosleep(&ts, NULL);
}

static int io_thread(void* usr)
{
	jio_thread_run();
	return 0;
}

const static int VERBOSE = 0;

const char* base_dir;
const char* test_dir;
int test_sequence;

static void new_test_dir(const char* name)
{
	char buf[1<<10];
	++test_sequence;
	snprintf(buf, sizeof buf, "%s/test-%.4d-%s", base_dir, test_sequence, name);
	if (test_dir) {
		free(test_dir);
		test_dir = NULL;
	}
	test_dir = strdup(buf);
	assert(0 == mkdir(test_dir, 0777));
}

static void test_readwrite(int N)
{
	new_test_dir("readwrite");

	if (VERBOSE) printf("N=%d\n", N);

	for (int pass=0; pass<3; ++pass) {
		if (VERBOSE) printf("  pass %d\n", pass);
		gig_host(test_dir);

		if (pass == 0) {
			begin_mim(1);
			mimex("newbook 1 mie-urlyd -");
			mimex("newdoc 1 50 art.mie");
			mimex("setdoc 1 50");
			mimf("0,1,1c");
			for (int i=0; i<N; ++i) mimi(0,"hello");
			end_mim();
		}

		struct mim_state* ms = NULL;
		struct document* doc = NULL;
		get_state_and_doc(1, &ms, &doc);

		assert(1 == ms->book_id);
		assert(50 == ms->doc_id);
		assert(1 == doc->book_id);
		assert(50 == doc->doc_id);
		const int nc = arrlen(doc->docchar_arr);
		assert(N*5 == nc);
		for (int i=0; i<nc; ++i) {
			assert(doc->docchar_arr[i].colorchar.codepoint == ("hello"[i%5]));
		}

		gig_unhost();
	}
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <dir>\n", argv[0]);
		fprintf(stderr, "(it creates test files inside that dir)\n");
		exit(EXIT_FAILURE);
	}
	base_dir = argv[1];

	jio_init();
	thrd_t t = {0};
	assert(0 == thrd_create(&t, io_thread, NULL));

	mie_thread_init();
	gig_init();

	test_readwrite(100);
	test_readwrite(1);
	test_readwrite(5);
	test_readwrite(50);
	test_readwrite(100);

	return EXIT_SUCCESS;
}
