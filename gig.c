#include <limits.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h> // XXX
#include <threads.h>

#include "stb_ds_sysalloc.h"
#include "stb_sprintf.h"
#include "jio.h"
#include "gig.h"
#include "util.h"
#include "bb.h"
#include "utf8.h"
#include "path.h"
#include "main.h"
#include "arg.h"
#include "bufstream.h"

#ifndef __EMSCRIPTEN__ // XXX not totally right?
#include "webserv.h"
#endif

#define DOJO_MAGIC ("DOJO0001")
#define DOSI_MAGIC ("DOSI0001")
#define DOSD_MAGIC ("DOSD0001")
#define DO_FORMAT_VERSION (10000)
#define SYNC (0xfa)
#define FILENAME_JOURNAL              "DO_JAM_JOURNAL"
#define FILENAME_SNAPSHOTCACHE_DATA   "snapshotcache.data"
#define FILENAME_SNAPSHOTCACHE_INDEX  "snapshotcache.index"

static int match_fundament(const char* s)
{
	#define X(ENUM,STR) if (0 == strcmp(STR,s)) return ENUM;
	LIST_OF_FUNDAMENTS
	#undef X
	return _NO_FUNDAMENT_;
}

static void snapshot_copy(struct snapshot* dst, struct snapshot* src)
{
	// books
	arrcpy(dst->book_arr, src->book_arr);

	// documents
	const int num_src_docs = arrlen(src->document_arr);
	const int num_dst_docs = arrlen(dst->document_arr);
	if (num_dst_docs < num_src_docs) {
		arrsetlen(dst->document_arr, num_src_docs);
		memset(&dst->document_arr[num_dst_docs], 0, sizeof(*dst->document_arr)*(num_src_docs-num_dst_docs));
	}
	for (int i=0; i<num_src_docs; ++i) {
		struct document* dstdoc = &dst->document_arr[i];
		struct document* srcdoc = &src->document_arr[i];
		struct document tmp = *dstdoc;
		*dstdoc = *srcdoc;
		dstdoc->name_arr = tmp.name_arr;
		arrcpy(dstdoc->name_arr, srcdoc->name_arr);
		dstdoc->docchar_arr = tmp.docchar_arr;
		arrcpy(dstdoc->docchar_arr , srcdoc->docchar_arr);
	}
	for (int i=num_src_docs; i<num_dst_docs; ++i) {
		struct document* doc = &dst->document_arr[i];
		arrfree(doc->name_arr);
		arrfree(doc->docchar_arr);
	}
	if (num_dst_docs > num_src_docs) {
		arrsetlen(dst->document_arr, num_src_docs);
	}

	// mim states
	const int num_src_ms = arrlen(src->mim_state_arr);
	const int num_dst_ms = arrlen(dst->mim_state_arr);
	if (num_dst_ms < num_src_ms) {
		arrsetlen(dst->mim_state_arr, num_src_ms);
		memset(&dst->mim_state_arr[num_dst_ms], 0, sizeof(*dst->mim_state_arr)*(num_src_ms-num_dst_ms));
	}
	for (int i=0; i<num_src_ms; ++i) {
		struct mim_state* dstms = &dst->mim_state_arr[i];
		struct mim_state* srcms = &src->mim_state_arr[i];
		struct mim_state tmp = *dstms;
		*dstms = *srcms;
		dstms->caret_arr = tmp.caret_arr;
		arrcpy(dstms->caret_arr, srcms->caret_arr);
	}
	for (int i=num_src_ms; i<num_dst_ms; ++i) {
		struct mim_state* ms = &dst->mim_state_arr[i];
		arrfree(ms->caret_arr);
	}
	if (num_dst_ms > num_src_ms) {
		arrsetlen(dst->mim_state_arr, num_src_ms);
	}
}

static void snapshot_free(struct snapshot* snap)
{
	memset(snap, 0, sizeof *snap);
	//TODO(free snapshot)
}

static int document_locate(struct document* doc, struct location* loc)
{
	struct doc_iterator it = doc_iterator(doc);
	doc_iterator_locate(&it, loc);
	return it.offset;
}

static struct location document_reverse_locate(struct document* doc, int index)
{
	struct doc_iterator it = doc_iterator(doc);
	int i=0;
	while (doc_iterator_next(&it)) {
		if (i==index) return it.location;
		i++;
	}
	assert(!"unreachable");
}

static struct document* snapshot_get_document_by_index(struct snapshot* snap, int index)
{
	return arrchkptr(snap->document_arr, index);
}

static struct document* snapshot_lookup_document_by_ids(struct snapshot* snap, int book_id, int doc_id)
{
	const int num_docs = arrlen(snap->document_arr);
	for (int i=0; i<num_docs; ++i) {
		struct document* doc = snapshot_get_document_by_index(snap, i);
		if ((doc->book_id == book_id) && (doc->doc_id == doc_id)) {
			return doc;
		}
	}
	return NULL;
}

#if 0
static struct document* snapshot_get_document_by_ids(struct snapshot* snap, int book_id, int doc_id)
{
	struct document* doc = snapshot_lookup_document_by_ids(snap, book_id, doc_id);
	assert((doc != NULL) && "document not found by id");
	return doc;
}
#endif

static struct mim_state* snapshot_lookup_mim_state_by_ids(struct snapshot* snap, int artist_id, int session_id)
{
	const int num_ms = arrlen(snap->mim_state_arr);
	for (int i=0; i<num_ms; ++i) {
		struct mim_state* s = arrchkptr(snap->mim_state_arr, i);
		if ((s->artist_id == artist_id) &&  (s->session_id == session_id)) {
			return s;
		}
	}
	return NULL;
}

#if 0
static struct mim_state* snapshot_get_mim_state_by_ids(struct snapshot* snap, int artist_id, int session_id)
{
	struct mim_state* ms = snapshot_lookup_mim_state_by_ids(snap, artist_id, session_id);
	assert(ms != NULL);
	return ms;
}
#endif

static struct mim_state* snapshot_get_or_create_mim_state_by_ids(struct snapshot* snap, int artist_id, int session_id)
{
	struct mim_state* ms = snapshot_lookup_mim_state_by_ids(snap, artist_id, session_id);
	if (ms != NULL) return ms;
	ms = arraddnptr(snap->mim_state_arr, 1);
	memset(ms, 0, sizeof *ms);
	ms->artist_id  = artist_id,
	ms->session_id = session_id;
	return ms;
}

struct ringbuf {
	int size_log2;
	uint8_t* buf;
	_Atomic(int64_t) head, tail;
};

static void ringbuf_init(struct ringbuf* rb, int size_log2)
{
	memset(rb, 0, sizeof *rb);
	rb->size_log2 = size_log2;
	rb->buf = calloc(1L << size_log2, sizeof *rb->buf);
}

static void ringbuf_free(struct ringbuf* rb)
{
	assert(rb->buf != NULL);
	free(rb->buf);
	memset(rb, 0, sizeof *rb);
}

static int ringbuf_write(struct ringbuf* rb, const uint8_t* data, int count)
{
	assert(rb->buf != NULL);
	const int size_log2 = rb->size_log2;
	const unsigned size = 1L << size_log2;
	const uint64_t mask = size-1;

	const int64_t tail = atomic_load(&rb->tail);
	const int64_t head = atomic_load(&rb->head);
	const int64_t writable_count = (size - (head-tail));
	if (count > writable_count) {
		// write all or nothing
		return -1;
	}

	const int64_t new_head = head + count;
	const int cycle0 = head         >> size_log2;
	const int cycle1 = (new_head-1) >> size_log2;
	if (cycle1 == cycle0) {
		memcpy(rb->buf + (head&mask), data, count);
	} else {
		const int64_t n0 = (size - (head&mask));
		assert(n0 > 0);
		memcpy(rb->buf + (head&mask) , data    , n0);
		memcpy(rb->buf               , data+n0 , count-n0);
	}

	atomic_store(&rb->head, new_head);

	return 0;
}

static void ringbuf_get_readable_range(struct ringbuf* rb, int64_t* out_p0, int64_t* out_p1)
{
	assert(rb->buf != NULL);
	const int64_t head = atomic_load(&rb->head);
	const int64_t tail = atomic_load(&rb->tail);
	if (out_p0) *out_p0 = tail;
	if (out_p1) *out_p1 = head;
}

static void ringbuf_read_range(struct ringbuf* rb, uint8_t* data, int64_t p0, int64_t p1)
{
	assert(rb->buf != NULL);
	const int size_log2 = rb->size_log2;
	const unsigned size = 1L << size_log2;
	const uint64_t mask = size-1;
	const int cycle0 =  p0    >> size_log2;
	const int cycle1 = (p1-1) >> size_log2;
	const int64_t num_bytes = p1-p0;
	if (cycle0 == cycle1) {
		memcpy(data, rb->buf + (p0&mask), num_bytes);
	} else {
		const int64_t n0 = num_bytes - (p0&mask);
		assert(n0 > 0);
		memcpy(data    , rb->buf+(p0&mask) , n0);
		memcpy(data+n0 , rb->buf           , num_bytes-n0);
	}
	atomic_store(&rb->tail, p1);
}

static struct {
	unsigned is_configured :1;
	unsigned is_host       :1;
	unsigned is_peer       :1;
	struct ringbuf peer2host_mim_ringbuf;
} g; // globals

static struct {
	int io_port_id;
	int64_t journal_offset_at_last_snapshotcache_push;
	struct jio* jio_journal;
	struct jio* jio_snapshotcache_data;
	struct jio* jio_snapshotcache_index;
	int64_t journal_timestamp_start;
	int journal_snapshot_growth_threshold;
} igo; // I/O globals

struct peer_state {
	int artist_id;
	uint8_t* cmdbuf_arr;
};

static struct {
	struct snapshot present_snapshot;
	// "present snapshot" is the "snapshot in effect"; it's always in sync with
	// the latest data added to the journal
	uint8_t* bb_arr;
	int next_artist_id;
	struct peer_state* peer_state_arr;
} hg; // host globals

static struct peer_state* host_get_or_create_peer_state_by_artist_id(int artist_id)
{
	const int num = arrlen(hg.peer_state_arr);
	for (int i=0; i<num; ++i) {
		struct peer_state* ps = &hg.peer_state_arr[i];
		if (ps->artist_id == artist_id) {
			return ps;
		}
	}
	struct peer_state* ps = arraddnptr(hg.peer_state_arr, 1);
	memset(ps, 0, sizeof *ps);
	ps->artist_id = artist_id;
	return ps;
}

static struct {
	int my_artist_id;
	uint8_t* bb_arr;
	int mim_session_id;
	struct snapshot upstream_snapshot;
	// "upstream snapshot" is the latest snapshot received from the host
	struct snapshot fiddle_snapshot;
	// "fiddle snapshot" is based on "upstream snapshot" but used for applying
	// edits; to run&verify code edits before pushing them to the host, but
	// also (optionally?) to see edits before receiving confirmation from the
	// host (waiting for roundtrip latency and all)
	int64_t journal_cursor;
	double artificial_mim_latency_mean;
	double artificial_mim_latency_variance;
	int64_t tracer_sequence;
	uint8_t* unackd_mimbuf_arr;
} pg; // peer globals

THREAD_LOCAL static struct {
	char errormsg[1<<14];
	int in_mim;
	//int mim_header_size;
	uint8_t* mim_buffer_arr;
	char* mimex_buffer_arr;
} tlg; // thread local globals

static void dumperr(void)
{
	if (strlen(tlg.errormsg) == 0) return;
	fprintf(stderr, "ERROR [%s]\n", tlg.errormsg);
}

FORMATPRINTF1
static int errf(const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	stbsp_vsnprintf(tlg.errormsg, sizeof tlg.errormsg, fmt, va);
	va_end(va);
	return -1;
}

#define FMTERR0(MSG)    errf("(format error): %s (at %s:%d)", (MSG), __FILE__, __LINE__)
#define FMTERR(PATH,MSG)    errf("%s (format error): %s (at %s:%d)", (PATH), (MSG), __FILE__, __LINE__)
#define IOERR(PATH,ERRCODE) errf("%s (jio error): %s (at %s:%d)", (PATH), io_error_to_string_safe(ERRCODE), __FILE__, __LINE__)

int peer_tick(void)
{
	assert(g.is_peer);

	if (pg.journal_cursor == 0) {
		pg.journal_cursor = 8*3; // XXX hack to skip header...
	}
	const int64_t jc0 = pg.journal_cursor;
	if (g.is_peer && g.is_host) {
		const int64_t jc1 = jio_get_size(igo.jio_journal);
		if (jc1 > jc0) {
			const int64_t size = (jc1 - jc0);
			uint8_t** bb = &pg.bb_arr;
			arrsetlen(*bb, size);
			if (jio_pread(igo.jio_journal, *bb, size, jc0) < 0) {
				FIXME(handle peer_tick journal read error)
				return 1;
			}

			peer_spool_raw_journal_into_upstream_snapshot(*bb, size);
			pg.journal_cursor = jc1;
			return 1;
		}
		return 0;
	} else if (g.is_peer && !g.is_host) {
		// already handled elsewhere, no?
		// or XXX should we read from savedir journal here?
		return 0;
	} else {
		assert(!"unreachable");
	}
	assert(!"unreachable");
}

static char* get_mim_buffer_top(void)
{
	assert(tlg.in_mim);
	arrsetmincap(tlg.mim_buffer_arr, arrlen(tlg.mim_buffer_arr) + STB_SPRINTF_MIN);
	return (char*)tlg.mim_buffer_arr + arrlen(tlg.mim_buffer_arr);
}

static void mimsrc(uint8_t* src, size_t n)
{
	assert(tlg.in_mim);
	uint8_t* dst = arraddnptr(tlg.mim_buffer_arr, n);
	memcpy(dst, src, n);
	(void)get_mim_buffer_top();
}

static char* wrote_mim_cb(const char* buf, void* user, int len)
{
	assert(tlg.in_mim);
	arrsetlen(tlg.mim_buffer_arr, arrlen(tlg.mim_buffer_arr)+len);
	return get_mim_buffer_top();
}

void mimf(const char* fmt, ...)
{
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
	assert((pg.my_artist_id>0) && "artist id not initialized");
	return pg.my_artist_id;
}

void set_my_artist_id(int artist_id)
{
	pg.my_artist_id = artist_id;
}

int alloc_artist_id(void)
{
	assert((hg.next_artist_id >= 1) && "alloc_artist_id() not allowed");
	return hg.next_artist_id++;
}

void free_artist_id(int artist_id)
{
	TODO(free artist id)
}

struct snapshot* get_snapshot(void)
{
	return &pg.fiddle_snapshot;
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
	struct snapshot* snap;
	struct mim_state* ms;
};

static void mimop_set_ms(struct mimop* mo, int artist_id, int session_id)
{
	assert(artist_id>0);
	assert(session_id>0);
	const int num_ms = arrlen(mo->snap->mim_state_arr);
	for (int i=0; i<num_ms; ++i) {
		struct mim_state* ms = &mo->snap->mim_state_arr[i];
		if ((ms->artist_id==artist_id) && (ms->session_id==session_id)) {
			mo->ms = ms;
			return;
		}
	}

	mo->ms = arraddnptr(mo->snap->mim_state_arr, 1);
	memset(mo->ms, 0, sizeof *mo->ms);
	mo->ms->artist_id = artist_id;
	mo->ms->session_id = session_id;
}

static struct mim_state* mimop_ms(struct mimop* mo)
{
	assert((mo->ms != NULL) && "missing mimop_set_ms()-call?");
	return mo->ms;
}

static struct document* mimop_lookup_doc(struct mimop* mo)
{
	if (mo->ms == NULL) return NULL;
	const int book_id = mimop_ms(mo)->book_id;
	const int doc_id  = mimop_ms(mo)->doc_id;
	if ((book_id <= 0) || (doc_id <= 0)) return NULL;
	return snapshot_lookup_document_by_ids(mo->snap, book_id, doc_id);
}

static struct document* mimop_get_doc(struct mimop* mo)
{
	struct document* doc = mimop_lookup_doc(mo);
	assert(doc != NULL);
	return doc;
}

static int mimop_has_doc(struct mimop* mo)
{
	return (mo->ms != NULL) && (mimop_lookup_doc(mo) != NULL);
}

static void doc_del(struct document* doc, struct snapshot* snap, int index)
{
	struct location dloc = document_reverse_locate(doc, index);
	const struct docchar dc = arrchkget(doc->docchar_arr, index);
	const int cp = dc.colorchar.codepoint;
	const int num_ms = arrlen(snap->mim_state_arr);
	for (int i=0; i<num_ms; ++i) {
		struct mim_state* ms = &snap->mim_state_arr[i];
		const int num_carets = arrlen(ms->caret_arr);
		for (int ii=0; ii<num_carets; ++ii) {
			struct caret* c = &ms->caret_arr[ii];
			for (int ca=0; ca<2; ++ca) {
				struct location* loc = (ca==0) ? &c->caret_loc : (ca==1) ? &c->anchor_loc : NULL;
				if (loc->line > dloc.line) {
					if (cp=='\n') {
						--loc->line;
						if (loc->line == dloc.line) {
							loc->column += (dloc.column-1);
						}
					}
				} else if (loc->line == dloc.line) {
					if (dloc.column < loc->column) {
						assert(cp!='\n');
						--loc->column;
					}
				}
			}
		}
	}
	arrdel(doc->docchar_arr, arrchk(doc->docchar_arr, index));
}

static void doc_ins(struct document* doc, struct snapshot* snap, int index, struct docchar dc)
{
	struct location iloc = document_reverse_locate(doc, index);
	const int cp = dc.colorchar.codepoint;
	const int num_ms = arrlen(snap->mim_state_arr);
	for (int i=0; i<num_ms; ++i) {
		struct mim_state* ms = &snap->mim_state_arr[i];
		const int num_carets = arrlen(ms->caret_arr);
		for (int ii=0; ii<num_carets; ++ii) {
			struct caret* c = &ms->caret_arr[ii];
			for (int ca=0; ca<2; ++ca) {
				struct location* loc = (ca==0) ? &c->caret_loc : (ca==1) ? &c->anchor_loc : NULL;
				if (loc->line > iloc.line) {
					if (cp=='\n') ++loc->line;
				} else if (loc->line == iloc.line) {
					if (iloc.column < loc->column) {
						if (cp=='\n') {
							++loc->line;
							loc->column -= (iloc.column-1);
							assert(loc->column >= 1);
						} else {
							++loc->column;
						}
					}
				}
			}
		}
	}
	arrins(doc->docchar_arr, index, dc);
}

static void mimop_delete(struct mimop* mo, struct location* loc0, struct location* loc1)
{
	struct document* rw_doc = mimop_get_doc(mo);
	location_sort2(&loc0, &loc1);
	const int o0 = document_locate(rw_doc, loc0);
	int o1 = document_locate(rw_doc, loc1);
	if (o1==o0) return;
	for (int o=o0; o<o1; ++o) {
		struct docchar* fc = arrchkptr(rw_doc->docchar_arr, o);
		if (fc->flags & FC_IS_INSERT) {
			doc_del(rw_doc, mo->snap, o);
			--o;
			--o1;
		} else if (!(fc->flags & FC_IS_DELETE)) {
			fc->flags |= FC_IS_DELETE;
		}
	}
}

struct mimexscanner {
	char* all0;
	char* all1;
	char* cmd;
	char* arg;
	char* err;
	char* cur;
	char* end;
	int cmdlen;
	unsigned did_match :1;
	unsigned has_error :1;
};

static int mimexscanner_errorf(struct mimexscanner* s, const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	stbsp_vsnprintf(tlg.errormsg, sizeof tlg.errormsg, fmt, va);
	va_end(va);
	s->has_error = 1;
	s->err = tlg.errormsg;
	return -1;
}

static int mimexscanner_init(struct mimexscanner* s, const char* c0, const char* c1)
{
	memset(s, 0, sizeof *s);
	const size_t num_bytes = (c1-c0);
	arrsetlen(tlg.mimex_buffer_arr, num_bytes+1);
	memcpy(tlg.mimex_buffer_arr, c0, num_bytes);
	tlg.mimex_buffer_arr[num_bytes] = 0;
	s->all0=tlg.mimex_buffer_arr;
	s->all1=s->all0 + num_bytes;
	// search for space in command if it exists, so whether
	// input is ":foo" or ":foo 42" we want the "foo" part
	// (which is the "ex" command, and the rest is arguments)
	int cmdlen=0;
	char* p=s->all0;
	for (p=s->all0; p<s->all1; ++cmdlen, ++p) if (*p == ' ') break;
	if (cmdlen == 0) return mimexscanner_errorf(s, "empty command");
	*p=0; // add NUL-terminator
	s->cmd = s->all0;
	s->cur = s->all0 + cmdlen + 1;
	s->end = s->all0 + num_bytes;
	return 0;
}

static char* mimexscanner_next_arg(struct mimexscanner* s)
{
	char* arg = s->cur;
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
	return arg;
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
	if (strcmp(s->cmd, cmd) != 0) return 0;

	s->did_match = 1;

	const int num_fmt = strlen(fmt);
	va_list va;
	va_start(va, fmt);
	for (int i=0; i<num_fmt; ++i) {
		if (s->has_error) break;
		switch (fmt[i]) {

		case 'i': {
			char* arg = mimexscanner_next_arg(s);
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
			char* arg = mimexscanner_next_arg(s);
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

					struct document* rw_doc = mimop_get_doc(mo);
					struct mim_state* ms = mimop_ms(mo);
					int num_chars = arrlen(rw_doc->docchar_arr);
					const int num_carets = arrlen(ms->caret_arr);
					for (int i=0; i<num_carets; ++i) {
						struct caret* car = arrchkptr(ms->caret_arr, i);
						if (car->tag != arg_tag) continue;
						struct location* loc0 = &car->caret_loc;
						struct location* loc1 = &car->anchor_loc;
						location_sort2(&loc0, &loc1);
						const int off0 = document_locate(rw_doc, loc0);
						const int off1 = document_locate(rw_doc, loc1);
						for (int off=off0; off<=off1; ++off) {
							for (int dir=0; dir<2; ++dir) {
								int d,o;
								if      (dir==0) { d= 1; o=off  ; }
								else if (dir==1) { d=-1; o=off-1; }
								else assert(!"unreachable");
								while ((0 <= o) && (o < num_chars)) {
									struct docchar* fc = arrchkptr(rw_doc->docchar_arr, o);
									const int is_fillable = fc->flags & (FC_IS_INSERT | FC_IS_DELETE);
									if ((fc->flags & (FC__FILL | FC_IS_DEFER)) || !is_fillable) break;
									if (is_fillable) {
										fc->flags |= FC__FILL;
									}
									o += d;
								}
							}
						}
						car->anchor_loc = car->caret_loc;
					}

					for (int i=0; i<num_chars; ++i) {
						struct docchar* fc = arrchkptr(rw_doc->docchar_arr, i);
						if (!(fc->flags & FC__FILL)) continue;

						if (chr=='!') {
							if (fc->flags & FC_IS_INSERT) { // commit
								fc->flags &= ~(FC__FILL | FC_IS_INSERT);
							}
							if (fc->flags & FC_IS_DELETE) {
								doc_del(rw_doc, mo->snap, i);
								--i;
								--num_chars;
							}
						} else if (chr=='/') { // cancel
							if (fc->flags & FC_IS_INSERT) {
								doc_del(rw_doc, mo->snap, i);
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
					struct document* rw_doc = mimop_get_doc(mo);
					struct mim_state* ms = mimop_ms(mo);
					const int num_carets = arrlen(ms->caret_arr);
					for (int i=0; i<num_carets; ++i) {
						struct caret* car = arrchkptr(ms->caret_arr, i);
						struct location* loc0 = &car->caret_loc;
						struct location* loc1 = &car->anchor_loc;
						location_sort2(&loc0, &loc1);
						const int off0 = document_locate(rw_doc, loc0);
						const int off1 = document_locate(rw_doc, loc1);
						assert(off0 <= off1);
						for (int o=off0; o<off1; ++o) {
							struct docchar* dc = arrchkptr(rw_doc->docchar_arr, o);
							if (dc->colorchar.splash4 != ms->splash4) {
								dc->colorchar.splash4 = ms->splash4;
							}
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

					struct document* rw_doc = mimop_get_doc(mo);
					struct mim_state* ms = mimop_ms(mo);
					const int num_carets = arrlen(ms->caret_arr);
					for (int i=0; i<num_carets; ++i) {
						struct caret* car = arrchkptr(ms->caret_arr, i);
						if (car->tag != arg_tag) continue;
						struct location* caret_loc = &car->caret_loc;
						struct location* anchor_loc = &car->anchor_loc;
						if (0 == location_compare(caret_loc, anchor_loc)) {
							int o = document_locate(rw_doc, caret_loc);
							int d,m0,m1,adj;
							if (chr == 'X') { // backspace
								d=-1; m0=-1; m1=-1;
								adj=0;
							} else if (chr == 'x') { // delete
								d=0;  m0=0;  m1=1;
								adj=1;
							} else {
								assert(!"unexpected char");
							}

							for (int i=0; i<arg_num; ++i) {
								const int num_chars = arrlen(rw_doc->docchar_arr);
								const int od = o+d;
								int dc=0;
								if ((0 <= od) && (od < num_chars)) {
									struct docchar* fc = arrchkptr(rw_doc->docchar_arr, od);
									if (fc->flags & FC_IS_INSERT) {
										doc_del(rw_doc, mo->snap, od);
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
								if (adj) {
									caret_loc->column += dc;
									doc_location_constraint(rw_doc, caret_loc);
								}
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

				int16_t splash4=0;
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

				struct document* rw_doc = mimop_get_doc(mo);
				const int num_carets = arrlen(ms->caret_arr);
				for (int i=0; i<num_carets; ++i) {
					struct caret* car = arrchkptr(ms->caret_arr, i);
					if (car->tag != arg_tag) continue;

					struct location* loc = &car->caret_loc;
					struct location* anchor = &car->anchor_loc;
					if (0 != location_compare(loc, anchor)) mimop_delete(mo, loc, anchor);
					*anchor = *loc;

					const int off = document_locate(rw_doc, loc);
					const int n0 = arrlen(rw_doc->docchar_arr);
					doc_ins(rw_doc, mo->snap, off, ((struct docchar){
						.colorchar = {
							.codepoint = chr,
							.splash4 = splash4,
						},
						.timestamp = now,
						//.artist_id = get_my_artist_id(),
						.flags = (FC_IS_INSERT | FC__FLIPPED_INSERT),
					}));
					const int n1 = arrlen(rw_doc->docchar_arr);
					assert(n1==(n0+1));
					//printf("%d => INS [%c] => %d\n", n0, chr, n1);
					if (chr == '\n') {
						++loc->line;
						loc->column = 1;
					} else {
						++loc->column;
					}
					doc_location_constraint(rw_doc, loc);
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
				struct document* readonly_doc = mimop_get_doc(mo);
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
					mimexscanner_init(&s, ex0, input_cursor);

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

								const int num_books = arrlen(mo->snap->book_arr);
								for (int ii=0; ii<num_books; ++ii) {
									struct book* book = &mo->snap->book_arr[ii];
									if (book_id == book->book_id) {
										return mimerr("book id %d already exists", book_id);
									}
								}

								arrput(mo->snap->book_arr, ((struct book){
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

							const int num_books = arrlen(mo->snap->book_arr);
							for (int i=0; i<num_books; ++i) {
								struct book* book = &mo->snap->book_arr[i];
								if (book->book_id == book_id) {
									book_id_exists = 1;
									break;
								}
							}

							if (!book_id_exists) {
								return mimerr("book id %d does not exist", book_id);
							}

							const int num_docs = arrlen(mo->snap->document_arr);
							for (int i=0; i<num_docs; ++i) {
								struct document* doc = &mo->snap->document_arr[i];
								if ((doc->book_id == book_id) && (doc->doc_id == doc_id)) {
									return mimerr(":newdoc %d %d collides with existing doc", book_id, doc_id);
								}
							}
							struct document doc = {
								.book_id = book_id,
								.doc_id  = doc_id,
							};
							const size_t n = strlen(name);
							arrsetlen(doc.name_arr, n+1);
							memcpy(doc.name_arr, name, n);
							doc.name_arr[n]=0;
							arrput(mo->snap->document_arr, doc);
						}
					}

					{
						int book_id, doc_id;
						if (mimex_matches(&s, "setdoc", "ii", &book_id, &doc_id)) {

							int book_id_exists = 0;
							const int num_books = arrlen(mo->snap->book_arr);
							for (int i=0; i<num_books; ++i) {
								struct book* book = &mo->snap->book_arr[i];
								if (book->book_id == book_id) {
									book_id_exists = 1;
									break;
								}
							}
							if (!book_id_exists) {
								return mimerr("setdoc on book id %d, but it doesn't exist", book_id);
							}

							int doc_id_exists = 0;
							const int num_docs = arrlen(mo->snap->document_arr);
							for (int i=0; i<num_docs; ++i) {
								struct document* doc = &mo->snap->document_arr[i];
								if ((doc->book_id == book_id) && (doc->doc_id == doc_id)) {
									doc_id_exists = 1;
									break;
								}
							}
							if (!doc_id_exists) {
								return mimerr("setdoc on doc id %d, but it doesn't exist", doc_id);
							}

							mimop_ms(mo)->book_id = book_id;
							mimop_ms(mo)->doc_id  = doc_id;
						}
					}

					if (s.has_error) {
						return mimerr("mimex error: %s", s.err);
					} else if (!s.did_match) {
						return mimerr("unhandled mimex command [%s]", s.cmd);
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

void peer_begin_mim(int session_id)
{
	assert(!tlg.in_mim);
	assert(g.is_peer);
	tlg.in_mim = 1;

	pg.mim_session_id = session_id;
	arrreset(tlg.mim_buffer_arr);
}

void peer_set_artificial_mim_latency(double mean, double variance)
{
	pg.artificial_mim_latency_mean     = mean;
	pg.artificial_mim_latency_variance = variance;
}

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

static int snapshot_spool(struct snapshot* snap, uint8_t* data, int num_bytes, int artist_id, int session_id)
{
	if (num_bytes == 0) return 0;
	struct mimop mo = { .snap = snap };
	if (artist_id > 0 && session_id > 0) {
		mimop_set_ms(&mo, artist_id, session_id);
	} else {
		assert(artist_id == 0);
		assert(session_id == 0);
	}
	int e = mim_spool(&mo, data, num_bytes);
	if (e<0) return e;
	#if 0
	if (!mimop_has_doc(&mo)) {
		return FMTERR0("mimop has no doc");
	}
	#endif
	return 0;
}

static int it_is_time_for_a_snapshotcache_push(uint64_t journal_offset)
{
	int64_t growth = (journal_offset - igo.journal_offset_at_last_snapshotcache_push);
	return growth > igo.journal_snapshot_growth_threshold;
}

static inline int jio_flush_bb(struct jio* jio, uint8_t** bb)
{
	const int e = jio_append(jio, *bb, arrlen(*bb));
	arrreset(*bb);
	return e;
}

static void pack_book(uint8_t** bb, struct book* book)
{
	bb_append_u8(bb, SYNC);
	bb_append_leb128(bb, book->book_id);
}

static void pack_document(uint8_t** bb, struct document* doc)
{
	bb_append_u8(bb, SYNC);
	bb_append_leb128(bb, doc->book_id);
	bb_append_leb128(bb, doc->doc_id);
	const int name_len = strlen(doc->name_arr);
	bb_append_leb128(bb, name_len);
	bb_append(bb, doc->name_arr, name_len);
	const int doc_len = arrlen(doc->docchar_arr);
	bb_append_leb128(bb, doc_len);
	for (int ii=0; ii<doc_len; ++ii) {
		struct docchar* c = &doc->docchar_arr[ii];
		struct colorchar* tc = &c->colorchar;
		bb_append_leb128(bb, tc->codepoint);
		assert((is_valid_splash4(tc->splash4)) && "did not expect bad splash4; don't want to write it");
		bb_append_leu16(bb, tc->splash4);
		bb_append_leb128(bb, c->flags);
	}
}

static void pack_mim_state(uint8_t** bb, struct mim_state* ms)
{
	bb_append_u8(bb, SYNC);
	bb_append_leb128(bb, ms->artist_id);
	bb_append_leb128(bb, ms->session_id);
	bb_append_leb128(bb, ms->book_id);
	bb_append_leb128(bb, ms->doc_id);
	assert((is_valid_splash4(ms->splash4)) && "did not expect bad splash4; don't want to write it");
	bb_append_leu16(bb, ms->splash4);
	const int num_carets = arrlen(ms->caret_arr);
	bb_append_leb128(bb, num_carets);
	for (int ii=0; ii<num_carets; ++ii) {
		struct caret* cr = &ms->caret_arr[ii];
		bb_append_leb128(bb, cr->tag);
		bb_append_leb128(bb, cr->caret_loc.line);
		bb_append_leb128(bb, cr->caret_loc.column);
		bb_append_leb128(bb, cr->anchor_loc.line);
		bb_append_leb128(bb, cr->anchor_loc.column);
	}
}

static void pack_full_snapshot(uint8_t** bb, struct snapshot* snap, int64_t journal_offset)
{
	bb_append_u8(bb, SYNC);

	bb_append_leb128(bb, journal_offset);

	const int num_books = arrlen(snap->book_arr);
	bb_append_leb128(bb, num_books);
	for (int i=0; i<num_books; ++i) {
		pack_book(bb, &snap->book_arr[i]);
	}

	const int num_docs = arrlen(snap->document_arr);
	bb_append_leb128(bb, num_docs);
	for (int i=0; i<num_docs; ++i) {
		pack_document(bb, &snap->document_arr[i]);
	}

	const int num_mim_states = arrlen(snap->mim_state_arr);
	bb_append_leb128(bb, num_mim_states);
	for (int i=0; i<num_mim_states; ++i) {
		pack_mim_state(bb, &snap->mim_state_arr[i]);
	}

	// TODO? server settings/prefs? or keep that separate?
}

void* get_present_snapshot_data(size_t* out_size)
{
	uint8_t** bb = &hg.bb_arr;
	arrreset(*bb);
	pack_full_snapshot(bb, &hg.present_snapshot, jio_get_size(igo.jio_journal));
	void* data = bb_dup2plain(bb);
	if (out_size) *out_size = arrlen(*bb);
	return data;
}

int copy_journal(void* dst, int64_t count, int64_t offset)
{
	return jio_pread(igo.jio_journal, dst, count, offset);
}

static void snapshotcache_push(struct snapshot* snap, uint64_t journal_offset)
{
	struct jio* jdat = igo.jio_snapshotcache_data;
	struct jio* jidx = igo.jio_snapshotcache_index;
	const size_t jdat0 = jio_get_size(jdat);

	uint8_t** bb = &hg.bb_arr;
	arrreset(*bb);

	const int num_books = arrlen(snap->book_arr);
	for (int i=0; i<num_books; ++i) {
		struct book* book = &snap->book_arr[i];
		if (book->snapshotcache_offset) continue;
		book->snapshotcache_offset = jdat0 + arrlen(*bb);
		pack_book(bb, book);
	}

	const int num_documents = arrlen(snap->document_arr);
	for (int i=0; i<num_documents; ++i) {
		struct document* doc = &snap->document_arr[i];
		if (doc->snapshotcache_offset) continue;
		doc->snapshotcache_offset = jdat0 + arrlen(*bb);
		pack_document(bb, doc);
	}

	const int num_mim_states = arrlen(snap->mim_state_arr);
	for (int i=0; i<num_mim_states; ++i) {
		struct mim_state* ms = &snap->mim_state_arr[i];
		if (ms->snapshotcache_offset) continue;
		ms->snapshotcache_offset = jdat0 + arrlen(*bb);
		pack_mim_state(bb, ms);
	}

	const int64_t snapshot_manifest_offset = jdat0 + arrlen(*bb);
	bb_append_u8(bb, SYNC);
	bb_append_leb128(bb, num_books);
	bb_append_leb128(bb, num_documents);
	bb_append_leb128(bb, num_mim_states);
	for (int i=0; i<num_books; ++i) {
		struct book* book = &snap->book_arr[i];
		bb_append_leb128(bb, book->snapshotcache_offset);
	}
	for (int i=0; i<num_documents; ++i) {
		struct document* doc = &snap->document_arr[i];
		bb_append_leb128(bb, doc->snapshotcache_offset);
	}
	for (int i=0; i<num_mim_states; ++i) {
		struct mim_state* ms = &snap->mim_state_arr[i];
		bb_append_leb128(bb, ms->snapshotcache_offset);
	}
	jio_flush_bb(jdat, bb);

	arrreset(*bb);

	// write index triplet
	bb_append_leu64(bb, get_nanoseconds_epoch()); // XXX correct timestamp?
	bb_append_leu64(bb, snapshot_manifest_offset);
	bb_append_leu64(bb, journal_offset);
	jio_flush_bb(jidx, bb);

	igo.journal_offset_at_last_snapshotcache_push = journal_offset;
}

void peer_end_mim(void)
{
	assert(g.is_peer);
	assert(tlg.in_mim);
	tlg.in_mim = 0;

	const int num_bytes = arrlen(tlg.mim_buffer_arr);
	if (num_bytes == 0) return;
	uint8_t* data = tlg.mim_buffer_arr;

	const int artist_id  = get_my_artist_id();
	const int session_id = pg.mim_session_id;
	assert(artist_id>0);
	assert(session_id>0);

	struct snapshot* snap = &pg.fiddle_snapshot;
	(void)snapshot_get_or_create_mim_state_by_ids(snap, artist_id, session_id);
	const int e = snapshot_spool(snap, data, num_bytes, artist_id, session_id);
	if (e<0) fprintf(stderr, "SPOOL ERR/0 %d!\n", e);

	if (arrlen(snap->document_arr)>0) {
		//TODO(proper mie/vmie document stuff)
		struct document* doc = &snap->document_arr[0];

		static struct colorchar* dodoc_arr = NULL;
		document_to_colorchar_da(&dodoc_arr, doc);

		mie_begin_scrallox();
		if (0 == setjmp(*mie_prep_scrallox_jmp_buf_for_out_of_memory())) {
			const int prg = mie_compile_colorcode(dodoc_arr, arrlen(dodoc_arr));
			if (prg < 0) {
				//TODO(handle compile error)
			} else {
				vmie_reset(prg);
				vmie_run();
				#if 0
				const char* err = mie_error();
				if ((err!=NULL) && (strlen(err)>0)) fprintf(stderr, "ERR: %s\n", err);
				vmie_dump_stack();
				#endif
				mie_program_free(prg);
			}
		} else {
			XXX(scrallox ran out of mem!)
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

	int64_t not_before_ts = 0;
	// set not_before_ts to now plus added artificial latency if it's
	// configured. not_before_ts=0 means "as soon as possible" (plus it takes
	// up less space due to LEB128)
	if ((pg.artificial_mim_latency_mean != 0) || (pg.artificial_mim_latency_variance != 0)) {
		// using a simple approximation to generate a values from the normal
		// distribution, see:
		// https://en.wikipedia.org/wiki/Irwin%E2%80%93Hall_distribution#Approximating_a_Normal_distribution
		double uacc = -6.0;
		const double scalar = 1.0 / (double)RAND_MAX;
		for (int i=0; i<12; ++i) uacc += (double)rand() * scalar;
		double dt = (uacc * pg.artificial_mim_latency_variance) + pg.artificial_mim_latency_mean;
		not_before_ts = get_nanoseconds_epoch() + (int64_t)(dt*1e9);
	}

	uint8_t** bb = &pg.bb_arr;
	arrreset(*bb);
	bb_append_leb128(bb, session_id);
	const int tracer = ++pg.tracer_sequence;
	bb_append_leb128(bb, tracer);
	bb_append_leb128(bb, not_before_ts);
	bb_append_leb128(bb, num_bytes);
	bb_append(bb, data, num_bytes);
	if (g.is_host) {
		if (ringbuf_write(&g.peer2host_mim_ringbuf, *bb, arrlen(*bb)) < 0) {
			fprintf(stderr, "peer2host_mim_ringbuf is full!\n");
			abort();
		}
	}
	uint8_t* p = arraddnptr(pg.unackd_mimbuf_arr, arrlen(*bb));
	memcpy(p, *bb, arrlen(*bb));

	if (!g.is_host) {
		transmit_mim(session_id, tracer, data, num_bytes);
	}
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
	struct jio* jdat = jio_open(pathbuf, IO_OPEN, igo.io_port_id, 16, &err);
	if (jdat == NULL) return IOERR(pathbuf, err);
	igo.jio_snapshotcache_data = jdat;
	const int64_t szdat = jio_get_size(jdat);
	if (szdat == 0) {
		jio_close(jdat);
		return FMTERR(pathbuf, "file is empty");
	}

	STATIC_PATH_JOIN(pathbuf, dir, FILENAME_SNAPSHOTCACHE_INDEX);
	struct jio* jidx = jio_open(pathbuf, IO_OPEN, igo.io_port_id, 16, &err);
	if (jidx == NULL) return IOERR(pathbuf, err);
	igo.jio_snapshotcache_index = jidx;
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
	struct jio* jdat = jio_open(pathbuf, IO_CREATE, igo.io_port_id, 16, &err);
	if (jdat == NULL) {
		return IOERR(FILENAME_SNAPSHOTCACHE_DATA, err);
	}
	igo.jio_snapshotcache_data = jdat;
	uint8_t** bb = &hg.bb_arr;
	arrreset(*bb);
	bb_append(bb, DOSD_MAGIC, strlen(DOSD_MAGIC));
	bb_append_leu64(bb, journal_insignia);
	jio_flush_bb(jdat, bb);

	STATIC_PATH_JOIN(pathbuf, dir, FILENAME_SNAPSHOTCACHE_INDEX);
	struct jio* jidx = jio_open(pathbuf, IO_CREATE, igo.io_port_id, 16, &err);
	if (jidx == NULL) {
		jio_close(jdat);
		return IOERR(FILENAME_SNAPSHOTCACHE_INDEX, err);
	}
	igo.jio_snapshotcache_index = jidx;
	bb_append(bb, DOSI_MAGIC, strlen(DOSI_MAGIC));
	bb_append_leu64(bb, journal_insignia);
	jio_flush_bb(jidx, bb);

	return 0;
}

static int unpack_book(struct book* book, struct bufstream* bs)
{
	memset(book, 0, sizeof *book);
	uint8_t sync = bs_read_u8(bs);
	if (sync != SYNC) return FMTERR0("expected SYNC");
	book->book_id = bs_read_leb128(bs);
	return bs->error;
}

static int unpack_document(struct document* doc, struct bufstream* bs)
{
	uint8_t sync = bs_read_u8(bs);
	if (sync != SYNC) return FMTERR0("expected SYNC");
	doc->book_id = bs_read_leb128(bs);
	doc->doc_id = bs_read_leb128(bs);
	const int64_t name_len = bs_read_leb128(bs);
	arrsetlen(doc->name_arr, 1+name_len);
	bs_read(bs, (uint8_t*)doc->name_arr, name_len);
	doc->name_arr[name_len] = 0;


	const int64_t doc_len = bs_read_leb128(bs);
	struct docchar* cs = arraddnptr(doc->docchar_arr, doc_len);
	for (int64_t i=0; i<doc_len; ++cs, ++i) {
		cs->colorchar.codepoint = bs_read_leb128(bs);
		cs->colorchar.splash4 = bs_read_leu16(bs);
		if (!is_valid_splash4(cs->colorchar.splash4)) {
			return -2; // XXX better error?
		}
		cs->flags = bs_read_leb128(bs);
	}
	return 0;
}

static int unpack_mim_state(struct mim_state* ms, struct bufstream* bs)
{
	uint8_t sync = bs_read_u8(bs);
	if (sync != SYNC) return FMTERR0("expected SYNC");
	ms->artist_id  = bs_read_leb128(bs);
	ms->session_id = bs_read_leb128(bs);
	ms->book_id    = bs_read_leb128(bs);
	ms->doc_id     = bs_read_leb128(bs);
	ms->splash4    = bs_read_leu16(bs);
	if (!is_valid_splash4(ms->splash4)) {
		return FMTERR0("bad splash4 color in mim state");
	}

	const int64_t num_carets = bs_read_leb128(bs);
	struct caret* cr = arraddnptr(ms->caret_arr, num_carets);
	for (int64_t i=0; i<num_carets; ++cr, ++i) {
		cr->tag               = bs_read_leb128(bs);
		cr->caret_loc.line    = bs_read_leb128(bs);
		cr->caret_loc.column  = bs_read_leb128(bs);
		cr->anchor_loc.line   = bs_read_leb128(bs);
		cr->anchor_loc.column = bs_read_leb128(bs);
	}
	return 0;
}

int restore_a_snapshot_from_data(struct snapshot* snap, void* data, size_t sz, int64_t* out_journal_offset)
{
	struct bufstream bs;
	bufstream_init_from_memory(&bs, data, sz);

	uint8_t sync = bs_read_u8(&bs);
	if (sync != SYNC) return FMTERR0("expected SYNC");

	const int64_t journal_offset = bs_read_leb128(&bs);
	if (out_journal_offset) *out_journal_offset = journal_offset;
	int e;

	// see also pack_book()
	const int64_t num_books = bs_read_leb128(&bs);
	arrreset(snap->book_arr);
	struct book* books = arraddnptr(snap->book_arr, num_books);
	memset(books, 0, num_books*sizeof(books[0]));
	for (int64_t i=0; i<num_books; ++i) {
		e = unpack_book(&books[i], &bs);
		if (e<0) return e;
	}

	// see also pack_document()
	const int64_t num_docs = bs_read_leb128(&bs);
	arrreset(snap->document_arr);
	struct document* docs = arraddnptr(snap->document_arr, num_docs);
	memset(docs, 0, num_docs*sizeof(docs[0]));
	for (int64_t i=0; i<num_docs; ++i) {
		e = unpack_document(&docs[i], &bs);
		if (e<0) return e;
	}

	// see also pack_mim_state()
	const int64_t num_mim_states = bs_read_leb128(&bs);
	arrreset(snap->mim_state_arr);
	struct mim_state* mim_states = arraddnptr(snap->mim_state_arr, num_mim_states);
	memset(mim_states, 0, num_mim_states*sizeof(mim_states[0]));
	for (int64_t i=0; i<num_mim_states; ++i) {
		e = unpack_mim_state(&mim_states[i], &bs);
		if (e<0) return e;
	}

	return 0;
}

int64_t restore_upstream_snapshot_from_data(void* data, size_t sz)
{
	struct snapshot* snap = &pg.upstream_snapshot; // XXX
	int64_t journal_cursor;
	if (restore_a_snapshot_from_data(snap, data, sz, &journal_cursor) < 0) {
		dumperr();
		TODO(handle snapshot restore error)
	}
	return journal_cursor;
}

static int restore_snapshot_from_disk(struct snapshot* snap, uint64_t snapshot_manifest_offset)
{
	memset(snap, 0, sizeof *snap);

	struct jio* jdat = igo.jio_snapshotcache_data;

	struct bufstream bs0,bs1;
	uint8_t buf0[1<<8], buf1[1<<8];
	bufstream_init_from_jio(&bs0, jdat, snapshot_manifest_offset, buf0, sizeof buf0);

	uint8_t sync = bs_read_u8(&bs0);
	const char* path = FILENAME_SNAPSHOTCACHE_DATA;
	if (sync != SYNC) return FMTERR(path, "expected SYNC");

	const int64_t num_books      = bs_read_leb128(&bs0);
	const int64_t num_documents  = bs_read_leb128(&bs0);
	const int64_t num_mim_states = bs_read_leb128(&bs0);

	for (int64_t i=0; i<num_books; ++i) {
		const int64_t oo1 = bs_read_leb128(&bs0);
		bufstream_init_from_jio(&bs1, jdat, oo1, buf1, sizeof buf1);
		struct book book={0};
		if (unpack_book(&book, &bs1) < 0) return FMTERR(path, "bad book");
		book.snapshotcache_offset = oo1;
		if (bs1.error) return IOERR(path, bs1.error);
		arrput(snap->book_arr, book);
	}

	for (int64_t i=0; i<num_documents; ++i) {
		const int64_t oo1 = bs_read_leb128(&bs0);
		bufstream_init_from_jio(&bs1, jdat, oo1, buf1, sizeof buf1);
		struct document doc={0};
		if (unpack_document(&doc, &bs1) < 0) return FMTERR(path, "bad doc");
		doc.snapshotcache_offset = oo1;
		if (bs1.error) return IOERR(path, bs1.error);
		arrput(snap->document_arr, doc);
	}

	for (int64_t i=0; i<num_mim_states; ++i) {
		const int64_t oo1 = bs_read_leb128(&bs0);
		bufstream_init_from_jio(&bs1, jdat, oo1, buf1, sizeof buf1);
		//printf("mim %ld => %ld\n", i, o1);
		struct mim_state ms={0};
		if (unpack_mim_state(&ms, &bs1) < 0) return FMTERR(path, "bad mim");
		ms.snapshotcache_offset = oo1;
		if (bs1.error) return IOERR(path, bs1.error);
		arrput(snap->mim_state_arr, ms);
	}

	if (bs0.error) return IOERR(path, bs0.error);

	return 0;
}

static int can_restore_latest_snapshot(void)
{
	struct jio* jidx = igo.jio_snapshotcache_index;
	int64_t sz = jio_get_size(jidx);
	if (!is_snapshotcache_index_size_valid(sz)) return 0;
	return get_num_snapshotcache_index_entries_from_size(sz) > 0;
}

static int restore_latest_snapshot(struct snapshot* snap, int64_t* out_journal_offset)
{
	if (!can_restore_latest_snapshot()) {
		return FMTERR(FILENAME_SNAPSHOTCACHE_INDEX, "bad index file");
	}
	struct jio* jidx = igo.jio_snapshotcache_index;
	const int64_t sz = jio_get_size(jidx);
	const int64_t o = (sz - 2*sizeof(uint64_t));

	struct bufstream bs0;
	uint8_t buf0[1<<8];
	bufstream_init_from_jio(&bs0, jidx, o, buf0, sizeof buf0);
	const uint64_t snapshot_manifest_offset = bs_read_leu64(&bs0);
	const uint64_t journal_offset = bs_read_leu64(&bs0);
	if (bs0.error) {
		return IOERR(FILENAME_SNAPSHOTCACHE_INDEX, bs0.error);
	}
	if (out_journal_offset) *out_journal_offset = journal_offset;
	return restore_snapshot_from_disk(snap, snapshot_manifest_offset);
}

static int spool_raw_journal_bs(struct snapshot* snap, struct bufstream* bs, int64_t until_offset, int64_t* out_max_tracer)
{
	static uint8_t* mimbuf_arr = NULL;

	while (bs->offset < until_offset) {
		const uint8_t sync = bs_read_u8(bs);
		if (sync != SYNC) {
			return FMTERR(FILENAME_JOURNAL, "expected SYNC");
		}
		const int64_t timestamp_us = bs_read_leb128(bs);
		(void)timestamp_us; // XXX? remove?
		const int64_t artist_id = bs_read_leb128(bs);
		const int64_t session_id = bs_read_leb128(bs);
		const int64_t tracer = bs_read_leb128(bs);
		if (out_max_tracer) {
			*out_max_tracer = tracer;
		}
		const int64_t num_bytes = bs_read_leb128(bs);
		arrsetlen(mimbuf_arr, num_bytes);
		bs_read(bs, mimbuf_arr, num_bytes);
		int e = snapshot_spool(snap, mimbuf_arr, num_bytes, artist_id, session_id);
		if (e<0) return e;
	}

	if (bs->offset != until_offset) {
		return FMTERR(FILENAME_JOURNAL, "expected to spool journal until end-of-file");
	}
	return 0;
}

int peer_spool_raw_journal_into_upstream_snapshot(void* data, int64_t count)
{
	assert(g.is_peer);

	struct bufstream bs;
	bufstream_init_from_memory(&bs, data, count);
	int64_t max_tracer = -1;
	struct snapshot* upsnap = &pg.upstream_snapshot;
	const int e = spool_raw_journal_bs(upsnap, &bs, count, &max_tracer);
	if (e<0) {
		dumperr();
		fprintf(stderr, "spool_raw_journal_bs() of %d bytes => %d:\n", (int)count, e);
		hexdump(data, count);
		return -1;
	}
	assert((max_tracer != -1) && "expected tracer to be set");

	// re-spool inflight mim that has not yet been ack'd
	struct snapshot* fidsnap = &pg.fiddle_snapshot;
	snapshot_copy(fidsnap, upsnap);

	const int64_t num_total = arrlen(pg.unackd_mimbuf_arr);
	bufstream_init_from_memory(&bs, pg.unackd_mimbuf_arr, num_total);
	int64_t trunc_to = 0;
	int64_t prev_tracer = -1;
	while (bs.offset < num_total) {
		const int64_t session_id     =  bs_read_leb128(&bs);
		const int64_t tracer         =  bs_read_leb128(&bs);
		/*const int64_t not_befor_ts =*/bs_read_leb128(&bs); // ignored
		const int64_t num_bytes      =  bs_read_leb128(&bs);
		if (tracer <= prev_tracer) {
			fprintf(stderr, "unordered tracer sequence [%ld,%ld]\n", (long)prev_tracer, (long)tracer);
			return -1;
		}
		assert(tracer > prev_tracer);
		prev_tracer = tracer;
		if (tracer <= max_tracer) {
			// journal received from host already covers this tracer, so skip
			// it and continue
			bs_skip(&bs, num_bytes);
			assert(bs.offset > trunc_to);
			trunc_to = bs.offset;
		} else {
			const int e = snapshot_spool(fidsnap, &pg.unackd_mimbuf_arr[bs.offset], num_bytes, get_my_artist_id(), session_id);
			if (e<0) fprintf(stderr, "SPOOL ERR/1 %d!\n", e);
			bs_skip(&bs, num_bytes);
		}
	}

	// remove that which was actually ack'd
	if (trunc_to > 0) {
		arrdeln(pg.unackd_mimbuf_arr, 0, trunc_to);
	}

	return 0;
}

void commit_mim_to_host(int artist_id, int session_id, int64_t tracer, uint8_t* data, int count)
{
	struct snapshot* snap = &hg.present_snapshot;
	const int e = snapshot_spool(snap, data, count, artist_id, session_id);
	if (e<0) {
		fprintf(stderr, "SPOOL ERR/2 %d!\n", e);
		return;
	}
	struct jio* jj = igo.jio_journal;
	assert(jj != NULL);
	uint8_t** bb = &pg.bb_arr;
	arrreset(*bb);
	bb_append_u8(bb, SYNC);
	const int64_t journal_timestamp = (get_nanoseconds() - igo.journal_timestamp_start)/1000LL;
	bb_append_leb128(bb, journal_timestamp);
	bb_append_leb128(bb, artist_id);
	bb_append_leb128(bb, session_id);
	bb_append_leb128(bb, tracer);
	bb_append_leb128(bb, count);
	bb_append(bb, data, count);
	jio_flush_bb(jj, bb);
	const int64_t jsz = jio_get_size(jj);
	if (it_is_time_for_a_snapshotcache_push(jsz)) {
		snapshotcache_push(snap, jsz);
	}
}

int host_tick(void)
{
	assert(g.is_host);

	// handle completion events from io port
	int did_work = 0;
	#ifndef __EMSCRIPTEN__
	struct io_event ev = {0};
	while (io_port_poll(igo.io_port_id, &ev)) {
		did_work = 1;
		io_echo ec = ev.echo;
		if (igo.jio_journal             && jio_ack(igo.jio_journal             , ec)) continue;
		if (igo.jio_snapshotcache_data  && jio_ack(igo.jio_snapshotcache_data  , ec)) continue;
		if (igo.jio_snapshotcache_index && jio_ack(igo.jio_snapshotcache_index , ec)) continue;
		assert(!"unhandled event");
	}
	#endif

	if (g.is_peer) {
		const int artist_id = get_my_artist_id();
		struct peer_state* ps = host_get_or_create_peer_state_by_artist_id(artist_id);
		uint8_t** bb = &ps->cmdbuf_arr;
		if (arrlen(*bb) == 0) {
			int64_t p0,p1;
			struct ringbuf* rb = &g.peer2host_mim_ringbuf;
			ringbuf_get_readable_range(rb, &p0, &p1);
			const int64_t nrb = (p1-p0);
			if (nrb>0) {
				assert(p1>p0);
				arrsetlen(*bb, nrb);
				ringbuf_read_range(rb, *bb, p0, p1);
				did_work = 1;
			}
		}
	}

	const int num_ps = arrlen(hg.peer_state_arr);
	for (int i=0; i<num_ps; ++i) {
		struct peer_state* ps = &hg.peer_state_arr[i];
		const int64_t n = arrlen(ps->cmdbuf_arr);
		struct bufstream bs;
		bufstream_init_from_memory(&bs, ps->cmdbuf_arr, n);
		int64_t cursor = 0;
		while (cursor < n) {
			const int64_t o0 = bs.offset;
			const int64_t session_id = bs_read_leb128(&bs);
			const int64_t tracer = bs_read_leb128(&bs);
			const int64_t not_before_ts  = bs_read_leb128(&bs);
			const int is_released = (not_before_ts == 0) || (get_nanoseconds_epoch() > not_before_ts);
			if (!is_released) break;
			const int64_t num_bytes  = bs_read_leb128(&bs);
			const int64_t o1 = bs.offset;
			cursor += (o1-o0);

			commit_mim_to_host(ps->artist_id, session_id, tracer, ps->cmdbuf_arr + cursor, num_bytes);
			bs_skip(&bs, num_bytes);
			did_work = 1;
			cursor += num_bytes;
			assert(cursor <= n);
		}

		assert(cursor <= n);
		if (cursor == n) {
			arrreset(ps->cmdbuf_arr);
		} else if (cursor > 0) {
			const int nn = n-cursor;
			assert(nn > 0);
			memmove(ps->cmdbuf_arr, ps->cmdbuf_arr + cursor, nn);
			arrsetlen(ps->cmdbuf_arr, nn);
		}
	}

	#ifndef __EMSCRIPTEN__ // XXX not totally right?
	did_work |= webserv_broadcast_journal(jio_get_size(igo.jio_journal));
	#endif

	return did_work;
}

#if 0
void gig_maybe_setup_stub(void)
{
	// XXX doesn't work too well
	struct snapshot* snap = &g.cool_snapshot;
	if (arrlen(snap->book_arr) > 0) {
		return;
	}
	peer_begin_mim(1);
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	mimex("setdoc 1 50");
	mimf("0,1,1c");
	peer_end_mim();
}
#endif

static void host_begin_mim(void)
{
	assert(g.is_host);
	assert(!tlg.in_mim);
	tlg.in_mim = 1;
	arrreset(tlg.mim_buffer_arr);
}

static void host_end_mim(void)
{
	assert(tlg.in_mim);
	tlg.in_mim = 0;
	assert(g.is_host);
	const int num_bytes = arrlen(tlg.mim_buffer_arr);
	if (num_bytes == 0) return;
	#if 0
	printf("MIM[");
	for (int i=0; i<num_bytes; ++i) printf("%c",tlg.mim_buffer_arr[i]);
	printf("]\n");
	#endif
	commit_mim_to_host(0, 0, 0, tlg.mim_buffer_arr, num_bytes);
}

static void setup_default_stub(void)
{
	// TODO later we probably want to allow choosing the "book fundament"
	// (mie-urlyd) and "book template" (-) /before/ creating the journal
	// file... this is because we're probably going to add 1000s of lines of
	// standard libraries and stuff which is a shame to put in the journal file
	// (which remembers everything) if we're not going to need it
	host_begin_mim();
	mimex("newbook 1 mie-urlyd -");
	mimex("newdoc 1 50 art.mie");
	host_end_mim();
}


static int setup_datadir(const char* dir)
{
	char pathbuf[1<<14];
	STATIC_PATH_JOIN(pathbuf, dir, FILENAME_JOURNAL);
	int err;
	struct jio* jj = jio_open(pathbuf, IO_OPEN_OR_CREATE, igo.io_port_id, 16, &err);
	igo.jio_journal = jj;
	if (jj == NULL) return IOERR(FILENAME_JOURNAL, err);

	// TODO setup journal jio for fdatasync?

	const int64_t sz = jio_get_size(jj);
	if (sz == 0) {
		uint8_t** bb = &hg.bb_arr;
		arrreset(*bb);
		bb_append(bb, DOJO_MAGIC, strlen(DOJO_MAGIC));
		bb_append_leu64(bb, DO_FORMAT_VERSION);
		const uint64_t insignia = make_insignia();
		bb_append_leu64(bb, insignia);
		// TODO epoch timestamp?
		jio_flush_bb(jj, bb);
		err = jio_get_error(jj);
		if (err<0) {
			return IOERR(FILENAME_JOURNAL, err);
		}
		igo.journal_timestamp_start = get_nanoseconds();

		err = snapshotcache_create(dir, insignia);
		if (err<0) {
			dumperr();
			return IOERR(FILENAME_JOURNAL, err);
		}

		if (g.is_host) setup_default_stub();
	} else {
		if (!g.is_host) {
			return FMTERR(pathbuf, "journal save file already exists; delete it or choose another savedir");
		}

		uint8_t magic[8];
		struct bufstream bs0;
		uint8_t buf0[1<<8];
		bufstream_init_from_jio(&bs0, jj, 0, buf0, sizeof buf0);
		bs_read(&bs0, magic, sizeof magic);
		if (memcmp(magic, DOJO_MAGIC, 8) != 0) {
			return FMTERR(pathbuf, "invalid magic in journal header");
		}
		const int64_t do_format_version = bs_read_leu64(&bs0);
		if (do_format_version != DO_FORMAT_VERSION) {
			return FMTERR(pathbuf, "unknown do-format-version");
		}
		const uint64_t insignia = bs_read_leu64(&bs0);

		int64_t journal_spool_offset = -1;

		struct snapshot* snap = &hg.present_snapshot;
		int err = snapshotcache_open(dir, insignia);
		if (err == IO_NOT_FOUND) {
			// OK just spool journal from beginning
		} else {
			if (can_restore_latest_snapshot()) {
				err = restore_latest_snapshot(snap, &journal_spool_offset);
				if (err<0) {
					return err;
				}
				assert(journal_spool_offset > 0);
			} else {
				// spool from beginning
			}
		}

		const int64_t jjsz = jio_get_size(jj);
		if (journal_spool_offset > 0) {
			if (journal_spool_offset  > jjsz) {
				return FMTERR(FILENAME_JOURNAL, "journal spool offset past end-of-file");
			}
			bufstream_init_from_jio(&bs0, jj, journal_spool_offset, buf0, sizeof buf0);
		}

		if (spool_raw_journal_bs(snap, &bs0, jjsz, NULL) < 0) {
			return bs0.error;
		}
		if (bs0.error<0) {
			return IOERR(FILENAME_JOURNAL, bs0.error);
		}
	}

	return 0;
}

int gig_configure_as_host_and_peer(const char* rootdir)
{
	assert(!g.is_configured);
	g.is_configured = 1;
	g.is_host = 1;
	g.is_peer = 1;
	pg.my_artist_id = 1;
	hg.next_artist_id = 2;
	ringbuf_init(&g.peer2host_mim_ringbuf, 16);
	int e = setup_datadir(rootdir);
	if (e<0) {
		dumperr();
		return e;
	}
	//snapshot_copy(&pg.upstream_snapshot, &hg.present_snapshot);
	return 0;
}

int gig_configure_as_host_only(const char* rootdir)
{
	assert(!g.is_configured);
	g.is_configured = 1;
	g.is_host = 1;
	hg.next_artist_id = 1;
	return setup_datadir(rootdir);
}

int gig_configure_as_peer_only(const char* savedir)
{
	assert(!g.is_configured);
	g.is_configured = 1;
	g.is_peer = 1;
	hg.next_artist_id = -1;
	return setup_datadir(savedir);
	return 0;
}

void gig_unconfigure(void)
{
	assert(g.is_configured);
	assert(g.is_host || g.is_peer);

	// globals (g)
	if (g.is_host && g.is_peer) {
		ringbuf_free(&g.peer2host_mim_ringbuf);
	} else {
		assert(g.peer2host_mim_ringbuf.buf != NULL);
	}
	memset(&g, 0, sizeof g);

	// I/O globals (igo)
	jio_close(igo.jio_journal);
	jio_close(igo.jio_snapshotcache_data);
	jio_close(igo.jio_snapshotcache_index);
	memset(&igo, 0, sizeof igo);

	// host globals
	arrfree(hg.bb_arr);
	arrfree(hg.peer_state_arr);
	snapshot_free(&hg.present_snapshot);
	memset(&hg, 0, sizeof hg);

	// peer globals
	arrfree(pg.bb_arr);
	arrfree(pg.unackd_mimbuf_arr);
	snapshot_free(&pg.upstream_snapshot);
	snapshot_free(&pg.fiddle_snapshot);
	memset(&pg, 0, sizeof pg);

}

void gig_set_journal_snapshot_growth_threshold(int t)
{
	assert(t > 0);
	igo.journal_snapshot_growth_threshold = t;
}

void gig_init(void)
{
	//assert(thrd_success == mtx_init(&g.snap_mutex, mtx_plain));
	#ifdef __EMSCRIPTEN__
	igo.io_port_id = -1;
	#else
	igo.io_port_id = io_port_create();
	#endif
	gig_set_journal_snapshot_growth_threshold(5000);
}

void gig_selftest(void)
{
	// TODO (used to be a mim test here)
}
