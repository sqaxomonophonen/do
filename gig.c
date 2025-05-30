#include <limits.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h> // XXX

#include "stb_ds.h"
#include "stb_sprintf.h"
#include "io.h"
#include "jio.h"
#include "gig.h"
#include "util.h"
#include "leb128.h"
#include "binary.h"
#include "utf8.h"
#include "path.h"
#include "main.h"
#include "arg.h"

#define DOJO_MAGIC ("DOJO0001")
#define DOSI_MAGIC ("DOSI0001")
#define DOSD_MAGIC ("DOSD0001")
#define DO_FORMAT_VERSION (1)
#define SYNC (0xfa)
#define FILENAME_JOURNAL              "DO_JAM_JOURNAL"
#define FILENAME_SNAPSHOTCACHE_DATA   "snapshotcache.data"
#define FILENAME_SNAPSHOTCACHE_INDEX  "snapshotcache.index"

enum fmterr {
	// these errors can be returned as io_status and should not clash with
	// `enum ioerr` (see io.h)
	FMT_ERROR = -30000,
};

static int match_fundament(const char* s)
{
	#define X(ENUM,STR) if (0 == strcmp(STR,s)) return ENUM;
	LIST_OF_FUNDAMENTS
	#undef X
	return _NO_FUNDAMENT_;
}

#define ARRINIT0(P,A) { assert((P)==NULL && "bad scrallox memory corruption bugs may happen if P is not NULL"); arrinit(P,A); }

static int document_equal(struct document* a, struct document* b)
{
	if (a->book_id != b->book_id) return 0;
	if (a->doc_id != b->doc_id) return 0;
	const int nna = arrlen(a->name_arr);
	const int nnb = arrlen(b->name_arr);
	if (nna != nnb) return 0;
	if (0 != memcmp(a->name_arr, b->name_arr, nna)) return 0;
	const int nda = arrlen(a->docchar_arr);
	const int ndb = arrlen(b->docchar_arr);
	if (nda != ndb) return 0;
	if (0 != memcmp(a->docchar_arr, b->docchar_arr, nda*sizeof(a->docchar_arr[0]))) return 0;
	return 1;
}

static int mim_state_equal(struct mim_state* a, struct mim_state* b)
{
	if (a->artist_id != b->artist_id) return 0;
	if (a->session_id != b->session_id) return 0;
	if (a->book_id != b->book_id) return 0;
	if (a->doc_id != b->doc_id) return 0;
	if (a->splash4 != b->splash4) return 0;
	const int na = arrlen(a->caret_arr);
	const int nb = arrlen(b->caret_arr);
	if (na != nb) return 0;
	if (0 != memcmp(a->caret_arr, b->caret_arr, na*sizeof(a->caret_arr[0]))) return 0;
	return 1;
}

static void document_copy(struct document* dst, struct document* src)
{
	struct document tmp = *dst;
	*dst = *src;
	dst->docchar_arr = tmp.docchar_arr;
	if (dst->docchar_arr == NULL) arrinit(dst->docchar_arr, &system_allocator);
	///printf("%zd / %zd\n", arrlen(dst->docchar_arr), arrlen(src->docchar_arr));
	arrcpy(dst->docchar_arr, src->docchar_arr);
	dst->name_arr = tmp.name_arr;
	if (dst->name_arr == NULL) arrinit(dst->name_arr, &system_allocator);
	arrcpy(dst->name_arr, src->name_arr);
}

static void mim_state_copy(struct mim_state* dst, struct mim_state* src)
{
	struct mim_state tmp = *dst;
	*dst = *src;
	dst->caret_arr = tmp.caret_arr;
	arrcpy(dst->caret_arr, src->caret_arr);
}

static int document_locate(struct document* doc, struct location* loc)
{
	struct doc_iterator it = doc_iterator(doc);
	doc_iterator_locate(&it, loc);
	return it.offset;
}

struct snapshot {
	struct book* book_arr;
	struct document*  document_arr;
	struct mim_state* mim_state_arr;
};

static int snapshot_get_num_documents(struct snapshot* ss)
{
	return arrlen(ss->document_arr);
}

static struct document* snapshot_get_document_by_index(struct snapshot* ss, int index)
{
	return arrchkptr(ss->document_arr, index);
}

static struct document* snapshot_lookup_document_by_ids(struct snapshot* ss, int book_id, int doc_id)
{
	const int n = snapshot_get_num_documents(ss);
	for (int i=0; i<n; ++i) {
		struct document* doc = snapshot_get_document_by_index(ss, i);
		if ((doc->book_id == book_id) && (doc->doc_id == doc_id)) {
			return doc;
		}
	}
	return NULL;
}

#if 0
static struct document* snapshot_get_document_by_ids(struct snapshot* ss, int book_id, int doc_id)
{
	struct document* doc = snapshot_lookup_document_by_ids(ss, book_id, doc_id);
	assert((doc != NULL) && "document not found by id");
	return doc;
}
#endif

static struct mim_state* snapshot_lookup_mim_state_by_ids(struct snapshot* ss, int artist_id, int session_id)
{
	const int n = arrlen(ss->mim_state_arr);
	for (int i=0; i<n; ++i) {
		struct mim_state* s = arrchkptr(ss->mim_state_arr, i);
		if ((s->artist_id == artist_id) &&  (s->session_id == session_id)) {
			return s;
		}
	}
	return NULL;
	assert(!"not found");
}

static struct mim_state* snapshot_get_mim_state_by_ids(struct snapshot* ss, int artist_id, int session_id)
{
	struct mim_state* ms = snapshot_lookup_mim_state_by_ids(ss, artist_id, session_id);
	assert(ms != NULL);
	return ms;
}

static struct mim_state* snapshot_get_or_create_mim_state_by_ids(struct snapshot* ss, int artist_id, int session_id)
{
	struct mim_state* ms = snapshot_lookup_mim_state_by_ids(ss, artist_id, session_id);
	if (ms != NULL) return ms;
	ms = arraddnptr(ss->mim_state_arr, 1);
	memset(ms, 0, sizeof *ms);
	ms->artist_id  = artist_id,
	ms->session_id = session_id,
	arrinit(ms->caret_arr, &system_allocator);
	return ms;
}

// "cow" stands for "copy-on-write". moo! it represents pending changes to a
// snapshot, and only contains the changed documents and mim states
struct cow_snapshot {
	struct allocator* allocator;
	struct snapshot* ref;
	struct document* cow_document_arr;
	struct book* new_book_arr;
	int* delete_book_arr;
	struct mim_state mim_state;
};

static void init_cow_snapshot(struct cow_snapshot* cows, struct snapshot* ref, struct mim_state* ms, struct allocator* a)
{
	memset(cows, 0, sizeof *cows);
	cows->allocator = a;
	cows->ref = ref;
	ARRINIT0(cows->cow_document_arr , cows->allocator);
	ARRINIT0(cows->new_book_arr     , cows->allocator);
	ARRINIT0(cows->delete_book_arr  , cows->allocator);
	struct mim_state* cms = &cows->mim_state;
	ARRINIT0(cms->caret_arr, cows->allocator);
	mim_state_copy(cms, ms);
}

static struct document* cow_snapshot_lookup_cow_document_by_ids(struct cow_snapshot* cows, int book_id, int doc_id)
{
	const int n = arrlen(cows->cow_document_arr);
	for (int i=0; i<n; ++i) {
		struct document* doc = &cows->cow_document_arr[i];
		if ((doc->book_id == book_id) && (doc->doc_id == doc_id)) {
			return doc;
		}
	}
	return NULL;
}

static struct document* cow_snapshot_lookup_readonly_document_by_ids(struct cow_snapshot* cows, int book_id, int doc_id)
{
	struct document* doc = cow_snapshot_lookup_cow_document_by_ids(cows, book_id, doc_id);
	if (doc != NULL) return doc;
	return snapshot_lookup_document_by_ids(cows->ref, book_id, doc_id);
}

static struct document* cow_snapshot_get_readonly_document_by_ids(struct cow_snapshot* cows, int book_id, int doc_id)
{
	struct document* doc = cow_snapshot_lookup_readonly_document_by_ids(cows, book_id, doc_id);
	assert(doc != NULL);
	return doc;
}

static struct document* cow_snapshot_lookup_readwrite_document_by_ids(struct cow_snapshot* cows, int book_id, int doc_id)
{
	struct document* doc = cow_snapshot_lookup_cow_document_by_ids(cows, book_id, doc_id);
	if (doc != NULL) return doc;

	struct document* src_doc = snapshot_lookup_document_by_ids(cows->ref, book_id, doc_id);
	if (src_doc == NULL) return NULL;

	if (cows->cow_document_arr == NULL) {
		ARRINIT0(cows->cow_document_arr, cows->allocator);
	}

	struct document* dst_doc = arraddnptr(cows->cow_document_arr, 1);
	memcpy(dst_doc, src_doc, sizeof *dst_doc);

	dst_doc->name_arr = NULL;
	ARRINIT0(dst_doc->name_arr, cows->allocator);
	arrcpy(dst_doc->name_arr, src_doc->name_arr);

	dst_doc->docchar_arr = NULL;
	ARRINIT0(dst_doc->docchar_arr, cows->allocator);
	arrcpy(dst_doc->docchar_arr, src_doc->docchar_arr);

	return dst_doc;
}

static struct document* cow_snapshot_get_readwrite_document_by_ids(struct cow_snapshot* cows, int book_id, int doc_id)
{
	struct document* doc = cow_snapshot_lookup_readwrite_document_by_ids(cows, book_id, doc_id);
	assert(doc != NULL);
	return doc;
}

static void cow_snapshot_commit(struct cow_snapshot* cows)
{
	assert((arrlen(cows->delete_book_arr) == 0) && "TODO delete books");

	const int num_new_books = arrlen(cows->new_book_arr);
	for (int i=0; i<num_new_books; ++i) {
		struct book* new_book = &cows->new_book_arr[i];
		const int num_books = arrlen(cows->ref->book_arr);
		for (int ii=0; ii<num_books; ++ii) {
			struct book* book = &cows->ref->book_arr[ii];
			assert((new_book->book_id != book->book_id) && "book id clash should've been resolved earlier");
		}
		arrput(cows->ref->book_arr, *new_book);
	}

	const int num_cow_doc = arrlen(cows->cow_document_arr);
	for (int i=0; i<num_cow_doc; ++i) {
		struct document* src_doc = &cows->cow_document_arr[i];
		struct document* dst_doc = snapshot_lookup_document_by_ids(cows->ref, src_doc->book_id, src_doc->doc_id);
		if (dst_doc) {
			if (!document_equal(dst_doc, src_doc)) {
				document_copy(dst_doc, src_doc);
				dst_doc->snapshotcache_offset = 0;
			}
		} else {
			struct document new_doc = *src_doc;
			new_doc.name_arr = NULL;
			arrinit(new_doc.name_arr, &system_allocator);
			arrcpy(new_doc.name_arr, src_doc->name_arr);
			new_doc.docchar_arr = NULL;
			arrinit(new_doc.docchar_arr, &system_allocator);
			arrcpy(new_doc.docchar_arr, src_doc->docchar_arr);
			arrput(cows->ref->document_arr, new_doc);
		}
	}
	if (num_cow_doc>0) arrsetlen(cows->cow_document_arr, 0);

	struct mim_state* src_ms = &cows->mim_state;
	struct mim_state* dst_ms = snapshot_get_mim_state_by_ids(cows->ref, src_ms->artist_id, src_ms->session_id);
	if (!mim_state_equal(dst_ms, src_ms)) {
		mim_state_copy(dst_ms, src_ms);
		dst_ms->snapshotcache_offset = 0;
	}
}

static struct state {
	struct snapshot cool_snapshot, hot_snapshot;
	int my_artist_id;
	uint64_t journal_insignia;
	int64_t journal_offset_at_last_snapshotcache_push;
	struct jio* jio_journal;
	struct jio* jio_snapshotcache_data;
	struct jio* jio_snapshotcache_index;
	int64_t journal_timestamp_start;
} gst;

static struct {
	int journal_snapshot_growth_threshold;
	uint8_t* mim_buffer_arr;
	int using_mim_session_id;
	int in_mim;
	char errormsg[1<<14];
	int io_port_id;
} g;

void gig_tick(void)
{
	struct io_event ev = {0};
	while (io_port_poll(g.io_port_id, &ev)) {
		io_echo ec = ev.echo;
		if (gst.jio_journal             && jio_ack(gst.jio_journal             , ec)) continue;
		if (gst.jio_snapshotcache_data  && jio_ack(gst.jio_snapshotcache_data  , ec)) continue;
		if (gst.jio_snapshotcache_index && jio_ack(gst.jio_snapshotcache_index , ec)) continue;
		assert(!"unhandled event");
	}
}

static char* get_mim_buffer_top(void)
{
	arrsetmincap(g.mim_buffer_arr, arrlen(g.mim_buffer_arr) + STB_SPRINTF_MIN);
	return (char*)g.mim_buffer_arr + arrlen(g.mim_buffer_arr);
}

static void mimsrc(uint8_t* src, size_t n)
{
	uint8_t* dst = arraddnptr(g.mim_buffer_arr, n);
	memcpy(dst, src, n);
	(void)get_mim_buffer_top();
}

static char* wrote_mim_cb(const char* buf, void* user, int len)
{
	arrsetlen(g.mim_buffer_arr, arrlen(g.mim_buffer_arr)+len);
	return get_mim_buffer_top();
}

void mimf(const char* fmt, ...)
{
	assert(g.in_mim);
	va_list va;
	va_start(va, fmt);
	stbsp_vsprintfcb(wrote_mim_cb, NULL, get_mim_buffer_top(), fmt, va);
	va_end(va);
}

void mim8(uint8_t v)
{
	mimsrc(&v, 1);
}

void mimex(const char* ex)
{
	mimf("%zd:%s", strlen(ex), ex);
}

void mimi(int tag, const char* text)
{
	mimf("%d,%zdi%s", tag, strlen(text), text);
}

int get_my_artist_id(void)
{
	assert((gst.my_artist_id>0) && "artist id not initialized");
	return gst.my_artist_id;
}

void get_state_and_doc(int session_id, struct mim_state** out_mim_state, struct document** out_doc)
{
	struct snapshot* ss = &gst.cool_snapshot;
	const int artist_id = get_my_artist_id();
	const int num_states = arrlen(ss->mim_state_arr);
	for (int i=0; i<num_states; ++i) {
		struct mim_state* ms = arrchkptr(ss->mim_state_arr, i);
		if ((ms->artist_id==artist_id) && (ms->session_id==session_id)) {
			const int book_id = ms->book_id;
			assert((book_id > 0) && "invalid book id in mim state");
			const int doc_id = ms->doc_id;
			assert((doc_id > 0) && "invalid document id in mim state");
			struct document* doc = NULL;
			const int num_documents = arrlen(ss->document_arr);
			for (int i=0; i<num_documents; ++i) {
				struct document* d = arrchkptr(ss->document_arr, i);
				if ((d->book_id == book_id) && (d->doc_id == doc_id)) {
					doc = d;
					break;
				}
			}
			assert((doc != NULL) && "invalid document id (not found) in mim state");
			if (out_mim_state) *out_mim_state = ms;
			if (out_doc) *out_doc = doc;
			return;
		}
	}
	assert(!"state not found");
}

FORMATPRINTF1
static int mimerr(const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	char msg[1<<12];
	stbsp_vsnprintf(msg, sizeof msg, fmt, va);
	va_end(va);
	fprintf(stderr, "MIMERR :: [%s]\n", msg);
	return -1;
}

static void doc_location_constraint(struct document* doc, struct location* loc)
{
	struct doc_iterator it = doc_iterator(doc);
	doc_iterator_locate(&it, loc);
	*loc = it.location;
}

struct mimop {
	struct cow_snapshot* cows;
};

static int mimop_get_book_id(struct mimop* mo)
{
	return mo->cows->mim_state.book_id;
}

static int mimop_get_doc_id(struct mimop* mo)
{
	return mo->cows->mim_state.doc_id;
}

static int mimop_has_doc(struct mimop* mo)
{
	const int book_id = mimop_get_book_id(mo);
	const int doc_id  = mimop_get_doc_id(mo);
	if ((book_id <= 0) || (doc_id <= 0)) return 0;
	return (NULL != cow_snapshot_lookup_readonly_document_by_ids(mo->cows, book_id, doc_id));
}

static struct document* mimop_get_readonly_doc(struct mimop* mo)
{
	const int book_id = mimop_get_book_id(mo);
	const int doc_id  = mimop_get_doc_id(mo);
	return cow_snapshot_get_readonly_document_by_ids(mo->cows, book_id, doc_id);
}

static struct document* mimop_get_readwrite_doc(struct mimop* mo)
{
	const int book_id = mimop_get_book_id(mo);
	const int doc_id  = mimop_get_doc_id(mo);
	return cow_snapshot_get_readwrite_document_by_ids(mo->cows, book_id, doc_id);
}

static struct mim_state* mimop_ms(struct mimop* mo)
{
	return &mo->cows->mim_state;
}

static void mimop_delete(struct mimop* mo, struct location* loc0, struct location* loc1)
{
	struct document* doc = mimop_get_readwrite_doc(mo);
	location_sort2(&loc0, &loc1);
	const int o0 = document_locate(doc, loc0);
	int o1 = document_locate(doc, loc1);
	for (int o=o0; o<o1; ++o) {
		struct docchar* fc = arrchkptr(doc->docchar_arr, o);
		if (fc->flags & FC_IS_INSERT) {
			arrdel(doc->docchar_arr, arrchk(doc->docchar_arr, o));
			--o;
			--o1;
		} else {
			fc->flags |= FC_IS_DELETE;
		}
	}
}

struct mimexscanner {
	struct allocator* alloc;
	char* all;
	const char* cmd;
	const char* err;
	char* cur;
	const char* end;
	int cmdlen;
	unsigned did_match :1;
	unsigned has_error :1;
};

static int mimexscanner_errorf(struct mimexscanner* s, const char* fmt, ...)
{
	const size_t max = 1L<<12;
	char* err = allocator_malloc(s->alloc, max);
	va_list va;
	va_start(va, fmt);
	stbsp_vsnprintf(err, max, fmt, va);
	va_end(va);
	s->err = err;
	s->has_error = 1;
	return -1;
}

static int mimexscanner_init(struct mimexscanner* s, struct allocator* alloc, const char* c0, const char* c1)
{
	memset(s, 0, sizeof *s);
	s->alloc = alloc;

	const size_t num_bytes = (c1-c0);
	s->all = allocator_malloc(alloc, num_bytes+1);
	memcpy(s->all, c0, num_bytes);

	// search for space in command if it exists, so whether
	// input is ":foo" or ":foo 42" we want the "foo" part
	// (which is the "ex" command, and the rest is arguments)
	int cmdlen=0;
	const char* p=c0;
	for (p=c0; p<c1; ++cmdlen, ++p) {
		if (*p == ' ') break;
	}
	if (cmdlen == 0) return mimexscanner_errorf(s, "empty command");
	s->all[cmdlen] = 0;
	s->cmd = s->all;
	s->cmdlen = cmdlen;
	s->cur = s->all + cmdlen + 1;
	s->end = s->all + num_bytes;
	return 0;
}

static const char* mimexscanner_next_arg(struct mimexscanner* s)
{
	const char* p0 = s->cur;
	int num=0;
	for (; s->cur < s->end; ++s->cur) {
		if (*s->cur == ' ') {
			if (num == 0) continue;
			break;
		}
		++num;
	}
	if (num==0) {
		(void)mimexscanner_errorf(s, "expected more arguments");
		return NULL;
	}
	*s->cur = 0;
	++s->cur;
	return p0;
}

static int parse_nonnegative_int(const char* s)
{
	long i=0;
	int n=0;
	for (const char* p=s; *p && n<10; ++n,++p) {
		const char c = *p;
		if (!is_digit(c)) return -1;
		i = (10*i) + (c-'0');
	}
	if ((INT_MIN <= i) && (i <= INT_MAX)) {
		return (int)i;
	} else {
		return -1;
	}
}

static int mimex_matches(struct mimexscanner* s, const char* cmd, const char* fmt, ...)
{
	// don't continue scanning if there's an error or we already found a match
	if ((s->did_match) || (s->has_error)) return 0;
	if (strcmp(cmd, s->cmd) != 0) return 0;

	s->did_match = 1;

	const int num_fmt = strlen(fmt);
	va_list va;
	va_start(va, fmt);
	for (int i=0; i<num_fmt; ++i) {
		if (s->has_error) break;
		switch (fmt[i]) {

		case 'i': {
			const char* arg = mimexscanner_next_arg(s);
			if (s->has_error) break;
			assert(arg != NULL);
			int* p = va_arg(va, int*);
			*p = parse_nonnegative_int(arg);
			if (*p < 0) {
				(void)mimexscanner_errorf(s, "invalid int arg [%s]", arg);
				break;
			}
		}	break;

		case 's': {
			const char* arg = mimexscanner_next_arg(s);
			if (s->has_error) break;
			assert(arg != NULL);
			const char** p = va_arg(va, const char**);
			*p = arg;
		}	break;

		default: assert(!"unhandled mimex_matches() fmt char");
		}
	}
	va_end(va);
	return !s->has_error;
}

// parses a mim message, typically written by mimf()/mim8()
static int mim_spool(struct mimop* mo, const uint8_t* input, int num_input_bytes)
{
	const char* input_cursor = (const char*)input;
	int remaining = num_input_bytes;

	static int* number_stack_arr = NULL;
	if (number_stack_arr == NULL) arrinit(number_stack_arr, &system_allocator);
	arrreset(number_stack_arr);

	enum {
		COMMAND=1,
		NUMBER,
		INSERT_STRING,
		INSERT_COLOR_STRING,
		MOTION,
		EX,
	};

	enum { UTF8, U16 };

	int mode = COMMAND;
	int datamode = UTF8;
	int previous_mode = -1;
	int number=0, number_sign=0;
	int push_chr = -1;
	int suffix_bytes_remaining = 0;
	int arg_tag = -1;
	int arg_num = -1;
	int motion_cmd = -1;
	const int64_t now = get_nanoseconds();
	int chr=0;
	uint16_t u16val=0;
	const char* ex0 = NULL;

	while ((push_chr>=0) || (remaining>0)) {
		int expect_suffix_bytes = 0;
		const char* p0 = input_cursor;
		if (datamode == UTF8) {
			chr = (push_chr>=0) ? push_chr : utf8_decode(&input_cursor, &remaining);
		} else if (datamode == U16) {
			assert(push_chr == -1);
			assert(remaining >= 2);
			const uint8_t* p8 = (const uint8_t*)input_cursor;
			u16val = leu16_pdecode(&p8);
			input_cursor = (const char*)p8;
			remaining -= 2;
		} else {
			assert(!"unhandled datamode");
		}
		const char* p1 = input_cursor;
		const int num_bytes = (p1-p0);
		push_chr = -1;
		if (chr == -1) return mimerr("invalid UTF-8 input");
		const int num_args = arrlen(number_stack_arr);

		switch (mode) {

		case COMMAND: {
			if (is_digit(chr) || chr=='-') {
				previous_mode = mode;
				mode = NUMBER;
				number = 0;
				if (chr == '-') {
					number_sign = -1;
				} else {
					number_sign = 1;
					push_chr = chr;
				}
			} else {
				switch (chr) {

				case ':': { // mimex command
					if (num_args != 1) {
						return mimerr("command '%c' expected 1 argument; got %d", chr, num_args);
					}
					suffix_bytes_remaining = arrchkget(number_stack_arr, 0);
					if (suffix_bytes_remaining > 0) {
						mode = EX;
						ex0 = input_cursor; // NOTE points at /next/ character after ':'
					}
				}	break;

				case '!': // commit
				case '/': // cancel
				case '-': // defer
				case '+': // fer (XXX stops at defer, hmm...)
				case '*': // toggle fer/defer..?
				{
					if (num_args != 1) {
						return mimerr("command '%c' expected 1 argument; got %d", chr, num_args);
					}
					arg_tag = arrchkget(number_stack_arr, 0);
					arrreset(number_stack_arr);

					if (!mimop_has_doc(mo)) return mimerr("command '%c' requires doc; mim state has none", chr);

					struct document* doc = mimop_get_readwrite_doc(mo);
					struct mim_state* ms = mimop_ms(mo);
					int num_chars = arrlen(doc->docchar_arr);
					const int num_carets = arrlen(ms->caret_arr);
					for (int i=0; i<num_carets; ++i) {
						struct caret* car = arrchkptr(ms->caret_arr, i);
						if (car->tag != arg_tag) continue;
						struct location* loc0 = &car->caret_loc;
						struct location* loc1 = &car->anchor_loc;
						location_sort2(&loc0, &loc1);
						const int off0 = document_locate(doc, loc0);
						const int off1 = document_locate(doc, loc1);
						for (int off=off0; off<=off1; ++off) {
							for (int dir=0; dir<2; ++dir) {
								int d,o;
								if      (dir==0) { d= 1; o=off  ; }
								else if (dir==1) { d=-1; o=off-1; }
								else assert(!"unreachable");
								while ((0 <= o) && (o < num_chars)) {
									struct docchar* fc = arrchkptr(doc->docchar_arr, o);
									const int is_fillable = fc->flags & (FC_IS_INSERT | FC_IS_DELETE);
									if ((fc->flags & (FC__FILL | FC_IS_DEFER)) || !is_fillable) break;
									if (is_fillable) fc->flags |= FC__FILL;
									o += d;
								}
							}
						}
						car->anchor_loc = car->caret_loc;
					}

					for (int i=0; i<num_chars; ++i) {
						struct docchar* fc = arrchkptr(doc->docchar_arr, i);
						if (!(fc->flags & FC__FILL)) continue;

						if (chr=='!') {
							if (fc->flags & FC_IS_INSERT) { // commit
								fc->flags &= ~(FC__FILL | FC_IS_INSERT);
							}
							if (fc->flags & FC_IS_DELETE) {
								arrdel(doc->docchar_arr, arrchk(doc->docchar_arr, i));
								--i;
								--num_chars;
							}
						} else if (chr=='/') { // cancel
							if (fc->flags & FC_IS_INSERT) {
								arrdel(doc->docchar_arr, arrchk(doc->docchar_arr, i));
								--i;
								--num_chars;
							}
							if (fc->flags & FC_IS_DELETE) {
								fc->flags &= ~(FC__FILL | FC_IS_DELETE);
							}
						} else {
							assert(!"unhandled command");
						}
					}

				}	break;

				case 'S':   // caret movement
				case 'M': { // anchor movement
					if (num_args != 1) {
						return mimerr("command '%c' expected 1 argument; got %d", chr, num_args);
					}
					arg_tag = arrchkget(number_stack_arr, 0);
					arrreset(number_stack_arr);
					if (!mimop_has_doc(mo)) return mimerr("command '%c' requires doc; mim state has none", chr);
					previous_mode = mode;
					motion_cmd = chr;
					mode = MOTION;
				}	break;

				case 'c': { // add caret
					if (num_args != 3) {
						return mimerr("command '%c' expected 3 arguments; got %d", chr, num_args);
					}
					arg_tag = arrchkget(number_stack_arr, 0);
					const int line   = arrchkget(number_stack_arr, 1);
					const int column = arrchkget(number_stack_arr, 2);
					arrreset(number_stack_arr);
					struct mim_state* ms = mimop_ms(mo);
					assert(ms->caret_arr != NULL);
					arrput(ms->caret_arr, ((struct caret) {
						.tag = arg_tag,
						.caret_loc  = { .line=line, .column=column },
						.anchor_loc = { .line=line, .column=column },
					}));
				}	break;

				case '~': { // set mim state color
					if (num_args != 1) {
						return mimerr("command '%c' expected 1 argument; got %d", chr, num_args);
					}
					const int splash4 = arrchkget(number_stack_arr, 0);
					arrreset(number_stack_arr);
					if (!is_valid_splash4(splash4)) {
						return mimerr("invalid splash4 color %d", splash4);
					}
					struct mim_state* ms = mimop_ms(mo);
					ms->splash4 = splash4;
				}	break;

				case 'P': { // paint selection with color
					if (num_args != 1) {
						return mimerr("command '%c' expected 1 argument; got %d", chr, num_args);
					}
					arg_tag = arrchkget(number_stack_arr, 0);
					arrreset(number_stack_arr);
					if (!mimop_has_doc(mo)) return mimerr("command '%c' requires doc; mim state has none", chr);
					struct document* doc = mimop_get_readwrite_doc(mo);
					struct mim_state* ms = mimop_ms(mo);
					const int num_carets = arrlen(ms->caret_arr);
					for (int i=0; i<num_carets; ++i) {
						struct caret* car = arrchkptr(ms->caret_arr, i);
						struct location* loc0 = &car->caret_loc;
						struct location* loc1 = &car->anchor_loc;
						location_sort2(&loc0, &loc1);
						const int off0 = document_locate(doc, loc0);
						const int off1 = document_locate(doc, loc1);
						assert(off0 <= off1);
						for (int o=off0; o<off1; ++o) {
							struct docchar* dc = arrchkptr(doc->docchar_arr, o);
							dc->colorchar.splash4 = ms->splash4;
						}
						car->anchor_loc = car->caret_loc;
					}
				}	break;

				case 'i':   // text insert
				case 'I': { // color text insert
					if (num_args != 2) {
						return mimerr("command '%c' expected 2 arguments; got %d", chr, num_args);
					}
					assert(suffix_bytes_remaining == 0);
					suffix_bytes_remaining = arrchkget(number_stack_arr, 1);
					arg_tag = arrchkget(number_stack_arr, 0);
					arrreset(number_stack_arr);
					if (suffix_bytes_remaining > 0) {
						if (!mimop_has_doc(mo)) return mimerr("command '%c' requires doc; mim state has none", chr);
						previous_mode = mode;
						switch (chr) {
						case 'i': mode = INSERT_STRING; break;
						case 'I': mode = INSERT_COLOR_STRING; break;
						default: assert(!"unexpected chr");
						}
					}
				}	break;

				case 'X': // backspace
				case 'x': // delete
				{
					if (num_args != 1) {
						return mimerr("command '%c' expected 1 argument; got %d", chr, num_args);
					}
					arg_tag = arrchkget(number_stack_arr, 0);
					arg_num = 1; // XXX make it an optional arg?
					arrreset(number_stack_arr);

					if (!mimop_has_doc(mo)) return mimerr("command '%c' requires doc; mim state has none", chr);

					struct document* doc = mimop_get_readwrite_doc(mo);
					struct mim_state* ms = mimop_ms(mo);
					const int num_carets = arrlen(ms->caret_arr);
					for (int i=0; i<num_carets; ++i) {
						struct caret* car = arrchkptr(ms->caret_arr, i);
						if (car->tag != arg_tag) continue;
						struct location* caret_loc = &car->caret_loc;
						struct location* anchor_loc = &car->anchor_loc;
						if (0 == location_compare(caret_loc, anchor_loc)) {
							int o = document_locate(doc, caret_loc);
							int d,m0,m1;
							if (chr == 'X') { // backspace
								d=-1; m0=-1; m1=-1;
							} else if (chr == 'x') { // delete
								d=0;  m0=0;  m1=1;
							} else {
								assert(!"unexpected char");
							}

							for (int i=0; i<arg_num; ++i) {
								const int num_chars = arrlen(doc->docchar_arr);
								const int od = o+d;
								int dc=0;
								if ((0 <= od) && (od < num_chars)) {
									struct docchar* fc = arrchkptr(doc->docchar_arr, od);
									if (fc->flags & FC_IS_INSERT) {
										arrdel(doc->docchar_arr, od);
										dc=m0;
									} else if (fc->flags & FC_IS_DELETE) {
										dc=m1;
									} else {
										assert(!(fc->flags & FC_IS_INSERT));
										assert(!(fc->flags & FC_IS_DELETE));
										fc->flags |= (FC_IS_DELETE | FC__FLIPPED_DELETE);
										fc->timestamp = now;
										dc=m1;
									}
								}
								caret_loc->column += dc;
								doc_location_constraint(doc, caret_loc);
								o += dc;
							}
						} else {
							mimop_delete(mo, caret_loc, anchor_loc);
						}
						*anchor_loc = *caret_loc;
					}
				}	break;

				default:
					return mimerr("invalid command '%c'/%d", chr, chr);

				}
			}
		}	break;

		case NUMBER: {
			if (is_digit(chr)) {
				number = ((10*number) + (chr-'0'));
			} else {
				number *= number_sign;
				arrput(number_stack_arr, number);
				mode = previous_mode;
				if (chr != ',') push_chr = chr;
			}
		}	break;

		case INSERT_STRING:
		case INSERT_COLOR_STRING: {
			assert(mimop_has_doc(mo));
			int do_insert = 0;
			int next_datamode = datamode;
			if (mode == INSERT_STRING) {
				assert(datamode == UTF8);
				do_insert = 1;
			} else if (mode == INSERT_COLOR_STRING) {
				if (datamode == U16) {
					do_insert = 1;
					next_datamode = UTF8;
				} else {
					next_datamode = U16;
				}
			} else {
				assert(!"unreachable");
			}

			if (do_insert) {
				if (chr < ' ') {
					switch (chr) {
					case '\n':
					case '\t': // XXX consider not supporting tabs?
						// accepted control codes
						break;
					default:
						return mimerr("invalid control code %d in string", chr);
					}
				}

				struct mim_state* ms = mimop_ms(mo);

				uint16_t splash4=0;
				if (mode == INSERT_STRING) {
					// insert with artist mim state color
					splash4 = ms->splash4;
				} else if (mode == INSERT_COLOR_STRING) {
					// insert with encoded color
					splash4 = u16val;
				} else {
					assert(!"unreachable");
				}

				if (!is_valid_splash4(splash4)) {
					return mimerr("invalid splash4 value (%d)", splash4);
				}

				struct document* doc = mimop_get_readwrite_doc(mo);
				const int num_carets = arrlen(ms->caret_arr);
				for (int i=0; i<num_carets; ++i) {
					struct caret* car = arrchkptr(ms->caret_arr, i);
					if (car->tag != arg_tag) continue;

					struct location* loc = &car->caret_loc;
					struct location* anchor = &car->anchor_loc;
					if (0 != location_compare(loc, anchor)) mimop_delete(mo, loc, anchor);
					*anchor = *loc;

					const int off = document_locate(doc, loc);
					assert(doc->docchar_arr != NULL);
					const int n0 = arrlen(doc->docchar_arr);
					arrins(doc->docchar_arr, off, ((struct docchar){
						.colorchar = {
							.codepoint = chr,
							.splash4 = splash4,
						},
						.timestamp = now,
						//.artist_id = get_my_artist_id(),
						.flags = (FC_IS_INSERT | FC__FLIPPED_INSERT),
					}));
					const int n1 = arrlen(doc->docchar_arr);
					assert(n1==(n0+1));
					//printf("%d => INS [%c] => %d\n", n0, chr, n1);
					if (chr == '\n') {
						++loc->line;
						loc->column = 1;
					} else {
						++loc->column;
					}
					doc_location_constraint(doc, loc);
					*anchor = *loc;
				}
			}

			datamode = next_datamode;
			expect_suffix_bytes = 1;
		}	break;

		case EX: {
			expect_suffix_bytes = 1;
		}	break;

		case MOTION: {

			assert(arrlen(number_stack_arr) == 0);
			// TODO read number?

			switch (chr) {

			case 'h': // left
			case 'l': // right
			case 'k': // up
			case 'j': // down
			{
				assert(mimop_has_doc(mo));

				const int is_left  = (chr=='h');
				const int is_right = (chr=='l');
				const int is_up    = (chr=='k');
				const int is_down  = (chr=='j');

				struct mim_state* ms = mimop_ms(mo);
				const int num_carets = arrlen(ms->caret_arr);
				struct document* readonly_doc = mimop_get_readonly_doc(mo);
				for (int i=0; i<num_carets; ++i) {
					struct caret* car = arrchkptr(ms->caret_arr, i);
					if (car->tag != arg_tag) continue;
					struct location* loc0 = motion_cmd=='S' ? &car->caret_loc : &car->anchor_loc;
					struct location* loc1 = &car->caret_loc;
					location_sort2(&loc0, &loc1);
					if (0 == location_compare(loc0,loc1)) {
						if (is_left) {
							--loc0->column;
							doc_location_constraint(readonly_doc, loc0);
						}
						if (is_right) {
							++loc1->column;
							doc_location_constraint(readonly_doc, loc1);
						}
						if (is_up) {
							--loc0->line;
							doc_location_constraint(readonly_doc, loc0);
						}
						if (is_down) {
							++loc1->line;
							doc_location_constraint(readonly_doc, loc1);
						}
					}
					if (is_left  || is_up  ) *loc1 = *loc0;
					if (is_right || is_down) *loc0 = *loc1;
				}
			}	break;

			default:
				assert(!"unhandled motion char");

			}

			assert(previous_mode == COMMAND);
			mode = previous_mode;

		}	break;

		default:
			assert(!"unhandled mim_spool()-mode");

		}

		if (expect_suffix_bytes) {
			assert(suffix_bytes_remaining > 0);
			suffix_bytes_remaining -= num_bytes;
			assert(suffix_bytes_remaining >= 0);
			if (suffix_bytes_remaining == 0) {
				assert(datamode == UTF8);
				arrreset(number_stack_arr);
				assert(previous_mode == COMMAND);

				if (mode == EX) {
					struct mimexscanner s;
					mimexscanner_init(&s, mo->cows->allocator, ex0, input_cursor);

					{
						int book_id;
						const char* book_fundament;
						const char* book_template;
						if (mimex_matches(&s, "newbook", "iss", &book_id, &book_fundament, &book_template)) {
							enum fundament fundament = match_fundament(book_fundament);
							if (fundament == _NO_FUNDAMENT_) {
								const char* web_prefix = "web-";
								if (0 == strncmp(book_fundament, web_prefix, strlen(web_prefix))) {
									return mimerr("TODO web-* fundament"); // TODO XXX
								} else {
									return mimerr(":newbook used with unsupported fundament \"%s\"", book_fundament);
								}
							} else {
								const int is_nil_template = (0 == strcmp(book_template, "-"));
								//printf("TODO newbook [%d] [%s/%d] [%s/nil=%d]\n", book_id, book_fundament, f, book_template, is_nil_template);
								if (!is_nil_template) {
									assert(!"TODO handle/import :newbook template");
								}

								const int num_books = arrlen(mo->cows->ref->book_arr);
								for (int ii=0; ii<num_books; ++ii) {
									struct book* book = &mo->cows->ref->book_arr[ii];
									if (book_id == book->book_id) {
										return mimerr("book id %d already exists", book_id);
									}
								}

								arrput(mo->cows->new_book_arr, ((struct book){
									.book_id   = book_id,
									.fundament = fundament,
								}));
							}
						}
					}

					{
						int book_id, doc_id;
						const char* name;
						if (mimex_matches(&s, "newdoc", "iis", &book_id, &doc_id, &name)) {
							int book_id_exists = 0;

							const int num_books = arrlen(mo->cows->ref->book_arr);
							for (int i=0; i<num_books; ++i) {
								struct book* book = &mo->cows->ref->book_arr[i];
								if (book->book_id == book_id) {
									book_id_exists = 1;
									break;
								}
							}

							const int num_new_books = arrlen(mo->cows->new_book_arr);
							for (int i=0; i<num_new_books; ++i) {
								struct book* book = &mo->cows->new_book_arr[i];
								if (book->book_id == book_id) {
									book_id_exists = 1;
									break;
								}
							}

							if (!book_id_exists) {
								return mimerr("book id %d does not exist", book_id);
							}

							const int num_docs = arrlen(mo->cows->ref->document_arr);
							for (int i=0; i<num_docs; ++i) {
								struct document* doc = &mo->cows->ref->document_arr[i];
								if ((doc->book_id == book_id) && (doc->doc_id == doc_id)) {
									return mimerr(":newdoc %d %d collides with existing doc", book_id, doc_id);
								}
							}
							struct document doc = {
								.book_id = book_id,
								.doc_id  = doc_id,
							};
							ARRINIT0(doc.name_arr, mo->cows->allocator);
							ARRINIT0(doc.docchar_arr, mo->cows->allocator);
							const size_t n = strlen(name);
							arrsetlen(doc.name_arr, n+1);
							memcpy(doc.name_arr, name, n);
							doc.name_arr[n]=0;
							arrput(mo->cows->cow_document_arr, doc);
						}
					}

					{
						int book_id, doc_id;
						if (mimex_matches(&s, "setdoc", "ii", &book_id, &doc_id)) {

							int book_id_exists = 0;
							const int num_books = arrlen(mo->cows->ref->book_arr);
							for (int i=0; i<num_books; ++i) {
								struct book* book = &mo->cows->ref->book_arr[i];
								if (book->book_id == book_id) {
									book_id_exists = 1;
									break;
								}
							}
							if (!book_id_exists) {
								const int num_new_books = arrlen(mo->cows->new_book_arr);
								for (int i=0; i<num_new_books; ++i) {
									struct book* book = &mo->cows->new_book_arr[i];
									if (book->book_id == book_id) {
										book_id_exists = 1;
										break;
									}
								}
							}
							if (!book_id_exists) {
								return mimerr("setdoc on book id %d, but it doesn't exist", book_id);
							}

							int doc_id_exists = 0;
							const int num_docs = arrlen(mo->cows->ref->document_arr);
							for (int i=0; i<num_docs; ++i) {
								struct document* doc = &mo->cows->ref->document_arr[i];
								if ((doc->book_id == book_id) && (doc->doc_id == doc_id)) {
									doc_id_exists = 1;
									break;
								}
							}
							if (!doc_id_exists) {
								const int num_cow_doc = arrlen(mo->cows->cow_document_arr);
								for (int i=0; i<num_cow_doc; ++i) {
									struct document* doc = &mo->cows->cow_document_arr[i];
									if ((doc->book_id == book_id) && (doc->doc_id == doc_id)) {
										doc_id_exists = 1;
										break;
									}
								}
							}
							if (!doc_id_exists) {
								return mimerr("setdoc on doc id %d, but it doesn't exist", doc_id);
							}

							mo->cows->mim_state.book_id = book_id;
							mo->cows->mim_state.doc_id  = doc_id;
						}
					}

					if (s.has_error) {
						return mimerr("mimex error: %s", s.err);
					} else if (!s.did_match) {
						return mimerr("unhandled mimex command");
					}
				}

				mode = previous_mode;
			}
		}
	}

	if (mode != COMMAND) {
		return mimerr("mode (%d) not terminated", mode);
	}

	const int num_args = arrlen(number_stack_arr);
	if (num_args > 0) {
		return mimerr("non-empty number stack (n=%d) at end of mim-input", num_args);
	}

	return 0;
}

void begin_mim(int session_id)
{
	assert(!g.in_mim);
	g.in_mim = 1;
	arrreset(g.mim_buffer_arr);
	g.using_mim_session_id = session_id;
	struct snapshot* ss = &gst.cool_snapshot;
	(void)snapshot_get_or_create_mim_state_by_ids(ss, get_my_artist_id(), session_id);
}

#if 0
static void u8pp_write(uint8_t v, void* userdata)
{
	uint8_t** p = (uint8_t**)userdata;
	**p = v;
	++(*p);
}

static void write_varint64(uint8_t** pp, uint8_t* end, int64_t value)
{
	assert(((end - *pp) >= LEB128_MAX_LENGTH) && "not enough space for largest-case leb128 encoding");
	leb128_encode_int64(u8pp_write, pp, value);
}
#endif

static void document_to_colorchar_da(struct colorchar** arr, struct document* doc)
{
	const int num_src = arrlen(doc->docchar_arr);
	arrsetmincap(*arr, num_src);
	arrreset(*arr);
	for (int i=0; i<num_src; ++i) {
		struct docchar* fc = arrchkptr(doc->docchar_arr, i);
		if (fc->flags & FC_IS_INSERT) continue; // not yet inserted
		arrput(*arr, fc->colorchar);
	}
}

static void snapshot_spool_ex(struct snapshot* snapshot, uint8_t* data, int num_bytes, int artist_id, int session_id)
{
	if (num_bytes == 0) return;
	mie_begin_scrallox();
	if (0 == setjmp(*mie_prep_scrallox_jmp_buf_for_out_of_memory())) {
		struct cow_snapshot cows = {0};
		init_cow_snapshot(
			&cows,
			snapshot,
			snapshot_get_or_create_mim_state_by_ids(snapshot, artist_id, session_id),
			mie_borrow_scrallox());
		struct mimop mo/*o*/ = { .cows = &cows };

		if (mim_spool(&mo, data, num_bytes) < 0) {
			assert(!"TODO mim protocol error"); // XXX? should I have a "failable" flag? eg. for human input
		}

		if (mimop_has_doc(&mo)) {
			// XXX kinda want to run /all/ docs here... this code is not right
			struct mim_state* ms = &cows.mim_state;
			struct document* doc = cow_snapshot_get_readonly_document_by_ids(&cows, ms->book_id, ms->doc_id);
			static struct colorchar* dodoc_arr = NULL;
			if (dodoc_arr == NULL) {
				arrinit(dodoc_arr, &system_allocator);
			}
			document_to_colorchar_da(&dodoc_arr, doc);
			const int prg = mie_compile_colorcode(dodoc_arr, arrlen(dodoc_arr));
			//printf("prg=%d\n", prg);
			if (prg == -1) {
				//printf("TODO compile error [%s]\n", mie_error());
			} else {
				vmie_reset(prg);
				vmie_run();
				#if 0
				const char* err = mie_error();
				if (err != NULL) printf("ERR: %s\n", err);
				vmie_dump_stack();
				#endif
				mie_program_free(prg);
			}
		}

		cow_snapshot_commit(&cows);
	} else {
		printf("ERROR out of scratch memory!\n"); // XXX?
	}

	#if 0
	{
		size_t allocated, capacity;
		mie_scrallox_stats(&allocated, &capacity);
		printf("scrallox scope: allocated %zd/%zd (%.1f%%)\n",
			allocated, capacity,
			(double)(allocated*100L) / (double)(capacity));
	}
	#endif

	mie_end_scrallox();
}

static int it_is_time_for_a_snapshotcache_push(uint64_t journal_offset)
{
	int64_t growth = (journal_offset - gst.journal_offset_at_last_snapshotcache_push);
	return growth > g.journal_snapshot_growth_threshold;
}

static void snapshotcache_push(struct snapshot* snapshot, uint64_t journal_offset)
{
	struct jio* jdat = gst.jio_snapshotcache_data;
	struct jio* jidx = gst.jio_snapshotcache_index;

	const int num_books = arrlen(snapshot->book_arr);
	const int num_documents = arrlen(snapshot->document_arr);
	const int num_mim_states = arrlen(snapshot->mim_state_arr);

	for (int i=0; i<num_books; ++i) {
		struct book* book = &snapshot->book_arr[i];
		if (book->snapshotcache_offset) continue;
		book->snapshotcache_offset = jio_get_size(jdat);
		jio_append_u8(jdat, SYNC);
		jio_append_leb128(jdat, book->book_id);
	}

	for (int i=0; i<num_documents; ++i) {
		struct document* doc = &snapshot->document_arr[i];
		if (doc->snapshotcache_offset) continue;
		doc->snapshotcache_offset = jio_get_size(jdat);
		jio_append_u8(jdat, SYNC);
		jio_append_leb128(jdat, doc->book_id);
		jio_append_leb128(jdat, doc->doc_id);
		const int name_len = strlen(doc->name_arr);
		jio_append_leb128(jdat, name_len);
		jio_append(jdat, doc->name_arr, name_len);
		const int doc_len = arrlen(doc->docchar_arr);
		jio_append_leb128(jdat, doc_len);
		for (int ii=0; ii<doc_len; ++ii) {
			struct docchar* c = &doc->docchar_arr[ii];
			struct colorchar* tc = &c->colorchar;
			jio_append_leb128(jdat, tc->codepoint);
			assert((is_valid_splash4(tc->splash4)) && "did not expect bad splash4; don't want to write it");
			jio_append_leu16(jdat, tc->splash4);
			jio_append_leb128(jdat, c->flags);
		}
	}

	for (int i=0; i<num_mim_states; ++i) {
		struct mim_state* ms = &snapshot->mim_state_arr[i];
		if (ms->snapshotcache_offset) continue;
		ms->snapshotcache_offset = jio_get_size(jdat);
		jio_append_u8(jdat, SYNC);
		jio_append_leb128(jdat, ms->artist_id);
		jio_append_leb128(jdat, ms->session_id);
		jio_append_leb128(jdat, ms->book_id);
		jio_append_leb128(jdat, ms->doc_id);
		assert((is_valid_splash4(ms->splash4)) && "did not expect bad splash4; don't want to write it");
		jio_append_leu16(jdat, ms->splash4);
		const int num_carets = arrlen(ms->caret_arr);
		jio_append_leb128(jdat, num_carets);
		for (int ii=0; ii<num_carets; ++ii) {
			struct caret* cr = &ms->caret_arr[ii];
			jio_append_leb128(jdat, cr->tag);
			jio_append_leb128(jdat, cr->caret_loc.line);
			jio_append_leb128(jdat, cr->caret_loc.column);
			jio_append_leb128(jdat, cr->anchor_loc.line);
			jio_append_leb128(jdat, cr->anchor_loc.column);
		}
	}

	const int64_t snapshot_manifest_offset = jio_get_size(jdat);
	jio_append_u8(jdat, SYNC);
	jio_append_leb128(jdat, num_books);
	jio_append_leb128(jdat, num_documents);
	jio_append_leb128(jdat, num_mim_states);
	for (int i=0; i<num_books; ++i) {
		struct book* book = &snapshot->book_arr[i];
		jio_append_leb128(jdat, book->snapshotcache_offset);
	}
	for (int i=0; i<num_documents; ++i) {
		struct document* doc = &snapshot->document_arr[i];
		jio_append_leb128(jdat, doc->snapshotcache_offset);
	}
	for (int i=0; i<num_mim_states; ++i) {
		struct mim_state* ms = &snapshot->mim_state_arr[i];
		jio_append_leb128(jdat, ms->snapshotcache_offset);
	}

	// write index triplet
	jio_append_leu64(jidx, get_nanoseconds_epoch()); // XXX correct timestamp?
	jio_append_leu64(jidx, snapshot_manifest_offset);
	jio_append_leu64(jidx, journal_offset);

	gst.journal_offset_at_last_snapshotcache_push = journal_offset;
}

void end_mim(void)
{
	assert(g.in_mim);
	const int num_bytes = arrlen(g.mim_buffer_arr);
	g.in_mim = 0;
	if (num_bytes == 0) return;

	struct snapshot* ss = &gst.cool_snapshot;

	uint8_t* data = g.mim_buffer_arr;
	snapshot_spool_ex(ss, data, num_bytes, get_my_artist_id(), g.using_mim_session_id);

	struct jio* jj = gst.jio_journal;
	if (jj != NULL) {
		jio_append_u8(jj, SYNC);
		const int64_t journal_timestamp = (get_nanoseconds() - gst.journal_timestamp_start)/1000LL;
		jio_append_leb128(jj, journal_timestamp);
		jio_append_leb128(jj, get_my_artist_id());
		jio_append_leb128(jj, g.using_mim_session_id);
		jio_append_leb128(jj, num_bytes);
		jio_append(jj, data, num_bytes);
		const int64_t s = jio_get_size(jj);
		if (it_is_time_for_a_snapshotcache_push(s)) {
			snapshotcache_push(ss, s);
		}
	}

	#if 0
	// old command stream/ringbuf stuff. relavant for network? or throw out?
	uint8_t header[1<<8];
	uint8_t* hp = header;
	uint8_t* end = header + sizeof(header);
	write_varint64(&hp, end, g.using_mim_session_id);
	write_varint64(&hp, end, num_bytes);
	const size_t nh = hp - header;
	ringbuf_writen(&g.command_ringbuf, header, nh);
	ringbuf_writen(&g.command_ringbuf, data, num_bytes);
	ringbuf_commit(&g.command_ringbuf);
	#endif
}

int doc_iterator_next(struct doc_iterator* it)
{
	assert((!it->done) && "you cannot call this function after it has returned 0");
	struct document* d = it->doc;
	const int num_chars = arrlen(d->docchar_arr);
	if (it->last) {
		it->done = 1;
		assert(it->offset == num_chars);
		return 0;
	}

	if (it->new_line) {
		++it->location.line;
		it->location.column = 1;
		it->new_line = 0;
	} else {
		++it->location.column;
	}

	++it->offset;
	const int off = it->offset;
	if (off < num_chars) {
		it->docchar = arrchkptr(d->docchar_arr, off);
		if (it->docchar->colorchar.codepoint == '\n') {
			it->new_line = 1;
		}
	} else {
		assert(off == num_chars);
		it->docchar = NULL;
		it->last = 1;
	}

	return 1;
}

static uint64_t make_insignia(void)
{
	return get_nanoseconds_epoch(); // FIXME a random number might be slightly better here (no biggie)
}

FORMATPRINTF1
static int errf(const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	stbsp_vsnprintf(g.errormsg, sizeof g.errormsg, fmt, va);
	va_end(va);
	return -1;
}

#define FMTERR(PATH,MSG)     errf("%s (format error): %s (at %s:%d)", (PATH), (MSG), __FILE__, __LINE__)
#define IOERR(PATH,ERRCODE) errf("%s (jio error): %s (at %s:%d)", (PATH), io_error_to_string_safe(ERRCODE), __FILE__, __LINE__)

static int is_snapshotcache_index_size_valid(int64_t sz)
{
	return ((sz-16L) % 24L) == 0L;
}

static int get_num_snapshotcache_index_entries_from_size(int64_t sz)
{
	assert(is_snapshotcache_index_size_valid(sz));
	return (sz-16L) / 24L;
}

static int snapshotcache_open(const char* dir, uint64_t journal_insignia)
{
	char pathbuf[1<<14];

	int err;
	STATIC_PATH_JOIN(pathbuf, dir, FILENAME_SNAPSHOTCACHE_DATA);
	struct jio* jdat = jio_open(pathbuf, IO_OPEN, g.io_port_id, 10, &err);
	if (jdat == NULL) return IOERR(pathbuf, err);
	gst.jio_snapshotcache_data = jdat;
	const int64_t szdat = jio_get_size(jdat);
	if (szdat == 0) {
		jio_close(jdat);
		return FMTERR(pathbuf, "file is empty");
	}

	STATIC_PATH_JOIN(pathbuf, dir, FILENAME_SNAPSHOTCACHE_INDEX);
	struct jio* jidx = jio_open(pathbuf, IO_OPEN, g.io_port_id, 10, &err);
	if (jidx == NULL) return IOERR(pathbuf, err);
	gst.jio_snapshotcache_index = jidx;
	const int64_t szidx = jio_get_size(jidx);
	if (szidx < 16) {
		jio_close(jidx);
		jio_close(jdat);
		return FMTERR(pathbuf, "incomplete header");
	}
	if (!is_snapshotcache_index_size_valid(szidx)) {
		jio_close(jidx);
		jio_close(jdat);
		return FMTERR(pathbuf, "invalid index size");
	}

	// read&check snapshotcache headers and insignias

	uint8_t idx_header[16];
	err = jio_pread(jidx, idx_header, sizeof idx_header, 0);
	if (err<0) {
		jio_close(jidx);
		jio_close(jdat);
		return IOERR(FILENAME_SNAPSHOTCACHE_INDEX, err);
	}

	uint8_t dat_header[16];
	err = jio_pread(jdat, dat_header, sizeof dat_header, 0);
	if (err<0) {
		jio_close(jidx);
		jio_close(jdat);
		return IOERR(FILENAME_SNAPSHOTCACHE_DATA, err);
	}

	assert(strlen(DOSI_MAGIC) == 8);
	if (memcmp(idx_header, DOSI_MAGIC, 8) != 0) {
		jio_close(jidx);
		jio_close(jdat);
		return FMTERR(FILENAME_SNAPSHOTCACHE_INDEX, "magic missing from header");
	}

	const uint64_t index_insignia = leu64_decode(&idx_header[8]);
	if (index_insignia != journal_insignia) {
		jio_close(jidx);
		jio_close(jdat);
		return FMTERR(FILENAME_SNAPSHOTCACHE_INDEX, "insignia mismatch");
	}

	assert(strlen(DOSD_MAGIC) == 8);
	if (memcmp(dat_header, DOSD_MAGIC, 8) != 0) {
		jio_close(jidx);
		jio_close(jdat);
		return FMTERR(FILENAME_SNAPSHOTCACHE_DATA, "magic missing from header");
	}

	const uint64_t data_insignia = leu64_decode(&dat_header[8]);
	if (data_insignia != journal_insignia) {
		jio_close(jidx);
		jio_close(jdat);
		return FMTERR(FILENAME_SNAPSHOTCACHE_DATA, "insignia mismatch");
	}

	return 0;
}

static int snapshotcache_create(const char* dir, uint64_t journal_insignia)
{
	char pathbuf[1<<14];

	STATIC_PATH_JOIN(pathbuf, dir, FILENAME_SNAPSHOTCACHE_DATA);
	int err;
	struct jio* jdat = jio_open(pathbuf, IO_CREATE, g.io_port_id, 10, &err);
	if (jdat == NULL) {
		return IOERR(FILENAME_SNAPSHOTCACHE_DATA, err);
	}
	gst.jio_snapshotcache_data = jdat;
	jio_append(jdat, DOSD_MAGIC, strlen(DOSD_MAGIC));
	jio_append_leu64(jdat, journal_insignia);

	STATIC_PATH_JOIN(pathbuf, dir, FILENAME_SNAPSHOTCACHE_INDEX);
	struct jio* jidx = jio_open(pathbuf, IO_CREATE, g.io_port_id, 10, &err);
	if (jidx == NULL) {
		jio_close(jdat);
		return IOERR(FILENAME_SNAPSHOTCACHE_INDEX, err);
	}
	gst.jio_snapshotcache_index = jidx;
	jio_append(jidx, DOSI_MAGIC, strlen(DOSI_MAGIC));
	jio_append_leu64(jidx, journal_insignia);

	return 0;
}

static int restore_snapshot(struct snapshot* ss, uint64_t snapshot_manifest_offset)
{
	memset(ss, 0, sizeof *ss);
	arrinit(ss->book_arr      , &system_allocator);
	arrinit(ss->document_arr  , &system_allocator);
	arrinit(ss->mim_state_arr , &system_allocator);

	struct jio* jdat = gst.jio_snapshotcache_data;

	int64_t o0 = snapshot_manifest_offset;
	uint8_t sync = jio_ppread_u8(jdat, &o0);
	const char* path = FILENAME_SNAPSHOTCACHE_DATA;
	if (sync != SYNC) return FMTERR(path, "expected SYNC");

	const int64_t num_books      = jio_ppread_leb128(jdat, &o0);
	const int64_t num_documents  = jio_ppread_leb128(jdat, &o0);
	const int64_t num_mim_states = jio_ppread_leb128(jdat, &o0);

	for (int64_t i=0; i<num_books; ++i) {
		const int64_t oo1 = jio_ppread_leb128(jdat, &o0);
		int64_t o1 = oo1;
		uint8_t sync = jio_ppread_u8(jdat, &o1);
		if (sync != SYNC) return FMTERR(path, "expected SYNC");
		struct book book = { .snapshotcache_offset = oo1 };
		book.book_id = jio_ppread_leb128(jdat, &o1);
		if (jio_get_error(jdat) < 0) {
			return FMTERR(path, "expected SYNC");
		}
		arrput(ss->book_arr, book);
	}

	for (int64_t i=0; i<num_documents; ++i) {
		const int64_t oo1 = jio_ppread_leb128(jdat, &o0);
		int64_t o1 = oo1;
		uint8_t sync = jio_ppread_u8(jdat, &o1);
		if (sync != SYNC) return FMTERR(path, "expected SYNC");
		struct document doc = { .snapshotcache_offset = oo1 };
		doc.book_id = jio_ppread_leb128(jdat, &o1);
		doc.doc_id = jio_ppread_leb128(jdat, &o1);
		const int64_t name_len = jio_ppread_leb128(jdat, &o1);
		arrinit(doc.name_arr, &system_allocator);
		arrsetlen(doc.name_arr, 1+name_len);
		jio_ppread(jdat, doc.name_arr, name_len, &o1);
		doc.name_arr[name_len] = 0;

		const int64_t doc_len = jio_ppread_leb128(jdat, &o1);
		arrinit(doc.docchar_arr, &system_allocator);
		struct docchar* cs = arraddnptr(doc.docchar_arr, doc_len);
		for (int64_t i=0; i<doc_len; ++cs, ++i) {
			cs->colorchar.codepoint = jio_ppread_leb128(jdat, &o1);
			cs->colorchar.splash4 = jio_ppread_leu16(jdat, &o1);
			if (!is_valid_splash4(cs->colorchar.splash4)) {
				return FMTERR(path, "bad splash4 color in doc");
			}
			cs->flags = jio_ppread_leb128(jdat, &o1);
		}

		if (jio_get_error(jdat) < 0) return IOERR(path, jio_get_error(jdat));

		arrput(ss->document_arr, doc);
	}

	for (int64_t i=0; i<num_mim_states; ++i) {
		const int64_t oo1 = jio_ppread_leb128(jdat, &o0);
		int64_t o1 = oo1;
		//printf("mim %ld => %ld\n", i, o1);
		uint8_t sync = jio_ppread_u8(jdat, &o1);
		if (sync != SYNC) return FMTERR(path, "expected SYNC");
		struct mim_state ms = { .snapshotcache_offset = oo1 };
		ms.artist_id  = jio_ppread_leb128(jdat, &o1);
		ms.session_id = jio_ppread_leb128(jdat, &o1);
		ms.book_id    = jio_ppread_leb128(jdat, &o1);
		ms.doc_id     = jio_ppread_leb128(jdat, &o1);
		ms.splash4    = jio_ppread_leu16( jdat, &o1);
		if (!is_valid_splash4(ms.splash4)) {
			return FMTERR(path, "bad splash4 color in mim state");
		}

		const int64_t num_carets = jio_ppread_leb128(jdat, &o1);
		arrinit(ms.caret_arr, &system_allocator);
		struct caret* cr = arraddnptr(ms.caret_arr, num_carets);
		for (int64_t i=0; i<num_carets; ++cr, ++i) {
			cr->tag               = jio_ppread_leb128(jdat, &o1);
			cr->caret_loc.line    = jio_ppread_leb128(jdat, &o1);
			cr->caret_loc.column  = jio_ppread_leb128(jdat, &o1);
			cr->anchor_loc.line   = jio_ppread_leb128(jdat, &o1);
			cr->anchor_loc.column = jio_ppread_leb128(jdat, &o1);
		}

		if (jio_get_error(jdat) < 0) return IOERR(path, jio_get_error(jdat));

		arrput(ss->mim_state_arr, ms);
	}

	return 0;
}

static int can_restore_latest_snapshot(void)
{
	struct jio* jidx = gst.jio_snapshotcache_index;
	int64_t sz = jio_get_size(jidx);
	if (!is_snapshotcache_index_size_valid(sz)) return 0;
	return get_num_snapshotcache_index_entries_from_size(sz) > 0;
}

static int restore_latest_snapshot(struct snapshot* ss, int64_t* out_journal_offset)
{
	if (!can_restore_latest_snapshot()) {
		return FMTERR(FILENAME_SNAPSHOTCACHE_INDEX, "bad index file");
	}
	struct jio* jidx = gst.jio_snapshotcache_index;
	const int64_t sz = jio_get_size(jidx);
	int64_t o = (sz - 2*sizeof(uint64_t));
	const uint64_t snapshot_manifest_offset = jio_ppread_leu64(jidx, &o);
	const uint64_t journal_offset = jio_ppread_leu64(jidx, &o);
	int err = jio_get_error(jidx);
	if (err<0) {
		return IOERR(FILENAME_SNAPSHOTCACHE_INDEX, err);
	} else {
		assert(o == sz);
	}
	if (out_journal_offset) *out_journal_offset = journal_offset;
	return restore_snapshot(ss, snapshot_manifest_offset);
}

static void snapshot_init(struct snapshot* ss)
{
	assert(ss->book_arr == NULL);
	assert(ss->document_arr == NULL);
	assert(ss->mim_state_arr == NULL);
	memset(ss, 0, sizeof *ss);
	arrinit(ss->book_arr      , &system_allocator);
	arrinit(ss->document_arr  , &system_allocator);
	arrinit(ss->mim_state_arr , &system_allocator);
}

static int host_dir(const char* dir)
{
	char pathbuf[1<<14];
	STATIC_PATH_JOIN(pathbuf, dir, FILENAME_JOURNAL);
	int err;
	struct jio* jj = jio_open(pathbuf, IO_OPEN_OR_CREATE, g.io_port_id, 10, &err);
	gst.jio_journal = jj;
	if (jj == NULL) return IOERR(FILENAME_JOURNAL, err);

	// TODO setup journal jio for fdatasync?

	struct snapshot* ss = &gst.cool_snapshot; // XXX
	snapshot_init(ss);

	const int64_t sz = jio_get_size(jj);
	if (sz == 0) {
		jio_append(jj, DOJO_MAGIC, strlen(DOJO_MAGIC));
		jio_append_leb128(jj, DO_FORMAT_VERSION);
		gst.journal_insignia = make_insignia();
		jio_append_leu64(jj, gst.journal_insignia);
		// TODO epoch timestamp?
		err = jio_get_error(jj);
		if (err<0) return IOERR(FILENAME_JOURNAL, err);
		gst.journal_timestamp_start = get_nanoseconds();

		err = snapshotcache_create(dir, gst.journal_insignia);
		if (err<0) return IOERR(FILENAME_JOURNAL, err);
	} else {
		uint8_t magic[8];
		int64_t o = 0;
		err = jio_ppread(jj, magic, sizeof magic, &o);
		if (err<0) return IOERR(FILENAME_JOURNAL, err);
		if (memcmp(magic, DOJO_MAGIC, 8) != 0) {
			return FMTERR(pathbuf, "invalid magic in journal header");
		}

		const int64_t do_format_version = jio_ppread_leb128(jj, &o);
		if (do_format_version != DO_FORMAT_VERSION) {
			return FMTERR(pathbuf, "unknown do-format-version");
		}
		gst.journal_insignia = jio_ppread_leu64(jj, &o);

		int64_t journal_spool_offset = o;

		err = snapshotcache_open(dir, gst.journal_insignia);
		if (err == IO_NOT_FOUND) {
			// OK just spool journal from beginning
		} else {
			if (can_restore_latest_snapshot()) {
				journal_spool_offset = 0;
				err = restore_latest_snapshot(ss, &journal_spool_offset);
				if (err<0) return err;
				assert(journal_spool_offset > 0);
			} else {
				// spool from beginning
			}
		}

		static uint8_t* mimbuf_arr = NULL;
		if (mimbuf_arr == NULL) arrinit(mimbuf_arr, &system_allocator);

		o = journal_spool_offset;
		const int64_t jjsz = jio_get_size(jj);
		if (o > jjsz) return FMTERR(FILENAME_JOURNAL, "journal spool offset past end-of-file");
		while (o < jjsz) {
			const uint8_t sync = jio_ppread_u8(jj, &o);
			if (sync != SYNC) return FMTERR(FILENAME_JOURNAL, "expected SYNC");
			const int64_t timestamp_us = jio_ppread_leb128(jj, &o);
			(void)timestamp_us; // XXX? remove?
			const int64_t artist_id = jio_ppread_leb128(jj, &o);
			const int64_t session_id = jio_ppread_leb128(jj, &o);
			const int64_t num_bytes = jio_ppread_leb128(jj, &o);
			arrsetlen(mimbuf_arr, num_bytes);
			jio_ppread(jj, mimbuf_arr, num_bytes, &o);
			//printf("going to spool [");for(int i=0;i<num_bytes;++i)printf("%c",mimbuf_arr[i]);printf("]\n");
			snapshot_spool_ex(ss, mimbuf_arr, num_bytes, artist_id, session_id);
		}
		if (o != jjsz) return FMTERR(FILENAME_JOURNAL, "expected to spool journal until end-of-file");

		err = jio_get_error(jj);
		if (err<0) return IOERR(FILENAME_JOURNAL, err);
	}

	assert(ss->book_arr != NULL);
	assert(ss->document_arr != NULL);
	assert(ss->mim_state_arr != NULL);

	return 0;
}

void gig_host(const char* dir)
{
	memset(&gst, 0, sizeof gst);
	gst.my_artist_id = 1; // XXX?
	const int err = host_dir(dir);
	if (err<0) {
		fprintf(stderr, "host_dir(\"%s\") failed: [%s]/%d\n", dir, g.errormsg, err);
		abort();
	}
}

void gig_host_no_jio(void)
{
	// XXX? find a better solution, or?
	memset(&gst, 0, sizeof gst);
	gst.my_artist_id = 1; // XXX?
	struct snapshot* ss = &gst.cool_snapshot; // XXX
	snapshot_init(ss);
}

void gig_unhost(void)
{
	jio_close(gst.jio_journal);
	jio_close(gst.jio_snapshotcache_data);
	jio_close(gst.jio_snapshotcache_index);
	// XXX hmmm... is this all? or even right?
	struct snapshot* ss = &gst.cool_snapshot;
	arrfree(ss->book_arr);
	arrfree(ss->document_arr);
	arrfree(ss->mim_state_arr);
}

void gig_maybe_setup_stub(void)
{
	// XXX doesn't work too well
	struct snapshot* ss = &gst.cool_snapshot;
	if (arrlen(ss->book_arr) > 0) return;
	begin_mim(1);
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	mimex("setdoc 1 50");
	mimf("0,1,1c");
	end_mim();
}

void gig_set_journal_snapshot_growth_threshold(int t)
{
	assert(t > 0);
	g.journal_snapshot_growth_threshold = t;
}

void gig_init(void)
{
	g.io_port_id = io_port_create();
	gig_set_journal_snapshot_growth_threshold(5000);
	arrinit(g.mim_buffer_arr, &system_allocator);
}

void gig_selftest(void)
{
	// TODO (used to be a mim test here)
}
