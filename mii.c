#include <stdio.h> // XXX
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "mii.h"
#include "util.h"
#include "utf8.h"
#include "stb_sprintf.h"
#include "stb_ds.h"
#include "allocator.h"

enum compiler_state {
	WORD=1,
	STRING,
	COMMENT,
};

#define MAX_ERROR_MESSAGE_SIZE (1<<14)
#define WORD_BUF_CAP (1<<8)

struct compiler {
	struct vmii* vm;
	int line, column;
	enum compiler_state state;
	int comment_depth;
	char word_buf[WORD_BUF_CAP];
	int word_size;
	unsigned has_error              :1;
	unsigned no_break_on_word_space :1;
	struct {
		char* key;
		int x;
	}* word_lut;
	// fields below this line are kept (not cleared) between compilations. new
	// fields must be explicitly added to and handled in compiler_begin().
	char* error_message;
	struct allocator scratch_allocator;
	struct scratch_context_header* scratch_config;
};

static struct {
	struct compiler compiler;
} g;

FORMATPRINTF2
static void compiler_errorf(struct compiler* cm, const char* fmt, ...)
{
	char* p = cm->error_message;
	int r = MAX_ERROR_MESSAGE_SIZE;
	int n;
	va_list va;
	va_start(va, fmt);
	n = stbsp_vsnprintf(p, r, fmt, va);
	va_end(va);
	p+=n; r-=n;
	n = stbsp_snprintf(p, r, " at line %d:%d", cm->line, cm->column);
	p+=n; r-=n;
	cm->has_error = 1;
}

static void compiler_init(struct compiler* cm, size_t scratch_capacity)
{
	memset(cm, 0, sizeof *cm);
	cm->scratch_config = init_scratch_allocator(&cm->scratch_allocator, scratch_capacity);
}

static void compiler_begin(struct compiler* cm, struct vmii* vm)
{
	// keep some fields, and zero initialize the rest
	char* keep0 = cm->error_message;
	struct allocator keep1 = cm->scratch_allocator;
	struct scratch_context_header* keep2 = cm->scratch_config;
	memset(cm, 0, sizeof *cm);
	cm->error_message = keep0;
	cm->scratch_allocator = keep1;
	cm->scratch_config = keep2;

	if (cm->error_message == NULL) {
		cm->error_message = calloc(MAX_ERROR_MESSAGE_SIZE, sizeof *cm->error_message);
	}
	assert(cm->word_lut == NULL);
	hminit(cm->word_lut, &cm->scratch_allocator);
	cm->vm = vm;
	cm->line = 1;
	cm->column = 0;
	cm->state = WORD;
}

static void compiler_push_word(struct compiler* cm)
{
	printf("w0rd [%s]\n", cm->word_buf);
	cm->word_buf[0] = 0;
	cm->word_size = 0;
}

static void compiler_push(struct compiler* cm, struct thicchar ch)
{
	if (cm->has_error) return;

	const int c = ch.codepoint;
	if (c == '\n') {
		++cm->line;
		cm->column = 0;
	} else {
		++cm->column;
	}

	switch (cm->state) {

	case WORD: {
		int is_break=0, also_emit_1ch_word=0;
		if (c=='(') {
			cm->state = COMMENT;
			cm->comment_depth = 1;
			is_break = 1;
		} else if ((c=='"') || (c=='\\')) {
			cm->state = STRING;
			is_break = 1;
		} else if (c == ';') {
			is_break = 1;
			also_emit_1ch_word = 1;
		} else if (c <= ' ' && !cm->no_break_on_word_space) {
			is_break = 1;
		}

		if (is_break) {
			if (cm->word_size > 0) {
				compiler_push_word(cm);
				assert(cm->word_size == 0);
				cm->no_break_on_word_space = 0;
			}
			if (also_emit_1ch_word) {
				assert(cm->word_size == 0);
				char* e = utf8_encode(cm->word_buf, c);
				*e=0;
				compiler_push_word(cm);
				assert(cm->word_size == 0);
			}
		} else {
			// reserve enough space for:
			//  string nul terminator (1)
			//  max utf8 sequence length (4)
			const int max_word_size = (WORD_BUF_CAP - 1 - UTF8_MAX_SIZE);
			if (cm->word_size > max_word_size) {
				compiler_errorf(cm, "word too long (exceeded %d bytes)\n", max_word_size);
				return;
			}
			if ((cm->word_size==0)) {
				cm->no_break_on_word_space = (c==':');
			} else if (c > ' ') {
				cm->no_break_on_word_space = 0;
			}
			char* p0 = &cm->word_buf[cm->word_size];
			char* p1;
			p1 = utf8_encode(p0, c);
			cm->word_size += (p1-p0);
			assert(cm->word_size < WORD_BUF_CAP);
			cm->word_buf[cm->word_size] = 0; // word_buf is MAX_WORD_SIZE+1 long
		}
	}	break;

	case STRING: {
		assert(!"TODO");
	}	break;

	case COMMENT: {
		// TODO we may want to extract some comments as "word-docs" you can
		// lookup? meaning we don't want to throw them away
		// TODO I had a comments-as-contracts idea... that, say, (x -- x x)
		// checks that the stack got 1 higher since the beginning of the source
		// line, and also that the stack is at least 2 elements high, and the
		// top 2 elements are identical?
		if (c == '(') {
			++cm->comment_depth;
		} else if (c == ')') {
			--cm->comment_depth;
		}
		if (cm->comment_depth == 0) {
			cm->state = WORD;
		}
	}	break;

	default: assert(!"unhandled compiler state");
	}
}

static void compiler_end(struct compiler* cm)
{
	compiler_push(cm, ((struct thicchar){.codepoint = 0}));
	if (cm->state != WORD) {
	}
	switch (cm->state) {
	case WORD: /* OK */ break;
	case STRING:  compiler_errorf(cm, "EOF in unterminated string"); break;
	case COMMENT: compiler_errorf(cm, "EOF in unterminated comment"); break;
	default:      compiler_errorf(cm, "EOF in unexpected compiler state (%d)", cm->state); break;
	}
	cm->vm->compile_error = cm->has_error ? cm->error_message : NULL;
	assert(cm->word_lut != NULL);
	hmfree(cm->word_lut);
	assert(cm->word_lut == NULL);
}

void mii_compile_thicc(struct vmii* vm, const struct thicchar* src, int num_chars)
{
	struct compiler* cm = &g.compiler;
	compiler_begin(cm, vm);
	for (int i=0; i<num_chars; ++i) compiler_push(cm, src[i]);
	compiler_end(cm);
}

// with apologies to Frank Gray
void mii_compile_graycode(struct vmii* vm, const char* utf8src, int num_bytes)
{
	struct compiler* cm = &g.compiler;
	const char* p = utf8src;
	int remaining = num_bytes;
	compiler_begin(cm, vm);
	while (remaining > 0) {
		unsigned codepoint = utf8_decode(&p, &remaining);
		if (codepoint == -1) continue;
		compiler_push(cm, ((struct thicchar){.codepoint = codepoint}));
	}
	compiler_end(cm);
}

void mii_init(void)
{
	compiler_init(&g.compiler, 1LL<<20);
}

// TODO I think I'd like a debugging feature where I can see the stack height
// at the beginning of each line. that means I should inject some code at the
// beginning of each line that stores the stack height (maybe it stores min/max
// actually). I dont think it should be handled by gig.c because it needs to be
// aware of multi-line features like comments and strings? but it might be a
// compiler flag or something. maybe I want a similar feature for displaying
// "stack under the caret"? anyway, the per-line call is probably something
// like "7 _linedebug" where 7 is the line number, and _linedebug is a word
// that measures the stack height and stores it at "line 7 for the current
// file" (what if multiple files..?)
