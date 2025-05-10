// mii (forth-like language) compiler and vmii virtual machine
//  - see mii_selftest() near bottom for small programs that test compiler+vm
//  - see built-in word definitions near top of this file
//  - copious use of macros, including "X macros". search for e.g. BINOP for
//    how binary operators (like `a+b`/`a b +`) are implemented with macros
//  - compilation and program execution (vmii) uses "scratch memory" to
//    simplify memory mangement. this implies that 1) you can have "complex
//    lifetimes" without worrying about memory leaks because you don't need to
//    free individual allocations (in fact, the scratch allocator's fn_free()
//    is a no-op) and, 2) memory allocation is very fast (see scratch_realloc()
//    in allocator.c; O(1) although upsizing realloc()s do a O(N) memcpy()).
//    since we don't want resource-heavy and/or long-running compilations
//    and/or executions in a live-coding environment, scratch memory seems like
//    a good fit.

#include <stdio.h> // XXX remove me eventually?
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
#include "stb_divide.h"
#include "stb_ds.h" // NOTE we use various allocators, so no stb_ds_sysalloc.h
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


// these have direct mappings to vmii VM ops
#define LIST_OF_OP_WORDS \
	/*<ENUM>      <STR>     <DOC> */ \
	/* ==========,==========,============================================== */ \
	X( NOP       , NULL     , "No operation ( -- )") \
	X( RETURN    , "return" , "Return from subroutine [returnaddr -- ]") \
	X( DROP      , "drop"   , "Remove top element from stack (x --)") \
	X( PICK      , "pick"   , "Duplicate stack value (n -- stack[-1-n])") \
	X( ROLL      , "roll"   , "Pop n, rotate left n+1 items on stack (example : a b c 2 -- b c a)") \
	X( LLOR      , "llor"   , "Pop n, rotate right n+1 items on stack (example: a b c 2 -- c a b)") \
	X( EQ        , "="      , "Equals (x y -- x=y)") \
	X( TYPEOF    , NULL     , "Get type (x -- typeof(x))") \
	X( CAST      , NULL     , "Set type (x T -- T(x))") \
	X( HERE      , "here"   , "Push current instruction pointer to rstack [-- ip]") \
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
	X( FNE       , "F!="    , "Floating-point not equal (x y -- x!=y)") \
	X( FEQ       , "F="     , "Floating-point equal (x y -- x=y)") \
	X( FGE       , "F>="    , "Floating-point greater than or equal (x y -- x>=y)") \
	X( FGT       , "F>"     , "Floating-point greater than (x y -- x>y)") \
	/* ==========,==========,============================================== */ \
	X( IADD      , "I+"     , "Integer add (x y -- x+y)") \
	X( INEG      , "I~"     , "Integer negate (x -- -x)") \
	X( IMUL      , "I*"     , "Integer multiply (x y -- x*y)") \
	X( IDIV      , "I/"     , "Integer euclidean division (x y -- x//y)") \
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
	X( INE       , "I!="    , "Integer equal (x y -- x!=y)") \
	X( IGE       , "I>="    , "Integer greater than or equal (x y -- x>=y)") \
	X( IGT       , "I>"     , "Integer greater than (x y -- x>y)") \
	/* ==========,==========,============================================== */ \
	X( ARRNEW    , NULL     , "Create array (-- xs)") \
	X( ARRLEN    , NULL     , "Length of array (xs -- len(xs))") \
	X( ARRPUT    , NULL     , "Append item to array ([..] x -- [..x])") \
	/* TODO more array words */ \
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

enum opcode {
	// ops that have a corresponding word (1:1)
	#define X(E,_S,_D) OP_##E,
	LIST_OF_OP_WORDS
	#undef X

	// "word-less ops", ops that are indirectly encoded with "syntax", in part
	// because they have "arguments"
	_FIRST_WORDLESS_OP_,
	OP_INT_LITERAL = _FIRST_WORDLESS_OP_, // integer literal, followed by i32
	OP_FLOAT_LITERAL, // floating-point literal, followed by f32
	OP_JMP, // unconditional jump, followed by address
	OP_JMP0, // conditional jump, pops x, jumps if x=0. followed by address.
	OP_JSR, // jump to subroutine, followed by address

	_NUM_OPS_,
	_OP_NONE_
};

static_assert(OP_NOP==0,"expected NOP to be first (XXX but why?)");

static const char* get_opcode_str(enum opcode opcode)
{
	switch (opcode) {

	#define X(E,_S,_D) case OP_##E: return #E;
	LIST_OF_OP_WORDS
	#undef X

	case OP_INT_LITERAL: return "INT_LITERAL";
	case OP_FLOAT_LITERAL: return "FLOAT_LITERAL";
	case OP_JMP     : return "JMP";
	case OP_JMP0    : return "JMP0";
	case OP_JSR     : return "JSR";

	default: return "<?>";

	}
	assert(!"unreachable");
}

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
		++i; \
	}
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
	unsigned is_sealed   :1; // 0 until after ";"
	int addr;
};

union pword {
	uint32_t u32;
	int32_t  i32;
	float    f32;
};
static_assert(sizeof(union pword)==sizeof(uint32_t),"");

struct program {
	union pword* op_arr;
	int entrypoint_address;
	// TODO: compile error?
};

struct compiler {
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
	int* word_index_arr;
	int* wordscope0_arr;
	int* wordskip_addraddr_arr;
	int prefix_word, remaining_suffix_words;
	int next_user_word_id;
	int is_comptime;
	struct program comptime_program;
	struct program* runtime_program;

	// fields below this line are kept (not cleared) between compilations. new
	// fields must be explicitly added to and handled in compiler_begin().
	char* error_message;
};

static inline int val2int(struct val v, int* out_int)
{
	int i;
	switch (v.type) {
	case VAL_INT:   i=v.i32; break;
	case VAL_FLOAT: i=(int)roundf(v.f32); break;
	default: return 0;
	}
	if (out_int) *out_int=i;
	return 1;
}

static inline int val2float(struct val v, float* out_float)
{
	int f;
	switch (v.type) {
	case VAL_INT:   f=v.i32; break;
	case VAL_FLOAT: f=v.f32; break;
	default: return 0;
	}
	if (out_float) *out_float=f;
	return 1;
}

struct vmii {
	// TODO?
	//  - globals?
	//  - instruction counter/remaining (for cycle limiting)
	struct val* stack_arr;
	uint32_t* rstack_arr;
	int pc, error_pc;
	struct program* program;
	int has_error;
	char* error_message;
};

static struct {
	//mtx_t program_alloc_mutex;
	struct program* program_arr;
	int* program_freelist_arr;
	unsigned globals_were_initialized  :1;
} g;

THREAD_LOCAL static struct {
	struct compiler compiler;
	struct allocator scratch_allocator;
	struct scratch_context_header* scratch_header;
	struct vmii vmii;
	const char* error_message;
	unsigned thread_locals_were_initialized  :1;
} tlg; // thread-local globals

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
	tlg.error_message = cm->error_message;
}

static void reset_our_scratch(void)
{
	reset_scratch(tlg.scratch_header);
}

static void compiler_begin(struct compiler* cm, struct program* program)
{
	// clear cm, but keep some field(s)
	char* keep0 = cm->error_message;
	memset(cm, 0, sizeof *cm);
	cm->error_message = keep0;

	if (cm->error_message == NULL) {
		cm->error_message = calloc(MAX_ERROR_MESSAGE_SIZE, sizeof *cm->error_message);
	}

	reset_our_scratch();

	assert(cm->word_lut == NULL);
	sh_new_strdup_with_context(cm->word_lut, &tlg.scratch_allocator);
	assert(cm->word_index_arr == NULL);
	arrinit(cm->word_index_arr, &tlg.scratch_allocator);
	assert(cm->wordscope0_arr == NULL);
	arrinit(cm->wordscope0_arr, &tlg.scratch_allocator);
	assert(cm->wordskip_addraddr_arr == NULL);
	arrinit(cm->wordskip_addraddr_arr, &tlg.scratch_allocator);
	assert(cm->comptime_program.op_arr == NULL);
	arrinit(cm->comptime_program.op_arr, &tlg.scratch_allocator);

	cm->runtime_program = program;
	if (cm->runtime_program->op_arr == NULL) {
		arrinit(cm->runtime_program->op_arr, &system_allocator); // << system allocator
		arrreset(cm->runtime_program->op_arr);
	}

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
	// TODO 0x... 0b...? integer only? Q: should "i"-suffix be mandatory for
	// consistency?

	// NOTE this is "utf8 unaware", but it should be fine because:
	//  - supported numbers use an ASCII subset
	//  - utf8 multi-byte sequences are always distinct (high bit set) from
	//    ASCII characters (the encoding is also documented in utf8.h if you're
	//    curious)

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
	int type=OP_FLOAT_LITERAL;
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
			type=OP_INT_LITERAL;
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

	case OP_FLOAT_LITERAL: {
		if (ni==0 && nf==0) return 0; // no digits
		const double log10 = log(10.0);
		double v = ((double)vi + vf*exp(-nf*log10)) * exp((negative_exp?-vx:vx)*log10);
		if (negative) v = -v;
		number.f32 = (float)v;
		//printf("parse_number => f32 %f\n", number.f32);
	}	break;

	case OP_INT_LITERAL: {
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

static inline struct program* compiler_program(struct compiler* cm)
{
	return cm->is_comptime ? &cm->comptime_program : cm->runtime_program;
}

static int compiler_addr(struct compiler* cm)
{
	return arrlen(compiler_program(cm)->op_arr);
}

static union pword compiler_read(struct compiler* cm, int addr)
{
	return arrchkget(compiler_program(cm)->op_arr, addr);
}

static int compiler_write(struct compiler* cm, union pword pword)
{
	const int addr = compiler_addr(cm);
	arrput(compiler_program(cm)->op_arr, pword);
	return addr;
}

static void compiler_patch(struct compiler* cm, int addr, union pword pword)
{
	struct program* os = compiler_program(cm);
	arrchk(os->op_arr, addr);
	os->op_arr[addr] = pword;
}

static int compiler_write_opcode(struct compiler* cm, enum opcode op)
{
	assert((0<=op) && (op<_NUM_OPS_));
	return compiler_write(cm, ((union pword){ .i32=op }));
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
				.is_sealed = 0,
			}));
			arrput(cm->word_index_arr, word_index);
			arrput(cm->wordscope0_arr, arrlen(cm->word_index_arr));

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
			compiler_write_opcode(cm, OP_JMP);
			// insert invalid placeholder address (we don't know it yet)
			const int placeholder_addraddr = compiler_write(cm, ((union pword){.u32=0xffffffffL}));
			// remember address of the placeholder address, so we can patch it
			// when we see a semicolon:
			arrput(cm->wordskip_addraddr_arr, placeholder_addraddr);
		}	break;

		case SEMICOLON: {
			if (arrlen(cm->wordscope0_arr) == 0) {
				compiler_errorf(cm, "too many semi-colons");
				return;
			}
			compiler_write_opcode(cm, OP_RETURN);

			// finalize word skip jump by overwriting the placeholder address
			// now that we know where the word ends
			assert((arrlen(cm->wordskip_addraddr_arr) > 0) && "broken compiler?");
			const int skip_addraddr = arrpop(cm->wordskip_addraddr_arr);
			const int write_addr = compiler_addr(cm);
			assert((compiler_read(cm, skip_addraddr).u32 == 0xffffffffL) && "expected placeholder value");
			compiler_patch(cm, skip_addraddr, ((union pword){.u32=write_addr}));

			const int c = arrpop(cm->wordscope0_arr);
			assert(c >= 0);
			const int n = arrlen(cm->word_index_arr);
			assert(n>0);
			const int closing_word_index = cm->word_index_arr[n-1];
			assert(closing_word_index >= 0);
			struct word_info* info = &cm->word_lut[closing_word_index].value;
			info->is_sealed = 1;

			if (n > c) {
				// remove inner words from scope
				for (int i=(n-1); i>=c; --i) {
					const int index = cm->word_index_arr[i];
					const char* key = cm->word_lut[index].key;
					assert(shdel(cm->word_lut, key) && "expected to delete word, but key was not found");
				}
				arrsetlen(cm->word_index_arr, c);
			}
		}	break;

		#define X(E,_S,_D) case E: do_encode_opcode = OP_##E; break;
		LIST_OF_OP_WORDS
		#undef X

		default: assert(!"TODO add missing built-in word handler"); break;
		}

		if (do_encode_opcode != _OP_NONE_) compiler_write_opcode(cm, do_encode_opcode);

	} else if (parse_number(word, &number)) {
		compiler_write_opcode(cm, number.type);
		switch (number.type) {
		case OP_INT_LITERAL: compiler_write(cm, ((union pword){.i32=number.i32})); break;
		case OP_FLOAT_LITERAL: compiler_write(cm, ((union pword){.f32=number.f32})); break;
		default: assert(!"unhandled case");
		}
	} else if ((wi = shgeti(cm->word_lut, word)) >= 0) {
		struct word_info* info = &cm->word_lut[wi].value;
		if (!info->is_comptime) {
			compiler_write_opcode(cm, OP_JSR);
			compiler_write(cm, ((union pword){.u32=info->addr}));
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
	if (cm->has_error) return;

	if (cm->remaining_suffix_words > 0) {
		compiler_errorf(cm, "EOF while expecting %d more suffix word(s)", cm->remaining_suffix_words);
		return;
	}

	const int depth = arrlen(cm->wordscope0_arr);
	if (depth > 0) {
		compiler_errorf(cm, "EOF inside word definition (depth=%d)", depth);
		return;
	}

	#if 0
	// stats
	const int num_runtime_ops  = arrlen(cm->runtime_program->op_arr);
	const int num_comptime_ops = arrlen(cm->comptime_program.op_arr);
	printf("COMPILER STATS\n");
	printf(" allocated scratch : %zd/%zd\n", tlg.scratch_header->allocated, tlg.scratch_header->capacity);
	printf("       runtime ops : %d\n", num_runtime_ops);
	printf("      comptime ops : %d\n", num_comptime_ops);
	#endif

	assert(cm->word_lut != NULL);
	hmfree(cm->word_lut);
	assert(cm->word_lut == NULL);
}

static int alloc_program_index(void)
{
	//assert(thrd_success == mtx_lock(&g.program_alloc_mutex));
	int r;
	struct program* program;
	if (arrlen(g.program_freelist_arr) > 0) {
		r = arrpop(g.program_freelist_arr);
		program = arrchkptr(g.program_arr, r);
	} else {
		r = arrlen(g.program_arr);
		program = arraddnptr(g.program_arr, 1);
		memset(program, 0, sizeof *program);
		arrinit(program->op_arr, &system_allocator);
	}
	//assert(thrd_success == mtx_unlock(&g.program_alloc_mutex));
	program->entrypoint_address = 0;
	arrsetlen(program->op_arr, 0);
	return r;
}

void mii_program_free(int program_index)
{
	//assert(thrd_success == mtx_lock(&g.program_alloc_mutex));
	// XXX this could be regarded as an "expensive assert"?
	const int n = arrlen(g.program_freelist_arr);
	for (int i=0; i<n; ++i) {
		assert((program_index != g.program_freelist_arr[i]) && "program double-free!");
	}
	arrput(g.program_freelist_arr, program_index);
	//assert(thrd_success == mtx_unlock(&g.program_alloc_mutex));
}

static struct program* get_program(int index)
{
	return arrchkptr(g.program_arr, index);
}

int mii_compile_thicc(const struct thicchar* src, int num_chars)
{
	const int program_index = alloc_program_index();
	struct compiler* cm = &tlg.compiler;
	compiler_begin(cm, get_program(program_index));
	for (int i=0; i<num_chars; ++i) {
		compiler_push_char(cm, src[i]);
		if (cm->has_error) return -1;
	}
	compiler_end(cm);
	if (cm->has_error) return -1;
	return program_index;
}

// with apologies to Frank Gray
int mii_compile_graycode(const char* utf8src, int num_bytes)
{
	const int program_index = alloc_program_index();
	struct compiler* cm = &tlg.compiler;
	const char* p = utf8src;
	int remaining = num_bytes;
	compiler_begin(cm, get_program(program_index));
	while (remaining > 0) {
		unsigned codepoint = utf8_decode(&p, &remaining);
		if (codepoint == -1) continue;
		compiler_push_char(cm, ((struct thicchar){.codepoint = codepoint}));
		if (cm->has_error) return -1;
	}
	compiler_end(cm);
	if (cm->has_error) return -1;
	return program_index;
}

const char* mii_error(void)
{
	return tlg.error_message;
}

static void global_init(void)
{
	if (g.globals_were_initialized) return;
	g.globals_were_initialized = 1;
	//assert(thrd_success == mtx_init(&g.program_alloc_mutex, mtx_plain));
	arrinit(g.program_arr, &system_allocator);
	arrinit(g.program_freelist_arr, &system_allocator);
}

void mii_thread_init(void)
{
	global_init();
	if (tlg.thread_locals_were_initialized) return;
	tlg.thread_locals_were_initialized = 1;
	tlg.scratch_header = init_scratch_allocator(&tlg.scratch_allocator, 1L<<24);
	struct vmii* vm = &tlg.vmii;
	memset(vm, 0, sizeof *vm);
	arrinit(vm->stack_arr, &tlg.scratch_allocator);
	arrinit(vm->rstack_arr, &tlg.scratch_allocator);
	vm->error_message = calloc(MAX_ERROR_MESSAGE_SIZE, sizeof *vm->error_message);
}

void vmii_reset2(struct vmii* vm, struct program* program)
{
	arrreset(vm->stack_arr);
	arrreset(vm->rstack_arr);
	vm->program = program;
	vm->pc = program->entrypoint_address;
}

void vmii_reset(int program_index)
{
	vmii_reset2(&tlg.vmii, get_program(program_index));
}

FORMATPRINTF2
void vmii_error(struct vmii* vm, const char* fmt, ...)
{
	char* p = vm->error_message;
	int r = MAX_ERROR_MESSAGE_SIZE;
	int n;
	va_list va;
	va_start(va, fmt);
	n = stbsp_vsnprintf(p, r, fmt, va);
	va_end(va);
	p+=n; r-=n;
	vm->error_pc = vm->pc;
	vm->has_error = 1;
	tlg.error_message = vm->error_message;
}

int vmii_run2(struct vmii* vm)
{
	const int TRACE = 0;

	struct program const* prg = vm->program;
	assert((prg != NULL) && "no program; forgot vmii_reset()?");
	const int prg_len = arrlen(prg->op_arr);
	int pc = vm->pc;
	int num_defer = 0;
	uint32_t deferred_op = 0;
	// TODO cycle limiting in while() condition?
	while ((0 <= pc) && (pc < prg_len)) {
		if (vm->has_error) return -1;

		union pword* pw = &prg->op_arr[pc];
		int next_pc = pc+1;
		int set_num_defer = 0;

		#define STACK_HEIGHT()  ((int)arrlen(vm->stack_arr))
		#define RSTACK_HEIGHT() ((int)arrlen(vm->rstack_arr))

		if (num_defer > 0) {
			--num_defer;
			switch (deferred_op) {

			case OP_JMP:
				if (TRACE) printf(" JMP pc %d->%d\n", pc, pw->u32);
				next_pc = pw->u32;
				break;

			case OP_JSR:
				if (TRACE) printf(" JSR pc %d->%d %d=>R\n", pc, pw->u32, pc+2);
				arrput(vm->rstack_arr, (pc+1));
				next_pc = pw->u32;
				break;

			case OP_INT_LITERAL:
				if (TRACE) printf(" INT_LITERAL %d\n", pw->i32);
				arrput(vm->stack_arr, ((struct val){
					.type = VAL_INT,
					.i32 = pw->i32,
				}));
				break;

			case OP_FLOAT_LITERAL:
				if (TRACE) printf(" FLITERAL %f\n", pw->f32);
				arrput(vm->stack_arr, ((struct val){
					.type = VAL_FLOAT,
					.f32 = pw->f32,
				}));
				break;

			default:
				fprintf(stderr, "vmii: unhandled deferred op %s (%u)\n", get_opcode_str(deferred_op), deferred_op);
				abort();
				break;
			}

			assert(set_num_defer == 0);
		} else {
			const uint32_t op = prg->op_arr[pc].u32;

			if (TRACE) printf("[%.4x] %.2x %s\n", pc, op, get_opcode_str(op));

			switch (op) {

			case OP_NOP:
				if (TRACE) printf(" NOP\n");
				break;

			// these ops take another argument
			case OP_JMP:
			case OP_JMP0:
			case OP_JSR:
			case OP_INT_LITERAL:
			case OP_FLOAT_LITERAL:
				set_num_defer = 1;
				break;

			case OP_RETURN: {
				if (RSTACK_HEIGHT() == 0) {
					vmii_error(vm, "rstack underflow due to RETURN at pc=%d\n", pc);
					return -1;
				}
				next_pc = arrpop(vm->rstack_arr);
				if (TRACE) printf(" RETURN pc -> %d\n", next_pc);
			}	break;

			case OP_DROP: {
				if (STACK_HEIGHT() == 0) {
					vmii_error(vm, "DROP called on empty stack");
					return -1;
				}
				(void)arrpop(vm->stack_arr);
			}	break;

			case OP_PICK: {
				if (STACK_HEIGHT() == 0) {
					vmii_error(vm, "PICK called on empty stack");
					return -1;
				}
				struct val vi = arrpop(vm->stack_arr);
				int i=0;
				if (!val2int(vi, &i)) {
					vmii_error(vm, "could not convert %x:%x to int", vi.type, vi.i32);
					return -1;
				}
				if (i<0) {
					vmii_error(vm, "`%d pick` - negative pick is invalid", i);
					return -1;
				}
				const int min_height = 1+i;
				if (STACK_HEIGHT() < min_height) {
					vmii_error(vm, "%d pick out-of-bounds; stack height is only %d", i, STACK_HEIGHT());
					return -1;
				}
				struct val vd = arrchkget(vm->stack_arr, STACK_HEIGHT()-1-i);
				arrput(vm->stack_arr, vd);
			}	break;


			// == floating-point binary ops ==

			#define DEF_FBINOP(ENUM, TYPE, ASSIGN) \
			case ENUM: { \
				if (STACK_HEIGHT() < 2) { \
					vmii_error(vm, "`%s` requires 2 arguments, stack height was %d", #ENUM, STACK_HEIGHT()); \
					return -1; \
				} \
				const struct val vb = arrpop(vm->stack_arr); \
				const struct val va = arrpop(vm->stack_arr); \
				const float a=va.f32; \
				const float b=vb.f32; \
				arrput(vm->stack_arr, ((struct val){ \
					.type=TYPE, \
					ASSIGN, \
				})); \
			}	break;
			DEF_FBINOP( OP_FADD , VAL_FLOAT , .f32=(a+b)        )
			DEF_FBINOP( OP_FMUL , VAL_FLOAT , .f32=(a*b)        )
			DEF_FBINOP( OP_FDIV , VAL_FLOAT , .f32=(a/b)        )
			DEF_FBINOP( OP_FMOD , VAL_FLOAT , .f32=(fmodf(a,b)) )
			DEF_FBINOP( OP_FLT  , VAL_INT   , .i32=(a<b)        )
			DEF_FBINOP( OP_FLE  , VAL_INT   , .i32=(a<=b)       )
			DEF_FBINOP( OP_FEQ  , VAL_INT   , .i32=(a==b)       )
			DEF_FBINOP( OP_FNE  , VAL_INT   , .i32=(a!=b)       )
			DEF_FBINOP( OP_FGE  , VAL_INT   , .i32=(a>=b)       )
			DEF_FBINOP( OP_FGT  , VAL_INT   , .i32=(a>b)        )
			#undef DEF_FBINOP

			// == floating-point unary ops ==

			#define DEF_FUNOP(ENUM, TYPE, ASSIGN) \
			case ENUM: { \
				if (STACK_HEIGHT() < 1) { \
					vmii_error(vm, "`%s` requires 1 argument, stack height was %d", #ENUM, STACK_HEIGHT()); \
					return -1; \
				} \
				const struct val va = arrpop(vm->stack_arr); \
				const float a=va.f32; \
				arrput(vm->stack_arr, ((struct val){ \
					.type=TYPE, \
					ASSIGN, \
				})); \
			}	break;
			DEF_FUNOP( OP_FNEG , VAL_FLOAT , .f32=(-a)     )
			DEF_FUNOP( OP_FINV , VAL_FLOAT , .f32=(1.0f/a) )
			#undef DEF_FUNOP

			// == integer binary ops ==

			#define DEF_IBINOP(ENUM, EXPR) \
			case ENUM: { \
				if (STACK_HEIGHT() < 2) { \
					vmii_error(vm, "`%s` requires 2 arguments, stack height was %d", #ENUM, STACK_HEIGHT()); \
					return -1; \
				} \
				const struct val vb = arrpop(vm->stack_arr); \
				const struct val va = arrpop(vm->stack_arr); \
				const int32_t a=va.i32; \
				const int32_t b=vb.i32; \
				arrput(vm->stack_arr, ((struct val){ \
					.type=VAL_INT, \
					.i32=(EXPR), \
				})); \
			}	break;
			DEF_IBINOP( OP_IADD    , (a+b)  )
			DEF_IBINOP( OP_IMUL    , (a*b)  )
			DEF_IBINOP( OP_IDIV    , (stb_div_eucl(a,b)) )
			DEF_IBINOP( OP_IMOD    , (stb_mod_eucl(a,b)) )
			DEF_IBINOP( OP_IAND    , (a&b)  )
			DEF_IBINOP( OP_IOR     , (a|b)  )
			DEF_IBINOP( OP_IXOR    , (a^b)  )
			DEF_IBINOP( OP_ILSHIFT , (a<<b) )
			DEF_IBINOP( OP_IRSHIFT , (a>>b) )
			DEF_IBINOP( OP_ILT     , (a<b)  )
			DEF_IBINOP( OP_ILE     , (a<=b) )
			DEF_IBINOP( OP_IEQ     , (a==b) )
			DEF_IBINOP( OP_INE     , (a!=b) )
			DEF_IBINOP( OP_IGE     , (a>=b) )
			DEF_IBINOP( OP_IGT     , (a>b)  )
			#undef DEF_IBINOP

			// == integer unary ops ==

			#define DEF_IUNOP(ENUM, EXPR) \
			case ENUM: { \
				if (STACK_HEIGHT() < 1) { \
					vmii_error(vm, "`%s` requires 1 argument, stack height was %d", #ENUM, STACK_HEIGHT()); \
					return -1; \
				} \
				const struct val va = arrpop(vm->stack_arr); \
				const int32_t a=va.i32; \
				arrput(vm->stack_arr, ((struct val){ \
					.type=VAL_INT, \
					.i32=(EXPR), \
				})); \
			}	break;
			DEF_IUNOP( OP_INEG , (-a) )
			DEF_IUNOP( OP_INOT , (~a) )
			#undef DEF_IUNOP

			// ==============================

			default:
				fprintf(stderr, "vmii: unhandled op %s (%u) at pc=%d\n", get_opcode_str(op), op, pc);
				abort();
				break;
			}

			if (set_num_defer > 0) {
				num_defer = set_num_defer;
				deferred_op = op;
			}
		}

		pc = next_pc;
	}
	return STACK_HEIGHT();
}

int vmii_run(void)
{
	return vmii_run2(&tlg.vmii);
}

int vmii_get_stack_height(void)
{
	struct vmii* vm = &tlg.vmii;
	return arrlen(vm->stack_arr);
}

struct val vmii_val(int i)
{
	struct vmii* vm = &tlg.vmii;
	struct val v = arrchkget(vm->stack_arr, arrlen(vm->stack_arr)-1-i);
	return v;
}

static void selftest_fail(const char* context, const char* src)
{
	fprintf(stderr, "selftest %s error [%s] for:\n %s\n", context, mii_error(), src);
	abort();
}

static void selftest_dump_val(struct val v)
{
	FILE* out = stderr;
	switch (v.type) {
	case VAL_INT:    fprintf(out, "%di", v.i32); break;
	case VAL_FLOAT:  fprintf(out, "%f", v.f32); break;
	default:         fprintf(out, "%uT:%u(?)", v.type, v.u32); break;
	}
}

static void selftest_dump_stack(void)
{
	const int d = vmii_get_stack_height();
	fprintf(stderr, "=== STACK ===\n");
	for (int i=0; i<d; ++i) {
		struct val v = vmii_val(i);
		fprintf(stderr, " stack[%d] = ", (-1-i));
		selftest_dump_val(v);
		fprintf(stderr, "\n");
	}
}

void mii_selftest(void)
{
	mii_thread_init();

	const char* programs_that_fail_to_compile[] = {
		"xxxxxxxxxxxx", // word not defined
		// word not closed:
		":",
		": foo ",
		": foo 42",
		// various unbalanced comments:
		"42 (",
		"42 ((",
		"42 (()",
		"42 ())",
		"42 (()))",
	};

	for (int i=0; i<ARRAY_LENGTH(programs_that_fail_to_compile); ++i) {
		const char* src = programs_that_fail_to_compile[i];
		const int prg = mii_compile_graycode(src, strlen(src));
		if (prg != -1) {
			fprintf(stderr, "selftest expected compile error, but didn't get it for:\n %s\n", src);
			abort();
		}
	}

	// TODO test runtime errors?

	// these must all leave 1i (and only 1i) on the stack after execution
	const char* programs_that_eval_to_1i[] = {
		"1i",
		"NOP 1i (blah (blah)) NOP",
		"1i 1i I=",
		"1i 0i I!=",
		"-1i I~",
		"1i 0i pick drop",
		"1i I~ I~",
		"-1i 2i I+",
		"1 1 F=",
		"1 -1 F~ F=",
		"-1 F~ 1 F=",
		// NOTE euclidean integer division tests (e.g. -3/4=-1); some would
		// fail with "truncating integer division" (-3/4=0) (you can try this
		// for yourself by changing `stb_div_eucl(a,b)` to `a/b` somewhere
		// above, assuming your CPU/compiler uses truncating division)
		"7i 4i I/",
		"-3i 4 I/ I~",
		"-5i -7i I/",
		": fsqr 0 pick F* ; 42 fsqr 1764 F=",
		": fsqr 0 : foo 101 ; pick F* ; -42 fsqr 1764 F=",
		"-1i : foo 101 drop ; foo foo I~ foo foo",
	};

	for (int i=0; i<ARRAY_LENGTH(programs_that_eval_to_1i); ++i) {
		const char* src = programs_that_eval_to_1i[i];
		const int prg = mii_compile_graycode(src, strlen(src));
		if (prg == -1) selftest_fail("compile", src);
		vmii_reset(prg);
		const int r = vmii_run();
		if (r == -1) selftest_fail("run", src);
		if (r != 1) {
			fprintf(stderr, "selftest assert fail, expected stack height of 1\n");
			selftest_dump_stack();
			abort();
		}
		struct val v = vmii_val(0);
		if (v.type != VAL_INT || v.i32 != 1) {
			fprintf(stderr, "selftest expected one stack element, 1i, got: ");
			selftest_dump_val(v);
			fprintf(stderr, " for:\n %s\n", src);
			abort();
		}
	}
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
