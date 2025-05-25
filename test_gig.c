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
char* test_dir;
int test_sequence;

static struct {
	struct mim_state* ms;
	struct document* doc;
	struct caret* cr;
} g;

static void new_test(const char* name)
{
	memset(&g, 0, sizeof g);
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
	new_test("readwrite");

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

		get_state_and_doc(1, &g.ms, &g.doc);

		assert(1 == g.ms->book_id);
		assert(50 == g.ms->doc_id);
		assert(1 == g.doc->book_id);
		assert(50 == g.doc->doc_id);
		const int nc = arrlen(g.doc->docchar_arr);
		assert(N*5 == nc);
		for (int i=0; i<nc; ++i) {
			assert(g.doc->docchar_arr[i].colorchar.codepoint == ("hello"[i%5]));
		}

		gig_unhost();
	}
}

static void expect_col_and_doc(int expected_col, const char* expected_doc)
{
	get_state_and_doc(1, &g.ms, &g.doc);
	assert(arrlen(g.ms->caret_arr) == 1);
	g.cr = &g.ms->caret_arr[0];
	assert(g.cr->tag == 0);
	assert(g.cr->caret_loc.line == 1);
	int fail=0;
	if (g.cr->caret_loc.column != expected_col) {
		fprintf(stderr, "expected caret column to be %d, but it was %d\n", expected_col, g.cr->caret_loc.column);
		fail=1;
	}
	//assert(g.cr->anchor_loc.line == g.cr->caret_loc.line);
	//assert(g.cr->anchor_loc.column == g.cr->caret_loc.column);
	const int num_actual   = arrlen(g.doc->docchar_arr);
	const int num_expected = strlen(expected_doc);
	int match = (num_actual == num_expected);
	if (match) {
		for (int i=0; i<num_actual; ++i) {
			if (g.doc->docchar_arr[i].colorchar.codepoint != expected_doc[i]) {
				match = 0;
			}
		}
	}
	if (!match) {
		fprintf(stderr, "expected [%s] (%d chars), got [", expected_doc, num_expected);
		for (int i=0; i<num_actual; ++i) fprintf(stderr, "%c", g.doc->docchar_arr[i].colorchar.codepoint);
		fprintf(stderr, "] (%d chars)\n", num_actual);
		fail=1;
	}
	if (fail) abort();
}

static void test_regress_0a(void)
{
	new_test("regress0");
	gig_host(test_dir);

	begin_mim(1);
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	mimex("setdoc 1 50");
	mimf("0,1,1c");
	end_mim();
	expect_col_and_doc(1,"");

	begin_mim(1);
	mimi(0,"abc");
	end_mim();
	expect_col_and_doc(4,"abc");

	begin_mim(1);
	mimf("0Mh0Mh");
	end_mim();
	expect_col_and_doc(2,"abc");

	begin_mim(1);
	mimi(0,"12");
	end_mim();
	expect_col_and_doc(4,"a12bc");

	gig_unhost();
}

static void test_regress_0b(void)
{
	new_test("regress0");
	gig_host(test_dir);

	begin_mim(1);
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	mimex("setdoc 1 50");
	mimf("0,1,1c");
	end_mim();
	expect_col_and_doc(1,"");

	begin_mim(1);
	mimi(0,"abc");
	end_mim();
	expect_col_and_doc(4,"abc");

	begin_mim(1);
	mimf("0Mh0Mh0Mh");
	end_mim();
	expect_col_and_doc(1,"abc");

	begin_mim(1);
	// no-op
	end_mim();
	expect_col_and_doc(1,"abc");

	begin_mim(1);
	mimi(0,"12");
	end_mim();
	expect_col_and_doc(3,"12abc");

	gig_unhost();
}

static void test_regress_0c(void)
{
	new_test("regress1");
	gig_host(test_dir);

	begin_mim(1);
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	mimex("setdoc 1 50");
	mimf("0,1,1c");
	end_mim();
	expect_col_and_doc(1,"");

	begin_mim(1);
	mimi(0,"abc");
	mimf("0Mh0Mh0Mh");
	mimi(0,"123");
	end_mim();
	expect_col_and_doc(4,"123abc");

	gig_unhost();
}

static void test_regress_0d(void)
{
	new_test("regress1");
	gig_host(test_dir);

	begin_mim(1);
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	mimex("setdoc 1 50");
	mimf("0,1,1c");
	end_mim();
	expect_col_and_doc(1,"");

	// insert "abc"
	begin_mim(1);
	mimi(0,"abc");
	end_mim();
	expect_col_and_doc(4,"abc");

	// commit
	begin_mim(1);
	mimf("0!");
	end_mim();
	expect_col_and_doc(4,"abc");

	// 1 x caret left
	begin_mim(1);
	mimf("0Mh");
	end_mim();
	expect_col_and_doc(3,"abc");

	// 2 x caret left
	begin_mim(1);
	mimf("0Mh0Mh");
	end_mim();
	expect_col_and_doc(1,"abc");

	// insert "123" and commit
	begin_mim(1);
	mimi(0,"123");
	mimf("0!");
	end_mim();
	expect_col_and_doc(4,"123abc");

	// insert "xxx" and commit
	begin_mim(1);
	mimi(0,"xxx");
	mimf("0!");
	end_mim();
	expect_col_and_doc(7,"123xxxabc");

	// insert "y" and commit
	begin_mim(1);
	mimi(0,"y");
	mimf("0!");
	end_mim();
	expect_col_and_doc(8,"123xxxyabc");

	gig_unhost();
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

	test_regress_0a();
	test_regress_0b();
	test_regress_0c();
	test_regress_0d();

	return EXIT_SUCCESS;
}
