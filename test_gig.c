// run with test_gig.sh
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
#include "util.h"
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

const static int VERBOSE = 0;
static int growth_threshold;

static const char* base_dir;
static char* test_dir;
static int test_sequence;

static struct {
	struct mim_state* ms;
	struct document* doc;
	struct caret* cr;
	int is_up;
	mtx_t mutex;
} g;

static void LOCK(void)
{
	assert(thrd_success == mtx_lock(&g.mutex));
}

static void UNLOCK(void)
{
	assert(thrd_success == mtx_unlock(&g.mutex));
}

static int io_thread_run(void* usr)
{
	for (;;) {
		LOCK();
		if (g.is_up) {
			host_tick();
		}
		UNLOCK();
		io_tick();
		sleep_nanoseconds(500000L); // 500µs
	}
	return 0;
}

static void new_test(const char* name)
{
	memset(&g, 0, sizeof g);
	char buf[1<<10];
	++test_sequence;
	snprintf(buf, sizeof buf, "%s/test-%.4d-gt%d-%s", base_dir, test_sequence, growth_threshold, name);
	if (test_dir) {
		free(test_dir);
		test_dir = NULL;
	}
	test_dir = strdup(buf);
	assert(0 == mkdir(test_dir, 0777));
}

static void setup(const char* path)
{
	LOCK();
	assert(gig_configure_as_host_and_peer(path) >= 0);
	g.is_up = 1;
	UNLOCK();
}

static void teardown(void)
{
	LOCK();
	g.is_up = 0;
	gig_unconfigure();
	UNLOCK();
}

static void get_state_and_doc(int session_id, struct mim_state** out_ms, struct document** out_doc)
{
	struct snapshot* snap = get_snapshot();
	struct mim_state* ms = NULL;
	const int num_ms = arrlen(snap->mim_state_arr);
	for (int i=0; i<num_ms; ++i) {
		struct mim_state* m = &snap->mim_state_arr[i];
		if (m->artist_id==get_my_artist_id() && m->session_id==session_id) {
			ms = m;
			break;
		}
	}
	assert(ms != NULL);

	const int num_doc = arrlen(snap->document_arr);
	struct document* doc = NULL;
	for (int i=0; i<num_doc; ++i) {
		struct document* d = &snap->document_arr[i];
		if ((d->book_id==ms->book_id) && (d->doc_id==ms->doc_id)) {
			doc = d;
			break;
		}
	}
	assert(doc != NULL);

	if (out_ms)  *out_ms  = ms;
	if (out_doc) *out_doc = doc;
}

static void test_readwrite(int N)
{
	new_test("readwrite");

	if (VERBOSE) printf("N=%d\n", N);

	for (int pass=0; pass<3; ++pass) {
		if (VERBOSE) printf("  pass %d\n", pass);
		setup(test_dir);

		if (pass == 0) {
			peer_begin_mim(1);
			mimex("newbook 1 mie-urlyd -");
			mimex("newdoc 1 50 art.mie");
			mimex("setdoc 1 50");
			mimf("0,1,1c");
			for (int i=0; i<N; ++i) mimi(0,"hello");
			peer_end_mim();
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

		teardown();
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
	assert(g.cr->anchor_loc.line == g.cr->caret_loc.line);
	assert(g.cr->anchor_loc.column == g.cr->caret_loc.column);
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
	setup(test_dir);

	peer_begin_mim(1);
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	mimex("setdoc 1 50");
	mimf("0,1,1c");
	peer_end_mim();
	expect_col_and_doc(1,"");

	peer_begin_mim(1);
	mimi(0,"abc");
	peer_end_mim();
	expect_col_and_doc(4,"abc");

	peer_begin_mim(1);
	mimf("0Mh0Mh");
	peer_end_mim();
	expect_col_and_doc(2,"abc");

	peer_begin_mim(1);
	mimi(0,"12");
	peer_end_mim();
	expect_col_and_doc(4,"a12bc");

	teardown();

	setup(test_dir);
	expect_col_and_doc(4,"a12bc");
	teardown();
}

static void test_regress_0b(void)
{
	new_test("regress0");
	setup(test_dir);

	peer_begin_mim(1);
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	mimex("setdoc 1 50");
	mimf("0,1,1c");
	peer_end_mim();
	expect_col_and_doc(1,"");

	peer_begin_mim(1);
	mimi(0,"abc");
	peer_end_mim();
	expect_col_and_doc(4,"abc");

	peer_begin_mim(1);
	mimf("0Mh0Mh0Mh");
	peer_end_mim();
	expect_col_and_doc(1,"abc");

	peer_begin_mim(1);
	// no-op
	peer_end_mim();
	expect_col_and_doc(1,"abc");

	peer_begin_mim(1);
	mimi(0,"12");
	peer_end_mim();
	expect_col_and_doc(3,"12abc");

	teardown();

	setup(test_dir);
	expect_col_and_doc(3,"12abc");
	teardown();
}

static void test_regress_0c(void)
{
	new_test("regress1");
	setup(test_dir);

	peer_begin_mim(1);
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	mimex("setdoc 1 50");
	mimf("0,1,1c");
	peer_end_mim();
	expect_col_and_doc(1,"");

	peer_begin_mim(1);
	mimi(0,"abc");
	mimf("0Mh0Mh0Mh");
	mimi(0,"123");
	peer_end_mim();
	expect_col_and_doc(4,"123abc");

	teardown();

	setup(test_dir);
	expect_col_and_doc(4,"123abc");
	teardown();
}

static void test_regress_0d(void)
{
	new_test("regress1");
	setup(test_dir);

	peer_begin_mim(1);
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	mimex("setdoc 1 50");
	mimf("0,1,1c");
	peer_end_mim();
	expect_col_and_doc(1,"");

	// insert "abc"
	peer_begin_mim(1);
	mimi(0,"abc");
	peer_end_mim();
	expect_col_and_doc(4,"abc");

	// commit
	peer_begin_mim(1);
	mimf("0!");
	peer_end_mim();
	expect_col_and_doc(4,"abc");

	// 1 x caret left
	peer_begin_mim(1);
	mimf("0Mh");
	peer_end_mim();
	expect_col_and_doc(3,"abc");

	// 2 x caret left
	peer_begin_mim(1);
	mimf("0Mh0Mh");
	peer_end_mim();
	expect_col_and_doc(1,"abc");

	// insert "123" and commit
	peer_begin_mim(1);
	mimi(0,"123");
	mimf("0!");
	peer_end_mim();
	expect_col_and_doc(4,"123abc");

	// insert "xxx" and commit
	peer_begin_mim(1);
	mimi(0,"xxx");
	mimf("0!");
	peer_end_mim();
	expect_col_and_doc(7,"123xxxabc");

	// insert "y" and commit
	peer_begin_mim(1);
	mimi(0,"y");
	mimf("0!");
	peer_end_mim();
	expect_col_and_doc(8,"123xxxyabc");

	teardown();

	setup(test_dir);
	expect_col_and_doc(8,"123xxxyabc");

	peer_begin_mim(1);
	mimf("0Mh0Mh0Mh0Mh");
	peer_end_mim();
	expect_col_and_doc(4,"123xxxyabc");

	peer_begin_mim(1);
	mimi(0,"--");
	peer_end_mim();
	expect_col_and_doc(6,"123--xxxyabc");

	teardown();

	setup(test_dir);
	expect_col_and_doc(6,"123--xxxyabc");
	teardown();
}

int webserv_broadcast_journal(int64_t until_journal_cursor)
{
	return 0;
}

void transmit_mim(int mim_session_id, int64_t tracer, uint8_t* data, int count)
{
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <dir>\n", argv[0]);
		fprintf(stderr, "(it creates test files inside that dir)\n");
		exit(EXIT_FAILURE);
	}
	base_dir = argv[1];

	assert(thrd_success == mtx_init(&g.mutex, mtx_plain));

	thrd_t t = {0};
	assert(0 == thrd_create(&t, io_thread_run, NULL));

	mie_thread_init();
	gig_init();

	const int ts[] = {1000,100,10};
	for (int i=0; i<ARRAY_LENGTH(ts); ++i) {
		growth_threshold = ts[i];
		gig_set_journal_snapshot_growth_threshold(growth_threshold);

		test_readwrite(100);
		test_readwrite(1);
		test_readwrite(5);
		test_readwrite(50);
		test_readwrite(100);

		test_regress_0a();
		test_regress_0b();
		test_regress_0c();
		test_regress_0d();

		printf("OK (gt=%d)\n", growth_threshold);
	}

	return EXIT_SUCCESS;
}
