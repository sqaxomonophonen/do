#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stdio.h> // XXX

#include "stb_ds.h"
#include "gig.h"
#include "util.h"
#include "leb128.h"
#include "utf8.h"
#include "main.h"

struct ringbuf {
	uint8_t* data;
	int size_log2;
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

static void ringbuf_init(struct ringbuf* r, int size_log2)
{
	memset(r, 0, sizeof *r);
	r->size_log2 = size_log2;
	r->data = calloc(1L << size_log2, sizeof r->data[0]);
}

// copies everything except the data itself; used for having a different "view"
// with its own read/write state
static void ringbuf_shallow_copy(struct ringbuf* dst, struct ringbuf* src)
{
	memcpy(dst, src, sizeof *dst);
	atomic_store(&dst->acknowledge_cursor, atomic_load(&src->acknowledge_cursor));
	atomic_store(&dst->commit_cursor,      atomic_load(&src->commit_cursor));
}

static void ringbuf_write_u8(struct ringbuf* r, uint8_t v)
{
	if (r->write_error) return;
	const size_t s = (1L << r->size_log2);
	if (!r->no_acknowledge && (r->write_cursor >= (atomic_load(&r->acknowledge_cursor) + s))) {
		printf("WRITE ERROR\n"); // XXX
		r->write_error = 1;
		return;
	}
	r->data[r->write_cursor] = v;
	r->write_cursor = (1 + r->write_cursor) & (s-1);
}

static uint8_t ringbuf_read_u8_at(struct ringbuf* r, size_t position)
{
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
	const size_t at = r->read_cursor;
	++r->read_cursor;
	return ringbuf_read_u8_at(r, at);
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
	const int num_fat_chars = arrlen(src->fat_char_arr);
	arrsetlen(dst->fat_char_arr, num_fat_chars);
	memcpy(dst->fat_char_arr, src->fat_char_arr, num_fat_chars*sizeof(dst->fat_char_arr[0]));

	dst->caret_arr = copy.caret_arr;
	const int num_carets = arrlen(src->caret_arr);
	arrsetlen(dst->caret_arr, num_carets);
	memcpy(dst->caret_arr, src->caret_arr, num_carets*sizeof(dst->caret_arr[0]));
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

struct journal_commands {
	int artist_id;
	int document_id;
	uint16_t num_commands;
};

struct request_commands {
	int document_id;
	int version;
	int64_t not_before_timestamp; // used to simulate latency
	uint16_t num_commands;
};

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

static void codec_command_ptr(struct codec* c, struct command* cm)
{
	codec_debug_tracer(c, -1003);
	CODEC_VARINT(c, cm->type);
	switch (cm->type) {
	case COMMAND_SET_CARET:
		CODEC_VARINT(c, cm->set_caret.id);
		codec_range_ptr(c, &cm->set_caret.range);
		break;
	case COMMAND_DELETE_CARET:
		CODEC_VARINT(c, cm->delete_caret.id);
		break;
	case COMMAND_INSERT:
		codec_location_ptr(c, &cm->insert.at);
		codec_cstr_ptr(c, &cm->insert.text);
		break;
	case COMMAND_DELETE:
		codec_range_ptr(c, &cm->delete.range);
		break;
	case COMMAND_COMMIT:
		CODEC_VARINT(c, cm->commit.artist_id);
		codec_range_ptr(c, &cm->commit.range);
		break;
	case COMMAND_CANCEL:
		CODEC_VARINT(c, cm->cancel.artist_id);
		codec_range_ptr(c, &cm->cancel.range);
		break;
	case COMMAND_DEFER:
		CODEC_VARINT(c, cm->defer.artist_id);
		codec_range_ptr(c, &cm->defer.range);
		break;
	case COMMAND_UNDEFER:
		CODEC_VARINT(c, cm->undefer.artist_id);
		codec_range_ptr(c, &cm->undefer.range);
		break;
	case COMMAND_SET_COLOR:
		CODEC_VARINT(c, cm->set_color.red);
		CODEC_VARINT(c, cm->set_color.green);
		CODEC_VARINT(c, cm->set_color.blue);
		break;
	case COMMAND_CREATE_DOCUMENT:
		CODEC_VARINT(c, cm->create_document.id);
		CODEC_VARINT(c, cm->create_document.type);
		CODEC_VARINT(c, cm->create_document.flags);
		break;
	default: assert(!"unhandled command");
	}

	++c->num_commands;
}

static void codec_journal_commands_ptr(struct codec* c, struct journal_commands* jc)
{
	codec_debug_tracer(c, -1001);
	CODEC_VARINT(c, jc->artist_id);
	CODEC_VARINT(c, jc->document_id);
	CODEC_UINT16(c, jc->num_commands);
}

static void codec_request_commands_ptr(struct codec* c, struct request_commands* rc)
{
	codec_debug_tracer(c, -1002);
	CODEC_VARINT(c, rc->document_id);
	CODEC_VARINT(c, rc->version);
	CODEC_VARINT64(c, rc->not_before_timestamp);
	CODEC_UINT16(c, rc->num_commands);
}

static void codec_begin_encode_journal_commands(struct codec* c, struct ringbuf* rb, int artist_id, int document_id)
{
	assert(c->in_journal_commands == 0);
	c->began_at_position = rb->write_cursor;
	codec_begin_encode(c, rb);
	struct journal_commands jc = {
		.artist_id = artist_id,
		.document_id = document_id,
		.num_commands = 0xffff,
	};
	codec_journal_commands_ptr(c, &jc);
	c->num_commands = 0;
	c->in_journal_commands = 1;
}

static void codec_end_encode_journal_commands(struct codec* c)
{
	assert(c->in_journal_commands == 1);
	if (c->num_commands == 0) {
		// reset ringbuf
		c->ringbuf->write_cursor = c->began_at_position;
	} else {
		assert(c->num_commands > 0);

		struct ringbuf tmp_rb;
		ringbuf_shallow_copy(&tmp_rb, c->ringbuf);
		tmp_rb.no_commit = 1;

		tmp_rb.read_cursor = c->began_at_position;
		struct codec c2={0};
		struct journal_commands jc = {0};
		codec_begin_decode(&c2, &tmp_rb);
		codec_journal_commands_ptr(&c2, &jc);
		codec_end_decode(&c2);

		assert((jc.num_commands == 0xffff) && "expected temporary marker value 0xffff");
		jc.num_commands = c->num_commands;
		tmp_rb.write_cursor = c->began_at_position;
		codec_begin_encode(&c2, &tmp_rb);
		codec_journal_commands_ptr(&c2, &jc);
		codec_end_encode(&c2);
	}
	c->in_journal_commands = 0;
	codec_end_encode(c);
}

static void snapshot_spool(struct snapshot* ss, struct ringbuf* journal)
{
	const size_t commit_cursor = atomic_load(&journal->commit_cursor);
	struct ringbuf tmp_rb;
	ringbuf_shallow_copy(&tmp_rb, journal);
	tmp_rb.read_cursor = ss->journal_cursor;
	struct codec cc={0};
	codec_begin_decode(&cc, &tmp_rb);
	struct journal_commands jc;
	while (tmp_rb.read_cursor < commit_cursor) {
		codec_journal_commands_ptr(&cc, &jc);
		const int num_commands = jc.num_commands;
		const int artist_id = jc.artist_id;
		struct document* doc = NULL;
		if (jc.document_id > 0) {
			doc = snapshot_find_document_by_id(ss, jc.document_id);
		}
		for (int i=0; i<num_commands; ++i) {
			struct command cm;
			codec_command_ptr(&cc, &cm);
			switch (cm.type) {

			case COMMAND_CREATE_DOCUMENT: {
				// XXX this crash can be caused by requests over network:
				assert((jc.document_id == 0 && doc == NULL) && "journal_commands.document_id must be zero when creating docs");
				struct document* newdoc = arraddnptr(ss->document_arr, 1);
				memset(newdoc, 0, sizeof *newdoc);
				// XXX what to do with id collisions?
				newdoc->id = cm.create_document.id;
				newdoc->type = cm.create_document.type;
				newdoc->flags = cm.create_document.flags;
			}	break;

			case COMMAND_SET_CARET: {
				assert(doc != NULL); // XXX this crash can be caused by requests over network
				const int num_carets = arrlen(doc->caret_arr);
				struct caret* caret = NULL;
				for (int i=0; i<num_carets; ++i) {
					struct caret* cr = &doc->caret_arr[i];
					if (cr->id != cm.set_caret.id) continue;
					caret = cr;
					break;
				}
				if (caret == NULL) {
					caret = arraddnptr(doc->caret_arr, 1);
					memset(caret, 0, sizeof *caret);
					caret->id = cm.set_caret.id;
					caret->artist_id = artist_id;
				}
				assert(caret != NULL);
				caret->range = cm.set_caret.range;
			}	break;

			case COMMAND_INSERT: {
				assert(doc != NULL); // XXX this crash can be caused by requests over network
				const char* text_pointer = cm.insert.text;
				// XXX FIXME find insert position.
				// XXX FIXME allocate the proper number of codepoint upfront instead of adding them one by one
				int bytes_remaining = strlen(text_pointer);
				while (bytes_remaining > 0) {
					const int codepoint = utf8_decode(&text_pointer, &bytes_remaining);
					if (codepoint < 1) continue;
					arrput(doc->fat_char_arr, ((struct fat_char){
						.codepoint = codepoint,
						.artist_id = artist_id,
					}));
				}
			}	break;

			default: assert(!"unhandled command type");
			}
		}
	}
	codec_end_decode(&cc);
	ss->journal_cursor = tmp_rb.read_cursor;
}

void gig_spool(void)
{
	snapshot_spool(&g.outside, &g.journal_ringbuf);
}

void ed_begin(int document_id, int64_t add_latency_ns)
{
	assert(!g.ed_is_begun);

	const int64_t not_before_timestamp =
		(add_latency_ns > 0)
		? (get_nanoseconds() + add_latency_ns)
		: 0;

	struct ringbuf* rb = &g.command_ringbuf;
	g.ed_began_at_write_position = rb->write_cursor;
	struct document* d = get_document_by_id(document_id);
	struct codec* c = &g.ed_codec;
	codec_begin_encode(c, rb);
	struct request_commands rc = {
		.document_id = document_id,
		.version = d->version,
		.not_before_timestamp = not_before_timestamp,
		.num_commands = 0xffff, // is patched later
	};
	codec_request_commands_ptr(c, &rc);
	codec_end_encode(c);

	g.ed_num_commands = 0;
	g.ed_is_begun = 1;
}

void ed_do(struct command* cm)
{
	assert(g.ed_is_begun);
	struct codec* c = &g.ed_codec;
	codec_begin_encode(c, &g.command_ringbuf);
	codec_command_ptr(c, cm);
	codec_end_encode(c);
	++g.ed_num_commands;
}

void ed_end(void)
{
	assert(g.ed_is_begun);
	assert((0 <= g.ed_num_commands) && (g.ed_num_commands < 0xffff));

	if (g.ed_num_commands > 0) {
		struct codec* c = &g.ed_codec;

		struct ringbuf* rb = &g.command_ringbuf;
		struct ringbuf tmp_rb;
		ringbuf_shallow_copy(&tmp_rb, rb);
		tmp_rb.no_commit = 1;

		tmp_rb.read_cursor = g.ed_began_at_write_position;
		codec_begin_decode(c, &tmp_rb);
		struct request_commands q = {0};
		codec_request_commands_ptr(c, &q);
		codec_end_decode(c);

		// XXX FIXME the "0xffff" thing is also done for journal_commands:
		// handle with some common code instead?
		assert((q.num_commands == 0xffff) && "expected temporary marker value 0xffff");
		q.num_commands = g.ed_num_commands;

		tmp_rb.write_cursor = g.ed_began_at_write_position;
		codec_begin_encode(c, &tmp_rb);
		codec_request_commands_ptr(c, &q);
		codec_end_encode(c);

		ringbuf_commit(rb);
	} else {
		ringbuf_rollback(&g.command_ringbuf);
	}
	g.ed_is_begun = 0;
}

void gig_thread_tick(void)
{
	struct ringbuf* src = &g.command_ringbuf;
	struct ringbuf* dst = &g.journal_ringbuf;
	const size_t commit_cursor = atomic_load(&src->commit_cursor);
	struct codec csrc={0};
	codec_begin_decode(&csrc, src);
	const int64_t now = get_nanoseconds();
	while (src->read_cursor < commit_cursor) {
		const size_t save_read_cursor = src->read_cursor;
		struct request_commands rq;
		codec_request_commands_ptr(&csrc, &rq);

		if (rq.not_before_timestamp != 0) {
			if (now < rq.not_before_timestamp) {
				src->read_cursor = save_read_cursor;
				break;
			}
		}
		struct codec cdst={0};
		codec_begin_encode_journal_commands(&cdst, dst, g.my_artist_id, rq.document_id);
		const int num_commands = rq.num_commands;
		for (int i=0; i<num_commands; ++i) {
			struct command cm;
			codec_command_ptr(&csrc, &cm);
			codec_command_ptr(&cdst, &cm);
		}
		codec_end_encode_journal_commands(&cdst);
		ringbuf_acknowledge(src);
	}
	codec_end_decode(&csrc);
	ringbuf_commit(dst);
	snapshot_spool(&g.inside, &g.journal_ringbuf);
}

void gig_init(void)
{
	ringbuf_init(&g.command_ringbuf, 16);

	g.my_artist_id = 1;
	// XXX this is probably correct if we're host of the venue, but otherwise
	// we need an assigned artist id

	{
		struct ringbuf* rb = &g.journal_ringbuf;
		ringbuf_init(rb, 18); // XXX 256kB ringbuf. is this good/bad?
		rb->no_acknowledge = 1; // we'

		// put a document here

		struct codec c={0};
		codec_begin_encode_journal_commands(&c, rb, 0, 0);

		struct command cm = {
			.type = COMMAND_CREATE_DOCUMENT,
			.create_document = {
				.id = 1,
				.type = DOCUMENT_TYPE_AUDIO,
			},
		};
		codec_command_ptr(&c, &cm);

		codec_end_encode_journal_commands(&c);

		//codec_end_encode(&c);
		ringbuf_commit(rb);
	}

	gig_thread_tick();
	gig_spool();
}

int get_my_artist_id(void)
{
	return g.my_artist_id;
}

void gig_selftest(void)
{
	// TODO
}
