/*
XXX leaks memory, dies on errors, drowns kittens; don't use this in Do code
without cleaning it up first
*/

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "atlsvm.h"
#include "dya.h"


struct glyph {
	struct stbrp_rect rect;
	int all_rects_index;
	int codepoint;
	long bitmap_offset;
	int xoff;
	int yoff;
};

struct glyph_table {
	struct glyph* glyphs;
	int n_glyphs;
	int height;
	int baseline;
	char* filename;
	int is_dummy;
};

struct layer {
	char* path;
	char* program;
	int program_id;
	struct layer* next;
};

struct cell {
	struct stbrp_rect* rects;
	int all_rects_index;
	int x0, y0;
};

struct cell_table {
	struct cell* cells;
	int n_columns;
	int n_rows;
	int *vertical;
	int *horizontal;
	int n_layers;
	struct layer* layers;
	stbi_uc** layer_bitmaps;
	int width;
	int height;
};

enum table_type {
	TT_NONE = 0,
	TT_GLYPH,
	TT_CELL,
};

struct table {
	int name_sid;
	enum table_type type;
	union {
		struct glyph_table glyph;
		struct cell_table cell;
	};
};


static void fread_line(FILE* f, char* buf, size_t sz)
{
	int n = 0;
	while (!feof(f)) {
		if (fread(buf, 1, 1, f) == 0 || (n+1) >= sz || *buf == '\n') {
			*buf = 0;
			return;
		}
		n++;
		buf++;
	}
	assert(0);
}

static int split2(char* str, char** part2)
{
	while (*str) {
		if (*str == ' ') {
			*str = 0;
			*part2 = str+1;
			return 1;
		}
		str++;
	}
	return 0;
}

static void hex2pixels(char* str, unsigned char* out, size_t sz)
{
	for (;;) {
		char ch = *str;
		if (!ch) break;
		int value = (ch >= '0' && ch <= '9') ? (ch-'0') : (ch >= 'A' && ch <= 'F') ? (10+ch-'A') : -1;
		for (int i = 3; i >= 0; i--) {
			*out++ = (value & (1<<i)) ? 255 : 0;
			sz--;
			if (sz == 0) return;
		}
		str++;
	}
}


static int glyph_codepoint_compar(const void* va, const void* vb)
{
	const struct glyph* a = va;
	const struct glyph* b = vb;
	return a->codepoint - b->codepoint;
}

static void blit(unsigned char* dst, int dst_stride, unsigned char* src, int w, int h, int x0, int y0)
{
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			dst[x + x0 + (y + y0)*dst_stride] = src[x + y*w];
		}
	}
}

static void blit4(unsigned char* dst, int dst_stride, unsigned char* src, int src_width, int w, int h, int dst_x0, int dst_y0, int src_x0, int src_y0)
{
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			dst[x + dst_x0 + (y + dst_y0)*dst_stride] = src[(x + src_x0 + (y + src_y0)*src_width)*4+3];
		}
	}
}


static int endswith(char* str, const char* suffix)
{
	size_t str_len = strlen(str);
	size_t suffix_len = strlen(suffix);
	if (str_len < suffix_len) return 0;
	return strcmp(str + (str_len - suffix_len), suffix) == 0;
}

static char** explode_strings(char* s, int* out_n)
{
	int n = 1;
	char* p = s;

	while (*p) {
		if (*p == ' ') n++;
		p++;
	}

	char** lst = malloc(n * sizeof(char*));
	assert(lst != NULL);

	p = s;
	for (int i = 0; i < n; i++) {
		char* p0 = p;
		while (*p) {
			if (*p == ' ') break;
			p++;
		}
		*p = 0;
		p++;
		lst[i] = strdup(p0); // hohum
		assert(lst[i] != NULL);
	}

	if (out_n != NULL) *out_n = n;

	return lst;
}

static int* explode_integers(char* s, int* out_n)
{
	int n;
	char** ss = explode_strings(s, &n); // hehe
	if (out_n != NULL) *out_n = n;

	int* lst = malloc(n * sizeof(int));
	assert(lst != NULL);

	for (int i = 0; i < n; i++) {
		lst[i] = atoi(ss[i]);
	}

	free(ss);

	return lst;
}

#define MAX_N (2048)

struct table tables[MAX_N];
int n_tables;

struct table* table_alloc(enum table_type tt, int name_sid)
{
	assert(n_tables < MAX_N);
	struct table* t = &tables[n_tables++];
	memset(t, 0, sizeof(*t));
	t->type = tt;
	t->name_sid = name_sid;
	return t;
}

static void table_reset()
{
	n_tables = 0;
}

static int alpha_sum_rect(stbi_uc* bitmap, int stride, int x0, int y0, int w, int h)
{
	int sum = 0;
	for (int y = y0; y < (y0+h); y++) {
		for (int x = x0; x < (x0+w); x++) {
			stbi_uc* pixel = &bitmap[(x + y * stride) << 2];
			sum += pixel[3];
		}
	}
	return sum;
}

struct blk {
	char type[4];
	int cap_log2;
	int data_sz;
	char* data;
	struct blk* next;
};

void blk_append(struct blk* b, unsigned char* data, int sz)
{
	int dst_offset = b->data_sz;
	b->data_sz += sz;
	{ // grow buffer if necessary
		int old_cap_log2 = b->cap_log2;
		while ((1<<b->cap_log2) < b->data_sz) b->cap_log2++;
		if (b->cap_log2 != old_cap_log2) {
			b->data = realloc(b->data, 1<<b->cap_log2);
			assert(b->data != NULL);
		}
	}
	memcpy(&b->data[dst_offset], data, sz);
}

struct blk* blk_new(char* type)
{
	assert(strlen(type) == 4);
	struct blk* b = calloc(1, sizeof(*b));
	assert(b != NULL);
	memcpy(b->type, type, 4);
	return b;
}

void encode_u32(unsigned int v, unsigned char* out)
{
	out[0] = v & 255;
	out[1] = (v>>8) & 255;
	out[2] = (v>>16) & 255;
	out[3] = (v>>24) & 255;
}

void encode_u16(unsigned int v, unsigned char* out)
{
	out[0] = v & 255;
	out[1] = (v>>8) & 255;
}

void blk_finalize(struct blk* b, FILE* f)
{
	fwrite(&b->type, sizeof(b->type), 1, f);

	unsigned char c32[4];
	encode_u32(b->data_sz, c32);
	fwrite(c32, sizeof(c32), 1, f);

	fwrite(b->data, b->data_sz, 1, f);
	free(b->data);
	free(b);
}

void blk_u16(struct blk* b, unsigned int v)
{
	unsigned char c16[2];
	encode_u16(v, c16);
	blk_append(b, c16, sizeof(c16));
}


void blk_u32(struct blk* b, unsigned int v)
{
	unsigned char c32[4];
	encode_u32(v, c32);
	blk_append(b, c32, sizeof(c32));
}

void blk_s32(struct blk* b, int v)
{
	blk_u32(b, (unsigned int)v);
}

void blk_f32(struct blk* b, float v)
{
	union {
		unsigned int u;
		float f;
	} both;
	both.f = v;
	blk_u32(b, both.u);
}


struct strtbl {
	struct blk* strings;
};

void strtbl_init(struct strtbl* t)
{
	t->strings = blk_new("STRI");
}

unsigned int strtbl_addn(struct strtbl* t, char* str, int n)
{
	int cursor = 0;
	while (cursor < t->strings->data_sz) {
		char* s = t->strings->data + cursor;
		size_t sn = strlen(s);
		if (sn == n && memcmp(s, str, sn) == 0) {
			return cursor;
		}
		cursor += sn+1;
	}
	int p = t->strings->data_sz;
	blk_append(t->strings, (unsigned char*)str, n);
	unsigned char nullterm = 0;
	blk_append(t->strings, &nullterm, 1);
	return p;
}

unsigned int strtbl_add(struct strtbl* t, char* str)
{
	return strtbl_addn(t, str, strlen(str));
}


char* strtbl_get(struct strtbl* t, unsigned int id)
{
	assert(id < t->strings->data_sz);
	return t->strings->data + id;
}

void strtbl_finalize(struct strtbl* t, FILE* f)
{
	blk_finalize(t->strings, f);
}

static char* penultimate(char* name)
{
	size_t len = strlen(name);
	char* retval = name;
	for (int o = (len-1); o >= 0; o--) {
		char ch = name[o];
		if (ch == '.') {
			name[o] = 0;
			break;
		}
	}
	len = strlen(name);
	for (int o = (len-1); o >= 0; o--) {
		char ch = name[o];
		if (ch == '.') {
			retval = &name[o+1];
			break;
		}
	}
	return retval;
}

static int atoi8(char* s)
{
	int v = atoi(s);
	assert(v >= 0);
	assert(v < 256);
	return v;
}

enum token_type {
	T_FLOAT = 1,
	T_RGBA,

	T_IDENT, // "foo"
	T_IDENT_CTX_VAR, // "$foo"
	T_IDENT_COLOR_NAME, // "@foo"

	T_PLUS,
	T_MINUS,
	T_MUL,
	T_DIV,
	T_MOD,

	T_EQ,
	T_NEQ,
	T_LT,
	T_LE,
	T_GT,
	T_GE,
	T_AND,
	T_OR,
	T_NOT,

	T_LPAREN,
	T_RPAREN,

	T_COMMA,

	T_WHITESPACE,

	T_EOF
};

struct token {
	enum token_type type;
	int pos, len;
};

struct lexer;

// (returns lexer_state_fn* but I can't do recursive typedefs :-/ )
typedef void* (*lexer_state_fn)(struct lexer*);

struct lexer {
	char* src;
	int src_len, pos, start;
	struct token token, previous_token;
	int has_token;
	lexer_state_fn state_fn;
};

static void* lex_main(struct lexer* l);

static void lexer_init(struct lexer* l, char* src)
{
	memset(l, 0, sizeof(*l));
	l->src = src;
	l->src_len = strlen(src);
	l->state_fn = lex_main;
}


static int lexer_ch(struct lexer* l)
{
	if (l->pos >= l->src_len) {
		l->pos++;
		return -1;
	}
	int ch = l->src[l->pos];
	l->pos++;
	return ch;
}

static void lexer_backup(struct lexer* l)
{
	l->pos--;
}

static int lexer_accept_char(struct lexer* l, char valid)
{
	int ch = lexer_ch(l);
	if (ch == valid) return 1;
	lexer_backup(l);
	return 0;
}

static int char_in(char ch, const char* valid)
{
	size_t valid_length = strlen(valid);
	for (int i = 0; i < valid_length; i++) if (valid[i] == ch) return 1;
	return 0;
}

static int lexer_accept(struct lexer* l, const char* valid)
{
	int ch = lexer_ch(l);
	if (char_in(ch, valid)) return 1;
	lexer_backup(l);
	return 0;
}

static int lexer_accept_fn(struct lexer* l, int(*fn)(int))
{
	int ch = lexer_ch(l);
	if (fn(ch)) return 1;
	lexer_backup(l);
	return 0;
}


static int lexer_accept_run(struct lexer* l, const char* valid)
{
	int i = 0;
	while (lexer_accept(l, valid)) i++;
	return i;
}

static void lexer_accept_run_fn(struct lexer* l, int(*fn)(int))
{
	while (lexer_accept_fn(l, fn)) {}
}

static void lexer_eat(struct lexer* l)
{
	l->start = l->pos;
}

static void lexer_emit(struct lexer* l, enum token_type type)
{
	l->token.type = type;
	l->token.pos = l->start;
	l->token.len = l->pos - l->start;

	l->has_token = 1;

	lexer_eat(l);
}

static void* lex_eof(struct lexer* l) {
	lexer_eat(l);
	lexer_emit(l, T_EOF);
	return lex_eof;
}

static void* lex_float(struct lexer* l)
{
	lexer_accept_char(l, '-');
	const char* digits = "0123456789";
	lexer_accept_run(l, digits);
	if (lexer_accept_char(l, '.')) {
		lexer_accept_run(l, digits);
	}
	if (lexer_accept(l, "eE")) {
		lexer_accept(l, "+-");
		lexer_accept_run(l, digits);
	}
	lexer_emit(l, T_FLOAT);
	return lex_main;
}

static void* lex_rgba(struct lexer* l)
{
	if (lexer_accept_run(l, "0123456789ABCDEFabcdef") == 8) {
		lexer_emit(l, T_RGBA);
		return lex_main;
	} else {
		return NULL;
	}
}

static int is_alpha(int ch)
{
	return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || ch == '_';
}

static int is_num(int ch)
{
	return ch >= '0' && ch <= '9';
}

static int is_identifier(int ch)
{
	return is_alpha(ch) || is_num(ch) || ch == '.';
}

static void* lex_ident_ctx_var(struct lexer* l)
{
	lexer_accept_run_fn(l, is_identifier);
	lexer_emit(l, T_IDENT_CTX_VAR);
	return lex_main;
}

static void* lex_ident_color_name(struct lexer* l)
{
	lexer_accept_run_fn(l, is_identifier);
	lexer_emit(l, T_IDENT_COLOR_NAME);
	return lex_main;
}

static void* lex_ident(struct lexer* l)
{
	lexer_accept_run_fn(l, is_identifier);
	lexer_emit(l, T_IDENT);
	return lex_main;
}

static void* lex_main(struct lexer* l)
{
	if (lexer_ch(l) == -1) return lex_eof;
	lexer_backup(l);

	const char* ws = " \t";
	if (lexer_accept_run(l, ws)) {
		lexer_emit(l, T_WHITESPACE);
		return lex_main;
	}

	// multi-character ops
	{
		struct {
			const char* str;
			enum token_type tt;
		} pnct[] = {
			{"==", T_EQ},
			{"!=", T_NEQ},
			{">=", T_GE},
			{"<=", T_LE},
			{"&&", T_AND},
			{"||", T_OR},
			{NULL}
		};
		for (int i = 0; pnct[i].str; i++) {
			const char* str = pnct[i].str;
			enum token_type tt = pnct[i].tt;
			size_t len = strlen(str);
			int n;
			for (n = 0; n < len; n++) {
				if (!lexer_accept_char(l, str[n])) {
					break;
				}
			}
			if (n == len) {
				lexer_emit(l, tt);
				return lex_main;
			}
			for (int j = 0; j < n; j++) lexer_backup(l);
		}
	}

	// single-character ops
	{
		struct {
			char ch;
			enum token_type tt;
		} pnct[] = {
			{'+', T_PLUS},
			{'-', T_MINUS},
			{'*', T_MUL},
			{'/', T_DIV},
			{'%', T_MOD},
			{'(', T_LPAREN},
			{')', T_RPAREN},
			{'!', T_NOT},
			{',', T_COMMA},
			{0}
		};
		for (int i = 0; pnct[i].ch; i++) {
			char ch = pnct[i].ch;
			enum token_type tt = pnct[i].tt;
			if (lexer_accept_char(l, ch)) {
				lexer_emit(l, tt);
				return lex_main;
			}
		}
	}

	// literals
	if (lexer_accept(l, ".-0123456789")) {
		lexer_backup(l);
		return lex_float;
	}
	if (lexer_accept_char(l, '#')) {
		return lex_rgba;
	}

	// identifiers
	if (lexer_accept_char(l, '$')) {
		return lex_ident_ctx_var;
	}
	if (lexer_accept_char(l, '@')) {
		return lex_ident_color_name;
	}
	if (lexer_accept_fn(l, is_identifier)) {
		lexer_backup(l);
		return lex_ident;
	}

	return NULL;
}

static struct token lexer_next(struct lexer* l)
{
	for (;;) {
		assert(l->state_fn != NULL);
		l->state_fn = l->state_fn(l);
		if (l->state_fn == NULL) {
			assert(!"lexer error"); // TODO show error
		}
		if (l->has_token) {
			if (l->token.type == T_WHITESPACE) continue;
			l->has_token = 0;
			l->previous_token = l->token;
			return l->token;
		}
	}
}

#define SEXPR_TYPE_ATOM (0)
#define SEXPR_TYPE_LIST (1)

struct sexpr {
	int type;
	union {
		struct token atom;
		struct sexpr* list;
	};
	struct sexpr* next;
};


static inline int sexpr_is_atom(struct sexpr* e)
{
	return e->type == SEXPR_TYPE_ATOM;
}

static inline int sexpr_is_list(struct sexpr* e)
{
	return e->type == SEXPR_TYPE_LIST;
}

static struct sexpr* _sexpr_new()
{
	struct sexpr* e = calloc(1, sizeof(*e));
	assert(e != NULL);
	return e;
}

static inline struct sexpr* sexpr_index(struct sexpr* e, int index)
{
	if (e == NULL || e->list == NULL) return NULL;
	struct sexpr* o = e->list;
	for (int i = 0; i < index; i++) {
		o = o->next;
		if (o == NULL) break;
	}
	return o;
}

static struct sexpr* sexpr_new_atom(struct token atom)
{
	struct sexpr* e = _sexpr_new();
	e->type = SEXPR_TYPE_ATOM;
	e->atom = atom;
	return e;
}

static struct sexpr* sexpr_new_list(struct sexpr* head, ...)
{
	struct sexpr* e = _sexpr_new();
	e->type = SEXPR_TYPE_LIST;
	e->list = head;

	if (head != NULL) {
		struct sexpr* cur = head;
		va_list args;
		va_start(args, head);
		for (;;) {
			struct sexpr* arg = va_arg(args, struct sexpr*);
			if (arg == NULL) break;
			cur->next = arg;
			cur = cur->next;
		}
		va_end(args);
	}

	return e;
}

static struct sexpr** sexpr_get_append_cursor(struct sexpr* e)
{
	assert(sexpr_is_list(e));
	struct sexpr** cursor = &e->list;
	while (*cursor != NULL) cursor = &((*cursor)->next);
	return cursor;
}

static void sexpr_append(struct sexpr*** append_cursor, struct sexpr* e)
{
	assert(e->next == NULL);
	assert(**append_cursor == NULL);
	**append_cursor = e;
	*append_cursor = &e->next;
}


struct parser {
	struct lexer lexer;
	struct token current_token, next_token, stashed_token;
	int can_rewind, has_stashed_token;
};

static inline struct token parser_next_token(struct parser* p)
{
	p->current_token = p->next_token;
	if (p->has_stashed_token) {
		p->next_token = p->stashed_token;
		p->has_stashed_token = 0;
	} else {
		p->next_token = lexer_next(&p->lexer);
	}
	p->can_rewind = 1;
	return p->current_token;
}

static void parser_init(struct parser* p, char* src)
{
	memset(p, 0, sizeof(*p));
	lexer_init(&p->lexer, src);
	parser_next_token(p);
}

static inline void parser_rewind(struct parser* p)
{
	assert(p->can_rewind);
	p->stashed_token = p->next_token;
	p->next_token = p->current_token;
	p->can_rewind = 0;
	p->has_stashed_token = 1;
	memset(&p->current_token, 0, sizeof(p->current_token));
}

static inline int parser_accept_and_get(struct parser* p, enum token_type tt, struct token* tp)
{
	struct token t = parser_next_token(p);
	if (t.type != tt) {
		parser_rewind(p);
		return 0;
	} else {
		if (tp != NULL) *tp = t;
		return 1;
	}
}

static inline int parser_accept(struct parser* p, enum token_type tt)
{
	return parser_accept_and_get(p, tt, NULL);
}

static inline int parser_expect(struct parser* p, enum token_type tt)
{
	if (!parser_accept(p, tt)) {
		fprintf(stderr, "expected token %d\n", tt);
		abort();
	}
	return 1;
}


static inline int prefix_bp(enum token_type tt)
{
	switch (tt) {
		case T_LPAREN:
			return 1;
		case T_NOT:
			return 100; // XXX?
		case T_PLUS:
		case T_MINUS:
			return 100;
		default:
			return -1;
	}
}

static inline int infix_bp(enum token_type tt)
{
	switch (tt) {
		// expression terminators
		case T_EOF:
		case T_RPAREN:
		case T_COMMA:
			return 0;
		case T_PLUS:
		case T_MINUS:
			return 40;
		case T_MUL:
		case T_DIV:
		case T_MOD:
			return 50;
		case T_OR:
		case T_EQ:
		case T_NEQ:
		case T_GT:
		case T_GE:
		case T_LT:
		case T_LE:
			return 70; // XXX?
		case T_LPAREN:
			return 100;
		default:
			return -1;
	}
}

static inline int is_unary_op(enum token_type tt) {
	switch (tt) {
		case T_PLUS:
		case T_MINUS:
			return 1;
		default:
			return 0;
	}
}

static inline int is_binary_op(enum token_type tt) {
	switch (tt) {
		case T_PLUS:
		case T_MINUS:
		case T_MUL:
		case T_DIV:
		case T_MOD:
		case T_AND:
		case T_OR:
		case T_EQ:
		case T_NEQ:
		case T_GT:
		case T_GE:
		case T_LT:
		case T_LE:
			return 1;
		default:
			return 0;
	}
}

static inline int is_binary_op_right_associcative(enum token_type tt)
{
	return 0;
}

static struct sexpr* parse_expr_rec(struct parser* p, int rbp, int depth)
{
	if (depth >= 1024) {
		assert(!"maximum expression recursion depth exceeded");
	}

	struct token t = parser_next_token(p);
	enum token_type tt = t.type;

	// null denotation
	struct sexpr* left = NULL;
	{
		int bp;
		if (tt == T_FLOAT || tt == T_RGBA || tt == T_IDENT || tt == T_IDENT_CTX_VAR || tt == T_IDENT_COLOR_NAME) {
			left = sexpr_new_atom(t);
		} else if ((bp = prefix_bp(tt)) != -1) {
			struct sexpr* operand = parse_expr_rec(p, bp, depth+1);
			if (operand == NULL) return NULL;
			if (is_unary_op(tt)) {
				left = sexpr_new_list(sexpr_new_atom(t), operand, NULL);
			} else if (tt == T_LPAREN) {
				left = operand;
				if (!parser_expect(p, T_RPAREN)) return NULL;
			} else {
				fprintf(stderr, "unexpected token (1) %d\n", tt);
				abort();
			}
		} else {
			fprintf(stderr, "unexpected token (2) %d\n", tt);
			abort();
		}
	}

	for (;;) {
		int lbp = infix_bp(p->next_token.type);
		if (lbp == -1) {
			fprintf(stderr, "unexpected token (3) %d\n", tt);
			abort();
		}
		if (rbp >= lbp) break;

		// left denotation
		t = parser_next_token(p);
		tt = t.type;
		if (is_binary_op(tt)) { // parse binary op
			int bp = lbp;
			if (is_binary_op_right_associcative(tt)) bp--;
			struct sexpr* right = parse_expr_rec(p, bp, depth+1);
			if (right == NULL) return NULL;
			left = sexpr_new_list(sexpr_new_atom(t), left, right, NULL);
		} else if (tt == T_LPAREN) { // parse call
			left = sexpr_new_list(left, NULL);
			struct sexpr** cursor = sexpr_get_append_cursor(left);
			int more = !parser_accept(p, T_RPAREN);
			while (more) {
				struct sexpr* a = parse_expr_rec(p, 0, depth+1);
				if (a == NULL) return NULL;
				sexpr_append(&cursor, a);

				if (p->next_token.type != T_RPAREN && p->next_token.type != T_COMMA) {
					fprintf(stderr, "unexpected token (4) %d\n", tt);
					abort();
				}
				more = p->next_token.type == T_COMMA;
				parser_next_token(p);
			}
		} else {
			fprintf(stderr, "unexpected token (5) %d\n", tt);
			abort();
		}
	}

	return left;
}

static struct sexpr* parse_expr(struct parser* p)
{
	return parse_expr_rec(p, 0, 0);
}

struct prgs {
	int n_programs;
	struct strtbl* strtbl;
	/*
	struct blk* sexprs;
	struct blk* constants;
	struct blk* ctx_keys;
	struct blk* color_keys;
	struct blk* program_keys;
	struct blk* program_offsets;
	struct blk* code;
	*/


	struct dya sexprs_dya;
	struct sexpr** sexprs;

	struct dya constants_dya;
	float* constants;

	struct dya ctx_keys_dya;
	unsigned int* ctx_keys;

	struct dya color_keys_dya;
	unsigned int* color_keys;

	struct dya program_keys_dya;
	unsigned int* program_keys;

	struct dya program_offsets_dya;
	unsigned int* program_offsets;

	struct dya code_dya;
	unsigned short* code;

};

static struct prgs* prgs_new(struct strtbl* strtbl)
{
	struct prgs* prgs = calloc(1, sizeof(*prgs));
	assert(prgs != NULL);
	prgs->strtbl = strtbl;

	dya_init(&prgs->sexprs_dya, (void**)&prgs->sexprs, sizeof(*prgs->sexprs), 0);
	dya_init(&prgs->constants_dya, (void**)&prgs->constants, sizeof(*prgs->constants), 0);
	dya_init(&prgs->ctx_keys_dya, (void**)&prgs->ctx_keys, sizeof(*prgs->ctx_keys), 0);
	dya_init(&prgs->color_keys_dya, (void**)&prgs->color_keys, sizeof(*prgs->color_keys), 0);
	dya_init(&prgs->program_keys_dya, (void**)&prgs->program_keys, sizeof(*prgs->program_keys), 0);
	dya_init(&prgs->program_offsets_dya, (void**)&prgs->program_offsets, sizeof(*prgs->program_offsets), 0);
	dya_init(&prgs->code_dya, (void**)&prgs->code, sizeof(*prgs->code), 0);

	return prgs;
}

static double nobs_atoi(char* str, int n)
{
	if (n <= 0) return 0.0;
	double val = 0.0;
	double mult = 1.0;
	for (int i = (n-1); i >= 0; i--) {
		val += (str[i]-'0') * mult;
		mult *= 10.0;
	}
	return val;
}

static float nobs_atof(char* str, int n)
{
	assert(n > 0);

	double sgn = 1.0;

	char* p = str;

	if (p[0] == '-') {
		sgn = -1.0;
		p++; n--;
		assert(n > 0);
	}

	char* p00 = p;
	while (n > 0 && *p != '.' && *p != 'e' && *p != 'E') { p++; n--; }
	char* p01 = p;

	double prepoint = nobs_atoi(p00, p01-p00);
	double exponent = 0.0;
	double exponent_sgn = 1.0;
	double postpoint = 0.0;

	for (;;) {
		if (n <= 0) {
			break;
		} else if (*p == '.') {
			p++; n--;
			double mult = 0.1;
			while (n > 0 && *p != 'e' && *p != 'E') {
				postpoint += (*p-'0') * mult;
				mult *= 0.1;
				p++; n--;
			}
		} else if (*p == 'e' || *p == 'E') {
			p++; n--;
			assert(n > 0);
			if (*p == '-') {
				exponent_sgn = -1.0;
				p++; n--;
				assert(n > 0);
			}
			char* p20 = p;
			while (n > 0) { p++; n--; }
			char* p21 = p;
			exponent = nobs_atoi(p20, p21-p20);
		} else {
			assert(!"what?");
		}
	}

	double val = (sgn*prepoint + postpoint) * pow(10.0, exponent_sgn*exponent);

	return val;
}

static unsigned int prgs_add_float(struct prgs* prgs, float v)
{
	unsigned int n = prgs->constants_dya.n;
	for (unsigned int i = 0; i < n; i++) {
		if (v == prgs->constants[i]) return i;
	}
	dya_append(&prgs->constants_dya, (void**)&prgs->constants, &v);
	return n;
}

static unsigned int prgs_add_float4(struct prgs* prgs, float* v4)
{
	unsigned int n = prgs->constants_dya.n;
	for (unsigned int i = 0; i < n; i++) {
		for (int j = 0; j < 4; j++) {
			int k = i+j;
			if (k >= n) break;
			float ex = prgs->constants[i+j];
			if (v4[j] != ex) break;
			if (j == 3) return i;
		}
	}
	for (int i = 0; i < 4; i++) dya_append(&prgs->constants_dya, (void**)&prgs->constants, &v4[i]);
	return n;
}

static int hexdigit(char h)
{
	if (h >= '0' && h <= '9') return h-'0';
	if (h >= 'a' && h <= 'f') return 10+(h-'a');
	if (h >= 'A' && h <= 'F') return 10+(h-'A');
	assert(!"invalid hex digit");
}

static int prgs_emit(struct prgs* prgs, char* src, struct sexpr* e)
{
	int top = prgs->code_dya.n;
	if (e->type == SEXPR_TYPE_ATOM) {
		enum token_type tt = e->atom.type;
		char* base = src + e->atom.pos;
		if (tt == T_FLOAT) {
			float v = nobs_atof(base, e->atom.len);
			unsigned int const_id = prgs_add_float(prgs, v);
			assert(const_id < (1<<14));
			unsigned short op = 1 | (const_id << 2);
			dya_append(&prgs->code_dya, (void**)&prgs->code, &op);
		} else if (tt == T_RGBA) {
			assert(e->atom.len == 8);
			float rgba[4];
			for (int i = 0; i < 4; i++) {
				int b = (hexdigit(base[i*2])<<4) + hexdigit(base[i*2+1]);
				rgba[i] = (float)b / (float)0xff;
			}
			unsigned int const_id = prgs_add_float4(prgs, rgba);
			assert(const_id <= ((1<<14)-4));
			unsigned short op = 3 | (const_id << 2);
			dya_append(&prgs->code_dya, (void**)&prgs->code, &op);
		} else if (tt == T_IDENT_CTX_VAR || tt == T_IDENT_COLOR_NAME) {
			int str_id = strtbl_addn(prgs->strtbl, base+1, e->atom.len-1);

			struct dya* idtbl_dya = tt == T_IDENT_CTX_VAR ? &prgs->ctx_keys_dya : tt == T_IDENT_COLOR_NAME ? &prgs->color_keys_dya : NULL;
			assert(idtbl_dya != NULL);
			unsigned int** idtbl = tt == T_IDENT_CTX_VAR ? &prgs->ctx_keys : tt == T_IDENT_COLOR_NAME ? &prgs->color_keys : NULL;
			assert(idtbl != NULL);

			int n = idtbl_dya->n;
			int idid = -1;
			for (int i = 0; i < n; i++) {
				int str_id = (*idtbl)[i];
				char* key = strtbl_get(prgs->strtbl, str_id);
				size_t keyn = strlen(key);
				if (keyn != (e->atom.len-1)) continue;
				if (memcmp(base+1, key, keyn) != 0) continue;
				idid = i;
				break;
			}
			if (idid == -1) {
				idid = n;
				dya_append(idtbl_dya, (void**)idtbl, &str_id);
			}
			assert(idid < (1<<13));
			int magic = tt == T_IDENT_CTX_VAR ? 2 : tt == T_IDENT_COLOR_NAME ? 6 : -1;
			assert(magic >= 0);
			unsigned short op = magic | (idid << 3);
			dya_append(&prgs->code_dya, (void**)&prgs->code, &op);
		} else if (tt == T_IDENT) {
			int found = 0;
			for (int i = 0; i < prgs->n_programs; i++) {
				int str_id = prgs->program_keys[i];
				char* c = strtbl_get(prgs->strtbl, str_id);
				size_t cn = strlen(c);
				if (cn != e->atom.len) continue;
				if (memcmp(base, c, cn) != 0) continue;
				found = 1;
				prgs_emit(prgs, src, prgs->sexprs[i]);
			}
			if (!found) {
				fprintf(stderr, "T_IDENT not found: ");
				fwrite(base, e->atom.len, 1, stderr);
				fprintf(stderr, "\n");
				abort();
			}
		} else {
			assert(!"what token type?");
		}
	} else if (e->type == SEXPR_TYPE_LIST) {
		struct sexpr* s = e->list;
		assert(s != NULL);
		assert(s->type == SEXPR_TYPE_ATOM);
		enum token_type tt = s->atom.type;
		enum atlsvm_fcode fc = ATLSVM__ZERO;
		switch (tt) {
		case T_IDENT:
			break;
		case T_PLUS:  fc = ATLSVM_ADD; break;
		case T_MINUS: fc = ATLSVM_SUB; break; // XXX might be ATLSVM_NEG?
		case T_MUL:   fc = ATLSVM_MUL; break;
		case T_DIV:   fc = ATLSVM_DIV; break;
		case T_MOD:   fc = ATLSVM_MOD; break;
		case T_EQ:    fc = ATLSVM_EQ;  break;
		case T_NEQ:   fc = ATLSVM_NEQ; break;
		case T_LT:    fc = ATLSVM_LT;  break;
		case T_LE:    fc = ATLSVM_LE;  break;
		case T_GT:    fc = ATLSVM_GT;  break;
		case T_GE:    fc = ATLSVM_GE;  break;
		case T_AND:   fc = ATLSVM_AND; break;
		case T_OR:    fc = ATLSVM_OR;  break;
		case T_NOT:   fc = ATLSVM_NOT; break;

		default:
			assert(!"unexpected atom token type");
		}

		int is_select = 0;
		if (fc == ATLSVM__ZERO) {
			assert(tt == T_IDENT);
			#define MATCH(str) (strlen(str) == s->atom.len && memcmp(str, src+s->atom.pos, s->atom.len) == 0)
			if (MATCH("select")) {
				is_select = 1;
			}
			else if (MATCH("sin"))   { fc = ATLSVM_SIN; }
			else if (MATCH("cos"))   { fc = ATLSVM_COS; }
			else if (MATCH("atan"))  { fc = ATLSVM_ATAN; }
			else if (MATCH("sqrt"))  { fc = ATLSVM_SQRT; }
			else if (MATCH("exp"))   { fc = ATLSVM_EXP; }
			else if (MATCH("pow"))   { fc = ATLSVM_POW; }
			else if (MATCH("floor")) { fc = ATLSVM_FLOOR; }
			else if (MATCH("ceil"))  { fc = ATLSVM_CEIL; }
			else if (MATCH("round")) { fc = ATLSVM_ROUND; }
			else if (MATCH("lerp"))  { fc = ATLSVM_LERP; }
			else {
				fprintf(stderr, "unhandled function: '");
				fwrite(src + s->atom.pos, s->atom.len, 1, stderr);
				fprintf(stderr, "'\n");
				abort();
			}
			#undef MATCH
		}

		if (is_select) {
			struct sexpr* condition = s->next;
			assert(condition != NULL);

			struct sexpr* args = condition->next;
			int n_args = 0;
			for (struct sexpr* i = args; i != NULL; i = i->next) {
				n_args++;
			}
			assert(n_args < (1<<10));

			unsigned short op = 4 | (n_args << 6);
			dya_append(&prgs->code_dya, (void**)&prgs->code, &op);

			int offset_tbl_begin = prgs->code_dya.n;
			for (int i = 0; i <= n_args; i++) {
				unsigned short offset_tbl_placeholder = 0xCAFE;
				dya_append(&prgs->code_dya, (void**)&prgs->code, &offset_tbl_placeholder);
			}

			// emit condition code
			int code_offset = prgs->code_dya.n;
			prgs_emit(prgs, src, condition);

			struct sexpr* o = args;
			for (int i = 0; i <= n_args; i++) {
				prgs->code[offset_tbl_begin + i] = prgs->code_dya.n - code_offset;
				if (i < n_args) {
					prgs_emit(prgs, src, o);
					o = o->next;
				}
			}
			assert(o == NULL);
		} else {
			assert(fc != ATLSVM__ZERO);
			int n_operands_expected = 0;
			char* opname = "?!";
			switch (fc) {
			#define ATLSVM_DEF(t,n,fn) case t: n_operands_expected = n; opname = #t; break;
			ATLSVM_FCODE_LST
			#undef ATLSVM_DEF
			default:
				assert(!"unexpected fc value");
			}

			int n_operands_actual = 0;
			for (struct sexpr* o = s->next; o != NULL; o = o->next) n_operands_actual++;

			if (n_operands_expected != n_operands_actual) {
				fprintf(stderr, "number of %s operands expected to be %d, but was %d\n", opname, n_operands_expected, n_operands_actual);
				abort();
			}

			unsigned short op = fc << 3;
			dya_append(&prgs->code_dya, (void**)&prgs->code, &op);

			struct sexpr* o = s->next;
			for (int i = 0; i < n_operands_actual; i++) {
				assert(o != NULL);
				prgs_emit(prgs, src, o);
				o = o->next;
			}
			assert(o == NULL);
		}
	} else {
		assert(!"what sexpr type?");
	}
	return top;
}

static void wrstr(char** d, const char* s)
{
	strcpy(*d, s);
	*d += strlen(s);
}

static void sexpr_str_rec(char* src, struct sexpr* e, char** sp)
{
	if (e == NULL) {
		wrstr(sp, "NULL");
	} else if (sexpr_is_atom(e)) {
		if (e->atom.len > 0) {
			memcpy(*sp, src + e->atom.pos, e->atom.len);
			*sp += e->atom.len;
		} else {
			assert(!"invalid atom");
		}
	} else if (sexpr_is_list(e)) {
		wrstr(sp, "(");
		int first = 1;
		for (struct sexpr* i = e->list; i; i = i->next) {
			if (!first) wrstr(sp, " ");
			sexpr_str_rec(src, i, sp);
			first = 0;
		}
		wrstr(sp, ")");
	} else {
		assert(!"not atom or list?!");
	}
}

char tmpstr[65536];
static char* sexpr_str(char* src, struct sexpr* e)
{
	char* s = tmpstr;
	char* cpy = s;
	sexpr_str_rec(src, e, &cpy);
	*cpy = 0;
	return s;
}

static unsigned int prgs_compile(struct prgs* prgs, char* key, char* src)
{
	struct parser parser;
	parser_init(&parser, src);
	struct sexpr* se = parse_expr(&parser);
	assert(se != NULL);

	int c0 = prgs->code_dya.n;

	dya_append(&prgs->sexprs_dya, (void**)&prgs->sexprs, &se);
	unsigned int program_key = strtbl_add(prgs->strtbl, key);
	dya_append(&prgs->program_keys_dya, (void**)&prgs->program_keys, &program_key);
	unsigned int offset = prgs->code_dya.n;
	dya_append(&prgs->program_offsets_dya, (void**)&prgs->program_offsets, &offset);

	// TODO sexpr transforms? e.g. if(c,t,f) => select(c,f,t)
	prgs_emit(prgs, src, se);

	prgs->n_programs++;

	int nops = prgs->code_dya.n - c0;

	printf("  C  %s: '%s' => %s (%d ops)\n", key, src, sexpr_str(src, se), nops);
	for (int i = 0; i < nops; i++) {
		printf("       %04x\n", prgs->code[c0 + i]);
	}
	return offset;
}

struct color {
	char* name;
	unsigned char rgba[4];
};

struct cols {
	char* name;
	struct dya colors_dya;
	struct color* colors;
};

static struct cols* cols_new(char* name)
{
	struct cols* cols = calloc(1, sizeof(*cols));
	assert(cols != NULL);
	cols->name = strdup(name);
	dya_init(&cols->colors_dya, (void**)&cols->colors, sizeof(*cols->colors), 0);
	return cols;
}

static void cols_add(struct cols* cols, char* name, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
{
	struct color c;
	c.name = strdup(name);
	c.rgba[0] = r;
	c.rgba[1] = g;
	c.rgba[2] = b;
	c.rgba[3] = a;
	dya_append(&cols->colors_dya, (void**)&cols->colors, &c);
}

static int try_sz(int sz, char* outfile, int n, char** filenames)
{
	assert(n < MAX_N);

	table_reset();

	int atlas_width = sz;
	int atlas_height = sz;

	printf("trying %d×%d...\n", atlas_width, atlas_height);

	char line[4096];

	int n_total_rects = 0;

	struct strtbl strtbl;
	strtbl_init(&strtbl);

	struct prgs* prgs = prgs_new(&strtbl);
	struct dya cols_array_dya;
	struct cols** cols_array;
	dya_init(&cols_array_dya, (void**)&cols_array, sizeof(*cols_array), 0);

	for (int i = 0; i < n; i++) {
		char* filename = filenames[i];

		char* name = strdup(filename);
		assert(name != NULL);

		{
			int get_name_from_filename = 1;
			size_t len = strlen(name);
			for (int o = 0; o < len; o++) {
				char ch = name[o];
				if (ch == ':') {
					name[o] = 0;
					filename = &filename[o+1];
					get_name_from_filename = 0;
					break;
				}
			}

			if (get_name_from_filename) {
				name = penultimate(name);
			}
		}

		if (endswith(filename, ".tbl")) {
			struct table* tbl = table_alloc(TT_CELL, strtbl_add(&strtbl, name));
			struct cell_table* clt = &tbl->cell;
			struct layer** layer_cursor = &clt->layers;
			FILE* f = fopen(filename, "r");
			for (;;) {
				fread_line(f, line, sizeof(line));
				if (strcmp(line, "END") == 0) break;

				char* tail;
				split2(line, &tail);

				if (strcmp(line, "LAYER") == 0) {
					char* program;
					split2(tail, &program);
					struct layer* layer = calloc(1, sizeof(*layer));
					layer->path = strdup(tail);
					layer->program = strdup(program);
					layer->program_id = prgs_compile(prgs, "", program);
					*layer_cursor = layer;
					layer_cursor = &layer->next;
					clt->n_layers++;
				} else if (strcmp(line, "VERTICAL") == 0) {
					assert(clt->vertical == NULL);
					clt->vertical = explode_integers(tail, &clt->n_columns);
				} else if (strcmp(line, "HORIZONTAL") == 0) {
					assert(clt->horizontal == NULL);
					clt->horizontal = explode_integers(tail, &clt->n_rows);
				} else {
					continue;
				}
			}
			fclose(f);
			assert(clt->n_layers > 0);

			clt->layer_bitmaps = calloc(clt->n_layers, sizeof(*clt->layer_bitmaps));
			assert(clt->layer_bitmaps != NULL);

			struct layer* layer = clt->layers;
			for (int j = 0; j < clt->n_layers; j++) {
				assert(layer != NULL);
				int comp = 4;
				clt->layer_bitmaps[j] = stbi_load(layer->path, &clt->width, &clt->height, &comp, comp);
				if (clt->layer_bitmaps[j] == NULL) {
					fprintf(stderr, "stbi_load failed for %s\n", layer->path);
					exit(EXIT_FAILURE);
				}
				assert("image is RGBA" && comp == 4);
				layer = layer->next;
			}

			int n_cells = clt->n_columns * clt->n_rows;
			clt->cells = calloc(n_cells, sizeof(*clt->cells));
			assert(clt->cells != NULL);

			int j = 0;
			int y0 = 0;
			for (int row = 0; row < clt->n_rows; row++) {
				int x0 = 0;
				for (int column = 0; column < clt->n_columns; column++) {
					struct cell* c = &clt->cells[j++];
					c->rects = calloc(clt->n_layers, sizeof(*c->rects));
					assert(c->rects != NULL);
					c->x0 = x0;
					c->y0 = y0;
					int w = clt->vertical[column];
					int h = clt->horizontal[row];
					for (int k = 0; k < clt->n_layers; k++) {
						if (alpha_sum_rect(clt->layer_bitmaps[k], clt->width, x0, y0, w, h) == 0) {
							c->rects[k].w = 0;
							c->rects[k].h = 0;
						} else {
							c->rects[k].w = w;
							c->rects[k].h = h;
						}
					}
					x0 += clt->vertical[column];
				}
				y0 += clt->horizontal[row];
			}

			n_total_rects += n_cells * clt->n_layers;

		} else if (endswith(filename, ".rects")) {
			struct table* tbl = table_alloc(TT_GLYPH, strtbl_add(&strtbl, name));
			struct glyph_table* glt = &tbl->glyph;

			glt->is_dummy = 1; // no bitmaps associated

			FILE* f = fopen(filename, "r");
			assert(f);

			glt->n_glyphs = 0;

			for (;;) {
				fread_line(f, line, sizeof(line));
				if (strcmp(line, "END") == 0) break;

				char* arg0;
				split2(line, &arg0);

				if (strcmp(line, "RECT") == 0) {
					glt->n_glyphs++;
				} else {
					continue;
				}
			}

			rewind(f);

			n_total_rects += glt->n_glyphs;
			glt->glyphs = calloc(glt->n_glyphs, sizeof(*glt->glyphs));

			int i = 0;
			for (;;) {
				fread_line(f, line, sizeof(line));
				if (strcmp(line, "END") == 0) break;

				char* arg0;
				split2(line, &arg0);

				if (strcmp(line, "RECT") == 0) {
					char* ws;
					char* hs;
					split2(arg0, &ws);
					split2(ws, &hs);

					int w = atoi(ws);
					int h = atoi(hs);
					int codepoint = atoi(arg0);

					assert(w > 0);
					assert(h > 0);
					assert(codepoint > 0);

					struct glyph* gly = &glt->glyphs[i++];
					gly->rect.w = w;
					gly->rect.h = h;
					gly->codepoint = codepoint;
				} else {
					continue;
				}
			}
		} else if (endswith(filename, ".bdf")) {
			struct table* tbl = table_alloc(TT_GLYPH, strtbl_add(&strtbl, name));
			struct glyph_table* glt = &tbl->glyph;
			glt->filename = filename;

			FILE* f = fopen(filename, "r");
			assert(f);

			{
				fread_line(f, line, sizeof(line));

				char* arg;
				split2(line, &arg);

				assert(strcmp(line, "STARTFONT") == 0);
				assert(strcmp(arg, "2.1") <= 0);
			}

			for (;;) {
				fread_line(f, line, sizeof(line));
				char* arg;
				split2(line, &arg);

				if (strcmp(line, "FONTBOUNDINGBOX") == 0) {
					char* arg2;
					char* arg3;
					char* arg4;
					split2(arg, &arg2);
					split2(arg2, &arg3);
					split2(arg3, &arg4);
					glt->height = atoi(arg2);
					glt->baseline = glt->height + atoi(arg4);
					break;
				}
			}

			glt->n_glyphs = 0;

			for (;;) {
				fread_line(f, line, sizeof(line));
				char* arg;
				split2(line, &arg);

				if (strcmp(line, "CHARS") == 0) {
					glt->n_glyphs = atoi(arg);
					break;
				}
			}

			assert(glt->n_glyphs);
			n_total_rects += glt->n_glyphs;

			glt->glyphs = calloc(glt->n_glyphs, sizeof(*glt->glyphs));

			for (int j = 0; j < glt->n_glyphs; j++) {
				struct glyph* glyph = &glt->glyphs[j];

				for (;;) {
					fread_line(f, line, sizeof(line));
					char* arg;
					split2(line, &arg);

					if (strcmp(line, "BITMAP") == 0) {
						glyph->bitmap_offset = ftell(f);
						break;
					} else if (strcmp(line, "ENCODING") == 0) {
						glyph->codepoint = atoi(arg);
					} else if (strcmp(line, "BBX") == 0) {
						char* arg2;
						char* arg3;
						char* arg4;
						split2(arg, &arg2);
						split2(arg2, &arg3);
						split2(arg3, &arg4);
						glyph->rect.w = atoi(arg) + 1;
						glyph->rect.h = atoi(arg2) + 1;
						glyph->xoff = atoi(arg3);
						glyph->yoff = -glyph->rect.h - atoi(arg4);
					}
				}
			}

			fclose(f);
		} else if (endswith(filename, ".progs")) {
			FILE* f = fopen(filename, "r");
			assert(f);
			while (!feof(f)) {
				fread_line(f, line, sizeof(line));
				char* code;
				if (!split2(line, &code)) continue;
				prgs_compile(prgs, line, code);
			}
			fclose(f);
		} else if (endswith(filename, ".cols")) {
			FILE* f = fopen(filename, "r");
			assert(f);

			struct cols* cols = cols_new(name);

			while (!feof(f)) {
				fread_line(f, line, sizeof(line));
				char* red;
				char* green;
				char* blue;
				char* alpha;
				if (
					!split2(line, &red) ||
					!split2(red, &green) ||
					!split2(green, &blue) ||
					!split2(blue, &alpha)) continue;

				cols_add(cols, line, atoi8(red), atoi8(green), atoi8(blue), atoi8(alpha));
			}

			dya_append(&cols_array_dya, (void**)&cols_array, &cols);
			fclose(f);
		} else {
			fprintf(stderr, "unknown extension in '%s'\n", filename);
			exit(EXIT_FAILURE);
		}
	}

	// codepoints are used for binary search; make sure they're sorted
	for (int i = 0; i < n_tables; i++) {
		struct table* t = &tables[i];
		if (t->type != TT_GLYPH) continue;
		struct glyph_table* glt = &t->glyph;
		qsort(glt->glyphs, glt->n_glyphs, sizeof(*glt->glyphs), glyph_codepoint_compar);
	}

	// zip rects into all_rects array
	struct stbrp_rect* all_rects = calloc(1, n_total_rects * sizeof(*all_rects));
	assert(all_rects != NULL);
	int offset = 0;
	for (int i = 0; i < n_tables; i++) {
		struct table* t = &tables[i];
		switch (t->type) {
		case TT_GLYPH: {
			struct glyph_table* glt = &t->glyph;
			for (int j = 0; j < glt->n_glyphs; j++) {
				memcpy(all_rects + offset, &glt->glyphs[j].rect, sizeof(stbrp_rect));
				glt->glyphs[j].all_rects_index = offset;
				offset++;
			}
		} break;
		case TT_CELL: {
			struct cell_table* clt = &t->cell;
			int n_cells = clt->n_columns*clt->n_rows;
			for (int j = 0; j < n_cells; j++) {
				clt->cells[j].all_rects_index = offset;
				for (int k = 0; k < clt->n_layers; k++) {
					memcpy(all_rects + offset, &clt->cells[j].rects[k], sizeof(stbrp_rect));
					offset++;
				}
			}
		} break;
		default:
			assert(!"unknown type");
		}
	}

	// pack rects using stb_rect_pack.h
	int n_nodes = atlas_width;
	stbrp_node* nodes = calloc(1, n_nodes * sizeof(*nodes));
	assert(nodes);
	stbrp_context ctx;
	stbrp_init_target(&ctx, atlas_width, atlas_height, nodes, n_nodes);
	stbrp_pack_rects(&ctx, all_rects, n_total_rects);

	// bail if not all rects were packed
	for (int i = 0; i < n_total_rects; i++) if (all_rects[i].w > 0 && !all_rects[i].was_packed) return 0;

	unsigned char* bitmap = calloc(atlas_width*atlas_height, 1);
	assert(bitmap);

	int n_cell_tables = 0;
	int n_glyph_tables = 0;

	// blit source bitmaps into atlas
	for (int i = 0; i < n_tables; i++) {
		struct table* t = &tables[i];
		switch (t->type) {
		case TT_GLYPH: {
			n_glyph_tables++;

			struct glyph_table* glt = &t->glyph;

			if (glt->is_dummy) {
				continue;
			}

			FILE* f = fopen(glt->filename, "r");
			assert(f);

			for (int j = 0; j < glt->n_glyphs; j++) {
				struct glyph* gly = &glt->glyphs[j];

				struct stbrp_rect* prect = &all_rects[gly->all_rects_index];
				assert(fseek(f, gly->bitmap_offset, SEEK_SET) == 0);

				for (int k = 0; k < (prect->h-1); k++) {
					fread_line(f, line, sizeof(line));
					unsigned char row[1024];
					hex2pixels(line, row, sizeof(row));
					blit(bitmap, atlas_width, row, (prect->w-1), 1, prect->x, prect->y + k);
				}
			}

			fclose(f);
		} break;
		case TT_CELL: {
			n_cell_tables++;

			struct cell_table* clt = &t->cell;
			int n_cells = clt->n_columns*clt->n_rows;

			for (int j = 0; j < n_cells; j++) {
				struct cell* c = &clt->cells[j];
				for (int k = 0; k < clt->n_layers; k++) {
					struct stbrp_rect* prect = &all_rects[c->all_rects_index + k];
					if (prect->w == 0 || prect->h == 0) continue;

					blit4(
						bitmap,
						atlas_width,

						clt->layer_bitmaps[k],
						clt->width,

						prect->w, prect->h,
						prect->x, prect->y,
						c->x0, c->y0);
				}
			}
		} break;
		default:
			assert(!"unknown type");
		}
	}

	if (endswith(outfile, ".png")) {
		int comp = 3;

		char* atlas = calloc(atlas_width*atlas_height, comp);
		assert(atlas != NULL);

		// put atlas bitmap into green color
		for (int y = 0; y < atlas_height; y++) {
			for (int x = 0; x < atlas_width; x++) {
				atlas[(x + y*atlas_width) * comp + 1] = bitmap[x + y*atlas_width];
			}
		}

		// paint rects red/blue "checkerboard"
		for (int i = 0; i < n_total_rects; i++) {
			struct stbrp_rect* r = &all_rects[i];
			if (r->w == 0 || r->h == 0) continue;

			int d = i&1 ? 0 : 2;
			int v = i&1 ? 60 : 90;

			for (int y = r->y; y < (r->y + r->h); y++) {
				for (int x = r->x; x < (r->x + r->w); x++) {
					int v2 = (x==r->x||y==r->y) ? v*2 : (x==r->x+r->w-1||y==r->y+r->h-1) ? v/2 : v;
					atlas[(x + y*atlas_width) * comp + d] = v2;
				}
			}
		}

		assert(stbi_write_png(outfile, atlas_width, atlas_height, comp, atlas, atlas_width * comp));
	} else if (endswith(outfile, ".atls")) {
		FILE* f = fopen(outfile, "wb");

		{
			struct blk* b = blk_new("ATLS");
			blk_u32(b, 5); // version

			blk_finalize(b, f);
		}

		{
			struct blk* b = blk_new("PROG");

			int n_programs = prgs->n_programs;
			blk_u32(b, n_programs);

			int n_constants = prgs->constants_dya.n;
			blk_u32(b, n_constants);

			int n_ctx_keys = prgs->ctx_keys_dya.n;
			blk_u32(b, n_ctx_keys);

			int n_colors_per_colorscheme = prgs->color_keys_dya.n;
			blk_u32(b, n_colors_per_colorscheme);

			for (int i = 0; i < n_programs; i++) {
				blk_u32(b, prgs->program_keys[i]);
				blk_u32(b, prgs->program_offsets[i]);
			}

			for (int i = 0; i < n_constants; i++) {
				blk_f32(b, prgs->constants[i]);
			}

			for (int i = 0; i < n_ctx_keys; i++) {
				blk_u32(b, prgs->ctx_keys[i]);
			}

			for (int i = 0; i < n_colors_per_colorscheme; i++) {
				blk_u32(b, prgs->color_keys[i]);
			}

			for (int i = 0; i < prgs->code_dya.n; i++) {
				blk_u16(b, prgs->code[i]);
			}

			blk_finalize(b, f);
		}

		for (int i = 0; i < cols_array_dya.n; i++) {
			struct cols* cols = cols_array[i];
			struct blk* b = blk_new("COLS");
			blk_u32(b, strtbl_add(&strtbl, cols->name));
			for (int i = 0; i < prgs->color_keys_dya.n; i++) {
				unsigned int rgba = 0x00ffffff;
				char* n0 = strtbl_get(&strtbl, prgs->color_keys[i]);
				for (int j = 0; j < cols->colors_dya.n; j++) {
					struct color* color = &cols->colors[j];
					char* n1 = color->name;
					if (strcmp(n0, n1) == 0) {
						rgba = (color->rgba[0]) | (color->rgba[1]<<8) | (color->rgba[2]<<16) | (color->rgba[3]<<24);
						break;
					}
				}
				blk_u32(b, rgba);
			}
			blk_finalize(b, f);
		}

		for (int i = 0; i < n_tables; i++) {
			struct table* t = &tables[i];
			struct blk* b = NULL;
			switch (t->type) {
			case TT_GLYPH: {
				struct glyph_table* glt = &t->glyph;
				b = blk_new("GLYT");
				blk_u32(b, t->name_sid);
				blk_u32(b, glt->n_glyphs);
				blk_u32(b, glt->height);
				blk_u32(b, glt->baseline);

				for (int i = 0; i < glt->n_glyphs; i++) {
					struct glyph* gly = &glt->glyphs[i];
					blk_u32(b, gly->codepoint);

					struct stbrp_rect* r = &all_rects[gly->all_rects_index];
					blk_u32(b, r->x);
					blk_u32(b, r->y);
					blk_u32(b, r->w);
					blk_u32(b, r->h);
					blk_u32(b, gly->xoff);
					blk_u32(b, gly->yoff);
				}
			} break;
			case TT_CELL: {
				struct cell_table* clt = &t->cell;
				b = blk_new("CELT");
				blk_u32(b, t->name_sid);
				blk_u32(b, clt->n_columns);
				blk_u32(b, clt->n_rows);
				blk_u32(b, clt->n_layers);

				struct layer* layer = clt->layers;
				for (int j = 0; j < clt->n_layers; j++) {
					assert(layer != NULL);
					blk_u32(b, layer->program_id);
					layer = layer->next;
				}
				for (int j = 0; j < clt->n_columns; j++) {
					blk_u32(b, clt->vertical[j]);
				}
				for (int j = 0; j < clt->n_rows; j++) {
					blk_u32(b, clt->horizontal[j]);
				}

				int idx = 0;
				for (int y = 0; y < clt->n_rows; y++) {
					for (int x = 0; x < clt->n_columns; x++) {
						struct cell* c = &clt->cells[idx++];
						for (int j = 0; j < clt->n_layers; j++) {
							struct stbrp_rect* r = &all_rects[c->all_rects_index + j];
							if (r->w == 0 || r->h == 0) {
								blk_s32(b, -1);
								blk_s32(b, -1);
							} else {
								blk_s32(b, r->x);
								blk_s32(b, r->y);
							}
						}
					}
				}
			} break;
			default:
				assert(!"unknown type");
			}
			assert(b != NULL);
			blk_finalize(b, f);
		}

		{
			struct blk* b = blk_new("BTMP");
			blk_u32(b, atlas_width);
			blk_u32(b, atlas_height);
			blk_u32(b, 1); // uncompressed
			blk_append(b, bitmap, atlas_width*atlas_height);
			blk_finalize(b, f);
		}

		strtbl_finalize(&strtbl, f);

		/*
		TODO
		 - glyph/cell table aliases
		*/

		fclose(f);
	} else {
		assert(!"unhandled outfile extension");
	}
	return 1;
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <out.atls> <file> [file]...\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	int n = argc - 2;

	for (int sz = 512; sz <= 8192; sz <<= 1) {
		if (try_sz(sz, argv[1], n, argv + 2)) {
			exit(EXIT_SUCCESS);
		}
	}

	exit(EXIT_FAILURE);
}
