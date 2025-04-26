#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stdio.h> // XXX

#include "stb_ds.h"
#include "stb_sprintf.h"

#include "gig.h"
#include "util.h"
#include "leb128.h"
#include "utf8.h"
#include "main.h"

struct ringbuf {
	uint8_t* data;
	int size_log2;
	int spillover_size;
	unsigned no_acknowledge :1;
	unsigned no_commit      :1;

	// fields on separate cache lines to avoid "false sharing" (hope I'm not
	// cargo culting too much)

	_Alignas(CACHE_LINE_SIZE)         int     read_error;
	_Alignas(CACHE_LINE_SIZE)         size_t  read_cursor;
	_Alignas(CACHE_LINE_SIZE) _Atomic(size_t) acknowledge_cursor;

	_Alignas(CACHE_LINE_SIZE)         int     write_error;
	_Alignas(CACHE_LINE_SIZE)         size_t  write_cursor;
	_Alignas(CACHE_LINE_SIZE) _Atomic(size_t) commit_cursor;
};

static void ringbuf_init_with_pointer(struct ringbuf* rb, void* data, int size_log2, int spillover_size)
{
	memset(rb, 0, sizeof *rb);
	assert((spillover_size <= (1L << size_log2)) && "spillover doesn't work if larger than actual ringbuf");
	rb->size_log2 = size_log2;
	rb->spillover_size = spillover_size;
	rb->data = data;
}

static size_t ringbuf_get_data_size(int size_log2, int spillover_size)
{
	return (1L << size_log2) + spillover_size;
}

static void ringbuf_init(struct ringbuf* rb, int size_log2, int spillover_size)
{
	ringbuf_init_with_pointer(
		rb,
		calloc(ringbuf_get_data_size(size_log2, spillover_size), 1),
		size_log2,
		spillover_size);
}

static inline void* ringbuf_get_write_pointer_at(struct ringbuf* rb, size_t position)
{
	return &rb->data[position & ((1L << rb->size_log2)-1)];
}

static inline void* ringbuf_get_write_pointer(struct ringbuf* rb)
{
	return ringbuf_get_write_pointer_at(rb, rb->write_cursor);
}

// advance write cursor and handle spillover. it assumes that you wrote
// num_bytes beginning at ringbuf_get_write_pointer(), and handles spillover at
// the end of the buffer. thus num_bytes must never exceed rb->spillover_size.
static void ringbuf_wrote_linear(struct ringbuf* rb, size_t num_bytes)
{
	if (num_bytes == 0) return;
	if (rb->spillover_size == 0) {
		assert((num_bytes == 1) && "no spillover configured: you can write at most 1 byte linearly at a time");
		return;
	}
	assert((num_bytes <= rb->spillover_size) && "you can't write more than spillover_size");
	// XXX I think you can actually write 1+rb->spillover_size bytes? but that
	// might be a problem if ringbuf size is exactly the same as the spillover
	// size? (overlapping memcpy()?). besides, setting spillover size to the
	// maximum number of bytes you can write is easier to understand, maybe?
	const size_t c0 = rb->write_cursor;
	rb->write_cursor += num_bytes;
	const size_t c1 = rb->write_cursor;
	const int size_log2 = rb->size_log2;
	if (((c1-1) >> size_log2) > (c0 >> size_log2)) {
		// wrote past end of buffer; do "spillover" by copying back to
		// beginning of buffer
		const size_t size = 1L << size_log2;
		const size_t n = c1 & (size-1);
		assert(n>0);
		memcpy(rb->data, rb->data + size, n);
	}
}

// copies everything except the data itself; used for having a different "view"
// with its own read/write state
static void ringbuf_shallow_copy(struct ringbuf* dst, struct ringbuf* src)
{
	memcpy(dst, src, sizeof *dst);
	atomic_store(&dst->acknowledge_cursor, atomic_load(&src->acknowledge_cursor));
	atomic_store(&dst->commit_cursor,      atomic_load(&src->commit_cursor));
}

static void ringbuf_write_u8(struct ringbuf* rb, uint8_t v)
{
	if (rb->write_error) return;
	const size_t s = (1L << rb->size_log2);
	if (!rb->no_acknowledge && (rb->write_cursor >= (atomic_load(&rb->acknowledge_cursor) + s))) {
		printf("WRITE ERROR\n"); // XXX
		rb->write_error = 1;
		return;
	}
	*(uint8_t*)ringbuf_get_write_pointer(rb) = v;
	ringbuf_wrote_linear(rb, 1);
}

static inline uint8_t ringbuf_read_u8_at(struct ringbuf* r, size_t position)
{
	//printf("read %zd => %d\n", position, r->data[position & ((1L << r->size_log2)-1)]);
	return r->data[position & ((1L << r->size_log2)-1)];
}

static uint8_t ringbuf_read_u8(struct ringbuf* r)
{
	if (r->read_error) return 0;
	if (!r->no_commit && (r->read_cursor >= atomic_load(&r->commit_cursor))) {
		printf("READ ERROR\n"); // XXX
		r->read_error = 1;
		return 0;
	}
	return ringbuf_read_u8_at(r, r->read_cursor++);
}

static void ringbuf_rollback(struct ringbuf* r)
{
	if (r->write_error) return;
	r->write_cursor = r->commit_cursor;
}

static void ringbuf_commit(struct ringbuf* r)
{
	assert((!r->no_commit) && "commit is disabled");
	if (r->write_error) return;
	atomic_store(&r->commit_cursor, r->write_cursor);
}

static void ringbuf_acknowledge(struct ringbuf* r)
{
	assert((!r->no_acknowledge) && "acknowledge is disabled");
	if (r->read_error) return;
	atomic_store(&r->acknowledge_cursor, r->read_cursor);
}

static void document_copy(struct document* dst, struct document* src)
{
	const struct document copy = *dst;
	memcpy(dst, src, sizeof *dst);

	// the arrays are handled differently, reusing the "_arr"s in the dst
	// document (simplifies memory management)
	dst->fat_char_arr = copy.fat_char_arr;
	//dst->caret_arr = copy.caret_arr;
	dst->mim_state_arr = copy.mim_state_arr;

	ARRCOPY(dst->fat_char_arr, src->fat_char_arr);
	ARRCOPY(dst->mim_state_arr, src->mim_state_arr);
}

struct codec {
	struct ringbuf* ringbuf;
	int num_commands;
	size_t began_at_position;
	unsigned is_encoding : 1;
	unsigned is_decoding : 1;
	unsigned in_journal_commands : 1;
};

struct snapshot {
	size_t journal_cursor;
	struct document* document_arr;
};

static int snapshot_get_num_documents(struct snapshot* ss)
{
	return arrlen(ss->document_arr);
}

struct document* snapshot_get_document_by_index(struct snapshot* ss, int index)
{
	assert((0 <= index) && (index < get_num_documents()));
	return &ss->document_arr[index];
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

static struct {
	struct ringbuf command_ringbuf;

	int document_id_sequence;

	struct snapshot outside, inside;
	//struct document scratch_doc;
	struct ringbuf journal_ringbuf;

	struct codec ed_codec;
	int ed_num_commands;
	size_t ed_began_at_write_position;
	int ed_is_begun;
	int my_artist_id;

	struct mim_state vs1;
	struct mim_state_cool vs1cool;
} g;

int get_num_documents(void)
{
	return snapshot_get_num_documents(&g.outside);
}

struct document* get_document_by_index(int index)
{
	return snapshot_get_document_by_index(&g.outside, index);
}

struct document* find_document_by_id(int id)
{
	assert((id >= 1) && "id must be at least 1");
	return snapshot_find_document_by_id(&g.outside, id);
}

struct document* get_document_by_id(int id)
{
	struct document* doc = find_document_by_id(id);
	assert((doc != NULL) && "document id not found");
	return doc;
}

static void codec_sanity_check(struct codec* c)
{
	assert(!(c->is_encoding && c->is_decoding) && "both cannot be true");
}

static int is_encoding(struct codec* c)
{
	codec_sanity_check(c);
	return c->is_encoding;
}

static int is_decoding(struct codec* c)
{
	codec_sanity_check(c);
	return c->is_decoding;
}

static int is_coding(struct codec* c)
{
	return is_encoding(c) || is_decoding(c);
}

static void codec_begin_encode(struct codec* c, struct ringbuf* r)
{
	assert(!is_coding(c));
	assert(c->ringbuf == NULL);
	c->ringbuf = r;
	c->began_at_position = r->write_cursor;
	// TODO remember document revision we're editing against?
	c->is_encoding = 1;
}

static void codec_end_encode(struct codec* c)
{
	assert(is_encoding(c));
	assert(!c->in_journal_commands);
	assert(c->ringbuf != NULL);
	c->ringbuf = NULL;
	c->is_encoding = 0;
}

static void codec_begin_decode(struct codec* c, struct ringbuf* r)
{
	assert(!is_coding(c));
	assert(c->ringbuf == NULL);
	c->ringbuf = r;
	c->began_at_position = r->write_cursor;
	c->is_decoding = 1;
}

static void codec_end_decode(struct codec* c)
{
	assert(is_decoding(c));
	assert(c->ringbuf != NULL);
	c->ringbuf = NULL;
	c->is_decoding = 0;
}

// codec functions: designed so they can be used both for encoding and decoding
//
// _ptr suffix means it uses a pointer:
//    codec_foo_ptr(&foo)
//
// _io suffix means you should "connect" both input and output:
//    x->foo = codec_foo_io(x->foo);
// please mark _io functions with "NO_DISCARD" (causes compiler to complain if
// you forget to use the return value). also consider adding a CODEC_FOO()
// macro that does it for you.

static uint8_t codec_read_u8(struct codec* c)
{
	assert(is_decoding(c));
	assert(c->ringbuf != NULL);
	return ringbuf_read_u8(c->ringbuf);
}

static uint8_t codec_read_u8_wrapper(void* userdata)
{
	return codec_read_u8((struct codec*)userdata);
}

static void codec_write_u8(struct codec* c, uint8_t v)
{
	assert(is_encoding(c));
	assert(c->ringbuf != NULL);
	ringbuf_write_u8(c->ringbuf, v);
}

static void codec_write_u8_wrapper(uint8_t v, void* userdata)
{
	codec_write_u8((struct codec*)userdata, v);
}

static inline void codec_write_varint(struct codec* c, int v)
{
	leb128_encode_int(codec_write_u8_wrapper, v, (void*)c);
}

static inline void codec_write_varint64(struct codec* c, int64_t v)
{
	leb128_encode_int64(codec_write_u8_wrapper, v, (void*)c);
}

static inline int codec_read_varint(struct codec* c)
{
	return leb128_decode_int(codec_read_u8_wrapper, (void*)c);
}

static inline int64_t codec_read_varint64(struct codec* c)
{
	return leb128_decode_int64(codec_read_u8_wrapper, (void*)c);
}

NO_DISCARD
static int codec_varint_io(struct codec* c, int v)
{
	if (is_encoding(c)) {
		codec_write_varint(c, v);
		return v;
	} else if (is_decoding(c)) {
		(void)v;
		return codec_read_varint(c);
	}
	assert(!"bad state");
}
#define CODEC_VARINT(c,V) (V)=codec_varint_io(c,V)

NO_DISCARD
static int64_t codec_varint64_io(struct codec* c, int64_t v)
{
	if (is_encoding(c)) {
		codec_write_varint64(c, v);
		return v;
	} else if (is_decoding(c)) {
		(void)v;
		return codec_read_varint64(c);
	}
	assert(!"bad state");
}
#define CODEC_VARINT64(c,V) (V)=codec_varint64_io(c,V)

NO_DISCARD
static uint16_t codec_uint16_io(struct codec* c, uint16_t v)
{
	if (is_encoding(c)) {
		codec_write_u8(c, v & 0xff);
		codec_write_u8(c, (v>>8) & 0xff);
		return v;
	} else if (is_decoding(c)) {
		(void)v;
		uint8_t b0 = codec_read_u8(c);
		uint8_t b1 = codec_read_u8(c);
		return (uint16_t)b0 + ((uint16_t)b1 << 8);
	}
	assert(!"bad state");
}
#define CODEC_UINT16(c,V) (V)=codec_uint16_io(c,V)

// defining `DEBUG_PROTOCOL` means that extra dummy data is added to the
// protocol, and read back in order to test it. this makes it incompatible with
// data written by a non-DEBUG_PROTOCOL executable! so it should only be used
// for debugging.
#ifdef DEBUG_PROTOCOL
static void codec_debug_tracer(struct codec* c, int tracer)
{
	// write `tracer` when encoding, assert that we read `tracer` back when
	// decoding.
	const int tracer2 = codec_varint_io(c, tracer);
	if (is_decoding(c) && tracer2 != tracer) {
		fprintf(stderr, "codec_debug_tracer() fail: expected to read %d but read %d", tracer, tracer2);
		abort();
	}
}
#else
static void codec_debug_tracer(struct codec* c, int tracer)
{
	// no-op
}
#endif

static inline int s2i(size_t x)
{
	assert((x <= INT_MAX) && "size to int conversion failed: size too large");
	return (int)x;
}

static void codec_cstr_ptr(struct codec* c, const char** p)
{
	if (is_encoding(c)) {
		const int n = s2i(strlen(*p));
		codec_write_varint(c, n);
		for (int i=0; i<n; ++i) codec_write_u8(c, (*p)[i]);
	} else if (is_decoding(c)) {
		const int n = codec_read_varint(c);
		char* s = malloc(n+1);
		// XXX ^^^ do something smarter that malloc'ing here (and leaking
		// likely)? note that nul-terminating the string in the ringbuf and
		// pointing into it is a baad idea, far too smart :)
		for (int i=0; i<n; ++i) s[i] = codec_read_u8(c);
		s[n] = 0;
		*p = s;
	} else {
		assert(!"bad state");
	}
}

static void codec_location_ptr(struct codec* c, struct location* l)
{
	CODEC_VARINT(c, l->line);
	CODEC_VARINT(c, l->column);
}

static void codec_range_ptr(struct codec* c, struct range* r)
{
	codec_location_ptr(c, &r->from);
	codec_location_ptr(c, &r->to);
}

static void snapshot_spool(struct snapshot* ss, struct ringbuf* journal)
{
}

void gig_spool(void)
{
	snapshot_spool(&g.outside, &g.journal_ringbuf);
}

void gig_thread_tick(void)
{
}

void gig_init(void)
{
	ringbuf_init(&g.command_ringbuf, 16, STB_SPRINTF_MIN);

	g.my_artist_id = 1;
	// XXX this is probably correct if we're host of the venue, but otherwise
	// we need an assigned artist id

	struct document* doc = arraddnptr(g.outside.document_arr, 1);
	memset(doc, 0, sizeof *doc);
	doc->id = 1;

	g.vs1.cool.document_id = doc->id;
	g.vs1.hot.document_id = doc->id;
	g.vs1.cool.mode = MIM_COMMAND;
	memcpy(&g.vs1cool, &g.vs1.cool, sizeof g.vs1cool);

	gig_thread_tick();
	gig_spool();
}

int get_my_artist_id(void)
{
	return g.my_artist_id;
}

struct mim_state* get_mim_state_by_id(int id)
{
	assert((id==1) && "oh no");
	return &g.vs1;
}

struct mim_state_cool* get_own_cool_mim_state_by_id(int id)
{
	assert((id==1) && "oh no");
	return &g.vs1cool;
}

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

static void mim_chew(struct mim_state_cool* cool, struct mim_state_hot* hot, uint8_t input, int* hot_update)
{
	const int is_escape = ('\033'==input);
	const int is_return = ('\n'==input);

	if (cool != NULL) {
		switch (cool->mode) {
		case MIM_COMMAND: {
			switch (input) {
			case ':': cool->mode = MIM_EX_COMMAND; break;
			case '/': cool->mode = MIM_SEARCH_FORWARD; break;
			case '?': cool->mode = MIM_SEARCH_BACKWARD; break;

			case 'h':
			case 'j':
			case 'k':
			case 'l':
				printf("mim_chew(): TODO caret movement (%c)\n", input);
				break;

			case 'i':
			case 'I':
			case 'a':
			case 'A':
			case 'o':
			case 'O':
				// XXX how to handle caret movements and line insertions here?
				if (hot_update) *hot_update=1;
				cool->mode = MIM_INSERT;
				break;

			//default: assert(!"unhandled command"); // XXX harsh?
			default:
				printf("mim_chew(): unhandled input [%c/%d]\n", input, input);
				break;
			}
		}	break;
		case MIM_VISUAL:
		case MIM_VISUAL_LINE:
		//MIM_VISUAL_BLOCK?
			assert(!"TODO visual command");
		case MIM_EX_COMMAND:
		case MIM_SEARCH_FORWARD:
		case MIM_SEARCH_BACKWARD: {
			if (is_escape) {
				arrsetlen(cool->query_arr, 0);
				cool->mode = MIM_COMMAND;
			} else if (is_return) {
				arrput(cool->query_arr, 0);
				char* q = cool->query_arr;
				if (cool->mode == MIM_EX_COMMAND) {
					char* q1;
					for (q1=q; *q1 && *q1!=' '; ++q1) {}
					*q1=0;
					if (strcmp("document",q)==0) {
						const long document_id = strtol(q1+1, NULL, 10);
						cool->document_id = document_id;
					} else {
						printf("mim_chew(): unhandled ex command [%s]\n", q);
					}
				} else {
					assert(!"TODO search");
				}
				cool->mode = MIM_COMMAND;
			} else {
				arrput(cool->query_arr, input);
			}
		}	break;
		case MIM_INSERT:
			if (is_escape) {
				cool->mode = MIM_COMMAND;
			} else {
				if (hot_update) *hot_update=1;
			}
			break;
		default: assert(!"unhandled mim mode");
		}
	}

	assert((hot == NULL) && "unhandled");
}

static char* wrote_command_cb(const char* buf, void* user, int len)
{
	struct ringbuf* rb = &g.command_ringbuf;
	ringbuf_wrote_linear(rb, len);
	return ringbuf_get_write_pointer(rb);
}

static void mimf_raw_va(int mim_state_id, const char* fmt, va_list va0)
{
	assert(mim_state_id >= 1);
	struct codec c = {0};
	struct ringbuf* rb = &g.command_ringbuf;
	codec_begin_encode(&c, rb);
	codec_write_varint(&c, mim_state_id);
	const size_t w0 = rb->write_cursor;
	codec_write_varint(&c, -1); // size placeholder
	const size_t w1 = rb->write_cursor;
	assert(w1 == (w0+1));
	va_list va;
	va_copy(va, va0);
	stbsp_vsprintfcb(wrote_command_cb, NULL, ringbuf_get_write_pointer(rb), fmt, va);
	const size_t w2 = rb->write_cursor;
	const size_t print_size = w2-w1;
	rb->write_cursor = w0; // reset and write size
	codec_write_varint64(&c, print_size);
	const size_t w1b = rb->write_cursor;
	if ((w1b-w0) > 1) {
		// oops, 1 byte wasn't enough for the varint length. print again from
		// the correct position
		va_copy(va, va0);
		stbsp_vsprintfcb(wrote_command_cb, NULL, ringbuf_get_write_pointer(rb), fmt, va);
		assert(((rb->write_cursor - w1b) == print_size) && "expected to print same length again");
	} else {
		assert((w1b-w0) == 1);
		rb->write_cursor = w2; // restore write cursor
	}
	codec_end_encode(&c);
}

static void mimf_raw(int mim_state_id, const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	mimf_raw_va(mim_state_id, fmt, va);
	va_end(va);
}

void mimf(int mim_state_id, const char* fmt, ...)
{
	struct ringbuf* rb = &g.command_ringbuf;
	struct ringbuf rbr;
	ringbuf_shallow_copy(&rbr, rb);
	const size_t w0 = rb->write_cursor;
	va_list va;
	va_start(va, fmt);
	mimf_raw_va(mim_state_id, fmt, va);
	va_end(va);
	rbr.read_cursor = w0;
	rbr.no_commit = 1;
	struct codec c={0};
	codec_begin_decode(&c, &rbr);
	const int read_state_id = codec_read_varint(&c);
	assert(read_state_id==mim_state_id);
	const int ss = codec_read_varint(&c);
	int hot_update=0;
	for (int i=0; i<ss; ++i) {
		const uint8_t v = codec_read_u8(&c);
		mim_chew(&g.vs1cool, NULL, v, &hot_update);
	}
	codec_end_decode(&c);
	// TODO detect if command is discardable? e.g. ":document 1<cr>" when
	// already on document 1 and so on?
}

void gig_selftest(void)
{
	{ // test ringbuf spillover
		struct ringbuf rb;
		uint8_t buf[1<<8];
		memset(buf, 0, sizeof buf);
		ringbuf_init_with_pointer(&rb, buf, /*size_log2=*/4, /*spillover_size=*/8);
		// advance 12 bytes (can't advance more than `spillover_size`)
		ringbuf_wrote_linear(&rb, 8);
		ringbuf_wrote_linear(&rb, 4);
		assert(ringbuf_get_write_pointer(&rb) == buf+12);
		// write 8 bytes, 4 bytes past end of ringbuf main area, and expect 4
		// bytes spillover
		uint8_t* p = ringbuf_get_write_pointer(&rb);
		for (int i=0; i<8; ++i) p[i] = 3+7*i;
		ringbuf_wrote_linear(&rb, 8);
		for (int i=0; i<24; ++i) {
			assert(buf[i] ==
				(
				i<4  ? 3+7*(i+4) : // spillover copied to start
				i<12 ? 0 : // not written (zero initialized)
				i<20 ? 3+7*(i-12) : // 4 last bytes + 4 bytes spillover
				0 // not written (zero initialized)
				)
			);
		}
		assert(((uint8_t*)ringbuf_get_write_pointer(&rb) < p) && "expected pointer to move backwards due to wrap-around");
		assert(ringbuf_get_write_pointer(&rb) == buf+4);
	}

	{ // test mimf_raw
		uint8_t buf[(1<<9) + STB_SPRINTF_MIN];
		assert(sizeof(buf) == 1024);
		uint8_t* p;

		{
			ringbuf_init_with_pointer(&g.command_ringbuf, buf, 9, STB_SPRINTF_MIN);
			mimf_raw(42, "x=%d", 69);
			p = buf;
			assert(*(p++) == 42); // mim state id
			assert(*(p++) == 4); // string length
			assert(*(p++) == 'x'); assert(*(p++) == '=');
			assert(*(p++) == '6'); assert(*(p++) == '9');
		}

		{ // test special case when string is 64 bytes or longer (length is varint)
			ringbuf_init_with_pointer(&g.command_ringbuf, buf, 9, STB_SPRINTF_MIN);
			mimf_raw(66,
			"0123456789ABCDEF"
			"0123456789ABCDEF"
			"0123456789ABCDEF"
			"0123456789ABCDEF"
			"!"
			);
			p = buf;
			assert(*(p++) == 0xc2); assert(*(p++) == 0x00); // 66 (mim state id) as leb128
			assert(*(p++) == 0xc1); assert(*(p++) == 0x00); // 65 (string length) as leb128
			for (int i=0;i<4;++i) {
				for (int ii=0;ii<16;++ii) {
					assert(*(p++) == "0123456789ABCDEF"[ii]);
				}
			}
			assert(*p == '!');
		}
	}
}
