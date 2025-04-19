#include <assert.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdalign.h>

#include "stb_ds.h"
#include "gig.h"
#include "util.h"
#include "leb128.h"

struct ringbuf {
	uint8_t* data;
	int size_log2;
	_Alignas(CACHE_LINE_SIZE) _Atomic(size_t) read_cursor;
	_Alignas(CACHE_LINE_SIZE) _Atomic(size_t) write_cursor;
};

static void ringbuf_init(struct ringbuf* r, int size_log2)
{
	memset(r, 0, sizeof *r);
	r->size_log2 = size_log2;
	r->data = calloc(1L << size_log2, sizeof r->data[0]);
}

static void ringbuf_write_u8(struct ringbuf* r, uint8_t v)
{
	const size_t s = (1L << r->size_log2);
	assert(r->write_cursor < (r->read_cursor + s));
	r->data[r->write_cursor] = v;
	r->write_cursor = (1 + r->write_cursor) & (s-1);
}

static struct {
	struct document* document_arr;
	int next_document_id;

	//unsigned codec_version; // TODO?
	#if 0
	unsigned codec_bit_cursor;
	#endif

	struct ringbuf journal_ringbuf;
	struct ringbuf command_ringbuf;

	struct ringbuf* codec_writing_ringbuf;

	// bits
	unsigned ed_is_begun :1;

	unsigned codec_is_encoding   : 1;
	unsigned codec_is_decoding   : 1;
	unsigned codec_doing_bits    : 1;
} g;

void gig_init(void)
{
	g.next_document_id = 1;
	ringbuf_init(&g.journal_ringbuf, 20);
	ringbuf_init(&g.command_ringbuf, 16);

	new_document(DOC_AUDIO);
	struct document* doc = get_document_by_id(1);
	const char* code =
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	;
	const size_t n = strlen(code);
	for (int i=0; i<n; ++i) {
		arrput(doc->fat_char_arr, ((struct fat_char){
			.codepoint = code[i],
			.color = {100,100,100},
		}));
	}
}

int new_document(enum document_type type)
{
	const int id = g.next_document_id++;
	arrput(g.document_arr, ((struct document){
		.id = id,
		.type = type,

	}));
	return id;
}

int get_num_documents(void)
{
	return arrlen(g.document_arr);
}

struct document* get_document_by_index(int index)
{
	assert((0 <= index) && (index < get_num_documents()));
	return &g.document_arr[index];
}

struct document* find_document_by_id(int id)
{
	const int n = get_num_documents();
	for (int i=0; i<n; ++i) {
		struct document* doc = get_document_by_index(i);
		if (doc->id == id) return doc;
	}
	return NULL;
}

struct document* get_document_by_id(int id)
{
	struct document* doc = find_document_by_id(id);
	assert((doc != NULL) && "document id not found");
	return doc;
}

static void codec_sanity_check(void)
{
	assert(!(g.codec_is_encoding && g.codec_is_decoding) && "both cannot be true");
}

static int is_encoding(void)
{
	codec_sanity_check();
	return g.codec_is_encoding;
}

static int is_decoding(void)
{
	codec_sanity_check();
	return g.codec_is_decoding;
}

static int is_coding(void)
{
	return is_encoding() || is_decoding();
}

static void codec_begin_encode(struct ringbuf* r)
{
	assert(!is_coding());
	assert(g.codec_writing_ringbuf == NULL);
	g.codec_writing_ringbuf = r;
	// TODO remember document revision we're editing against?
	g.codec_is_encoding = 1;
}

static void codec_end_encode(void)
{
	assert(is_encoding());
	assert(g.codec_writing_ringbuf != NULL);
	g.codec_writing_ringbuf = NULL;
	g.codec_is_encoding = 0;
}

static void codec_begin_decode(void)
{
	assert(!is_coding());
	g.codec_is_decoding = 1;
}

static void codec_end_decode(void)
{
	assert(is_decoding());
	g.codec_is_decoding = 0;
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

static uint8_t codec_read_u8(void)
{
	assert(is_decoding());
	assert(!"TODO");
}

static void codec_write_u8(uint8_t v)
{
	assert(is_encoding());
	assert(g.codec_writing_ringbuf != NULL);
	ringbuf_write_u8(g.codec_writing_ringbuf, v);
}

static inline void codec_write_int(int v)
{
	leb128_encode_int(codec_write_u8, v);
}

static inline int codec_read_int(void)
{
	return leb128_decode_int(codec_read_u8);
}

NO_DISCARD
static int codec_int_io(int v)
{
	if (is_encoding()) {
		codec_write_int(v);
		return v;
	} else if (is_decoding()) {
		(void)v;
		return codec_read_int();
	}
	assert(!"bad state");
}
#define CODEC_INT(V) (V)=codec_int_io(V)

#if 0
static void codec_magic(int magic)
{
	int magic2 = codec_int_io(magic);
	if (magic2 != magic) {
		assert(!"TODO error handling?"); // XXX
	}
}
#endif

#if 0
static void codec_begin_bits(void)
{
	assert(!g.codec_doing_bits);
	assert(g.codec_bit_cursor == 0);
	g.codec_doing_bits = 1;
}

static void codec_end_bits(void)
{
	assert(g.codec_doing_bits);
	g.codec_doing_bits = 0;
	g.codec_bit_cursor = 0;
	assert(!"TODO"); // TODO
}

NO_DISCARD
static unsigned codec_bit_io(unsigned bit)
{
	assert(g.codec_doing_bits);
	assert(!"TODO"); // TODO
	return bit; // XXX
}
#define CODEC_BIT(V) (V)=codec_bit_io(V)
#endif

static inline int s2i(size_t x)
{
	assert(x <= INT_MAX);
	return (int)x;
}

static void codec_cstr_ptr(const char** p)
{
	if (is_encoding()) {
		const int n = s2i(strlen(*p));
		codec_write_int(n);
		for (int i=0; i<n; ++i) codec_write_u8((*p)[i]);
	} else if (is_decoding()) {
		assert(!"TODO b"); // TODO
	} else {
		assert(!"bad state");
	}
}

static void codec_location_ptr(struct location* l)
{
	CODEC_INT(l->line);
	CODEC_INT(l->column);
}

static void codec_range_ptr(struct range* r)
{
	codec_location_ptr(&r->from);
	codec_location_ptr(&r->to);
}

static void codec_command_ptr(struct command* c)
{
	#if 0
	codec_magic(42); // XXX magic marker here? or how does that work?
	#endif

	CODEC_INT(c->type);
	switch (c->type) {
	case COMMAND_SET_CARET:
		CODEC_INT(c->set_caret.id);
		codec_range_ptr(&c->set_caret.range);
		break;
	case COMMAND_DELETE_CARET:
		CODEC_INT(c->delete_caret.id);
		break;
	case COMMAND_INSERT:
		codec_location_ptr(&c->insert.at);
		codec_cstr_ptr(&c->insert.text);
		break;
	case COMMAND_DELETE:
		codec_range_ptr(&c->delete.range);
		break;
	case COMMAND_COMMIT:
		CODEC_INT(c->commit.author_id);
		codec_range_ptr(&c->commit.range);
		break;
	case COMMAND_CANCEL:
		CODEC_INT(c->cancel.author_id);
		codec_range_ptr(&c->cancel.range);
		break;
	case COMMAND_DEFER:
		CODEC_INT(c->defer.author_id);
		codec_range_ptr(&c->defer.range);
		break;
	case COMMAND_UNDEFER:
		CODEC_INT(c->undefer.author_id);
		codec_range_ptr(&c->undefer.range);
		break;
	case COMMAND_SET_COLOR:
		CODEC_INT(c->set_color.red);
		CODEC_INT(c->set_color.green);
		CODEC_INT(c->set_color.blue);
		break;
	default: assert(!"unhandled command");
	}
}

void gig_spool(void)
{
	// TODO spooling should go through new journal entries, and apply them so
	// that documents are up-to-date. the purpose of having a function to do
	// this is that you can do it when it's convenient, instead of using
	// mutexes in the background to do it automatically. spool can be run once
	// per video frame?
}

void ed_begin(void)
{
	assert(!g.ed_is_begun);
	g.ed_is_begun = 1;
}

void ed_do(struct command* c)
{
	assert(g.ed_is_begun);
	codec_begin_encode(&g.command_ringbuf);
	codec_command_ptr(c);
	codec_end_encode();
}

int ed_commit(void)
{
	assert(g.ed_is_begun);
	g.ed_is_begun = 0;
	// TODO
	return 1; // XXX
}

void gig_thread_tick(void)
{
	// TODO
}
