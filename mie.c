// mie (forth-like language) compiler and vmie virtual machine
//  - see mie_selftest() near bottom for small programs that test compiler+vm
//  - see built-in word definitions near top of this file
//  - copious use of macros, including "X macros". search for e.g. BINOP for
//    how binary operators (like `a+b`/`a b +`) are implemented with macros

// the vm (vmie) and the compiler (which also runs a compile-time vmie) uses a
// "scratch allocator" (aka "arena allocator") which does a couple of things:
//  - it lets in the memory corruption bugs from hell,.. but hear me out!
//  - it lets you code as if you had a garbage collector when it comes to
//    temporary memory
//  - it can (TODO) very likely be used to implement simple/fast compiler/vm
//    savestates and restores, each with a single memcpy() ("skipping stdlib")

// memory corruption may occur when a "temporary allocation" (see:
// scratch_alloc() / get_scratch_allocator()) is used after a
// free_all_our_scratch_allocations()-call, after which the same memory region
// can be allocated again. ouch!

// the compiler and vm(/vmie) state is global, but thread local, i.e. you have
// one instance of them per thread. this is because compilation plus program
// execution should always feel instant (<20ms?); it's not designed for
// long-running programs, so there's no point in having more than one instance
// per thread at that timescale. it also means we only need one scratch
// allocator per thread.

#include <stdio.h> // XXX remove me eventually?
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <limits.h>
#include <math.h>

#include "mie.h"
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
//  - <ENUM>: enum identifier for the word (ops also get an "OP_"-prefix)
//  - <STR>: string name, or NULL to use the string representation of <ENUM>
//  - <DOC>: short documentation of the word/op

#define LIST_OF_SYNTAX_WORDS \
	/*<ENUM>      <STR>           <DOC> */ \
	/* ====================================================================== */ \
	X( COLON       , ":"        , "Word definiton, followed by word (name), e.g. `: foo ... ;`") \
	X( COLONADDR   , ":&"       , "Address-word definiton, followed by word (name), e.g. `:& foo_addr ... ;`") \
	X( COLONLAMBDA , ":->"      , "Inline word; pushes its address; `:-> ... ; ( -- addr )`") \
	X( SEMICOLON   , ";"        , "End of word definition") \
	X( COMPTIME    , "comptime" , "Compile-time prefix") \
	X( ENTER_SEW   , "<#"       , "Increase sew-depth") \
	X( LEAVE_SEW   , "#>"       , "Decrease sea-depth") \
	/* ====================================================================== */

// `comptime` before colon defines a "comptime word"; the compiler generates
// code directly as it parses the source, but when it sees a call to a
// "comptime word", it immediately executes it in the comptime VM. calling "sew
// words" within a comptime word writes or modifies the program underneath it.
// the `there` word gets the code write position, and `navigate` sets it. all
// this allows you to define syntax (like if/else/then) in the language itself!
// (all this is somewhat related to the `immediate`-keyword [and `postpone?`]
// in forth, and the "comptime" word comes from zig)

// `comptime` before a word (or literal) executes it in the comptime VM. e.g.
// `comptime 5` immediately pushes 5.0 in the comptime VM. it can be used to
// push arguments for comptime words.

// "sew syntax", <# ... #>, generates code-generating-code (!), and can
// actually be nested for extra confusion (so code-generating
// code-genrta... sewception!!) but:
//   comptime : square <# dup * #> ;
// is like an inline version of:
//            : square    dup *    ;
// they basically do the same thing, but the comptime version "burns" `dup *`
// into the code, so there's no call ("JSR") at runtime. another cool comptime
// example:
//   comptime : square-root-of-2  2 FSQRT SEW-LIT ;
// this inserts the /result/ of sqrt(2) into the code whereever it sees
// `square-root-of-2`, so equivalent to typing 1.41421 basically, and doesn't
// execute the square-root operation at runtime.



// the words below have direct 1:1 mappings to vmie VM ops.
// `foo:i32` means `foo` is bitwise ("reinterpret") cast to i32, i.e. without
// any type checking. by convention, words that do bitwise casting should not
// be lowercased since that "namespace" is reserved for typesafe words (so
// "pick" can work with both i32 and f32 indices, but "PICK" assumes i32
// without checking)
// new column <#MIN> is the minimum stack height required by the op
#define LIST_OF_OP_WORDS \
	/*<ENUM>      <STR>        <#MIN>  <DOC> */ \
	/* ====================================================================== */ \
	X( NOP        , NULL         , 0 , "No operation ( -- )") \
	X( HALT       , "halt"       , 0 , "Halt execution with an optional error string") \
	X( RETURN     , "return"     , 0 , "Return from subroutine [returnaddr -- ]") \
	X( DROP       , "drop"       , 1 , "Remove top element from stack (x --)") \
	X( PICK       , NULL         , 1 , "Pop n:i32, duplicate nth stack value from top (n -- stack[-1-n])") \
	X( ROTATE     , NULL         , 2 , "Pop d:i32, then n:i32, then rotate n elements d places left") \
	X( EQ         , "=="         , 2 , "Equals (x y -- x==y)") \
	X( TYPEOF     , "typeof"     , 1 , "Get type (x -- typeof(x))") \
	X( CAST       , NULL         , 2 , "Set type (x T -- T(x))") \
	X( HERE       , "here"       , 0 , "Push current instruction pointer to stack (-- ip)") \
	X( JMPI       , NULL         , 1 , "Pop address:i32 from stack => indirect jump (address -- )" ) \
	X( JSRI       , NULL         , 1 , "Pop address:i32 from stack => indirect jump-to-subroutine (address --)" ) \
	X( F2I        , "F>I"        , 1 , "Pop a:f32, push after conversion to i32 (a:f32 -- i)") \
	X( I2F        , "I>F"        , 1 , "Pop a:i32, push after conversion to f32 (a:i32 -- f)") \
	X( SET_GLOBAL , "SET-GLOBAL" , 2 , "Pop index:i32, then value, set globals[index]=value (value index --)") \
	X( GET_GLOBAL , "GET-GLOBAL" , 1 , "Pop index:i32, push globals[index] (index -- globals[index])") \
	/* ====================================================================== */ \
	/* All inputs are x:f32/y:f32 (bitwise cast to f32) */ \
	X( FADD      , "F+"       , 2 , "Floating-point add (x y -- x+y)") \
	X( FNEG      , "F~"       , 1 , "Floating-point negate (x -- -x)") \
	X( FMUL      , "F*"       , 2 , "Floating-point multiply (x y -- x*y)") \
	X( FMOD      , "F%"       , 2 , "Floating-point remainder/modulus (x y -- x%y)") \
	X( FINV      , "F1/"      , 1 , "Floating-point reciprocal (x -- 1/x)") \
	X( FDIV      , "F/"       , 2 , "Floating-point division (x y -- x/y)") \
	X( FCOS      , NULL       , 1 , "(x -- cos(x))") \
	X( FSIN      , NULL       , 1 , "(x -- sin(x))") \
	X( FTAN      , NULL       , 1 , "(x -- tan(x))") \
	X( FACOS     , NULL       , 1 , "(x -- acos(x))") \
	X( FASIN     , NULL       , 1 , "(x -- asin(x))") \
	X( FATAN     , NULL       , 1 , "(x -- atan(x))") \
	X( FATAN2    , NULL       , 2 , "(x y -- atan2(x,y))") \
	X( FEXP      , NULL       , 1 , "(x -- exp(x))") \
	X( FLOG      , NULL       , 1 , "(x -- log(x))") \
	X( FPOW      , NULL       , 2 , "(x y -- pow(x,y))") \
	X( FSQRT     , NULL       , 1 , "(x -- sqrt(x))") \
	X( FFLOOR    , NULL       , 1 , "(x -- floor(x))") \
	X( FCEIL     , NULL       , 1 , "(x -- ceil(x))") \
	X( FROUND    , NULL       , 1 , "(x -- round(x))") \
	X( FABS      , NULL       , 1 , "(x -- abs(x))") \
	X( FLT       , "F<"       , 2 , "Floating-point less than (x y -- x<y)") \
	X( FLE       , "F<="      , 2 , "Floating-point less than or equal (x y -- x<=y)") \
	X( FNE       , "F!="      , 2 , "Floating-point not equal (x y -- x!=y)") \
	X( FEQ       , "F="       , 2 , "Floating-point equal (x y -- x=y)") \
	X( FGE       , "F>="      , 2 , "Floating-point greater than or equal (x y -- x>=y)") \
	X( FGT       , "F>"       , 2 , "Floating-point greater than (x y -- x>y)") \
	/* ====================================================================== */ \
	/* All inputs are x:i32/y:i32 (bitwise cast to i32) */ \
	X( IADD      , "I+"       , 2 , "Integer add (x y -- x+y)") \
	X( INEG      , "I~"       , 1 , "Integer negate (x -- -x)") \
	X( IMUL      , "I*"       , 2 , "Integer multiply (x y -- x*y)") \
	X( IDIV      , "I/"       , 2 , "Integer euclidean division (x y -- x//y)") \
	X( IMOD      , "I%"       , 2 , "Integer euclidean remainder/modulus (x y -- x%y)") \
	X( IABS      , NULL       , 1 , "(x -- abs(x))") \
	X( IBAND     , "I&"       , 2 , "Integer bitwise AND (x y -- x&y)") \
	X( IBOR      , "I|"       , 2 , "Integer bitwise OR (x y -- x|y)") \
	X( IBXOR     , "I^"       , 2 , "Integer bitwise XOR (x y -- x^y)") \
	X( IBNOT     , "I!"       , 2 , "Integer bitwise NOT (x -- !y)") \
	X( ILAND     , "I&&"      , 2 , "Integer logical AND (x y -- x&&y)") \
	X( ILOR      , "I||"      , 2 , "Integer logical OR (x y -- x||y)") \
	X( ILXOR     , "I^^"      , 2 , "Integer logical XOR (x y -- x^^y)") \
	X( ILNOT     , "I!!"      , 1 , "Integer logical NOT (x -- !!y)") \
	X( ILSHIFT   , "I<<"      , 2 , "Integer shift left (x y -- x<<y)") \
	X( IRSHIFT   , "I>>"      , 2 , "Integer shift right (x y -- x>>y)") \
	X( ILT       , "I<"       , 2 , "Integer less than (x y -- x<y)") \
	X( ILE       , "I<="      , 2 , "Integer less than or equal (x y -- x<=y)") \
	X( IEQ       , "I="       , 2 , "Integer equal (x y -- x=y)") \
	X( INE       , "I!="      , 2 , "Integer equal (x y -- x!=y)") \
	X( IGE       , "I>="      , 2 , "Integer greater than or equal (x y -- x>=y)") \
	X( IGT       , "I>"       , 2 , "Integer greater than (x y -- x>y)") \
	/* ====================================================================== */ \
	X( STRLEN    , "strlen"   , 1 , "Get length of string (s -- strlen(s))") \
	X( STRCOMPS  , NULL       , 2 , "Get string components at index (s i:32 -- s[i].codepoint s[i].rgbx)") \
	X( STRNEW    , NULL       , 1 , "Make string from [codepoint,rgbx] component pairs on stack ( ... n:i32 -- s )") \
	X( STRCATN   , NULL       , 1 , "Pop n:i32 and join n strings e.g. ( a b 2i -- strjoin(a,b) )") \
	/* ====================================================================== */ \
	/* convention (not sure if it's a good one): if it only reads the array */ \
	/* it consumes the array; if it mutates the array, the array is the */ \
	/* bottommost return value */ \
	X( ARRNEW    , "arrnew"   , 0 , "Create array (-- arr)") \
	X( ARRLEN    , "arrlen"   , 1 , "Length of array (arr -- len(arr))") \
	X( ARRGET    , NULL       , 2 , "Get element (arr i -- arr[i])") \
	X( ARRPUT    , "arrput"   , 2 , "Append item to array ([x] y -- [x,y])") \
	X( ARRPOP    , "arrpop"   , 1 , "Get top element ([x,y] -- [x] y)") \
	X( ARRSET    , NULL       , 3 , "Set element (arr index value -- arr)") \
	X( ARRJOIN   , "arrjoin"  , 2 , "Join two arrays (arr1 arr2 -- arr1..arr2)") \
	/* FIXME convert ARRJOIN to ARRCATN modelled after STRCATN? */ \
	X( ARRSPLIT  , NULL       , 2 , "Split array at pivot (arr pivot -- arr[0:pivot] arr[pivot:])") \
	/* ====================================================================== */ \
	X( MAPNEW    , "mapnew"   , 0 , "Create map (-- map)") \
	X( MAPHAS    , "maphas"   , 2 , "Returns if key exists in map (map key -- exists?)") \
	X( MAPGET    , "mapget"   , 2 , "Get value from map (map key -- value)") \
	X( MAPSET    , "mapset"   , 3 , "Set value in map (map key value -- map)") \
	X( MAPDEL    , "mapdel"   , 2 , "Delete key from map (map key -- map)") \
	/* ====================================================================== */ \
	X( THERE         , "there"         , 0 , "Get PC outside of comptime (-- addr)") \
	X( NAVIGATE      , "navigate"      , 1 , "Set PC outside of comptime (addr --)") \
	X( SEW           , NULL            , 1 , "Write raw i32 or f32 outside of comptime (val --)") \
	/* XXX should the following be stdlib stuff? */ \
	X( SEW_JMP       , "SEW-JMP"       , 1 , "Write JMP outside of comptime (addr --)") \
	X( SEW_JMP0      , "SEW-JMP0"      , 1 , "Write JMP0 outside of comptime (addr --)") \
	X( SEW_JSR       , "SEW-JSR"       , 1 , "Write JSR outside of comptime (addr --)") \
	X( SEW_ADDR      , "SEW-ADDR"      , 1 , "Write jump address outside of comptime (addr --)") \
	X( SEW_LIT       , "SEW-LIT"       , 1 , "Write literal outside of comptime (lit --)") \
	X( SEW_COLON     , "SEW-COLON"     , 1 , "Begin word def outside of comptime (name --)") \
	X( SEW_SEMICOLON , "SEW-SEMICOLON" , 0 , "End word def outside of comptime") \
	/* ====================================================================== */


enum builtin_word {
	_NO_WORD_=0,
	#define FIRST_WORD (_NO_WORD_ + 1)
	#define X(E,...) E,
	LIST_OF_SYNTAX_WORDS
	LIST_OF_OP_WORDS
	#undef X
	_FIRST_USER_WORD_ // ids below this are built-in words
};

enum opcode {
	// ops that have a corresponding word (1:1)
	#define X(E,_S,_N,_D) OP_##E,
	LIST_OF_OP_WORDS
	#undef X

	// "word-less ops", ops that are indirectly encoded with "syntax", in part
	// because they have "arguments"
	_FIRST_WORDLESS_OP_,
	OP_INT_LITERAL = _FIRST_WORDLESS_OP_, // integer literal, followed by i32
	OP_FLOAT_LITERAL, // floating-point literal, followed by f32
	OP_STR_LITERAL, // string literal, followed by i32 static(negative)/dynamic offset
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

	#define X(E,_S,_N,_D) case OP_##E: return #E;
	LIST_OF_OP_WORDS
	#undef X

	case OP_INT_LITERAL   : return "INT_LITERAL";
	case OP_FLOAT_LITERAL : return "FLOAT_LITERAL";
	case OP_STR_LITERAL   : return "STR_LITERAL";
	case OP_JMP     : return "JMP";
	case OP_JMP0    : return "JMP0";
	case OP_JSR     : return "JSR";

	default: return "<?>";

	}
	assert(!"unreachable");
}

static int get_opcode_minimum_stack_height(enum opcode opcode)
{
	switch (opcode) {

	#define X(E,_S,N,_D) case OP_##E: return N;
	LIST_OF_OP_WORDS
	#undef X

	default: return 0;

	}
	assert(!"unreachable");
}

static enum builtin_word match_builtin_word(const char* word)
{
	int i=FIRST_WORD;
	// match word's S (string representation) if not null, else #E (enum as
	// string)
	#define X(E,S,...) \
		if (strcmp(word,S!=NULL?S:#E) == 0) { \
			return (enum builtin_word)i; \
		} \
		++i;
	LIST_OF_SYNTAX_WORDS
	LIST_OF_OP_WORDS
	#undef X
	assert((i == _FIRST_USER_WORD_) && "something's not right");
	return _NO_WORD_;
}

enum tokenizer_state {
	WORD=1,
	STRING,
	STRING_ESCAPE,
	COMMENT,
};

#define MAX_ERROR_MESSAGE_SIZE (1<<14)
#define WORD_BUF_SIZE (1<<8)
#define MAX_SEW_DEPTH (8) // storage req scales 3^X-ish - be careful!

struct word_info {
	unsigned is_comptime :1;
	unsigned is_direct   :1; // calls word directly
	unsigned is_addr     :1; // pushes word address
	unsigned is_sealed   :1; // 0 until after ";"
	int addr;
};

union pword {
	uint32_t u32;
	int32_t  i32;
	float    f32;
};
static_assert(sizeof(union pword)==sizeof(uint32_t),"");

#define PWORD_INT(v)   ((union pword){.i32=(v)})
#define PWORD_FLOAT(v) ((union pword){.f32=(v)})

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

struct val_arr {
	struct val* arr;
};

struct val_map {
	struct {
		struct val key,value;
	}* map;
};

struct val_str {
	int32_t offset,length;
};

struct val_i32arr {
	int32_t* arr;
};

struct val_f32arr {
	float* arr;
};


struct valstore {
	// arr arr!
	struct val_arr*    arr_arr;
	struct val_map*    map_arr;
	struct thicchar*   char_store_arr;
	struct val_str*    str_arr;
	struct val_i32arr* i32arr_arr;
	struct val_f32arr* f32arr_arr;
};

struct program {
	int write_cursor;
	int entrypoint_address;
	union pword* op_arr;
	// be careful about grabbing references to op_arr; arrput may change the
	// pointer (realloc), and both the compiler and comptime VM may write new
	// instructions (in fact, comptime VM executes part of the program while
	// writing another part? feels "a bit fucked" but it should work as long as
	// you don't grab references?)
	struct location* pc_to_location_arr;
	struct thicchar* static_string_store_arr;
	struct val_str* static_string_arr;
	// TODO: compile error?
	// NOTE if you add any _arr fields, please update program_copy()
};

static void program_copy(struct program* dst, struct program* src)
{
	struct program tmp = *dst;
	*dst = *src;
	arrcpy(dst->op_arr, tmp.op_arr);
	arrcpy(dst->pc_to_location_arr, tmp.pc_to_location_arr);
	arrcpy(dst->static_string_store_arr, tmp.static_string_store_arr);
	arrcpy(dst->static_string_arr, tmp.static_string_arr);
}

struct compiler {
	struct location location, error_location;
	enum tokenizer_state tokenizer_state;
	size_t string_offset;
	int comment_depth;
	int word_size;
	char* word_buf;
	char* error_message;
	struct {
		char* key;
		struct word_info value;
	}* word_lut;
	int* word_index_arr;
	int* wordscope0_arr;
	int* wordskip_addraddr_arr;
	//int next_user_word_id;
	struct program* program;
	int sew_depth;
	int colon;

	unsigned has_error :1;
	unsigned prefix_comptime :1;
};


struct vmie {
	// TODO?
	//  - globals?
	//  - instruction counter/remaining (for cycle limiting)
	struct val* stack_arr;
	uint32_t*   rstack_arr;
	struct val* global_arr;
	int pc, error_pc;
	struct program* program;
	int has_error;
	char* error_message;
	struct program* sew_target;
	struct valstore vals;
};

static struct {
	//mtx_t program_alloc_mutex;
	struct program* program_arr;
	int* program_freelist_arr;
	unsigned globals_were_initialized  :1;
	const char* preamble_val_types;
	size_t preamble_val_types_size;
} g;

struct savestate {
	struct compiler compiler; // uses scrallox
	struct vmie vmie;         // uses scrallox
	struct program program;   // uses system allocator
	struct scratch_context_header* expected_header;
	uint8_t* scratch_buf; // scratch buffer contents are stored here
};

THREAD_LOCAL static struct {
	#ifndef MIE_ONLY_SYSALLOC
	struct allocator our_scratch_allocator;
	#endif
	struct compiler compiler;
	struct vmie vmie;
	const char* error_message;
	unsigned currently_compiling             :1;
	unsigned thread_locals_were_initialized  :1;
	struct savestate* savestate_arr;
} tlg; // thread-local globals

// returns our own thread-local scratch allocator; NOTE that I briefly
// considered using the same allocator for document edits ("mim" stuff in
// gig.c)
static struct allocator* scrallox(void)
{
	#ifndef MIE_ONLY_SYSALLOC
	return &tlg.our_scratch_allocator;
	#else
	return &system_allocator;
	#endif
}

static void program_reset(struct program* p)
{
	assert(p->op_arr != NULL);
	assert(p->pc_to_location_arr != NULL);
	arrreset(p->op_arr);
	arrreset(p->pc_to_location_arr);
	p->write_cursor = 0;
	p->entrypoint_address = 0;
}

static void program_init(struct program* p)
{
	memset(p, 0, sizeof *p);
	arrinit(p->op_arr, &system_allocator);
	arrinit(p->pc_to_location_arr, &system_allocator);
	arrinit(p->static_string_store_arr, &system_allocator);
	arrinit(p->static_string_arr, &system_allocator);
	program_reset(p);
}

static void program_push_loc(struct program* p, union pword pword, struct location loc)
{
	const int n = arrlen(p->op_arr);
	assert((0 <= p->write_cursor) && (p->write_cursor <= n));
	int has_loc = (loc.line>0);
	const int pc = arrlen(p->pc_to_location_arr);
	if (!has_loc && pc>0) {
		loc = p->pc_to_location_arr[pc-1];
		has_loc = (loc.line>0);
	}
	if (p->write_cursor == n) {
		arrput(p->op_arr, pword);
		arrput(p->pc_to_location_arr, loc);
	} else {
		p->op_arr[p->write_cursor] = pword;
		if (has_loc) {
			p->pc_to_location_arr[p->write_cursor] = loc;
		}
	}
	++p->write_cursor;
}

static void program_push(struct program* p, union pword pword)
{
	program_push_loc(p, pword, ((struct location) {0}));
}

static inline int program_addr(struct program* p)
{
	return arrlen(p->op_arr);
}

static union pword program_read_at(struct program* p, int addr)
{
	return arrchkget(p->op_arr, addr);
}

static void program_patch(struct program* p, int addr, union pword pword)
{
	arrchk(p->op_arr, addr);
	p->op_arr[addr] = pword;
}

static void program_sew(struct program* p, int sew_depth, int type, union pword pword, struct location loc)
{
	assert(sew_depth >= 0);
	if (sew_depth == 0) {
		program_push_loc(p, pword, loc);
	} else {
		const int d2 = sew_depth-1;
		assert(d2 >= 0);
		switch (type) {
		case VAL_INT:   program_sew(p, d2, VAL_INT, PWORD_INT(OP_INT_LITERAL), loc); break;
		case VAL_FLOAT: program_sew(p, d2, VAL_INT, PWORD_INT(OP_FLOAT_LITERAL), loc); break;
		default: assert(!"bad/unhandled type");
		}
		program_sew(p, d2, type   , pword, loc);
		program_sew(p, d2, VAL_INT, PWORD_INT(OP_SEW), loc);
	}
}

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

static struct location vmie_get_location(struct vmie* vm)
{
	struct location loc = {0};
	const int pc = vm->pc;
	struct program* p = vm->program;
	const int n = arrlen(p->pc_to_location_arr);
	if ((0 <= pc) && (pc < n)) loc = p->pc_to_location_arr[pc];
	return loc;
}

FORMATPRINTF2
void vmie_errorf(struct vmie* vm, const char* fmt, ...)
{
	char* p = vm->error_message;
	int r = MAX_ERROR_MESSAGE_SIZE;
	int n;
	va_list va;
	va_start(va, fmt);
	n = stbsp_vsnprintf(p, r, fmt, va);
	va_end(va);
	p+=n; r-=n;
	struct location loc = vmie_get_location(vm);
	n = stbsp_snprintf(p, r, " at line %d:%d", loc.line, loc.column);
	p+=n; r-=n;
	vm->error_pc = vm->pc;
	vm->has_error = 1;
	tlg.error_message = vm->error_message;
}

static void* scratch_alloc(size_t sz)
{
	return allocator_malloc(scrallox(), sz);
}

static void vmie_init(struct vmie* vm, struct program* program)
{
	memset(vm, 0, sizeof *vm);
	struct allocator* alloc = scrallox();
	arrinit(vm->stack_arr, alloc);
	arrinit(vm->rstack_arr, alloc);
	arrinit(vm->global_arr, alloc);
	vm->program = program;
	vm->pc = program->entrypoint_address;
	vm->error_message = scratch_alloc(MAX_ERROR_MESSAGE_SIZE);
	vm->error_message[0] = 0;
}

#define VMIE_SEW_GUARD \
	if (vm->sew_target == NULL) { \
		vmie_errorf(vm, "no sew target (called outside of comptime vm?)"); \
		return -1; \
	}

static struct thicchar* resolve_string(struct vmie* vm, int id, int* out_length)
{
	struct val_str vs = {0};
	struct thicchar* tc = NULL;
	if (id < 0) {
		const int index = -1-id;
		struct program* prg = vm->program;
		vs = arrchkget(prg->static_string_arr, index);
		if (vs.length > 0) {
			tc = arrchkptr(prg->static_string_store_arr, vs.offset);
			(void)arrchkptr(prg->static_string_store_arr, vs.offset+vs.length-1);
		}
	} else {
		struct valstore* vals = &vm->vals;
		vs = arrchkget(vals->str_arr, id);
		if (vs.length > 0) {
			tc = arrchkptr(vals->char_store_arr, vs.offset);
			(void)arrchkptr(vals->char_store_arr, vs.offset+vs.length-1);
		}
	}
	if (out_length) *out_length = vs.length;
	return tc;
}

static int vmie_pop_str(struct vmie* vm, struct thicchar** out_str, int* out_length)
{
	const struct val v = arrpop(vm->stack_arr);
	if (v.type != VAL_STR) {
		vmie_errorf(vm, "expected VAL_STR (%d), got type %d", VAL_STR, v.type);
		return 0;
	}
	const int id = v.i32;
	struct thicchar* s = resolve_string(vm, id, out_length);
	if (s && out_str) *out_str = s;
	return 1;
}


static struct val_arr* vmie_pop_arr(struct vmie* vm)
{
	const struct val v = arrpop(vm->stack_arr);
	if (v.type != VAL_ARR) {
		vmie_errorf(vm, "expected VAL_ARR (%d), got type %d", VAL_ARR, v.type);
		return NULL;
	}
	const int id = v.i32;
	struct valstore* vals = &vm->vals;
	const int n = arrlen(vals->arr_arr);
	if (!(0 <= id && id < n)) {
		vmie_errorf(vm, "VAL_ARR id (%d) out of valid range [0:%d]", id, n-1);
		return NULL;
	}
	return &vals->arr_arr[id];
}

static struct val_map* vmie_pop_map(struct vmie* vm)
{
	const struct val v = arrpop(vm->stack_arr);
	if (v.type != VAL_MAP) {
		vmie_errorf(vm, "expected VAL_MAP (%d), got type %d", VAL_MAP, v.type);
		return NULL;
	}
	const int id = v.i32;
	struct valstore* vals = &vm->vals;
	const int n = arrlen(vals->map_arr);
	if (!(0 <= id && id < n)) {
		vmie_errorf(vm, "VAL_MAP id (%d) out of valid range [0:%d]", id, n-1);
		return NULL;
	}
	return &vals->map_arr[id];
}

static void vmie_dropn(struct vmie* vm, int n)
{
	const int len0 = arrlen(vm->stack_arr);
	assert(n <= len0);
	const int len1 = len0-n;
	arrsetlen(vm->stack_arr, len1);
}

static struct val vmie_peek(struct vmie* vm, int index_from_top)
{
	assert(index_from_top >= 0);
	const int n = arrlen(vm->stack_arr);
	assert(n > index_from_top);
	return vm->stack_arr[n - 1 - index_from_top];
}

static struct val vmie_top(struct vmie* vm)
{
	return vmie_peek(vm, 0);
}

static void vmie_dup(struct vmie* vm)
{
	struct val v = vmie_top(vm);
	arrput(vm->stack_arr, v);
}

static int32_t vmie_alloc_string(struct vmie* vm, size_t len, struct thicchar** out_str)
{
	struct valstore* vals = &vm->vals;

	struct allocator* alloc = scrallox();
	if (vals->char_store_arr == NULL) arrinit(vals->char_store_arr, alloc);
	if (vals->str_arr == NULL) arrinit(vals->str_arr, alloc);
	assert(vals->char_store_arr != NULL);
	assert(vals->str_arr != NULL);

	const int off = arrlen(vals->char_store_arr);
	struct thicchar* str = arraddnptr(vals->char_store_arr, len);
	if (out_str) *out_str = str;

	const int32_t id = arrlen(vals->str_arr);
	arrput(vals->str_arr, ((struct val_str){
		.offset = off,
		.length = len,
	}));

	return id;
}

static inline int32_t thicchar_rgbx_to_i32(struct thicchar tc)
{
	uint32_t e = 0;
	for (int i=0; i<4; ++i) e |= ((uint32_t)(tc.color[i]) << (i*8));
	return (int32_t)e; // XXX unsafe in theory?
}

static inline void thicchar_set_rgbx(struct thicchar* tc, int32_t rgbx)
{
	uint32_t e = rgbx; // XXX unsafe in theory?
	for (int i=0; i<4; ++i) tc->color[i] = ((e >> (i*8)) & 0xff);
}

int vmie_run2(struct vmie* vm)
{
	const int TRACE = 0;

	struct program const* prg = vm->program;
	assert((prg != NULL) && "no program; forgot vmie_reset()?");
	const int prg_len = arrlen(prg->op_arr);
	int pc = vm->pc;
	int num_defer = 0;
	uint32_t deferred_op = 0;
	// TODO cycle limiting in while() condition?

	struct allocator* alloc = scrallox();

	while ((0 <= pc) && (pc < prg_len)) {
		vm->pc = pc;
		if (vm->has_error) return -1;

		union pword* pw = &prg->op_arr[pc];
		int next_pc = pc+1;
		int set_num_defer = 0;

		#define STACK_HEIGHT()  ((int)arrlen(vm->stack_arr))
		#define RSTACK_HEIGHT() ((int)arrlen(vm->rstack_arr))

		if (num_defer > 0) {
			--num_defer;
			assert((num_defer == 0) && "TODO wide op? or bug?");
			switch (deferred_op) {

			case OP_JMP: {
				if (TRACE) printf(" JMP pc %d->%d\n", pc, pw->u32);
				next_pc = pw->u32;
			}	break;

			case OP_JMP0: {
				if (TRACE) printf(" JMP pc %d->%d\n", pc, pw->u32);
				if (STACK_HEIGHT() < 1) {
					vmie_errorf(vm,
						"%s expected a minimum stack height of 1, but it was only %d",
						get_opcode_str(deferred_op),
						STACK_HEIGHT());
					return -1;
				}
				const struct val v = arrpop(vm->stack_arr);
				if ((v.type==VAL_INT && v.i32==0) || (v.type==VAL_FLOAT && v.f32==0.0f)) {
					next_pc = pw->u32;
				}
			}	break;

			case OP_JSR: {
				if (TRACE) printf(" JSR pc %d->%d %d=>R\n", pc, pw->u32, pc+2);
				arrput(vm->rstack_arr, (pc+1));
				next_pc = pw->u32;
			}	break;

			case OP_INT_LITERAL: {
				if (TRACE) printf(" INT_LITERAL %d\n", pw->i32);
				arrput(vm->stack_arr, intval(pw->i32));
			}	break;

			case OP_FLOAT_LITERAL: {
				if (TRACE) printf(" FLOAT_LITERAL %f\n", pw->f32);
				arrput(vm->stack_arr, floatval(pw->f32));
			}	break;

			case OP_STR_LITERAL: {
				if (TRACE) printf(" STR_LITERAL %d\n", pw->i32);
				arrput(vm->stack_arr, ((struct val) {
					.type = VAL_STR,
					.i32 = pw->i32,
				}));
			}	break;

			default:
				fprintf(stderr, "vmie: unhandled deferred op %s (%u)\n", get_opcode_str(deferred_op), deferred_op);
				abort();
				break;
			}

			assert(set_num_defer == 0);
		} else {
			const uint32_t op = prg->op_arr[pc].u32;

			if (TRACE) printf("[%.4x] %.2x %s\n", pc, op, get_opcode_str(op));

			const int opcode_minimum_stack_height = get_opcode_minimum_stack_height(op);

			if (STACK_HEIGHT() < opcode_minimum_stack_height) {
				vmie_errorf(vm,
					"%s expected a minimum stack height of %d, but it was only %d",
					get_opcode_str(op),
					opcode_minimum_stack_height,
					STACK_HEIGHT());
				return -1;
			}

			switch (op) {

			case OP_NOP: {
				if (TRACE) printf(" NOP\n");
			}	break;

			case OP_HALT: {
				if (TRACE) printf(" HALT\n");
				if (arrlen(vm->stack_arr) > 0) {
					const struct val top = vmie_top(vm);
					if (top.type == VAL_STR) {
						int len=0;
						struct thicchar* s = resolve_string(vm, top.i32, &len);
						int num_bytes=0;
						for (int i=0; i<len; ++i) num_bytes += utf8_num_bytes_for_codepoint(s[i].codepoint);
						++num_bytes; // space for NUL-terminator
						char* msg = scratch_alloc(num_bytes);
						char* p = msg;
						for (int i=0; i<len; ++i) p = utf8_encode(p, s[i].codepoint);
						*(p++) = 0; // NUL-terminator
						assert((p-msg) == num_bytes);
						vmie_errorf(vm, "HALT (%s)", msg);
						return -1;
					}
				}
				vmie_errorf(vm, "HALT (no msg)");
				return -1;
			}	break;

			// these ops take another argument
			case OP_JMP:
			case OP_JMP0:
			case OP_JSR:
			case OP_INT_LITERAL:
			case OP_FLOAT_LITERAL:
			case OP_STR_LITERAL:
				set_num_defer = 1;
				break;

			case OP_JMPI:
			case OP_JSRI: {
				const int32_t addr = arrpop(vm->stack_arr).i32;
				if (op==OP_JSRI) {
					arrput(vm->rstack_arr, (pc+1));
				}
				next_pc = addr;
			}	break;

			case OP_RETURN: {
				if (RSTACK_HEIGHT() == 0) {
					vmie_errorf(vm, "rstack underflow due to RETURN at pc=%d\n", pc);
					return -1;
				}
				next_pc = arrpop(vm->rstack_arr);
				if (TRACE) printf(" RETURN pc -> %d\n", next_pc);
			}	break;


			case OP_DROP: {
				(void)arrpop(vm->stack_arr);
			}	break;

			case OP_PICK: {
				struct val vi = arrpop(vm->stack_arr);
				const int i = vi.i32;
				if (i<0) {
					vmie_errorf(vm, "`%d PICK` - negative pick is invalid", i);
					return -1;
				}
				const int min_height = 1+i;
				if (STACK_HEIGHT() < min_height) {
					vmie_errorf(vm, "%d pick out-of-bounds; stack height is only %d", i, STACK_HEIGHT());
					return -1;
				}
				struct val vd = arrchkget(vm->stack_arr, STACK_HEIGHT()-1-i);
				arrput(vm->stack_arr, vd);
			}	break;

			case OP_ROTATE: {
				const struct val vd = arrpop(vm->stack_arr);
				const struct val vn = arrpop(vm->stack_arr);
				const int d=vd.i32; \
				const int n=vn.i32; \
				if (STACK_HEIGHT() < n) {
					vmie_errorf(vm, "ROTATE of n=%d elements, but stack height is only %d", n, STACK_HEIGHT());
					return -1;
				}
				if (n>=2) { // n<2 is a no-op
					const int i0 = STACK_HEIGHT()-n;
					const int n0 = arrlen(vm->stack_arr);
					struct val* p1 = arraddnptr(vm->stack_arr, n);
					struct val* p0 = arrchkptr(vm->stack_arr, i0);
					memcpy(p1, p0, n*sizeof(*p1));
					for (int i=0; i<n; ++i) p0[i] = p1[stb_mod_eucl(i+d,n)];
					arrsetlen(vm->stack_arr, n0);
				}
			}	break;

			case OP_EQ: {
				const struct val va = arrpop(vm->stack_arr);
				const struct val vb = arrpop(vm->stack_arr);
				arrput(vm->stack_arr, intval((va.i32==vb.i32)));
			}	break;

			case OP_TYPEOF: { // (x -- typeof(x))
				const struct val x = arrpop(vm->stack_arr);
				arrput(vm->stack_arr, intval(x.type));
			}	break;

			case OP_CAST: { // (value T -- T(value))
				const struct val T = arrpop(vm->stack_arr);
				struct val value = arrpop(vm->stack_arr);
				value.type = T.i32;
				arrput(vm->stack_arr, value);
			}	break;

			case OP_HERE: {
				arrput(vm->stack_arr, intval(pc));
			}	break;

			case OP_I2F: {
				const int v = arrpop(vm->stack_arr).i32;
				arrput(vm->stack_arr, floatval(v));
			}	break;

			case OP_F2I: {
				const float v = arrpop(vm->stack_arr).f32;
				arrput(vm->stack_arr, intval((int)floorf(v)));
			}	break;

			case OP_SET_GLOBAL: {
				const int index = arrpop(vm->stack_arr).i32;
				const struct val value = arrpop(vm->stack_arr);
				if (index < 0) {
					vmie_errorf(vm, "SET_GLOBAL with negative index (%d); only supports non-negative", index);
					return -1;
				}
				assert(index >= 0);
				const size_t n0 = arrlen(vm->global_arr);
				if (index >= n0) {
					// grow array and clear the new elements (don't want weird
					// garbage)
					const size_t dn = 1+index-n0;
					struct val* p = arraddnptr(vm->global_arr, dn);
					memset(p, 0, dn*sizeof(*p));
				}
				vm->global_arr[arrchk(vm->global_arr,index)] = value;
			}	break;

			case OP_GET_GLOBAL: {
				const int index = arrpop(vm->stack_arr).i32;
				const size_t n = arrlen(vm->global_arr);
				if (!(0 <= index && index < n)) {
					vmie_errorf(vm, "GET_GLOBAL index (%d) out of range [0;%zd]", index, n-1);
					return -1;
				}
				struct val value = vm->global_arr[index];
				arrput(vm->stack_arr, value);
			}	break;

			// == floating-point binary ops ==

			#define DEF_FBINOP(ENUM, TYPE, ASSIGN) \
			case ENUM: { \
				const struct val vb = arrpop(vm->stack_arr); \
				const struct val va = arrpop(vm->stack_arr); \
				const float a=va.f32; \
				const float b=vb.f32; \
				arrput(vm->stack_arr, ((struct val){ \
					.type=TYPE, \
					ASSIGN, \
				})); \
			}	break;
			DEF_FBINOP( OP_FADD   , VAL_FLOAT , .f32=(a+b)        )
			DEF_FBINOP( OP_FMUL   , VAL_FLOAT , .f32=(a*b)        )
			DEF_FBINOP( OP_FDIV   , VAL_FLOAT , .f32=(a/b)        )
			DEF_FBINOP( OP_FMOD   , VAL_FLOAT , .f32=(fmodf(a,b)) )
			DEF_FBINOP( OP_FATAN2 , VAL_FLOAT , .f32=(atan2f(a,b)) )
			DEF_FBINOP( OP_FPOW   , VAL_FLOAT , .f32=(powf(a,b))  )
			DEF_FBINOP( OP_FLT    , VAL_INT   , .i32=(a<b)        )
			DEF_FBINOP( OP_FLE    , VAL_INT   , .i32=(a<=b)       )
			DEF_FBINOP( OP_FEQ    , VAL_INT   , .i32=(a==b)       )
			DEF_FBINOP( OP_FNE    , VAL_INT   , .i32=(a!=b)       )
			DEF_FBINOP( OP_FGE    , VAL_INT   , .i32=(a>=b)       )
			DEF_FBINOP( OP_FGT    , VAL_INT   , .i32=(a>b)        )
			#undef DEF_FBINOP

			// == floating-point unary ops ==

			#define DEF_FUNOP(ENUM, EXPR) \
			case ENUM: { \
				const struct val va = arrpop(vm->stack_arr); \
				const float a=va.f32; \
				arrput(vm->stack_arr, ((struct val){ \
					.type=VAL_FLOAT, \
					.f32=(EXPR), \
				})); \
			}	break;
			DEF_FUNOP( OP_FNEG   , (-a)      )
			DEF_FUNOP( OP_FINV   , (1.0f/a)  )
			DEF_FUNOP( OP_FCOS   , cosf(a)   )
			DEF_FUNOP( OP_FSIN   , sinf(a)   )
			DEF_FUNOP( OP_FTAN   , tanf(a)   )
			DEF_FUNOP( OP_FACOS  , acosf(a)  )
			DEF_FUNOP( OP_FASIN  , asinf(a)  )
			DEF_FUNOP( OP_FATAN  , atanf(a)  )
			DEF_FUNOP( OP_FEXP   , expf(a)   )
			DEF_FUNOP( OP_FLOG   , logf(a)   )
			DEF_FUNOP( OP_FSQRT  , sqrtf(a)  )
			DEF_FUNOP( OP_FFLOOR , floorf(a) )
			DEF_FUNOP( OP_FCEIL  , ceilf(a)  )
			DEF_FUNOP( OP_FROUND , roundf(a) )
			DEF_FUNOP( OP_FABS   , fabsf(a)  )
			#undef DEF_FUNOP

			// == integer binary ops ==

			#define DEF_IBINOP(ENUM, EXPR) \
			case ENUM: { \
				const struct val vb = arrpop(vm->stack_arr); \
				const struct val va = arrpop(vm->stack_arr); \
				const int32_t a=va.i32; \
				const int32_t b=vb.i32; \
				arrput(vm->stack_arr, intval(EXPR)); \
			}	break;
			DEF_IBINOP( OP_IADD    , (a+b)  )
			DEF_IBINOP( OP_IMUL    , (a*b)  )
			DEF_IBINOP( OP_IDIV    , (stb_div_eucl(a,b)) )
			DEF_IBINOP( OP_IMOD    , (stb_mod_eucl(a,b)) )
			DEF_IBINOP( OP_IBAND   , (a&b)  )
			DEF_IBINOP( OP_IBOR    , (a|b)  )
			DEF_IBINOP( OP_IBXOR   , (a^b)  )
			DEF_IBINOP( OP_ILAND   , (a&&b) )
			DEF_IBINOP( OP_ILOR    , (a||b) )
			DEF_IBINOP( OP_ILXOR   , ((!!a)!=(!!b)) )
			// FIXME TODO shifts have implementation defined behavior; consider
			// only supporting b values in [0:32], or maybe [-32:32]?
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
				const struct val va = arrpop(vm->stack_arr); \
				const int32_t a=va.i32; \
				arrput(vm->stack_arr, intval(EXPR)); \
			}	break;
			DEF_IUNOP( OP_INEG  , (-a)   )
			DEF_IUNOP( OP_IBNOT , (~a)   )
			DEF_IUNOP( OP_ILNOT , (!a)   )
			DEF_IUNOP( OP_IABS  , abs(a) )
			#undef DEF_IUNOP

			// = strings ====================

			case OP_STRLEN: {
				int len=0;
				if (!vmie_pop_str(vm, NULL, &len)) return -1;
				arrput(vm->stack_arr, intval(len));
			}	break;

			case OP_STRCOMPS: {
				const int index = arrpop(vm->stack_arr).i32;
				struct thicchar* str=NULL;
				int len=0;
				if (!vmie_pop_str(vm, &str, &len)) return -1;
				if (!((0 <= index) && (index < len))) {
					vmie_errorf(vm, "STRCOMPS index %d outside valid range [0:%d]", index, len-1);
					return -1;
				}
				struct thicchar tc = str[index];
				arrput(vm->stack_arr, intval(tc.codepoint));
				arrput(vm->stack_arr, intval(thicchar_rgbx_to_i32(tc)));
			}	break;

			case OP_STRNEW: {
				const int n = arrpop(vm->stack_arr).i32;
				if (n < 0) {
					vmie_errorf(vm, "STRNEW got negative string length (%d)", n);
					return -1;
				}
				const int n2 = 2*n;
				if (STACK_HEIGHT() < n2) {
					vmie_errorf(vm, "%di STRNEW expected at least %d elements on stack", n, n2);
					return -1;
				}
				struct thicchar* str=NULL;
				const int32_t id = vmie_alloc_string(vm, n, &str);
				for (int i=(n-1); i>=0; --i) {
					const int rgbx = arrpop(vm->stack_arr).i32;
					str[i].codepoint = arrpop(vm->stack_arr).i32;
					thicchar_set_rgbx(&str[i], rgbx);
				}
				arrput(vm->stack_arr, typeval(VAL_STR, id));
			}	break;

			case OP_STRCATN: {
				const int n = arrpop(vm->stack_arr).i32;
				if (n < 0) {
					vmie_errorf(vm, "STRCATN got negative count (%d)", n);
					return -1;
				}
				if (STACK_HEIGHT() < n) {
					vmie_errorf(vm, "%di STRCATN expected at least %d elements on stack", n, n);
					return -1;
				}

				int len_total = 0;
				for (int i=0; i<n; ++i) {
					struct val v = vmie_peek(vm, i);
					if (v.type != VAL_STR) {
						vmie_errorf(vm, "%di STRCATN: element %d from top is not a string", n, i);
						return -1;
					}
					int len=0;
					resolve_string(vm, v.i32, &len);
					len_total += len;
				}

				struct thicchar* str=NULL;
				const int32_t id = vmie_alloc_string(vm, len_total, &str);
				assert(str != NULL);
				struct thicchar* wp = str;
				assert(id >= 0);
				for (int i=(n-1); i>=0; --i) {
					struct val v = vmie_peek(vm, i);
					assert(v.type == VAL_STR);
					int len=0;
					struct thicchar* src = resolve_string(vm, v.i32, &len);
					if (len > 0) {
						memcpy(wp, src, len*sizeof(*wp));
						wp += len;
					}
				}

				vmie_dropn(vm, n);

				arrput(vm->stack_arr, typeval(VAL_STR, id));
			}	break;

			// = arrays =====================

			case OP_ARRNEW: {
				struct valstore* vals = &vm->vals;
				if (vals->arr_arr == NULL) {
					arrinit(vals->arr_arr, alloc);
				}
				assert(vals->arr_arr != NULL);
				const int32_t id = (int32_t)arrlen(vals->arr_arr);
				struct val_arr* arr = arraddnptr(vals->arr_arr, 1);
				arrinit(arr->arr, alloc);
				arrput(vm->stack_arr, typeval(VAL_ARR, id));
			}	break;

			case OP_ARRLEN: {
				struct val_arr* arr = vmie_pop_arr(vm);
				if (arr == NULL) return -1;
				arrput(vm->stack_arr, intval(arrlen(arr->arr)));
			}	break;

			case OP_ARRPUT: {
				const struct val v = arrpop(vm->stack_arr);
				const struct val k = vmie_top(vm);
				struct val_arr* arr = vmie_pop_arr(vm);
				if (arr == NULL) return -1;
				arrput(arr->arr, v);
				arrput(vm->stack_arr, k);
			}	break;

			case OP_ARRGET: {
				const int32_t index = arrpop(vm->stack_arr).i32;
				struct val_arr* arr = vmie_pop_arr(vm);
				if (arr == NULL) return -1;
				const int n = arrlen(arr->arr);
				if (!(0 <= index && index < n)) {
					vmie_errorf(vm, "ARRGET index %d out of range [0:%d]", index, n-1);
					return -1;
				}
				arrput(vm->stack_arr, arr->arr[index]);
			}	break;

			// = maps =======================

			case OP_MAPNEW: {
				struct valstore* vals = &vm->vals;
				if (vals->map_arr == NULL) {
					arrinit(vals->map_arr, alloc);
				}
				assert(vals->map_arr != NULL);
				const int32_t id = (int32_t)arrlen(vals->map_arr);
				struct val_map* map = arraddnptr(vals->map_arr, 1);
				hminit(map->map, alloc);
				arrput(vm->stack_arr, typeval(VAL_MAP, id));
			}	break;

			case OP_MAPHAS: {
				const struct val key = arrpop(vm->stack_arr);
				struct val_map* map = vmie_pop_map(vm);
				arrput(vm->stack_arr, intval((hmgeti(map->map, key)>=0)?1:0));
			}	break;

			case OP_MAPGET: {
				const struct val key = arrpop(vm->stack_arr);
				struct val_map* map = vmie_pop_map(vm);
				const int index = hmgeti(map->map, key);
				if (index < 0) {
					vmie_errorf(vm, "mapget key T%d:%d not found", key.type, key.i32);
					return -1;
				}
				arrput(vm->stack_arr, map->map[index].value);
			}	break;

			case OP_MAPSET: {
				const struct val value = arrpop(vm->stack_arr);
				const struct val key = arrpop(vm->stack_arr);
				vmie_dup(vm);
				struct val_map* map = vmie_pop_map(vm);
				hmput(map->map, key, value);
			}	break;

			case OP_MAPDEL: {
				const struct val key = arrpop(vm->stack_arr);
				vmie_dup(vm);
				struct val_map* map = vmie_pop_map(vm);
				if (!hmdel(map->map, key)) {
					vmie_errorf(vm, "mapdel key T%d:%d not found", key.type, key.i32);
					return -1;
				}
			}	break;

			// = comptime/sewing ============

			case OP_THERE: {
				VMIE_SEW_GUARD
				const int c = vm->sew_target->write_cursor;
				arrput(vm->stack_arr, intval(c));
				if (TRACE) printf("THERE => %d\n", c);
			}	break;

			case OP_NAVIGATE: {
				VMIE_SEW_GUARD
				const struct val v = arrpop(vm->stack_arr);
				if (v.type != VAL_INT) {
					vmie_errorf(vm, "navigate called with non-integer arg (type %d)", v.type);
					return -1;
				}
				const int c = v.i32;
				const int end = arrlen(vm->sew_target->op_arr);
				if (!(0 <= c && c <= end)) {
					vmie_errorf(vm, "navigate out of bounds to %d, valid range is [0;%d]", c, end);
					return -1;
				}
				if (TRACE) printf("%d => NAVIGATE\n", c);
				vm->sew_target->write_cursor = c;
			}	break;

			case OP_SEW: {
				const struct val v = arrpop(vm->stack_arr);
				VMIE_SEW_GUARD
				switch (v.type) {
				case VAL_INT:   program_push(vm->sew_target, PWORD_INT(v.i32)); break;
				case VAL_FLOAT: program_push(vm->sew_target, PWORD_FLOAT(v.f32)); break;
				default:
					vmie_errorf(vm, "SEW with unhandled value type (%d)", v.type);
					return -1;
				}
			}	break;

			case OP_SEW_JMP0:
			case OP_SEW_JMP:
			case OP_SEW_JSR: {
				const struct val addr = arrpop(vm->stack_arr);
				VMIE_SEW_GUARD
				int op2;
				switch (op) {
				case OP_SEW_JMP0 : op2 = OP_JMP0; break;
				case OP_SEW_JMP  : op2 = OP_JMP;  break;
				case OP_SEW_JSR  : op2 = OP_JSR;  break;
				default: assert(!"unhandled op");
				}
				program_push(vm->sew_target, PWORD_INT(op2));
				program_push(vm->sew_target, PWORD_INT(addr.i32));
			}	break;

			case OP_SEW_ADDR: {
				const struct val addr = arrpop(vm->stack_arr);
				VMIE_SEW_GUARD
				const int op2 = program_read_at(vm->sew_target, vm->sew_target->write_cursor++).i32;
				switch (op2) {
				case OP_JMP0:
				case OP_JMP:
				case OP_JSR:
					// OK
					break;
				default:
					vmie_errorf(vm, "SEW_ADDR expected to patch a jump op, but found %s", get_opcode_str(op2));
					return -1;
				}
				program_push(vm->sew_target, PWORD_INT(addr.i32));
			}	break;

			case OP_SEW_LIT: {
				const struct val lit = arrpop(vm->stack_arr);
				VMIE_SEW_GUARD
				switch (lit.type) {
				case VAL_INT:
					program_push(vm->sew_target, PWORD_INT(OP_INT_LITERAL));
					program_push(vm->sew_target, PWORD_INT(lit.i32));
					break;
				case VAL_FLOAT:
					program_push(vm->sew_target, PWORD_INT(OP_FLOAT_LITERAL));
					program_push(vm->sew_target, PWORD_FLOAT(lit.f32));
					break;
				default:
					vmie_errorf(vm, "SEW-LIT on unhandled type (%d)", lit.type);
					return -1;
				}
			}	break;

			// ==============================

			default:
				vmie_errorf(vm, "%s (%u) invalid or not implemented at pc=%d", get_opcode_str(op), op, pc);
				return -1;

			}

			if (set_num_defer > 0) {
				num_defer = set_num_defer;
				deferred_op = op;
			}
		}

		pc = next_pc;
	}
	vm->pc = pc;
	return STACK_HEIGHT();
}

static void vmie_call(struct vmie* vm, int addr)
{
	vm->pc = addr;
	arrput(vm->rstack_arr, -1); // exit on RETURN
	vmie_run2(vm);
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

static int compiler_push_int(struct compiler* cm, int32_t v)
{
	const int addr = program_addr(cm->program);
	program_sew(cm->program, cm->sew_depth, VAL_INT, PWORD_INT(v), cm->location);
	return addr;
}

static int compiler_push_opcode(struct compiler* cm, enum opcode op)
{
	assert((0<=op) && (op<_NUM_OPS_));
	return compiler_push_int(cm, op);
}

static int compiler_push_float(struct compiler* cm, float v)
{
	const int addr = program_addr(cm->program);
	program_sew(cm->program, cm->sew_depth, VAL_FLOAT, PWORD_FLOAT(v), cm->location);
	return addr;
}

#define LAMBDA0 (-(1<<20))

static inline struct vmie* get_comptime_vm(void)
{
	return &tlg.vmie;
}

static void compiler_push_word(struct compiler* cm, const char* word)
{
	if (cm->has_error) return;

	if (cm->colon) { // handle word after colon

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
			.is_comptime = cm->prefix_comptime,
			.is_direct = (cm->colon==COLON),
			.is_addr = (cm->colon==COLONADDR),
			.is_sealed = 0,
			.addr = program_addr(cm->program),
		}));
		assert(word_index>=0);
		arrput(cm->word_index_arr, word_index);
		arrput(cm->wordscope0_arr, arrlen(cm->word_index_arr));

		cm->colon = 0;
		cm->prefix_comptime = 0;

		return;
	}

	struct number number;
	int wi;
	enum builtin_word bw = match_builtin_word(word);
	if (bw > _NO_WORD_) {
		assert(bw < _FIRST_USER_WORD_);

		enum opcode do_encode_opcode = _OP_NONE_;

		switch (bw) {

		case COLON:
		case COLONADDR:
		case COLONLAMBDA: {
			if (cm->sew_depth > 0) {
				compiler_errorf(cm, "colon not allowed in <#...#>");
				return;
			}

			assert(cm->colon == 0);
			if ((bw==COLON) || (bw==COLONADDR)) {
				cm->colon = bw;
				assert(cm->colon>0);
			}

			// insert skip jump (TODO skip-jump optimization?)
			compiler_push_opcode(cm, OP_JMP);
			// insert invalid placeholder address (we don't know it yet)
			const int placeholder_addraddr = compiler_push_int(cm, -1);
			// remember address of the placeholder address, so we can patch it
			// when we see a semicolon:
			arrput(cm->wordskip_addraddr_arr, placeholder_addraddr);

			if (bw==COLONLAMBDA) {
				if (cm->prefix_comptime) {
					compiler_errorf(cm, "cannot do comptime lambda");
					return;
				}
				const int wi = LAMBDA0 - program_addr(cm->program);
				arrput(cm->word_index_arr, wi);
				arrput(cm->wordscope0_arr, arrlen(cm->word_index_arr));
			}

		}	break;

		case SEMICOLON: {
			if (cm->sew_depth > 0) {
				compiler_errorf(cm, "semicolon not allowed in <#...#>");
				return;
			}

			if (cm->prefix_comptime) {
				compiler_errorf(cm, "nonsensical comptime'd semicolon");
				return;
			}

			if (arrlen(cm->wordscope0_arr) == 0) {
				compiler_errorf(cm, "too many semicolons");
				return;
			}

			struct program* prg = cm->program;
			compiler_push_opcode(cm, OP_RETURN);

			const int c = arrpop(cm->wordscope0_arr);
			assert(c >= 0);
			const int n = arrlen(cm->word_index_arr);
			assert(n>0);
			const int closing_word_index = cm->word_index_arr[n-1];
			assert((closing_word_index >= 0) || (closing_word_index <= LAMBDA0));
			if (closing_word_index >= 0) {
				struct word_info* info = &cm->word_lut[closing_word_index].value;
				info->is_sealed = 1;
			}

			// finalize word skip jump by overwriting the placeholder address
			// now that we know where the word ends
			assert((arrlen(cm->wordskip_addraddr_arr) > 0) && "broken compiler?");
			const int skip_addraddr = arrpop(cm->wordskip_addraddr_arr);
			const int write_addr = program_addr(prg);
			assert((program_read_at(prg, skip_addraddr).u32 == 0xffffffffL) && "expected placeholder value");
			program_patch(prg, skip_addraddr, ((union pword){.u32=write_addr}));

			if (closing_word_index <= LAMBDA0) {
				const int addr = -(closing_word_index - LAMBDA0);
				compiler_push_opcode(cm, OP_INT_LITERAL);
				compiler_push_int(cm, addr);
			}

			if (n > c) {
				// remove inner words from scope
				for (int i=(n-1); i>=c; --i) {
					const int index = cm->word_index_arr[i];
					assert((index >= 0) || (index <= LAMBDA0));
					if (index >= 0) {
						const char* key = cm->word_lut[index].key;
						assert(shdel(cm->word_lut, key) && "expected to delete word, but key was not found");
					}
				}
				arrsetlen(cm->word_index_arr, c);
			}

		}	break;

		case COMPTIME: {
			if (cm->sew_depth > 0) {
				compiler_errorf(cm, "comptime not allowed in <#...#>");
				return;
			}
			cm->prefix_comptime = 1;
		}	break;

		case ENTER_SEW: {
			++cm->sew_depth;
			if (cm->sew_depth > MAX_SEW_DEPTH) {
				compiler_errorf(cm, "sew depth exceeded maximum of %d -- sewception averted!", MAX_SEW_DEPTH);
				return;
			}
		}	break;

		case LEAVE_SEW: {
			if (cm->sew_depth == 0) {
				compiler_errorf(cm, "too many `#>`s");
				return;
			}
			--cm->sew_depth;
		}	break;

		// XXX should we try to have the word/op enums overlap, so this
		// conversion can be skipped?
		#define X(E,_S,_N,_D) case E: do_encode_opcode = OP_##E; break;
		LIST_OF_OP_WORDS
		#undef X

		default: assert(!"TODO add missing built-in word handler"); break;
		}

		if (do_encode_opcode != _OP_NONE_) {
			if (!cm->prefix_comptime) {
				compiler_push_opcode(cm, do_encode_opcode);
			} else {
				assert(cm->sew_depth == 0);
				struct program* prg = cm->program;
				const int addr = arrlen(prg->op_arr);
				assert(prg->write_cursor == addr);
				compiler_push_opcode(cm, do_encode_opcode);
				compiler_push_opcode(cm, OP_RETURN);
				vmie_call(get_comptime_vm(), addr);
				arrsetlen(prg->op_arr, addr);
				prg->write_cursor = addr;
				cm->prefix_comptime = 0;
			}
		}

	} else if (parse_number(word, &number)) {
		if (!cm->prefix_comptime) {
			compiler_push_opcode(cm, number.type);
			switch (number.type) {
			case OP_INT_LITERAL:     compiler_push_int(cm, number.i32); break;
			case OP_FLOAT_LITERAL: compiler_push_float(cm, number.f32); break;
			default: assert(!"unhandled case");
			}
		} else { // cm->prefix_comptime
			assert((cm->sew_depth == 0) && "comptime in <# ... #> not expected");
			struct vmie* vm = get_comptime_vm();
			switch (number.type) {
			case OP_INT_LITERAL:   arrput(vm->stack_arr, intval(number.i32));   break;
			case OP_FLOAT_LITERAL: arrput(vm->stack_arr, floatval(number.f32)); break;
			default: assert(!"unhandled case");
			}
			cm->prefix_comptime = 0;
		}
	} else if ((wi = shgeti(cm->word_lut, word)) >= 0) {
		struct word_info* info = &cm->word_lut[wi].value;
		if (info->is_comptime || cm->prefix_comptime) {
			struct vmie* vm = get_comptime_vm();
			assert(!vm->has_error);
			assert(vm->sew_target != NULL);
			vmie_call(vm, info->addr);
			if (vm->has_error) {
				struct location loc = vmie_get_location(vm);
				compiler_errorf(cm, " <# comptime error: %s at line %d:%d #> ", vm->error_message, loc.line, loc.column);
				return;
			}
			cm->prefix_comptime = 0;
		} else if (!cm->prefix_comptime) {
			if (info->is_direct) {
				assert((!info->is_addr) && "bad info: cannot be 1 together with is_direct");
				compiler_push_opcode(cm, OP_JSR);
				compiler_push_int(cm, info->addr);
			} else if (info->is_addr) {
				compiler_push_opcode(cm, OP_INT_LITERAL);
				compiler_push_int(cm, info->addr);
			} else {
				assert(!"unreachable");
			}
		} else {
			assert(!"unreachable");
		}
		assert(cm->prefix_comptime == 0);
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

	switch (cm->tokenizer_state) {

	case WORD: {
		int is_break=0;
		if (c=='(') {
			cm->tokenizer_state = COMMENT;
			cm->comment_depth = 1;
			is_break = 1;
		} else if (c=='"') {
			cm->tokenizer_state = STRING;
			cm->string_offset = arrlen(cm->program->static_string_store_arr);
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
			const int max_word_size = (WORD_BUF_SIZE - 1 - UTF8_MAX_SIZE);
			if (cm->word_size > max_word_size) {
				compiler_errorf(cm, "word too long (exceeded %d bytes)\n", max_word_size);
				return;
			}
			char* p0 = &cm->word_buf[cm->word_size];
			char* p1;
			p1 = utf8_encode(p0, c);
			cm->word_size += (p1-p0);
			assert(cm->word_size < WORD_BUF_SIZE);
			cm->word_buf[cm->word_size] = 0; // word_buf is MAX_WORD_SIZE+1 long
		}
	}	break;

	case STRING: {
		if (c == '"') {
			const int id = -1-arrlen(cm->program->static_string_arr);
			arrput(cm->program->static_string_arr, ((struct val_str) {
				.offset = cm->string_offset,
				.length = (arrlen(cm->program->static_string_store_arr) - cm->string_offset),
			}));
			if (!cm->prefix_comptime) {
				compiler_push_opcode(cm, OP_STR_LITERAL);
				compiler_push_int(cm, id);
			} else {
				assert((cm->sew_depth == 0) && "comptime in <# ... #> not expected");
				struct vmie* vm = get_comptime_vm();
				arrput(vm->stack_arr, typeval(VAL_STR, id));
				cm->prefix_comptime = 0;
			}
			cm->tokenizer_state = WORD;
		} else if (c == '\\') {
			cm->tokenizer_state = STRING_ESCAPE;
		} else {
			arrput(cm->program->static_string_store_arr, ch);
		}
	}	break;

	case STRING_ESCAPE: {
		arrput(cm->program->static_string_store_arr, ch);
		cm->tokenizer_state = STRING;
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
				cm->tokenizer_state = WORD;
			}
		}
	}	break;

	default: assert(!"unhandled tokenizer state");
	}
}


static int compiler_process_utf8src(struct compiler* cm, const char* utf8src, int num_bytes)
{
	const char* p = utf8src;
	int remaining = num_bytes;
	while (remaining > 0) {
		unsigned codepoint = utf8_decode(&p, &remaining);
		if (codepoint == -1) continue;
		compiler_push_char(cm, ((struct thicchar){
			.codepoint = codepoint,
			.color = {0x40,0x40,0x40,0},
		}));
		if (cm->has_error) return -1;
	}
	return 0;
}

static void compiler_reset_location(struct compiler* cm)
{
	cm->location.line = 1;
	cm->location.column = 0;
}

static void compiler_process_preamble(struct compiler* cm)
{
	compiler_reset_location(cm);
	if (-1 == compiler_process_utf8src(cm, g.preamble_val_types, g.preamble_val_types_size)) {
		fprintf(stderr, "[PREAMBLE ERROR] %s\n", cm->error_message);
		abort();
	}
}

static void compiler_begin(struct compiler* cm, struct program* program)
{
	assert(tlg.currently_compiling == 0);
	tlg.currently_compiling = 1;

	// clear cm, but keep some field(s)
	memset(cm, 0, sizeof *cm);
	cm->error_message = scratch_alloc(MAX_ERROR_MESSAGE_SIZE);
	cm->error_message[0] = 0;

	cm->word_buf = scratch_alloc(sizeof(*cm->word_buf)*WORD_BUF_SIZE);

	struct allocator* alloc = scrallox();
	assert(cm->word_lut == NULL);
	sh_new_strdup_with_context(cm->word_lut, alloc);
	assert(cm->word_index_arr == NULL);
	arrinit(cm->word_index_arr, alloc);
	assert(cm->wordscope0_arr == NULL);
	arrinit(cm->wordscope0_arr, alloc);
	assert(cm->wordskip_addraddr_arr == NULL);
	arrinit(cm->wordskip_addraddr_arr, alloc);

	cm->program = program;
	program_reset(cm->program);

	//cm->next_user_word_id = _FIRST_USER_WORD_;
	cm->tokenizer_state = WORD;

	// setup thread local VM as comptime VM
	struct vmie* vm = &tlg.vmie;
	vmie_init(vm, program);
	vm->sew_target = program;

	compiler_process_preamble(cm);

	compiler_reset_location(cm);
}

static void compiler_end(struct compiler* cm)
{
	assert(tlg.currently_compiling == 1);
	tlg.currently_compiling = 0;
	if (cm->has_error) return;

	compiler_push_char(cm, ((struct thicchar){.codepoint = 0}));

	switch (cm->tokenizer_state) {
	case WORD: /* OK */ break;
	case STRING:  compiler_errorf(cm, "EOF in unterminated string"); break;
	case COMMENT: compiler_errorf(cm, "EOF in unterminated comment"); break;
	default:      compiler_errorf(cm, "EOF in unexpected tokenizer state (%d)", cm->tokenizer_state); break;
	}
	if (cm->has_error) return;

	if (cm->colon) {
		compiler_errorf(cm, "EOF while expecting colon suffix");
		return;
	}

	if (cm->prefix_comptime) {
		compiler_errorf(cm, "EOF while expecting comptime suffix");
		return;
	}

	const int depth = arrlen(cm->wordscope0_arr);
	if (depth > 0) {
		compiler_errorf(cm, "EOF inside word definition (depth=%d)", depth);
		return;
	}

	// check comptime VM for post-EOF errors
	// XXX rather crude now; it assumes that a non-empty stack mean some kind
	// of syntax error (which is probably right), but ideally the comptime VM
	// itself should explain what the problem is (since the VM doesn't even
	// know what if/else/then looks like it can't say "looks like invalid
	// if/else/then syntax")
	struct vmie* vm = get_comptime_vm();
	const size_t comptime_stack_height = arrlen(vm->stack_arr);
	const size_t comptime_rstack_height = arrlen(vm->rstack_arr);
	if ((comptime_stack_height != 0) || (comptime_rstack_height != 0)) {
		compiler_errorf(cm, "EOF while comptime VM had non-empty stack/rstack (%zd/%zd); implies syntax error?",
			comptime_stack_height,
			comptime_rstack_height);
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
	shfree(cm->word_lut);
	assert(cm->word_lut == NULL);
}

static int alloc_program_index(void)
{
	assert((tlg.currently_compiling == 0) && "the compiler has a program reference, making it dangerous to mutate program_arr");
	//assert(thrd_success == mtx_lock(&g.program_alloc_mutex));
	int r;
	struct program* program;
	if (arrlen(g.program_freelist_arr) > 0) {
		r = arrpop(g.program_freelist_arr);
		program = arrchkptr(g.program_arr, r);
	} else {
		r = arrlen(g.program_arr);
		program = arraddnptr(g.program_arr, 1);
		program_init(program);
	}
	//assert(thrd_success == mtx_unlock(&g.program_alloc_mutex));
	program_reset(program);
	return r;
}

void mie_program_free(int program_index)
{
	//assert(thrd_success == mtx_lock(&g.program_alloc_mutex));
	// XXX the following could be regarded as an "expensive assert"?
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

int mie_compile_thicc(const struct thicchar* src, int num_chars)
{
	const int program_index = alloc_program_index();
	struct compiler* cm = &tlg.compiler;
	compiler_begin(cm, get_program(program_index));
	for (int i=0; i<num_chars; ++i) {
		compiler_push_char(cm, src[i]);
		if (cm->has_error) {
			compiler_end(cm);
			return -1;
		}
	}
	compiler_end(cm);
	if (cm->has_error) return -1;
	return program_index;
}

// with apologies to Frank Gray
int mie_compile_graycode(const char* utf8src, int num_bytes)
{
	const int program_index = alloc_program_index();
	struct compiler* cm = &tlg.compiler;
	compiler_begin(cm, get_program(program_index));
	if (-1 == compiler_process_utf8src(cm, utf8src, num_bytes)) {
		compiler_end(cm);
		return -1;
	}
	compiler_end(cm);
	if (cm->has_error) return -1;
	return program_index;
}

const char* mie_error(void)
{
	return tlg.error_message;
}


static void init_globals(void)
{
	if (g.globals_were_initialized) return;
	g.globals_were_initialized = 1;

	//assert(thrd_success == mtx_init(&g.program_alloc_mutex, mtx_plain));
	arrinit(g.program_arr, &system_allocator);
	arrinit(g.program_freelist_arr, &system_allocator);

	char buf[1<<14];
	stbsp_snprintf(buf, sizeof buf, ""

		// define words for VAL_INT etc...
		#define X(ENUM) ": " #ENUM " %di ;\n"
		LIST_OF_VAL_TYPES
		#undef X

		// replace %d in fmt with actual int values
		#define X(ENUM) ,ENUM
		LIST_OF_VAL_TYPES
		#undef X

	);
	//printf("%s\n", buf);
	g.preamble_val_types = strdup(buf);
	g.preamble_val_types_size = strlen(g.preamble_val_types);
}

static void init_thread_locals(void)
{
	if (tlg.thread_locals_were_initialized) return;
	tlg.thread_locals_were_initialized = 1;
	#ifndef MIE_ONLY_SYSALLOC
	init_scratch_allocator(&tlg.our_scratch_allocator, 1L<<24);
	#else
	printf("WARNING: using system allocator instead of scratch allocator in mie.c; not for production! leaks galore!\n");
	#endif
}

void mie_thread_init(void)
{
	init_globals();
	init_thread_locals();
}

void vmie_reset(int program_index)
{
	vmie_init(&tlg.vmie, get_program(program_index));
}

int vmie_run(void)
{
	return vmie_run2(&tlg.vmie);
}

int vmie_get_stack_height(void)
{
	struct vmie* vm = &tlg.vmie;
	return arrlen(vm->stack_arr);
}

struct val vmie_val(int i)
{
	struct vmie* vm = &tlg.vmie;
	struct val v = arrchkget(vm->stack_arr, arrlen(vm->stack_arr)-1-i);
	return v;
}

static void selftest_fail(const char* context, const char* src)
{
	fprintf(stderr, "selftest %s error [%s] for:\n %s\n", context, mie_error(), src);
	abort();
}

void vmie_dump_val(struct val v)
{
	FILE* out = stderr;
	switch (v.type) {
	case VAL_INT:    fprintf(out, "%di", v.i32); break;
	case VAL_FLOAT:  fprintf(out, "%f", v.f32); break;
	case VAL_STR:
		fprintf(out, "\"");
		int n;
		struct thicchar* tc = resolve_string(&tlg.vmie, v.i32, &n);
		for (int i=0; i<n; ++i) fprintf(out, "%c", tc[i].codepoint); // XXX no utf8 support
		fprintf(out, "\" (%d)", n);
		break;
	default:         fprintf(out, "%uT:%u(?)", v.type, (uint32_t)v.i32); break;
	}
}

void vmie_dump_stack(void)
{
	const int d = vmie_get_stack_height();
	if (d == 0) {
		fprintf(stderr, "=== EMPTY STACK ===\n");
	} else {
		fprintf(stderr, "=== STACK (n=%d) ===\n", d);
		for (int i=0; i<d; ++i) {
			struct val v = vmie_val(i);
			fprintf(stderr, " stack[%d] = ", (-1-i));
			vmie_dump_val(v);
			fprintf(stderr, "\n");
		}
	}
}

void mie_begin_scrallox(void)
{
	struct scratch_context_header* h = get_scratch_context_header(&tlg.our_scratch_allocator);
	h->has_abort_jmp_buf = 0;
	memset(&h->abort_jmp_buf, 0, sizeof h->abort_jmp_buf);
	begin_scratch_allocator(&tlg.our_scratch_allocator);
}

jmp_buf* mie_prep_scrallox_jmp_buf_for_out_of_memory(void)
{
	struct scratch_context_header* h = get_scratch_context_header(&tlg.our_scratch_allocator);
	assert((h->in_scope) && "not in scope (call mie_scrallox_get_jmpbuf() just after mie_begin_scrallox())");
	h->has_abort_jmp_buf = 1;
	return &h->abort_jmp_buf;
}

void mie_end_scrallox(void)
{
	// NOTE friendly reminder that this function is asked to be called even
	// when scrallox runs out of memory and longjmps (see:
	// mie_prep_scrallox_jmp_buf_for_out_of_memory()). so keep that in mind if
	// "overloading" this function.
	end_scratch_allocator(&tlg.our_scratch_allocator);
}

struct allocator* mie_borrow_scrallox(void)
{
	assert((get_scratch_context_header(&tlg.our_scratch_allocator)->in_scope) && "borrow out of scope");
	return &tlg.our_scratch_allocator;
}

void mie_scrallox_save(int savestate_id)
{
	struct scratch_context_header* h = get_scratch_context_header(&tlg.our_scratch_allocator);
	assert((h->in_scope) && "not in scrallox scope");
	assert(tlg.compiler.program == tlg.vmie.program);
	if (arrlen(tlg.savestate_arr) <= savestate_id) {
		arrsetlen(tlg.savestate_arr, savestate_id);
	}
	struct savestate* sav = &tlg.savestate_arr[savestate_id];
	program_copy(&sav->program, tlg.compiler.program);
	struct program* prg = &sav->program;
	sav->compiler = tlg.compiler;
	sav->compiler.program = prg;
	sav->vmie = tlg.vmie;
	sav->vmie.program = prg;
	sav->expected_header = h;
	if (sav->scratch_buf == NULL) arrinit(sav->scratch_buf, &system_allocator);
	arrsetlen(sav->scratch_buf, h->allocated);
	memcpy(sav->scratch_buf, get_scratch_memory_ptr(h), h->allocated);
}

void mie_scrallox_restore(int savestate_id)
{
	struct scratch_context_header* h = get_scratch_context_header(&tlg.our_scratch_allocator);
	assert((h->in_scope) && "not in scrallox scope");
	const int n = arrlen(tlg.savestate_arr);
	assert((0 <= savestate_id) && (savestate_id < n));
	struct savestate sav = tlg.savestate_arr[savestate_id];
	assert((sav.expected_header == h) && "cannot restore! scrallox was re-initialized?");
	// TODO maybe do a `int mie_scrallox_can_restore(int savestate_id)` if it makes sense?
	tlg.compiler = sav.compiler;
	tlg.vmie = sav.vmie;
	h->allocated = arrlen(sav.scratch_buf);
	memcpy(get_scratch_memory_ptr(h), sav.scratch_buf, h->allocated);
}

void mie_scrallox_stats(size_t* out_allocated, size_t* out_capacity)
{
	struct scratch_context_header* h = get_scratch_context_header(&tlg.our_scratch_allocator);
	assert((h->in_scope) && "not in scope (call stats before mie_end_scrallox())");
	if (out_allocated) *out_allocated = h->allocated;
	if (out_capacity) *out_capacity   = h->capacity;
}

void mie_selftest(void)
{
	mie_thread_init();

	// simple mechanism for only running the tests you want to run: put TAG
	// in-front of the source you want to whitelist (or make the first
	// character \a if you prefer), and set ONLY_TAGGED=1. it depends on
	// sections using our ACCEPT(src) macro, so keep that in mind (add it to
	// new sections / check for missing ACCEPT())
	#define TAG "\a"
	const int ONLY_TAGGED=0;
	#define ACCEPT(SRC) (ONLY_TAGGED ? (SRC)[0]=='\a' && !!(++(SRC)) : 1)

	// TODO maybe these tests should also assert the correct /cause/ of
	// failure? it's pretty easy to "break" these tests without noticing. e.g.
	// consider if "comptime" was renamed to "compile_time", then all the
	// "improper comptime position" tests would not notice that the error had
	// changed to "'comptime' not found". it might not be the worst idea to
	// just match a substring from the error message...

	const char* programs_that_fail_to_compile[] = {
		// word not defined:
		"xxxxxxxxxxxx",

		// bad EOF states:
		":",
		": foo ",
		": foo 42",
		": foo 42 : ",
		": foo 42 : bar ",
		": foo 42 : bar 420 ",
		": foo 42 : bar 420 ; ",
		"comptime",
		"comptime :",

		// nonsensical comptime
		": foo comptime ;",

		// unbalanced comments:
		"(",
		"((",
		"(()",
		"())",
		"(()))",
	};

	for (int i=0; i<ARRAY_LENGTH(programs_that_fail_to_compile); ++i) {
		const char* src = programs_that_fail_to_compile[i];
		if (!ACCEPT(src)) continue;
		mie_begin_scrallox();
		const int prg = mie_compile_graycode(src, strlen(src));
		mie_end_scrallox();
		if (prg != -1) {
			fprintf(stderr, "selftest expected compile error, but didn't get it for:\n %s\n", src);
			abort();
		}
	}

	const char* programs_that_fail_at_runtime[] = {
		// halt always fails
		"halt", "\"error message\" halt",

		// a bunch of stack underflows
		"drop", "ROTATE", "F*",
	};

	for (int i=0; i<ARRAY_LENGTH(programs_that_fail_at_runtime); ++i) {
		const char* src = programs_that_fail_at_runtime[i];
		if (!ACCEPT(src)) continue;
		mie_begin_scrallox();
		const int prg = mie_compile_graycode(src, strlen(src));
		if (prg == -1) selftest_fail("compile", src);
		vmie_reset(prg);
		const int r = vmie_run();
		mie_end_scrallox();
		if (r != -1) {
			fprintf(stderr, "selftest expected runtime error, but didn't get it for:\n %s\n", src);
			abort();
		}
	}

	// these must all leave 1i (and only 1i) on the stack after execution
	const char* programs_that_eval_to_1i[] = {
		"1i",
		"NOP 1i (blah (blah (blah))) NOP",
		"1i 1i I=",
		"1i 0i I!=",
		"-1i I~",
		"1i I~ I~",
		"1i 0i PICK drop",
		// TODO PICK for values larger than 0?

		"0  1i        2i  1i ROTATE (0 1 -- 1 0)   drop (1 0 -- 1)",
		"0  1i        2i -1i ROTATE (0 1 -- 1 0)   drop (1 0 -- 1)",
		"0  1i        2i -3i ROTATE (0 1 -- 1 0)   drop (1 0 -- 1)",
		"0  0  1i     3i  2i ROTATE (0 0 1 -- 1 0 0)   drop drop (1 0 0 -- 1)",
		"0  1i 0      3i -2i ROTATE (0 1 0 -- 1 0 0)   drop drop (1 0 0 -- 1)",
		"0  0  1i     3i  1i ROTATE (0 0 1 -- 0 1 0)   3i  1i ROTATE (0 1 0 -- 1 0 0)   drop drop (1 0 0 -- 1)",
		"0  1i 0      3i -1i ROTATE (0 1 0 -- 0 0 1)   3i -1i ROTATE (0 0 1 -- 1 0 0)   drop drop (1 0 0 -- 1)",
		"0  0  1i     3i -2i ROTATE (0 0 1 -- 0 1 0)   3i -2i ROTATE (0 1 0 -- 1 0 0)   drop drop (1 0 0 -- 1)",
		"0  1i 0      3i  2i ROTATE (0 1 0 -- 0 0 1)   3i  2i ROTATE (0 0 1 -- 1 0 0)   drop drop (1 0 0 -- 1)",
		"0 0 0 1i 0   5i  3i ROTATE (0 0 0 1 0 -- 1 0 0 0 0) drop drop drop drop",
		"0 0 0 1i 0   5i -2i ROTATE (0 0 0 1 0 -- 1 0 0 0 0) drop drop drop drop",

		"-1i 2i I+",
		"256i 8i I>>",
		"16i 4i I<< 8i I>>",
		"1 1 F=",
		"1 -1 F~ F=",
		"-1 F~ 1 F=",

		// NOTE euclidean integer division tests (e.g. -3/4=-1); some of these
		// fail with "truncating integer division" (-3/4=0) (you can try this
		// for yourself by changing `stb_div_eucl(a,b)` to `a/b` somewhere
		// above, assuming your CPU/compiler uses truncating division)
		"7i 4i I/",
		"-3i 4 I/ I~",
		"-5i -7i I/",

		": fsqr 0i PICK F* ; 42 fsqr 1764 F=",
		": fsqr 0i : foo 101 ; PICK F* ; -42 fsqr 1764 F=",
		"-1i : foo 101 drop ; foo foo I~ foo foo",

		" 1i : foooooooo ; ",
		" : foo ;   1i ",
		" comptime : foo ;   1i ",
		" 1i comptime : foo ;   foo ",
		" comptime : foo <# 1i #> ;  foo",
		" comptime : div-7 <# -7i I/ #> ;  -5i div-7 ",
	};

	for (int i=0; i<ARRAY_LENGTH(programs_that_eval_to_1i); ++i) {
		const char* src = programs_that_eval_to_1i[i];
		if (!ACCEPT(src)) continue;
		mie_begin_scrallox();
		const int prg = mie_compile_graycode(src, strlen(src));
		if (prg == -1) selftest_fail("compile", src);
		vmie_reset(prg);
		const int r = vmie_run();
		if (r == -1) selftest_fail("run", src);
		if (r != 1) {
			fprintf(stderr, "selftest assert fail, expected stack height of 1\n");
			vmie_dump_stack();
			abort();
		}
		struct val v = vmie_val(0);
		if (v.type != VAL_INT || v.i32 != 1) {
			fprintf(stderr, "selftest expected one stack element, 1i, got: ");
			vmie_dump_val(v);
			fprintf(stderr, " for:\n %s\n", src);
			abort();
		}
		mie_end_scrallox();
	}

	#undef ACCEPT
	#undef TAG
}

// TODO stack traces (just knowing location at pc isn't enough)... does this
// mean rstack ought to be "sealed"?

// FIXME comptime issues/thoughts
//  - complex values can't be exported/sewn from comptime. it would technically
//    be possible to sew any value by writing a program that reconstructs the
//    value. e.g. strings can be made from integer literals and STRNEW calls,
//    but I don't know it it's a good idea.
//  - globals stored during comptime aren't available at runtime.
//  - related: operator overloading doesn't work inside comptime because it
//    relies on globals being properly set up (currently only set up in the
//    runtime)
//  - it doesn't seem like the worst idea to migrate all globals from comptime
//    to runtime. you could argue that you risk migrating comptime-only globals
//    too, but we already do that with comptime words (no dead code elimination
//    yet)

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


// TODO optimizations?

// - entrypoint and skip-jump optimizations? words are wrapped in a "skip jump"
//   to prevent inadvertently entering a word without a JSR/call, but: skips
//   can be joined if several words are defined in a row without top-level code
//   in between, i.e. the first skip can jump to the last jump target. also,
//   word/top-level entrypoints can be "forwarded" to the first instruction.

// - "savepoints"? parsing a large stdlib before 10 lines of livecode seems
//   like a waste; can we snapshot it after parsing stdlib and restore from
//   that point repeatedly? (needs to be somewhat cheaper and not too difficult
//   to be worth it?). shouldn't it be "just a memcpy"? assuming that the
//   scratch base pointer doesn't move? (and it might, due to user prefs, but
//   then we "just" have to start over and parse stdlib again)

// - "dead word elimination"?

// - constant folding? i.e. replacing `3 4 *` with `12`? maybe it's possible to
//   do in an "optimizer pass" that works directly on bytecode? is it a
//   "comptime thing" somehow? operator overloading makes this harder..

// - optimize built-in word lookup? (currently a long series of strcmp()s)

// Not sure about these:
// - short jumps and short literals? during comptime, JMP/JMP0 has to be fixed
//   length for if/else/then to work, but it might be possible to optimize the
//   opcode stream afterwards?
