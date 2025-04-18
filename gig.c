#include <assert.h>

#include "stb_ds.h"
#include "gig.h"
#include "util.h"
#include "varint.h"

static struct {
	struct document* document_arr;
	int next_document_id;

	//unsigned codec_version; // TODO?
	unsigned codec_bit_cursor;

	// bits
	unsigned ed_is_begun :1;

	unsigned codec_is_encoding   : 1;
	unsigned codec_is_decoding   : 1;
	unsigned codec_doing_bits    : 1;
} g;

void gig_init(void)
{
	g.next_document_id = 1;
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
			.color = {1,1,1,1},
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

static void codec_begin_encode(void)
{
	assert(!is_coding());
	g.codec_is_encoding = 1;
}

static void codec_end_encode(void)
{
	assert(is_encoding());
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
	assert(!"TODO");
}

NO_DISCARD
static int codec_int_io(int v)
{
	if (is_encoding()) {
		varint_encode_int(codec_write_u8, v);
		return v;
	} else if (is_decoding()) {
		return varint_decode_int(codec_read_u8);
	}
	assert(!"bad state");
}
#define CODEC_INT(V) (V)=codec_int_io(V)

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

static void codec_cstr_ptr(const char** p)
{
	assert(!"TODO"); // TODO
}

static void codec_target_ptr(struct target* t)
{
	CODEC_INT(t->type);
	switch (t->type) {
	case TARGET_RELATIVE_COLUMN:
		CODEC_INT(t->relative_column.delta);
		break;
	case TARGET_RELATIVE_LINE:
		CODEC_INT(t->relative_line.delta);
		break;
	default: assert(!"unhandled target type");
	}
}

static void codec_command_ptr(struct command* c)
{
	CODEC_INT(c->type);
	switch (c->type) {
	case COMMAND_MOVE_CARET: {
		codec_target_ptr(&c->move_caret.target);
		codec_begin_bits();
		CODEC_BIT(c->move_caret.add_caret);
		CODEC_BIT(c->move_caret.clear_all_carets);
		CODEC_BIT(c->move_caret.set_selection_begin);
		CODEC_BIT(c->move_caret.set_selection_end);
		codec_end_bits();
	}	break;
	case COMMAND_INSERT: {
		codec_cstr_ptr(&c->insert.text);
	}	break;
	default: assert(!"unhandled command type");
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
	#if 0
	assert(0 == arrlen(g.command_arr));
	assert(0 == arrlen(g.string_buffer_arr));
	#endif
	g.ed_is_begun = 1;
}

void ed_do(struct command* c)
{
	assert(g.ed_is_begun);

	codec_begin_encode();
	codec_command_ptr(c);
	codec_end_encode();

	#if 0
	struct command* cn = arraddnptr(g.command_arr, 1);
	*cn = *c;
	if (cn->type == COMMAND_INSERT) {
		// copy text so that the caller of ed_do() is not obligated to
		// keep the pointer alive after we return
		const char* text = cn->insert.text;
		// pointers are unstable, so store the stable string buffer offset
		// instead (realloc() inside arraddnptr() may change the pointer).
		// this overwrites `text`.
		cn->insert._text_offset = arrlen(g.string_buffer_arr); // NOTE overwrites cn->insert.text
		const size_t n = 1+strlen(text);
		char* sn = arraddnptr(g.string_buffer_arr, n);
		memcpy(sn, text, n);
	}
	#endif
}

int ed_commit(void)
{
	assert(g.ed_is_begun);

	#if 0
	const int num_commands = arrlen(g.command_arr);
	for (int i=0; i<num_commands; ++i) {
		struct command* c = &g.command_arr[i];
		switch (c->type) {
		case COMMAND_MOVE_CARET: {
			assert(!"TODO CARET");
		}	break;
		case COMMAND_INSERT: {
			// restore text pointer (this overwrites `_text_offset`)
			c->insert.text = g.string_buffer_arr + c->insert._text_offset;
			assert(!"TODO INSERT");
		}	break;
		default: assert(!"unhandled command type");
		}
	}

	arrsetlen(g.command_arr, 0);
	arrsetlen(g.string_buffer_arr, 0);
	#endif

	g.ed_is_begun = 0;
	return 1; // XXX
}

