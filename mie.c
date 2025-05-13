// mie (forth-like language) compiler and vmie virtual machine
//  - see mie_selftest() near bottom for small programs that test compiler+vm
//  - see built-in word definitions near top of this file
//  - copious use of macros, including "X macros". search for e.g. BINOP for
//    how binary operators (like `a+b`/`a b +`) are implemented with macros
//  - compilation and program execution (vmie) uses "scratch memory" to
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
//  - ENUM: enum identifier for the word (ops also get an "OP_"-prefix)
//  - STR: string name; pass NULL to use the string representation of ENUM;
//    pass "" if the word cannot be used directly from the source.
//  - DOC: short documentation of the word/op

#define LIST_OF_SYNTAX_WORDS \
	/*<ENUM>      <STR>     <DOC> */ \
	/* ====================================================================== */ \
	X( COLON     , ":"        , "Word definiton, followed by word (name), e.g. `: foo`") \
	X( COLONADDR , ":&"       , "Word address definiton, followed by word (name), e.g. `: foo_addr`") \
	X( SEMICOLON , ";"        , "End of word definition") \
	X( COMPTIME  , "comptime" , "Compile-time prefix for next word") \
	X( ENTER_SEW , "<#"       , "Start writing instructions outside of comptime (\"sewing\")") \
	X( LEAVE_SEW , "#>"       , "Stop writing instructions outside of comptime") \
	/* ====================================================================== */


// these words have direct 1:1 mappings to vmie VM ops.
// `foo:i32` means `foo` is bitwise ("reinterpret") cast to i32, i.e. without
// any type checking. by convention, words that do bitwise casting should not
// be lowercased since that "namespace" is reserved for typesafe words (so
// "pick" can work with both i32 and f32 indices, but "PICK" assumes i32
// without checking)
#define LIST_OF_OP_WORDS \
	/*<ENUM>      <STR>       <DOC> */ \
	/* ====================================================================== */ \
	X( NOP        , NULL         , "No operation ( -- )") \
	X( HALT       , "halt"       , "Halt execution with an error") \
	X( RETURN     , "return"     , "Return from subroutine [returnaddr -- ]") \
	X( DROP       , "drop"       , "Remove top element from stack (x --)") \
	X( PICK       , NULL         , "Pop n:i32, duplicate stack value (n -- stack[-1-n])") \
	X( ROTATE     , NULL         , "Pop d:i32, then n:i32, then rotate n elements d places") \
	X( EQ         , "="          , "Equals (x y -- x=y)") \
	X( TYPEOF     , "typeof"     , "Get type (x -- typeof(x))") \
	X( CAST       , NULL         , "Set type (x T -- T(x))") \
	X( HERE       , "here"       , "Push current instruction pointer to rstack [-- ip]") \
	X( JMPI       , NULL         , "Pop address:i32 from stack => indirect jump (address -- )" ) \
	X( JSRI       , NULL         , "Pop address:i32 from stack => indirect jump-to-subroutine (address --)" ) \
	X( I2R        , "I>R"        , "Pop i:i32 from stack, push it onto rstack (i --) [-- i]") \
	X( R2I        , "R>I"        , "Moves integer value back from rstack [i --] (-- i)") \
	X( F2I        , "F>I"        , "Pop a:f32, push after conversion to i32 (a:f32 -- i)") \
	X( I2F        , "I>F"        , "Pop a:i32, push after conversion to f32 (a:i32 -- f)") \
	X( SET_GLOBAL , "SET-GLOBAL" , "Pop index:i32, then value, set globals[index]=value (value index --)") \
	X( GET_GLOBAL , "GET-GLOBAL" , "Pop index:i32, push globals[index] (index -- globals[index])") \
	/* ====================================================================== */ \
	/* All inputs are x:f32/y:f32 (bitwise cast to f32) */ \
	X( FADD      , "F+"       , "Floating-point add (x y -- x+y)") \
	X( FNEG      , "F~"       , "Floating-point negate (x -- -x)") \
	X( FMUL      , "F*"       , "Floating-point multiply (x y -- x*y)") \
	X( FMOD      , "F%"       , "Floating-point remainder/modulus (x y -- x%y)") \
	X( FINV      , "F1/"      , "Floating-point reciprocal (x -- 1/x)") \
	X( FDIV      , "F/"       , "Floating-point division (x y -- x/y)") \
	X( FLT       , "F<"       , "Floating-point less than (x y -- x<y)") \
	X( FLE       , "F<="      , "Floating-point less than or equal (x y -- x<=y)") \
	X( FNE       , "F!="      , "Floating-point not equal (x y -- x!=y)") \
	X( FEQ       , "F="       , "Floating-point equal (x y -- x=y)") \
	X( FGE       , "F>="      , "Floating-point greater than or equal (x y -- x>=y)") \
	X( FGT       , "F>"       , "Floating-point greater than (x y -- x>y)") \
	/* ====================================================================== */ \
	/* All inputs are x:i32/y:i32 (bitwise cast to i32) */ \
	X( IADD      , "I+"       , "Integer add (x y -- x+y)") \
	X( INEG      , "I~"       , "Integer negate (x -- -x)") \
	X( IMUL      , "I*"       , "Integer multiply (x y -- x*y)") \
	X( IDIV      , "I/"       , "Integer euclidean division (x y -- x//y)") \
	X( IMOD      , "I%"       , "Integer euclidean remainder/modulus (x y -- x%y)") \
	X( IBAND     , "I&"       , "Integer bitwise AND (x y -- x&y)") \
	X( IBOR      , "I|"       , "Integer bitwise OR (x y -- x|y)") \
	X( IBXOR     , "I^"       , "Integer bitwise XOR (x y -- x^y)") \
	X( IBNOT     , "I!"       , "Integer bitwise NOT (x -- !y)") \
	X( ILAND     , "I&&"      , "Integer logical AND (x y -- x&&y)") \
	X( ILOR      , "I||"      , "Integer logical OR (x y -- x||y)") \
	X( ILXOR     , "I^^"      , "Integer logical XOR (x y -- x^^y)") \
	X( ILNOT     , "I!!"      , "Integer logical NOT (x -- !!y)") \
	X( ILSHIFT   , "I<<"      , "Integer shift left (x y -- x<<y)") \
	X( IRSHIFT   , "I>>"      , "Integer shift right (x y -- x>>y)") \
	X( ILT       , "I<"       , "Integer less than (x y -- x<y)") \
	X( ILE       , "I<="      , "Integer less than or equal (x y -- x<=y)") \
	X( IEQ       , "I="       , "Integer equal (x y -- x=y)") \
	X( INE       , "I!="      , "Integer equal (x y -- x!=y)") \
	X( IGE       , "I>="      , "Integer greater than or equal (x y -- x>=y)") \
	X( IGT       , "I>"       , "Integer greater than (x y -- x>y)") \
	/* ====================================================================== */ \
	/* convention (not sure if it's a good one): if it only reads the array */ \
	/* it consumes the array; if it mutates the array, the array is the */ \
	/* bottommost return value */ \
	X( ARRNEW    , "arrnew"   , "Create array (-- arr)") \
	X( ARRLEN    , "arrlen"   , "Length of array (arr -- len(arr))") \
	X( ARRGET    , NULL       , "Get element (arr i -- arr[i])") \
	X( ARRPUT    , "arrput"   , "Append item to array ([x] y -- [x,y])") \
	X( ARRPOP    , "arrpop"   , "Get top element ([x,y] -- [x] y)") \
	X( ARRSET    , NULL       , "Set element (arr index value -- arr)") \
	X( ARRJOIN   , "arrjoin"  , "Join two arrays (arr1 arr2 -- arr1..arr2)") \
	X( ARRSPLIT  , NULL       , "Split array at pivot (arr pivot -- arr[0:pivot] arr[pivot:])") \
	/* ====================================================================== */ \
	X( MAPNEW    , "mapnew"   , "Create map (-- map)") \
	X( MAPHAS    , "maphas"   , "Returns if key exists in map (map key -- exists?)") \
	X( MAPGET    , "mapget"   , "Get value from map (map key -- value)") \
	X( MAPSET    , "mapset"   , "Set value in map (map key value -- map)") \
	X( MAPDEL    , "mapdel"   , "Delete key from map (map key -- map)") \
	/* ====================================================================== */ \
	X( THERE         , "there"         , "Get PC outside of comptime (-- addr)") \
	X( NAVIGATE      , "navigate"      , "Set PC outside of comptime (addr --)") \
	X( SEW           , NULL            , "Write raw i32 or f32 outside of comptime (val --)") \
	/* XXX should the following be stdlib stuff? */ \
	X( SEW_JMP       , "SEW-JMP"       , "Write JMP outside of comptime (addr --)") \
	X( SEW_JMP0      , "SEW-JMP0"      , "Write JMP0 outside of comptime (addr --)") \
	X( SEW_JSR       , "SEW-JSR"       , "Write JSR outside of comptime (addr --)") \
	X( SEW_ADDR      , "SEW-ADDR"      , "Write jump address outside of comptime (addr --)") \
	X( SEW_LIT       , "SEW-LIT"       , "Write literal outside of comptime (lit --)") \
	X( SEW_COLON     , "SEW-COLON"     , "Begin word def outside of comptime (name --)") \
	X( SEW_SEMICOLON , "SEW-SEMICOLON" , "End word def outside of comptime") \
	/* ====================================================================== */


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

	case OP_INT_LITERAL   : return "INT_LITERAL";
	case OP_FLOAT_LITERAL : return "FLOAT_LITERAL";
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

enum tokenizer_state {
	WORD=1,
	STRING,
	COMMENT,
};

#define MAX_ERROR_MESSAGE_SIZE (1<<14)
#define WORD_BUF_CAP (1<<8)
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

struct program {
	int write_cursor;
	union pword* op_arr;
	// be careful about grabbing references to op_arr; arrput may change the
	// pointer (realloc), and both the compiler and comptime VM may write new
	// instructions (in fact, comptime VM executes part of the program while
	// writing another part? feels "a bit fucked" but it should work as long as
	// you don't grab references?)
	struct location* pc_to_location_arr;
	int entrypoint_address;
	// TODO: compile error?
};


struct compiler {
	struct location location, error_location;
	enum tokenizer_state tokenizer_state;
	int comment_depth;
	char word_buf[WORD_BUF_CAP];
	int word_size;
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

struct val_arr {
	struct val* arr;
};

struct val_map {
	struct {
		struct val key,value;
	}* map;
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
	struct val_i32arr* i32arr_arr;
	struct val_f32arr* f32arr_arr;
};

struct vmie {
	// TODO?
	//  - globals?
	//  - instruction counter/remaining (for cycle limiting)
	struct val* stack_arr;
	uint32_t* rstack_arr;
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

THREAD_LOCAL static struct {
	struct compiler compiler;
	#ifndef MIE_ONLY_SYSALLOC
	struct allocator scratch_allocator;
	struct scratch_context_header* scratch_header;
	#endif
	struct vmie vmie;
	const char* error_message;
	unsigned thread_locals_were_initialized  :1;
	unsigned currently_compiling             :1;
} tlg; // thread-local globals


static inline struct allocator* get_scratch_allocator(void)
{
	#ifdef MIE_ONLY_SYSALLOC
	return &system_allocator; // only for test
	#else
	return &tlg.scratch_allocator;
	#endif
}

static void reset_our_scratch(void)
{
	#ifndef MIE_ONLY_SYSALLOC
	reset_scratch(tlg.scratch_header);
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
		case VAL_INT: {
			program_sew(p, d2, VAL_INT, PWORD_INT(OP_INT_LITERAL), loc);
			program_sew(p, d2, VAL_INT, pword, loc);
			program_sew(p, d2, VAL_INT, PWORD_INT(OP_SEW), loc);
		}	break;
		case VAL_FLOAT: {
			program_sew(p, d2, VAL_INT,   PWORD_INT(OP_FLOAT_LITERAL), loc);
			program_sew(p, d2, VAL_FLOAT, pword, loc);
			program_sew(p, d2, VAL_INT,   PWORD_INT(OP_SEW), loc);
		}	break;
		default: assert(!"bad/unhandled type");
		}
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
	struct allocator* a = get_scratch_allocator();
	return a->fn_realloc(a->allocator_context, NULL, sz);
}

static void vmie_init(struct vmie* vm, struct program* program)
{
	memset(vm, 0, sizeof *vm);
	arrinit(vm->stack_arr, get_scratch_allocator());
	arrinit(vm->rstack_arr, get_scratch_allocator());
	arrinit(vm->global_arr, get_scratch_allocator());
	vm->program = program;
	vm->pc = program->entrypoint_address;
	vm->error_message = scratch_alloc(MAX_ERROR_MESSAGE_SIZE);
	vm->error_message[0] = 0;
}

#define VMIE_STACK_GUARD(OP,N) \
	if (STACK_HEIGHT() < (N)) { \
		vmie_errorf(vm, "%s expected a minimum stack height of %d, but it was only %d", get_opcode_str(OP), (N), STACK_HEIGHT()); \
		return -1; \
	}

#define VMIE_OP_STACK_GUARD(N) VMIE_STACK_GUARD(op,N)

#define VMIE_SEW_GUARD \
	if (vm->sew_target == NULL) { \
		vmie_errorf(vm, "no sew target (called outside of comptime vm?)"); \
		return -1; \
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


struct val vmie_top(struct vmie* vm)
{
	const int n = arrlen(vm->stack_arr);
	assert(n>0);
	return vm->stack_arr[n-1];
}

static void vmie_dup(struct vmie* vm)
{
	struct val v = vmie_top(vm);
	arrput(vm->stack_arr, v);
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
			switch (deferred_op) {

			case OP_JMP: {
				if (TRACE) printf(" JMP pc %d->%d\n", pc, pw->u32);
				next_pc = pw->u32;
			}	break;

			case OP_JMP0: {
				if (TRACE) printf(" JMP pc %d->%d\n", pc, pw->u32);
				VMIE_STACK_GUARD(deferred_op,1)
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
				if (TRACE) printf(" FLITERAL %f\n", pw->f32);
				arrput(vm->stack_arr, floatval(pw->f32));
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

			switch (op) {

			case OP_NOP: {
				if (TRACE) printf(" NOP\n");
			}	break;

			case OP_HALT: {
				if (TRACE) printf(" HALT\n");
				vmie_errorf(vm, "HALT");
				return -1;
			}	break;

			// these ops take another argument
			case OP_JMP:
			case OP_JMP0:
			case OP_JSR:
			case OP_INT_LITERAL:
			case OP_FLOAT_LITERAL:
				set_num_defer = 1;
				break;

			case OP_JMPI:
			case OP_JSRI: {
				VMIE_OP_STACK_GUARD(1)
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
				VMIE_OP_STACK_GUARD(1)
				(void)arrpop(vm->stack_arr);
			}	break;

			case OP_PICK: {
				VMIE_OP_STACK_GUARD(1)
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
				VMIE_OP_STACK_GUARD(2)
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

			case OP_TYPEOF: { // (x -- typeof(x))
				VMIE_OP_STACK_GUARD(1)
				const struct val x = arrpop(vm->stack_arr);
				arrput(vm->stack_arr, intval(x.type));
			}	break;

			case OP_CAST: { // (value T -- T(value))
				VMIE_OP_STACK_GUARD(2)
				const struct val T = arrpop(vm->stack_arr);
				struct val value = arrpop(vm->stack_arr);
				value.type = T.i32;
				arrput(vm->stack_arr, value);
			}	break;

			case OP_I2F: {
				VMIE_OP_STACK_GUARD(1)
				const int v = arrpop(vm->stack_arr).i32;
				arrput(vm->stack_arr, floatval(v));
			}	break;

			case OP_F2I: {
				VMIE_OP_STACK_GUARD(1)
				const float v = arrpop(vm->stack_arr).f32;
				arrput(vm->stack_arr, intval((int)floorf(v)));
			}	break;

			case OP_SET_GLOBAL: {
				VMIE_OP_STACK_GUARD(2)
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
				VMIE_OP_STACK_GUARD(1)
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
				VMIE_OP_STACK_GUARD(2) \
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
				VMIE_OP_STACK_GUARD(1) \
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
				VMIE_OP_STACK_GUARD(2) \
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
				VMIE_OP_STACK_GUARD(1) \
				const struct val va = arrpop(vm->stack_arr); \
				const int32_t a=va.i32; \
				arrput(vm->stack_arr, intval(EXPR)); \
			}	break;
			DEF_IUNOP( OP_INEG  , (-a) )
			DEF_IUNOP( OP_IBNOT , (~a) )
			DEF_IUNOP( OP_ILNOT , (!a) )
			#undef DEF_IUNOP

			// = arrays =====================

			case OP_ARRNEW: {
				struct valstore* vals = &vm->vals;
				if (vals->arr_arr == NULL) {
					arrinit(vals->arr_arr, get_scratch_allocator());
				}
				assert(vals->arr_arr != NULL);
				const int32_t id = (int32_t)arrlen(vals->arr_arr);
				struct val_arr* arr = arraddnptr(vals->arr_arr, 1);
				arrinit(arr->arr, get_scratch_allocator());
				arrput(vm->stack_arr, typeval(VAL_ARR, id));
			}	break;

			case OP_ARRLEN: {
				VMIE_OP_STACK_GUARD(1)
				struct val_arr* arr = vmie_pop_arr(vm);
				if (arr == NULL) return -1;
				arrput(vm->stack_arr, intval(arrlen(arr->arr)));
			}	break;

			case OP_ARRPUT: {
				VMIE_OP_STACK_GUARD(2)
				const struct val v = arrpop(vm->stack_arr);
				const struct val k = vmie_top(vm);
				struct val_arr* arr = vmie_pop_arr(vm);
				if (arr == NULL) return -1;
				arrput(arr->arr, v);
				arrput(vm->stack_arr, k);
			}	break;

			case OP_ARRGET: {
				VMIE_OP_STACK_GUARD(2)
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
					arrinit(vals->map_arr, get_scratch_allocator());
				}
				assert(vals->map_arr != NULL);
				const int32_t id = (int32_t)arrlen(vals->map_arr);
				struct val_map* map = arraddnptr(vals->map_arr, 1);
				hminit(map->map, get_scratch_allocator());
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

			// ==============================

			case OP_THERE: {
				VMIE_SEW_GUARD
				const int c = vm->sew_target->write_cursor;
				arrput(vm->stack_arr, intval(c));
				if (TRACE) printf("THERE => %d\n", c);
			}	break;

			case OP_NAVIGATE: {
				VMIE_OP_STACK_GUARD(1)
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
				VMIE_OP_STACK_GUARD(1)
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
				VMIE_OP_STACK_GUARD(1)
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
				VMIE_OP_STACK_GUARD(1)
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
				VMIE_OP_STACK_GUARD(1)
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
					vmie_errorf(vm, "SEW_LIT on unhandled type (%d)", lit.type);
					return -1;
				}
			}	break;


			default:
				fprintf(stderr, "vmie: unhandled op %s (%u) at pc=%d\n", get_opcode_str(op), op, pc);
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
			.is_direct = (cm->colon==1),
			.is_addr = (cm->colon==2),
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
		case COLONADDR: {
			if (cm->sew_depth > 0) {
				compiler_errorf(cm, "colon not allowed in <#...#>");
				return;
			}

			assert(cm->colon == 0);
			cm->colon = bw==COLON?1:bw==COLONADDR?2:-1;
			assert(cm->colon>0);

			// insert skip jump (TODO skip-jump optimization?)
			compiler_push_opcode(cm, OP_JMP);
			// insert invalid placeholder address (we don't know it yet)
			const int placeholder_addraddr = compiler_push_int(cm, -1);
			// remember address of the placeholder address, so we can patch it
			// when we see a semicolon:
			arrput(cm->wordskip_addraddr_arr, placeholder_addraddr);
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
			assert(closing_word_index >= 0);
			struct word_info* info = &cm->word_lut[closing_word_index].value;
			info->is_sealed = 1;

			// finalize word skip jump by overwriting the placeholder address
			// now that we know where the word ends
			assert((arrlen(cm->wordskip_addraddr_arr) > 0) && "broken compiler?");
			const int skip_addraddr = arrpop(cm->wordskip_addraddr_arr);
			const int write_addr = program_addr(prg);
			assert((program_read_at(prg, skip_addraddr).u32 == 0xffffffffL) && "expected placeholder value");
			program_patch(prg, skip_addraddr, ((union pword){.u32=write_addr}));

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

		#define X(E,_S,_D) case E: do_encode_opcode = OP_##E; break;
		LIST_OF_OP_WORDS
		#undef X

		default: assert(!"TODO add missing built-in word handler"); break;
		}

		if (do_encode_opcode != _OP_NONE_) {
			if (!cm->prefix_comptime) {
				compiler_push_opcode(cm, do_encode_opcode);
			} else {
				assert(!"TODO execute comptime'd opcode");
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
		} else {
			assert(cm->sew_depth == 0);
			assert(!"TODO execute comptime'd literal");
			cm->prefix_comptime = 0;
		}
	} else if ((wi = shgeti(cm->word_lut, word)) >= 0) {
		struct word_info* info = &cm->word_lut[wi].value;
		if (info->is_comptime) {
			struct vmie* vm = &tlg.vmie;
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
		} else if (cm->prefix_comptime) {
			assert(!"TODO execute comptime'd user word");
			cm->prefix_comptime = 0;
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
		} else if ((c=='"') || (c=='\\')) {
			cm->tokenizer_state = STRING;
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
		compiler_push_char(cm, ((struct thicchar){.codepoint = codepoint}));
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

	reset_our_scratch();

	// clear cm, but keep some field(s)
	memset(cm, 0, sizeof *cm);
	cm->error_message = scratch_alloc(MAX_ERROR_MESSAGE_SIZE);
	cm->error_message[0] = 0;

	assert(cm->word_lut == NULL);
	sh_new_strdup_with_context(cm->word_lut, get_scratch_allocator());
	assert(cm->word_index_arr == NULL);
	arrinit(cm->word_index_arr, get_scratch_allocator());
	assert(cm->wordscope0_arr == NULL);
	arrinit(cm->wordscope0_arr, get_scratch_allocator());
	assert(cm->wordskip_addraddr_arr == NULL);
	arrinit(cm->wordskip_addraddr_arr, get_scratch_allocator());

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
	compiler_push_char(cm, ((struct thicchar){.codepoint = 0}));

	assert(tlg.currently_compiling == 1);
	tlg.currently_compiling = 0;

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
	struct vmie* vm = &tlg.vmie;
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

int mie_compile_thicc(const struct thicchar* src, int num_chars)
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
int mie_compile_graycode(const char* utf8src, int num_bytes)
{
	const int program_index = alloc_program_index();
	struct compiler* cm = &tlg.compiler;
	compiler_begin(cm, get_program(program_index));
	if (-1 == compiler_process_utf8src(cm, utf8src, num_bytes)) {
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

void mie_thread_init(void)
{
	#ifdef MIE_ONLY_SYSALLOC
	printf("(Built with MIE_ONLY_SYSALLOC; not for production!)\n");
	// memory checkers like valgrind have a harder time finding memory bugs
	// when you're managing memory yourself; MIE_ONLY_SYSALLOC prevents the use
	// of our own scratch allocator, and only uses the system_allocator. this
	// causes leaks, but valgrind can find some memory bugs this way.
	#endif
	init_globals();
	if (tlg.thread_locals_were_initialized) return;
	tlg.thread_locals_were_initialized = 1;
	#ifndef MIE_ONLY_SYSALLOC
	tlg.scratch_header = init_scratch_allocator(get_scratch_allocator(), 1L<<24);
	#endif
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
		const int prg = mie_compile_graycode(src, strlen(src));
		if (prg != -1) {
			fprintf(stderr, "selftest expected compile error, but didn't get it for:\n %s\n", src);
			abort();
		}
	}

	const char* programs_that_fail_at_runtime[] = {
		"halt", // halt always fails

		// a bunch of stack underflows
		"drop",
		"ROTATE",
		"F*",
	};

	for (int i=0; i<ARRAY_LENGTH(programs_that_fail_at_runtime); ++i) {
		const char* src = programs_that_fail_at_runtime[i];
		if (!ACCEPT(src)) continue;
		const int prg = mie_compile_graycode(src, strlen(src));
		if (prg == -1) selftest_fail("compile", src);
		vmie_reset(prg);
		const int r = vmie_run();
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
	}

	#undef ACCEPT
	#undef TAG
}

// TODO
//  - stack traces (just knowing location at pc isn't enough)... does this mean
//    rstack ought to be "sealed"?
//  - "address of user word" syntax


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
//   to be worth it?)

// - "dead word elimination"?

// - constant folding? i.e. replacing `3 4 *` with `12`? maybe it's possible to
//   do in an "optimizer pass" that works directly on bytecode? is it a
//   "comptime thing" somehow?

// - optimize built-in word lookup? (currently a long series of strcmp()s)

// Not sure about these:
// - program size optimizations, like encoding "short jumps" and "short
//   literals" as 1 program word instead of 2.
