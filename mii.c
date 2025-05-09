#include <stdio.h> // XXX
#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "mii.h"
#include "util.h"
#include "utf8.h"
#include "stb_sprintf.h"
#include "stb_ds.h"
// NOTE we don't use stb_ds_sysalloc.h 
#include "allocator.h"

#define EMIT_SYNTAX_WORDS \
	X( CTM       , NULL  , "Enter compile-time code (leave run-time code)") \
	X( TTT       , NULL  , "Drop value from compile-time into run-time (x --) ") \
	X( MTC       , NULL  , "Leave compile-time code (return to run-time code)") \
	X( COLON     , ":"   , "Word definiton, followed by word (name), e.g. `: foo`") \
	X( SEMICOLON , ";"   , "End of word definition") \


// direct mapping to vmii VM ops
#define EMIT_OP_WORDS \
	/* ==========,========,============================================== */ \
	X( PICK      , "pick" , "Duplicate stack value (n -- stack[-1-n])") \
	X( DROP      , "drop" , "Remove top element from stack (x --)") \
	X( ROLL      , "roll" , "Pop n, rotate left n+1 items on stack (example : a b c 2 -- b c a)") \
	X( LLOR      , "llor" , "Pop n, rotate right n+1 items on stack (example: a b c 2 -- c a b)") \
	X( EQ        , "="    , "Equals (x y -- x=y)") \
	/* ==========,========,============================================== */ \
	X( TYPEOF    , NULL   , "Get type (x -- typeof(x))") \
	X( CAST      , NULL   , "Set type (x T -- T(x))") \
	/* ==========,========,============================================== */ \
	X( FADD      , "F+"   , "Floating-point add (x y -- x+y)") \
	X( FNEG      , "F~"   , "Floating-point negate (x -- -x)") \
	X( FMUL      , "F*"   , "Floating-point multiply (x y -- x*y)") \
	/* X( FMA       , "F*+"  , "Floating-point multiply-add (x y z -- x*y+z)") */ \
	X( FMOD      , "F%"   , "Floating-point remainder/modulus (x y -- x%y)") \
	X( FINV      , "F1/"  , "Floating-point reciprocal (x -- 1/x)") \
	X( FDIV      , "F/"   , "Floating-point division (x y -- x/y)") \
	X( FLT       , "F<"   , "Floating-point less than (x y -- x<y)") \
	X( FLE       , "F<="  , "Floating-point less than or equal (x y -- x<=y)") \
	X( FEQ       , "F="   , "Floating-point equal (x y -- x=y)") \
	X( FGE       , "F>="  , "Floating-point greater than or equal (x y -- x>=y)") \
	X( FGT       , "F>"   , "Floating-point greater than (x y -- x>y)") \
	/* ==========,========,============================================== */ \
	X( IADD      , "I+"   , "Integer add (x y -- x+y)") \
	X( INEG      , "I~"   , "Integer negate (x -- -x)") \
	X( IMUL      , "I*"   , "Integer multiply (x y -- x*y)") \
	X( IDIV      , "I//"  , "Integer euclidean division (x y -- x//y)") \
	X( IMOD      , "I%"   , "Integer euclidean remainder/modulus (x y -- x%y)") \
	X( IAND      , "I&"   , "Integer bitwise AND (x y -- x&y)") \
	X( IOR       , "I|"   , "Integer bitwise OR (x y -- x|y)") \
	X( IXOR      , "I^"   , "Integer bitwise XOR (x y -- x^y)") \
	X( INOT      , "I!"   , "Integer bitwise NOT (x -- !y)") \
	X( ILSHIFT   , "I<<"  , "Integer shift left (x y -- x<<y)") \
	X( IRSHIFT   , "I>>"  , "Integer shift right (x y -- x>>y)") \
	X( ILT       , "I<"   , "Integer less than (x y -- x<y)") \
	X( ILE       , "I<="  , "Integer less than or equal (x y -- x<=y)") \
	X( IEQ       , "I="   , "Integer equal (x y -- x=y)") \
	X( IGE       , "I>="  , "Integer greater than or equal (x y -- x>=y)") \
	X( IGT       , "I>"   , "Integer greater than (x y -- x>y)") \
	/* ==========,========,============================================== */ \
	X( ARRNEW    , NULL   , "Create array (-- xs)") \
	X( ARRLEN    , NULL   , "Length of array (xs -- len(xs))") \
	X( ARRPUT    , NULL   , "Append item to array ([..] x -- [..x])") \


#define EMIT_WORDS \
	X(_NO_WORD_,"","") \
	EMIT_SYNTAX_WORDS \
	EMIT_OP_WORDS \


enum builtin_word {
	#define X(E,_S,_D) E,
	EMIT_WORDS
	#undef X
	_FIRST_USER_WORD_ // ids below this are built-in words
};

static enum builtin_word match_builtin_word(const char* word)
{
	int i=0;
	// match word's S (string representation) if not null, else #E (enum as
	// string)
	#define X(E,S,_D) \
		{ \
			const char* s = S; \
			const char* e = #E; \
			const int is_marker = e[0]=='_'; \
			if (!is_marker && strcmp(word,s!=NULL?s:e) == 0) return (enum builtin_word)i; \
		} \
		++i;
	EMIT_WORDS
	#undef X
	assert((i == _FIRST_USER_WORD_) && "something's not right");
	return _NO_WORD_;
}

enum compiler_state { // XXX tokenizer state?
	WORD=1,
	STRING,
	COMMENT,
};

#define MAX_ERROR_MESSAGE_SIZE (1<<14)
#define WORD_BUF_CAP (1<<8)

struct word_info {
	unsigned is_comptime :1;
};

struct compiler {
	struct vmii* vm;
	int line, column;
	enum compiler_state state;
	int comment_depth;
	char word_buf[WORD_BUF_CAP];
	int word_size;
	unsigned has_error              :1;
	struct {
		char* key;
		struct word_info value;
	}* word_lut;
	int prefix_word, remaining_suffix_words;
	int next_user_word_id;
	int is_comptime;
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
	shinit(cm->word_lut, &cm->scratch_allocator);

	cm->vm = vm;
	cm->line = 1;
	cm->column = 0;
	cm->next_user_word_id = _FIRST_USER_WORD_;
	cm->state = WORD;
}

struct number {
};

static int parse_number(const char* word, struct number* out_number)
{
	return 0; // TODO
}

static void compiler_push_word(struct compiler* cm, const char* word)
{
	if (cm->has_error) return;

	// handle suffix words for prefix words
	if (cm->remaining_suffix_words > 0) {
		switch (cm->prefix_word) {

		case COLON: {
			if (match_builtin_word(word) != _NO_WORD_) {
				compiler_errorf(cm, "cannot redefine built-in word (%s)", word);
				return;
			}
			if (parse_number(word, NULL)) {
				compiler_errorf(cm, "cannot define a number (%s)", word);
				return;
			}
			if (shgeti(cm->word_lut, word) >= 0) {
				compiler_errorf(cm, "cannot redefine previously defined word (%s)", word);
				return;
			}

			const int word_index = shputi(cm->word_lut, word, ((struct word_info){
				.is_comptime = cm->is_comptime,
			}));

			printf("word def [%s] => %d\n", word, word_index);
			assert(!"TODO word def");

		}	break;

		default: assert(!"internal error: unexpected prefix word");

		}

		--cm->remaining_suffix_words;
		assert(cm->remaining_suffix_words >= 0);
		if (cm->remaining_suffix_words == 0) cm->prefix_word = 0;

		return;
	}

	struct number number;
	enum builtin_word bw = match_builtin_word(word);
	if (bw > _NO_WORD_) {
		assert(bw < _FIRST_USER_WORD_);

		switch (bw) {

		case COLON: {
			cm->prefix_word = bw;
			cm->remaining_suffix_words = 1;
		}	break;

		default: assert(!"TODO built-in word"); break;
		}
	} else if (parse_number(word, &number)) {
		assert(!"TODO number word");
	} else if (shgeti(cm->word_lut, word) < 0) {
		assert(!"TODO lut word");
	}
	#if 0
		printf("word not found: [%s]\n", word);
		shput(cm->word_lut, word, 1);
	} else {
		printf("word found: [%s]\n", word);
	}
	#endif
}

static void compiler_push_char(struct compiler* cm, struct thicchar ch)
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
		int is_break=0;
		if (c=='(') {
			cm->state = COMMENT;
			cm->comment_depth = 1;
			is_break = 1;
		} else if ((c=='"') || (c=='\\')) {
			cm->state = STRING;
			is_break = 1;
		} else if (c <= ' ') {
			is_break = 1;
		}

		if (is_break) {
			if (cm->word_size > 0) {
				compiler_push_word(cm, cm->word_buf);
				cm->word_buf[0] = 0;
				cm->word_size = 0;
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
		// NOTE: contracts should probably be in double parenthesis, so that
		// the second level and beyond are "contract". this is to allow stuff
		// like ((x y -- 2*(x+y))) without breaking out of contract mode. also
		// "--" must exist at the second level for it to be a contract?
		//const int is_contract = ((cm->comment_depth > 2) || (cm->comment_depth == 2 && c != ')'));
		assert(cm->comment_depth > 0);
		if (c == '(') {
			++cm->comment_depth;
		} else if (c == ')') {
			--cm->comment_depth;
			assert(cm->comment_depth >= 0);
			if (cm->comment_depth == 0) {
				cm->state = WORD;
			}
		}
	}	break;

	default: assert(!"unhandled compiler state");
	}
}

static void compiler_end(struct compiler* cm)
{
	compiler_push_char(cm, ((struct thicchar){.codepoint = 0}));
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
	for (int i=0; i<num_chars; ++i) compiler_push_char(cm, src[i]);
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
		compiler_push_char(cm, ((struct thicchar){.codepoint = codepoint}));
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
