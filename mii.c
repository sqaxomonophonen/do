#include <stdio.h> // XXX
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <math.h>

#include "mii.h"
#include "gig.h"
#include "util.h"
#include "utf8.h"
#include "stb_sprintf.h"
#include "stb_ds.h" // NOTE we don't use stb_ds_sysalloc.h
#include "allocator.h"

// forth-like stack-manipulation notation:
//   (before -- after) for stack
//   [before -- after] for rstack ("return stack")

// the following "X macros" (it's a thing; look it up!) define built-in words
// and VM ops, and take (ENUM,STR,DOC) "arguments":
//  - ENUM: enum identifier for the word (ops also get an "OP_"-prefix)
//  - STR: string name; pass NULL to use the string representation of ENUM;
//    pass "" if the word cannot be used directly from the source.
//  - DOC: short documentation of the word/op

#define LIST_OF_SYNTAX_WORDS \
	/*<ENUM>      <STR>     <DOC> */ \
	/* ==========,========,============================================== */ \
	X( CTM       , NULL   , "Enter compile-time code (leave run-time code)") \
	X( TTT       , NULL   , "Drop value from compile-time into run-time (x --) ") \
	X( MTC       , NULL   , "Leave compile-time code (return to run-time code)") \
	X( COLON     , ":"    , "Word definiton, followed by word (name), e.g. `: foo`") \
	X( SEMICOLON , ";"    , "End of word definition") \
	/* ==========,========,============================================== */


// direct mapping to vmii VM ops
#define LIST_OF_OP_WORDS \
	/*<ENUM>      <STR>     <DOC> */ \
	/* ==========,==========,============================================== */ \
	X( NOP       , NULL     , "No operation ( -- )") \
	X( RETURN    , "return" , "Return from subroutine [returnaddr -- ]") \
	X( PICK      , "pick"   , "Duplicate stack value (n -- stack[-1-n])") \
	X( DROP      , "drop"   , "Remove top element from stack (x --)") \
	X( ROLL      , "roll"   , "Pop n, rotate left n+1 items on stack (example : a b c 2 -- b c a)") \
	X( LLOR      , "llor"   , "Pop n, rotate right n+1 items on stack (example: a b c 2 -- c a b)") \
	X( EQ        , "="      , "Equals (x y -- x=y)") \
	X( TYPEOF    , NULL     , "Get type (x -- typeof(x))") \
	X( CAST      , NULL     , "Set type (x T -- T(x))") \
	X( HERE      , "here"   , "Push current instruction pointer to rstack {-- ip}") \
	X( I2R       , "I>R"    , "Moves integer value to rstack (i --) [-- i]") \
	X( R2I       , "R>I"    , "Moves integer value back from rstack [i --] (-- i)") \
	X( JMPI      , NULL     , "Pop address from rstack => indirect jump [addr -- ]" ) \
	X( JSRI      , NULL     , "Pop address from rstack => indirect jump-to-subroutine [addr --]" ) \
	/* ==========,==========,============================================== */ \
	X( FADD      , "F+"     , "Floating-point add (x y -- x+y)") \
	X( FNEG      , "F~"     , "Floating-point negate (x -- -x)") \
	X( FMUL      , "F*"     , "Floating-point multiply (x y -- x*y)") \
	/* X( FMA       , "F*+  "  , "Floating-point multiply-add (x y z -- x*y+z)") */ \
	X( FMOD      , "F%"     , "Floating-point remainder/modulus (x y -- x%y)") \
	X( FINV      , "F1/"    , "Floating-point reciprocal (x -- 1/x)") \
	X( FDIV      , "F/"     , "Floating-point division (x y -- x/y)") \
	X( FLT       , "F<"     , "Floating-point less than (x y -- x<y)") \
	X( FLE       , "F<="    , "Floating-point less than or equal (x y -- x<=y)") \
	X( FEQ       , "F="     , "Floating-point equal (x y -- x=y)") \
	X( FGE       , "F>="    , "Floating-point greater than or equal (x y -- x>=y)") \
	X( FGT       , "F>"     , "Floating-point greater than (x y -- x>y)") \
	/* ==========,==========,============================================== */ \
	X( IADD      , "I+"     , "Integer add (x y -- x+y)") \
	X( INEG      , "I~"     , "Integer negate (x -- -x)") \
	X( IMUL      , "I*"     , "Integer multiply (x y -- x*y)") \
	X( IDIV      , "I//"    , "Integer euclidean division (x y -- x//y)") \
	X( IMOD      , "I%"     , "Integer euclidean remainder/modulus (x y -- x%y)") \
	X( IAND      , "I&"     , "Integer bitwise AND (x y -- x&y)") \
	X( IOR       , "I|"     , "Integer bitwise OR (x y -- x|y)") \
	X( IXOR      , "I^"     , "Integer bitwise XOR (x y -- x^y)") \
	X( INOT      , "I!"     , "Integer bitwise NOT (x -- !y)") \
	X( ILSHIFT   , "I<<"    , "Integer shift left (x y -- x<<y)") \
	X( IRSHIFT   , "I>>"    , "Integer shift right (x y -- x>>y)") \
	X( ILT       , "I<"     , "Integer less than (x y -- x<y)") \
	X( ILE       , "I<="    , "Integer less than or equal (x y -- x<=y)") \
	X( IEQ       , "I="     , "Integer equal (x y -- x=y)") \
	X( IGE       , "I>="    , "Integer greater than or equal (x y -- x>=y)") \
	X( IGT       , "I>"     , "Integer greater than (x y -- x>y)") \
	/* ==========,==========,============================================== */ \
	X( ARRNEW    , NULL     , "Create array (-- xs)") \
	X( ARRLEN    , NULL     , "Length of array (xs -- len(xs))") \
	X( ARRPUT    , NULL     , "Append item to array ([..] x -- [..x])") \
	/* ==========,==========,============================================== */


#define LIST_OF_WORDS \
	X(_NO_WORD_,"","") \
	LIST_OF_SYNTAX_WORDS \
	LIST_OF_OP_WORDS


enum builtin_word {
	#define X(E,_S,_D) E,
	LIST_OF_WORDS
	#undef X
	_FIRST_USER_WORD_ // ids below this are built-in words
};

union enc {
	uint32_t u32;
	int32_t i32;
	float f32;
};
static_assert(sizeof(union enc)==sizeof(uint32_t),"");

enum opcode {
	// ops that have a corresponding word (1:1)
	#define X(E,_S,_D) OP_##E,
	LIST_OF_OP_WORDS
	#undef X

	// "word-less ops", ops that are indirectly encoded with "syntax", in part
	// because they have "arguments"
	_FIRST_WORDLESS_OP_,
	OP_ILITERAL = _FIRST_WORDLESS_OP_, // integer literal, followed by i32
	OP_FLITERAL, // floating-point literal, followed by f32
	OP_JMP, // unconditional jump, followed by address
	OP_JMP0, // conditional jump, pops x, jumps if x=0. followed by address.
	OP_JSR, // jump to subroutine, followed by address

	_NUM_OPS_,
	_OP_NONE_
};

static_assert(OP_NOP==0,"expected NOP to be first (XXX but why?)");


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
	LIST_OF_WORDS
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
	int addr;
};

struct opstream {
	union enc* op_arr;
};

struct compiler {
	struct vmii* vm;
	struct location location, error_location;
	enum compiler_state state;
	int comment_depth;
	char word_buf[WORD_BUF_CAP];
	int word_size;
	unsigned has_error :1;
	struct {
		char* key;
		struct word_info value;
	}* word_lut;
	int* wordvis_stack_arr;
	int* wordscope_stack_arr;
	int* wordskip_addraddr_arr;
	int prefix_word, remaining_suffix_words;
	int next_user_word_id;
	int is_comptime;
	struct opstream runtime_opstream, comptime_opstream;

	// fields below this line are kept (not cleared) between compilations. new
	// fields must be explicitly added to and handled in compiler_begin().
	char* error_message;
	struct allocator scratch_allocator;
	struct scratch_context_header* scratch_header;
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
	cm->error_location = cm->location;
	n = stbsp_snprintf(p, r, " at line %d:%d", cm->error_location.line, cm->error_location.column);
	p+=n; r-=n;
	cm->has_error = 1;
}

static void compiler_init(struct compiler* cm, size_t scratch_capacity)
{
	memset(cm, 0, sizeof *cm);
	cm->scratch_header = init_scratch_allocator(&cm->scratch_allocator, scratch_capacity);
}

static void compiler_begin(struct compiler* cm, struct vmii* vm)
{
	// clear cm, but keep some fields
	char* keep0 = cm->error_message;
	struct allocator keep1 = cm->scratch_allocator;
	struct scratch_context_header* keep2 = cm->scratch_header;
	memset(cm, 0, sizeof *cm);
	cm->error_message = keep0;
	cm->scratch_allocator = keep1;
	cm->scratch_header = keep2;

	if (cm->error_message == NULL) {
		cm->error_message = calloc(MAX_ERROR_MESSAGE_SIZE, sizeof *cm->error_message);
	}

	reset_scratch(cm->scratch_header);

	assert(cm->word_lut == NULL);
	sh_new_strdup_with_context(cm->word_lut, &cm->scratch_allocator);
	assert(cm->wordvis_stack_arr == NULL);
	arrinit(cm->wordvis_stack_arr, &cm->scratch_allocator);
	assert(cm->wordscope_stack_arr == NULL);
	arrinit(cm->wordscope_stack_arr, &cm->scratch_allocator);
	assert(cm->wordskip_addraddr_arr == NULL);
	arrinit(cm->wordskip_addraddr_arr, &cm->scratch_allocator);
	assert(cm->runtime_opstream.op_arr == NULL);
	arrinit(cm->runtime_opstream.op_arr, &cm->scratch_allocator);
	assert(cm->comptime_opstream.op_arr == NULL);
	arrinit(cm->comptime_opstream.op_arr, &cm->scratch_allocator);

	cm->vm = vm;
	cm->location.line = 1;
	cm->location.column = 0;
	cm->next_user_word_id = _FIRST_USER_WORD_;
	cm->state = WORD;
}

struct number {
	enum opcode type;
	union {
		int32_t i32;
		float f32;
	};
};

static int parse_number(const char* word, struct number* out_number)
{
	const char* p = word;
	const char* pend = word+strlen(word);

	int negative = 0;
	if (p[0] == '-') {
		negative = 1;
		++p;
		assert(p<=pend);
		if (p==pend) return 0; // "-" alone is not a number
	}

	enum { INT,FRAC,EXP };
	int part=INT;
	uint64_t vi=0,vf=0,vx=0;
	int ni=0,nf=0,nx=0;

	int negative_exp = 0;
	int type=OP_FLITERAL;
	for (; p<pend; ++p) {
		char c = *p;
		if (c=='.') {
			if (part!=INT) return 0; // point only allowed during integer part
			part=FRAC;
		} else if (c=='-') {
			if (part!=EXP || nx!=0 || negative_exp) return 0; // "-" not in proper place
			negative_exp = 1;
		} else if ((c=='i') || (c=='I')) {
			if (p!=(pend-1)) return 0; // must be last character
			if (part!=INT) return 0; // suffix only valid for integers (no "1.2i")
			type=OP_ILITERAL;
		} else if ((c=='e') || (c=='E')) {
			if (part==EXP) return 0; // "e" must occur during integer or fractional part
			part=EXP;
		} else if (is_digit(c)) {
			uint64_t* vp=NULL;
			int* np=NULL;
			switch (part) {
			case INT:  vp=&vi; np=&ni; break;
			case FRAC: vp=&vf; np=&nf; break;
			case EXP:  vp=&vx; np=&nx; break;
			default: assert(!"bad state");
			}
			assert(vp!=NULL); assert(np!=NULL);
			*vp *= 10L;
			*vp += (uint64_t)(c-'0');
			++(*np);
		} else {
			return 0; // unexpected character
		}
	}

	struct number number = { .type=type };

	switch (type) {

	case OP_FLITERAL: {
		if (ni==0 && nf==0) return 0; // no digits
		const double log10 = log(10.0);
		double v = ((double)vi + vf*exp(-nf*log10)) * exp((negative_exp?-vx:vx)*log10);
		if (negative) v = -v;
		number.f32 = (float)v;
		//printf("parse_number => f32 %f\n", number.f32);
	}	break;

	case OP_ILITERAL: {
		assert(vf==0); assert(nf==0);
		assert(vx==0); assert(nx==0);
		if (ni>10) return 0; // too many digits
		int64_t v = vi;
		if (negative) v = -v;
		if (!((INT_MIN <= v) && (v <= INT_MAX))) return 0; // value doesn't fit in i32
		number.i32 = v;
		//printf("parse_number => i32 %d\n", number.i32);
	}	break;

	default: assert(!"bad state");

	}

	if (out_number/*but not out_gun*/) *out_number = number;

	return 1;
}

static inline struct opstream* compiler_opstream(struct compiler* cm)
{
	return cm->is_comptime ? &cm->comptime_opstream : &cm->runtime_opstream;
}

static int compiler_addr(struct compiler* cm)
{
	return arrlen(compiler_opstream(cm)->op_arr);
}

static union enc compiler_read(struct compiler* cm, int addr)
{
	return arrchkget(compiler_opstream(cm)->op_arr, addr);
}

static int compiler_encode(struct compiler* cm, union enc enc)
{
	const int addr = compiler_addr(cm);
	arrput(compiler_opstream(cm)->op_arr, enc);
	return addr;
}

static void compiler_patch(struct compiler* cm, int addr, union enc enc)
{
	struct opstream* os = compiler_opstream(cm);
	arrchk(os->op_arr, addr);
	os->op_arr[addr] = enc;
}

static int compiler_encode_opcode(struct compiler* cm, enum opcode op)
{
	assert((0<=op) && (op<_NUM_OPS_));
	return compiler_encode(cm, ((union enc){ .i32=op }));
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
				.addr = compiler_addr(cm),
			}));
			arrput(cm->wordvis_stack_arr, word_index);
			arrput(cm->wordscope_stack_arr, arrlen(cm->wordvis_stack_arr));

		}	break;

		default: assert(!"internal error: unexpected prefix word");

		}

		--cm->remaining_suffix_words;
		assert(cm->remaining_suffix_words >= 0);
		if (cm->remaining_suffix_words == 0) cm->prefix_word = 0;

		return;
	}

	struct number number;
	int wi;
	enum builtin_word bw = match_builtin_word(word);
	if (bw > _NO_WORD_) {
		assert(bw < _FIRST_USER_WORD_);

		enum opcode do_encode_opcode = _OP_NONE_;

		switch (bw) {

		case CTM: {
			if (cm->is_comptime) {
				compiler_errorf(cm, "cannot enter comptime; already in comptime");
				return;
			}
			cm->is_comptime = 1;
		}	break;

		case MTC: {
			if (!cm->is_comptime) {
				compiler_errorf(cm, "cannot enter runtime; already in runtime");
				return;
			}
			cm->is_comptime = 0;
		}	break;

		// TODO case TTT:

		case COLON: {
			cm->prefix_word = bw;
			cm->remaining_suffix_words = 1;

			// insert skip jump (TODO skip-jump optimization?)
			compiler_encode_opcode(cm, OP_JMP);
			// insert invalid placeholder address (we don't know it yet)
			const int placeholder_addraddr = compiler_encode(cm, ((union enc){.u32=0xffffffffL}));
			// remember address of the placeholder address, so we can patch it
			// when we see a semicolon:
			arrput(cm->wordskip_addraddr_arr, placeholder_addraddr);
		}	break;

		case SEMICOLON: {
			if (arrlen(cm->wordscope_stack_arr) == 0) {
				compiler_errorf(cm, "too many semi-colons");
				return;
			}
			compiler_encode_opcode(cm, OP_RETURN);

			// finalize word skip jump by overwriting the placeholder address
			// now that we know where the word ends
			assert((arrlen(cm->wordskip_addraddr_arr) > 0) && "broken compiler?");
			const int skip_addraddr = arrpop(cm->wordskip_addraddr_arr);
			const int write_addr = compiler_addr(cm);
			assert((compiler_read(cm, skip_addraddr).u32 == 0xffffffffL) && "expected placeholder value");
			compiler_patch(cm, skip_addraddr, ((union enc){.u32=write_addr}));

			const int c = arrpop(cm->wordscope_stack_arr);
			assert(c >= 0);
			const int n = arrlen(cm->wordvis_stack_arr);
			if (n > c) {
				for (int i=c; i<n; ++i) {
					const int index = cm->wordvis_stack_arr[i];
					const char* key = cm->word_lut[index].key;
					assert(1 == shdel(cm->word_lut, key));
				}
				arrsetlen(cm->wordvis_stack_arr, c);
			}
		}	break;

		#define X(E,_S,_D) case E: do_encode_opcode = OP_##E; break;
		LIST_OF_OP_WORDS
		#undef X

		default: assert(!"TODO add missing built-in word handler"); break;
		}

		if (do_encode_opcode != _OP_NONE_) compiler_encode_opcode(cm, do_encode_opcode);

	} else if (parse_number(word, &number)) {
		//printf("TODO number w0rd [%s]\n", word); // TODO lut word
		compiler_encode_opcode(cm, OP_ILITERAL); // XXX can also be OP_FLITERAL
		compiler_encode(cm, ((union enc){.u32=0})); // XXX need to parse number!
	} else if ((wi = shgeti(cm->word_lut, word)) >= 0) {
		struct word_info* info = &cm->word_lut[wi].value;
		if (!info->is_comptime) {
			compiler_encode_opcode(cm, OP_JSR);
			compiler_encode(cm, ((union enc){.u32=info->addr}));
		} else {
			assert(!"TODO call comptime word");
		}
	} else {
		compiler_errorf(cm, "undefined word [%s]", word);
		return;
	}
}

static void compiler_push_char(struct compiler* cm, struct thicchar ch)
{
	if (cm->has_error) return;

	const int c = ch.codepoint;
	if (c == '\n') {
		++cm->location.line;
		cm->location.column = 0;
	} else {
		++cm->location.column;
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

	struct vmii* vm = cm->vm;
	if (cm->has_error) {
		vm->has_compile_error = 1;
		vm->compile_error = cm->error_message;
		vm->compile_error_location = cm->error_location;
	} else {
		vm->has_compile_error = 0;
		vm->compile_error = NULL;
		memset(&vm->compile_error_location, 0, sizeof vm->compile_error_location);
	}

	// stats
	#if 1
	const int num_runtime_ops = arrlen(cm->runtime_opstream.op_arr);
	const int num_comptime_ops = arrlen(cm->comptime_opstream.op_arr);
	printf("COMPILER STATS\n");
	printf(" allocated scratch : %zd/%zd\n", cm->scratch_header->allocated, cm->scratch_header->capacity);
	printf("       runtime ops : %d\n", num_runtime_ops);
	printf("      comptime ops : %d\n", num_comptime_ops);
	#endif

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
