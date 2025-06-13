// run with test_gig.sh
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include "main.h"
#include "jio.h"
#include "gig.h"
#include "arg.h"
#include "path.h"
#include "util.h"
#include "stb_ds.h" // XXX?

int64_t get_microseconds_epoch(void)
{
	// XXX for testing purposes maybe make this "settable"?
	struct timespec ts;
	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000000LL + (ts.tv_nsec / 1000LL);
}

int64_t get_nanoseconds_monotonic(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void sleep_microseconds(int64_t us)
{
	const int64_t one_million = 1000000LL;
	const struct timespec ts = {
		.tv_nsec = (us % one_million) * 1000LL,
		.tv_sec  = us / one_million,
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
} g;

static void all_the_ticking(void)
{
	for (;;) {
		int did_work=0;
		did_work |= peer_tick();
		did_work |= host_tick();
		did_work |= io_tick();
		if (!did_work) return;
	}
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
	gig_init();
	assert(gig_configure_as_host_and_peer(path) >= 0);
	all_the_ticking();
}

static void teardown(void)
{
	all_the_ticking();
	gig_unconfigure();
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

	setup(test_dir);
	for (int pass=0; pass<3; ++pass) {
		if (VERBOSE) printf("  pass %d\n", pass);

		if (pass == 0) {
			peer_begin_mim(1);
			#if 0
			mimex("newbook 1 mie-urlyd -");
			mimex("newdoc 1 50 art.mie");
			#endif
			mimex("setdoc 1 50");
			mimf("0,1,1c");
			for (int i=0; i<N; ++i) mimi(0,"hello");
			peer_end_mim();
		}

		all_the_ticking();

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

	}
	teardown();
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
	#if 0
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	#endif
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
	#if 0
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	#endif
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
	#if 0
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	#endif
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
	#if 0
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	#endif
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

static void test_caret_adjustment(void)
{
	new_test("caradj1");
	setup(test_dir);
	peer_begin_mim(1);
	mimex("setdoc 1 50");
	mimf("0,1,1c");
	mimi(0,"abc");
	mimf("1,1,1c");
	mimi(1,"123");
	peer_end_mim();
	get_state_and_doc(1, &g.ms, &g.doc);
	assert(arrlen(g.ms->caret_arr)==2);
	struct caret c0 = g.ms->caret_arr[0];
	struct caret c1 = g.ms->caret_arr[1];

	assert(0==location_compare(&c0.caret_loc, &c0.anchor_loc));
	assert(0==location_compare(&c1.caret_loc, &c1.anchor_loc));

	assert(c0.caret_loc.line==1);
	assert(c0.caret_loc.column==7);
	assert(c1.caret_loc.line==1);
	assert(c1.caret_loc.column==4);

	peer_begin_mim(1);
	mimi(1,"\n");
	peer_end_mim();
	get_state_and_doc(1, &g.ms, &g.doc);
	c0 = g.ms->caret_arr[0];
	c1 = g.ms->caret_arr[1];
	assert(0==location_compare(&c0.caret_loc, &c0.anchor_loc));
	assert(0==location_compare(&c1.caret_loc, &c1.anchor_loc));

	assert(c0.caret_loc.line==2);
	assert(c0.caret_loc.column==4);
	assert(c1.caret_loc.line==2);
	assert(c1.caret_loc.column==1);

	peer_begin_mim(1);
	mimf("1x1x");
	peer_end_mim();
	get_state_and_doc(1, &g.ms, &g.doc);
	c0 = g.ms->caret_arr[0];
	c1 = g.ms->caret_arr[1];
	assert(0==location_compare(&c0.caret_loc, &c0.anchor_loc));
	assert(0==location_compare(&c1.caret_loc, &c1.anchor_loc));

	assert(c0.caret_loc.line==2);
	assert(c0.caret_loc.column==2);
	assert(c1.caret_loc.line==2);
	assert(c1.caret_loc.column==1);

	peer_begin_mim(1);
	mimf("1X");
	peer_end_mim();
	get_state_and_doc(1, &g.ms, &g.doc);
	c0 = g.ms->caret_arr[0];
	c1 = g.ms->caret_arr[1];
	assert(0==location_compare(&c0.caret_loc, &c0.anchor_loc));
	assert(0==location_compare(&c1.caret_loc, &c1.anchor_loc));

	assert(c0.caret_loc.line==1);
	assert(c0.caret_loc.column==5);
	assert(c1.caret_loc.line==1);
	assert(c1.caret_loc.column==4);

	// TODO '!'/'/' commit/cancel
	// TODO range delete tests
	// TODO "more of above"? (e.g. the "1X" test might not fully explore ''the
	// possibility space of "1X"'')

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

	mie_thread_init();

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

		test_caret_adjustment();

		printf("OK (gt=%d)\n", growth_threshold);
	}

	return EXIT_SUCCESS;
}

// TODO: in time some/most of this probably ought to be replaced by small
// DO_JAM_JOURNAL files combined with code that asserts the expected outcomes?
// or perhaps "automate" it completely where you can record a small session and
// then assert that the "final snapshot" stays the same?
