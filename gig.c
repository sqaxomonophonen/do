#include <limits.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h> // XXX

#include "stb_sprintf.h"

#include "gig.h"
#include "util.h"
#include "leb128.h"
#include "utf8.h"
#include "main.h"
#include "da.h"

struct ringbuf {
	uint8_t* data;
	int size_log2;

	//size_t read_cursor;
	size_t write_cursor;

	// fields on separate cache lines to avoid "false sharing"
	//_Alignas(CACHE_LINE_SIZE) _Atomic(size_t) acknowledge_cursor;
	_Alignas(CACHE_LINE_SIZE) _Atomic(size_t) commit_cursor;
};

static void ringbuf_init_with_pointer(struct ringbuf* rb, void* data, int size_log2)
{
	memset(rb, 0, sizeof *rb);
	rb->size_log2 = size_log2;
	rb->data = data;
}

static void ringbuf_init(struct ringbuf* rb, int size_log2)
{
	ringbuf_init_with_pointer(rb, calloc(1L<<size_log2, 1), size_log2);
}

static void ringbuf_commit(struct ringbuf* rb)
{
	atomic_store(&rb->commit_cursor, rb->write_cursor);
}


static void ringbuf_writen(struct ringbuf* rb, const void* src_data, size_t num_bytes)
{
	const size_t size = 1L << rb->size_log2;
	const size_t mask = size - 1L;
	const size_t cursor = rb->write_cursor;
	const size_t until_next_wrap_around = size - (cursor & mask);
	uint8_t* dst_data = rb->data;
	uint8_t* dst_first = dst_data + (cursor & mask);
	if (until_next_wrap_around >= num_bytes) {
		memcpy(dst_first, src_data, num_bytes);
	} else {
		memcpy(dst_first, src_data, until_next_wrap_around);
		const size_t remaining = num_bytes - until_next_wrap_around;
		assert((until_next_wrap_around + remaining) == num_bytes);
		assert((remaining>0) && "expected to write at least 1 byte");
		memcpy(dst_data, src_data + until_next_wrap_around, remaining);
	}
	rb->write_cursor = cursor + num_bytes;
}

static void document_copy(struct document* dst, struct document* src)
{
	struct document tmp;
	memcpy(&tmp, src, sizeof *src);
	memcpy(dst, src, sizeof *src);
	dst->fat_chars = tmp.fat_chars;
	daCopy(dst->fat_chars, src->fat_chars);
}

static void mim_state_copy(struct mim_state* dst, struct mim_state* src)
{
	struct mim_state tmp;
	memcpy(&tmp, src, sizeof *src);
	memcpy(dst, src, sizeof *src);
	dst->carets = tmp.carets;
	daCopy(dst->carets, src->carets);
}

static int document_locate(struct document* doc, struct location loc)
{
	const int num_chars = daLen(doc->fat_chars);
	int line=1;
	int column=0;
	for (int i=0; i<num_chars; ++i) {
		++column;
		if ((loc.line < line) || (loc.line == line && loc.column <= column)) {
			return i;
		}
		struct fat_char* fc = daPtr(doc->fat_chars, i);
		if (fc->codepoint == '\n') {
			++line;
			column=0;
		}
	}
	return num_chars;
}

struct snapshot {
	size_t journal_cursor;
	DA(struct document, documents);
	DA(struct mim_state, mim_states);
};

static int snapshot_get_num_documents(struct snapshot* ss)
{
	return daLen(ss->documents);
}

struct document* snapshot_get_document_by_index(struct snapshot* ss, int index)
{
	return daPtr(ss->documents, index);
}

struct document* snapshot_find_document_by_id(struct snapshot* ss, int id)
{
	const int n = snapshot_get_num_documents(ss);
	for (int i=0; i<n; ++i) {
		struct document* doc = snapshot_get_document_by_index(ss, i);
		if (doc->id == id) return doc;
	}
	return NULL;
}

struct document* snapshot_get_document_by_id(struct snapshot* ss, int id)
{
	struct document* doc = snapshot_find_document_by_id(ss, id);
	assert((doc != NULL) && "document not found by id");
	return doc;
}

static struct mim_state* snapshot_get_mim_state_by_ids(struct snapshot* ss, int artist_id, int personal_id)
{
	const int n = daLen(ss->mim_states);
	for (int i=0; i<n; ++i) {
		struct mim_state* s = daPtr(ss->mim_states, i);
		if ((s->artist_id == artist_id) &&  (s->personal_id == personal_id)) {
			return s;
		}
	}
	assert(!"not found");
}

static struct mim_state* snapshot_get_personal_mim_state_by_id(struct snapshot* ss, int personal_id)
{
	return snapshot_get_mim_state_by_ids(ss, get_my_artist_id(), personal_id);
}

static struct {
	struct ringbuf command_ringbuf;
	int document_id_sequence;
	struct snapshot cool_snapshot, hot_snapshot;
	struct ringbuf journal_ringbuf;
	int my_artist_id;
	DA(uint8_t, mim_buffer);
	int using_personal_mim_state_id;
	int in_mim;
} g;

static void snapshot_spool(struct snapshot* ss, struct ringbuf* journal)
{
	// TODO?
}

void gig_spool(void)
{
	snapshot_spool(&g.hot_snapshot, &g.journal_ringbuf);
}

void gig_thread_tick(void)
{
	// TODO?
}

void gig_init(void)
{
	ringbuf_init(&g.command_ringbuf, 16);

	// XXX "getting started"-stuff here:
	g.my_artist_id = 1;
	const int doc_id = 1;
	struct snapshot* ss = &g.cool_snapshot;
	struct document* doc = daAddNPtr(ss->documents, 1);
	memset(doc, 0, sizeof *doc);
	doc->id = doc_id;
	struct mim_state ms1 = {
		.artist_id = get_my_artist_id(),
		.personal_id = 1,
		.document_id = doc_id,
	};
	struct caret cr = {
		.tag=0,
		.range={
			.from={.line=1,.column=1},
			.to={.line=1,.column=1},
		},
	};
	daPut(ms1.carets, cr);
	daPut(ss->mim_states, ms1);


	#if 0
	gig_thread_tick();
	gig_spool();
	#endif
}

int get_my_artist_id(void)
{
	return g.my_artist_id;
}

void get_state_and_doc(int personal_id, struct mim_state** out_mim_state, struct document** out_doc)
{
	struct snapshot* ss = &g.cool_snapshot;
	const int artist_id = get_my_artist_id();
	const int num_states = daLen(ss->mim_states);
	for (int i=0; i<num_states; ++i) {
		struct mim_state* ms = daPtr(ss->mim_states, i);
		if ((ms->artist_id==artist_id) && (ms->personal_id==personal_id)) {
			const int document_id = ms->document_id;
			assert((document_id > 0) && "invalid document id in mim state");
			struct document* doc = NULL;
			const int num_docs = daLen(ss->documents);
			for (int i=0; i<num_docs; ++i) {
				struct document* d = daPtr(ss->documents, i);
				if (d->id == document_id) {
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

#if 0
int mim_state_cool_compar(struct mim_state_cool* a, struct mim_state_cool* b)
{
	const int d0 = a->document_id - b->document_id;
	if (d0!=0) return d0;
	const int d1 = (int)a->mode - (int)b->mode;
	if (d1!=0) return d1;
	const int n = arrlen(a->query_arr);
	const int d2 = n - arrlen(b->query_arr);
	if (d2!=0) return d2;
	if (n>0) {
		const int d3 = memcmp(a->query_arr, b->query_arr, n);
		if (d3!=0) return d3;
	}
	if (arrlen(a->caret_id_arr)>0 || arrlen(b->caret_id_arr)>0) {
		assert(!"TODO");
	}
	return 0;
}
#endif

FORMATPRINTF1
static void mimerr(const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	char msg[1<<12];
	stbsp_vsnprintf(msg, sizeof msg, fmt, va);
	va_end(va);
	fprintf(stderr, "MIMERR :: [%s]\n", msg);
}

static int mim_chew(struct mim_state* ms, struct document* doc, const uint8_t* input, int num_bytes)
{
	assert((ms->document_id == doc->id) && "mim state / document mismatch");

	const int num_carets = daLen(ms->carets);

	const char* p = (const char*)input;
	int remaining = num_bytes;

	static DA(int, number_stack) = {0};
	daReset(number_stack);

	enum {
		INIT=1,
		NUMBER=2,
		INSERT_STRING=3,
	};

	enum {
		ESCAPE='\033',
	};

	int mode = INIT;
	int previous_mode = -1;
	int number=0, number_sign=0;
	int push_cp = -1;
	int arg_tag = -1;

	while ((push_cp>=0) || (remaining>0)) {
		const int cp = (push_cp>=0) ? push_cp : utf8_decode(&p, &remaining);
		push_cp = -1;
		if (cp == -1) {
			mimerr("invalid UTF-8 input");
			return 0;
		}
		const int num_args = daLen(number_stack);

		switch (mode) {

		case INIT: {
			if (is_digit(cp) || cp=='-') {
				previous_mode = mode;
				mode = NUMBER;
				number = 0;
				if (cp == '-') {
					number_sign = -1;
				} else {
					number_sign = 1;
					push_cp = cp;
				}
			} else {
				switch (cp) {

				case 'c': {
					if (num_args != 3) {
						mimerr("command 'c' expected 3 arguments; got %d", num_args);
						return 0;
					}
					assert(!"TODO c");
					daReset(number_stack);
				}	break;

				case 'C': {
					if (num_args != 2) {
						mimerr("command 'C' expected 2 arguments; got %d", num_args);
						return 0;
					}
					assert(!"TODO C");
					daReset(number_stack);
				}	break;

				case 'i': {
					if (num_args != 1) {
						mimerr("command 'i' expected 1 argument; got %d", num_args);
						return 0;
					}
					arg_tag = daGet(number_stack, 0);
					daReset(number_stack);

					previous_mode = mode;
					mode = INSERT_STRING;
				}	break;

				default:
					mimerr("invalid command '%c'/%d", cp, cp);
					return 0;

				}
			}
		}	break;

		case NUMBER: {
			if (is_digit(cp)) {
				number = ((10*number) + (cp-'0'));
			} else {
				number *= number_sign;
				daPut(number_stack, number);
				mode = previous_mode;
				if (cp != ',') push_cp = cp;
			}
		}	break;

		case INSERT_STRING: {

			if (cp == ESCAPE) {
				daReset(number_stack);
				assert(previous_mode == INIT);
				mode = previous_mode;
			} else {
				if (cp < ' ') {
					switch (cp) {
					case '\n':
					case '\t': // XXX consider not supporting tabs?
						// accepted control codes
						break;
					default:
						mimerr("invalid control code %d in string", cp);
						return 0;
					}
				}
				for (int i=0; i<num_carets; ++i) {
					struct caret* car = daPtr(ms->carets, i);
					if (car->tag != arg_tag) continue;
					const int off = document_locate(doc, car->range.to);
					daIns(doc->fat_chars, off, ((struct fat_char){
						.codepoint = cp,
						.timestamp = get_nanoseconds(),
						.artist_id = get_my_artist_id(),
					}));
					++car->range.to.column;
					car->range.from = car->range.to;
				}
			}
		}	break;

		default: assert(!"unhandled mim_chew()-mode");

		}
	}

	if (mode != INIT) {
		mimerr("mode (%d) not terminated", mode);
		return 0;
	}

	const int num_args = daLen(number_stack);
	if (num_args > 0) {
		mimerr("non-empty number stack (n=%d) at end of mim-input", num_args);
		return 0;
	}

	return 1;
}

void begin_mim(int personal_mim_state_id)
{
	assert(!g.in_mim);
	g.in_mim = 1;
	g.using_personal_mim_state_id = personal_mim_state_id;
	daReset(g.mim_buffer);
}

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

void end_mim(void)
{
	assert(g.in_mim);
	const int num_bytes = daLen(g.mim_buffer);
	if (num_bytes>0) {
		uint8_t* data = g.mim_buffer.items;

		struct snapshot* ss = &g.cool_snapshot;
		struct mim_state* msr = snapshot_get_personal_mim_state_by_id(ss, g.using_personal_mim_state_id);
		struct document* doc = snapshot_get_document_by_id(ss, msr->document_id);

		// make "scratch copies" of state and doc and work on these; this
		// protects the actual state in case of partially invalid input
		static struct mim_state scratch_state = {0};
		static struct document scratch_doc = {0};
		mim_state_copy(&scratch_state, msr);
		document_copy(&scratch_doc, doc);

		if (!mim_chew(&scratch_state, &scratch_doc, data, num_bytes)) {
			assert(!"mim protocol error"); // XXX? should I have a "failable" flag? eg. for human input
		}

		mim_state_copy(msr, &scratch_state);
		document_copy(doc, &scratch_doc);

		uint8_t header[1<<8];
		uint8_t* hp = header;
		uint8_t* end = header + sizeof(header);
		write_varint64(&hp, end, g.using_personal_mim_state_id);
		write_varint64(&hp, end, num_bytes);
		const size_t nh = hp - header;
		ringbuf_writen(&g.command_ringbuf, header, nh);
		ringbuf_writen(&g.command_ringbuf, data, num_bytes);
		ringbuf_commit(&g.command_ringbuf);
	}
	g.in_mim = 0;
}

static char* get_mim_buffer_top(void)
{
	daSetMinCap(g.mim_buffer, daLen(g.mim_buffer) + STB_SPRINTF_MIN);
	return (char*)g.mim_buffer.items + daLen(g.mim_buffer);
}

static char* wrote_mim_cb(const char* buf, void* user, int len)
{
	daSetLen(g.mim_buffer, daLen(g.mim_buffer)+len);
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
	uint8_t* p = (uint8_t*)get_mim_buffer_top();
	*p = v;
	daSetLen(g.mim_buffer, daLen(g.mim_buffer)+1);
}

void gig_selftest(void)
{
	{ // test ringbuf_writen()
		struct ringbuf rb;
		uint8_t buf[1<<4] = {0};
		ringbuf_init_with_pointer(&rb, buf, 4);
		const char* p0 = "0123456";
		const size_t s0 = strlen(p0);
		const char* p1 = "xy";
		const size_t s1 = strlen(p1);

		// write p0 twice
		ringbuf_writen(&rb, p0, s0);
		assert(rb.write_cursor == (s0));
		ringbuf_writen(&rb, p0, s0);
		assert(rb.write_cursor == (s0+s0));

		// write exactly to the end of the ring buffer wrap around point: there
		// should be an assert that we don't do a zero-byte memcpy (i.e. that
		// we mistakenly split the write into two memcpy's)
		ringbuf_writen(&rb, p1, s1);
		assert(rb.write_cursor == (s0+s0+s1));
		assert((rb.write_cursor & ((1L<<rb.size_log2)-1L)) == 0L);

		// verify data, expect 2 × p0, 1 × p1
		char* p = (char*)rb.data;
		const char* expected = "01234560123456xy";
		assert(strlen(expected) == 16);
		for (int i=0; i<strlen(expected); ++i) assert(*(p++) == expected[i]);

		// write p0 again to advance write cursor
		ringbuf_writen(&rb, p0, s0);
		assert(rb.write_cursor == (s0+s0+s1+s0));

		// write across wrap-around point (should be 2 memcpy's)
		const char* p2 = "abcdefghijk";
		const size_t s2 = strlen(p2);
		ringbuf_writen(&rb, p2, s2);
		assert(rb.write_cursor == (s0+s0+s1+s0+s2));

		// verify final data
		expected = "jk23456abcdefghi";
		assert(strlen(expected) == 16);
		p = (char*)rb.data;
		for (int i=0; i<strlen(expected); ++i) assert(*(p++) == expected[i]);

		// test commit
		assert(rb.commit_cursor == 0);
		ringbuf_commit(&rb);
		assert(rb.commit_cursor == rb.write_cursor);
	}

	#if 0
	{ // test mimf_raw
		uint8_t buf[(1<<9) + STB_SPRINTF_MIN];
		assert(sizeof(buf) == 1024);
		uint8_t* p;

		{
			ringbuf_init_with_pointer(&g.command_ringbuf, buf, 9, STB_SPRINTF_MIN);
			mimf_raw("x=%d", 69);
			p = buf;
			assert(*(p++) == 4); // string length
			assert(*(p++) == 'x'); assert(*(p++) == '=');
			assert(*(p++) == '6'); assert(*(p++) == '9');
		}

		{ // test special case when string is 64 bytes or longer (length is varint)
			ringbuf_init_with_pointer(&g.command_ringbuf, buf, 9, STB_SPRINTF_MIN);
			mimf_raw(
			"0123456789ABCDEF"
			"0123456789ABCDEF"
			"0123456789ABCDEF"
			"0123456789ABCDEF"
			"!"
			);
			p = buf;
			assert(*(p++) == 0xc1); assert(*(p++) == 0x00); // 65 (string length) as leb128
			for (int i=0;i<4;++i) {
				for (int ii=0;ii<16;++ii) {
					assert(*(p++) == "0123456789ABCDEF"[ii]);
				}
			}
			assert(*p == '!');
		}
	}
	#endif
}
