#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "lsl.h"
#include "utf8.h"

static void draw_glyph(struct atls_glyph*);
static void fill_rect(struct rect posrect, union vec4 color);

#define ASSERT(cond) \
	do { \
		if (!(cond)) { \
			fprintf(stderr, "ASSERT(%s) failed in %s() in %s:%d\n", #cond, __func__, __FILE__, __LINE__); \
			exit(EXIT_FAILURE); \
		} \
	} while (0)

#define AN(expr) do { ASSERT((expr) != 0); } while(0)

struct atls* active_atls;
struct atls_glyph_table* active_glyph_table;
struct atls_cell_table* active_cell_table;
struct atls_glyph* rgly_dot;

int cursor_x;
int cursor_x0;
int cursor_y;

#define STACK_MAX (256)

int scope_stack_size;
u64 scope_stack[STACK_MAX];

union vec4 draw_color0, draw_color1, text_bg_color;

struct wglobal {
	union vec2 mdelta;

	int button[LSL_MAX_BUTTONS];
	int button_cycles[LSL_MAX_BUTTONS];

	u32 mod;

	int text_length;
	u32 text[LSL_MAX_TEXT_LENGTH]; // codepoints

	int press_active, press_active_modmask;
	u64 press_active_id;
};
static struct wglobal* wglobal_get();

static void wglobal_post_frame_reset()
{
	struct wglobal* wg = wglobal_get();
	wg->text_length = 0;
	for (int i = 0; i < LSL_MAX_BUTTONS; i++) {
		wg->button_cycles[i] = 0;
	}
}


struct wframe {
	struct rect rect; // rectangle (absolute coords)
	int minside; // is mouse inside rect?
	union vec2 mpos; // mouse position (relative to rect top-left corner)
} wframe_stack[STACK_MAX];
int wframe_stack_top_index;

static void new_frame(struct wframe* f)
{
	union vec2 new_mpos = f->mpos;
	union vec2 old_mpos = wframe_stack[0].mpos;
	wglobal_get()->mdelta = vec2_sub(new_mpos, old_mpos);
	wframe_stack_top_index = 0;
	wframe_stack[0] = *f;
}

static void assert_valid_wframe_stack_top(int i)
{
	assert(i >= 0 && i < STACK_MAX);
}

struct wframe* wframe_top()
{
	return &wframe_stack[wframe_stack_top_index];
}

static void push_text_input_code(struct wglobal* wg, u32 code)
{
	if ((wg->text_length + 1) >= LSL_MAX_TEXT_LENGTH) return;
	wg->text[wg->text_length++] = code | wg->mod;
}

static u64 hash(const void* key, int len, u64 seed)
{
	// migrated from MurmurHash64A
	const u64 m = 0xc6a4a7935bd1e995;
	const int r = 47;

	u64 h = seed ^ (len * m);

	const u64* data = (const u64*)key;
	const u64* end = data + (len/8);

	while (data != end) {
		u64 k = *data++;

		k *= m;
		k ^= k >> r;
		k *= m;

		h ^= k;
		h *= m;
	}

	const unsigned char* data2 = (const unsigned char*)data;

	switch(len & 7) {
		case 7: h ^= (u64)data2[6] << 48;
		case 6: h ^= (u64)data2[5] << 40;
		case 5: h ^= (u64)data2[4] << 32;
		case 4: h ^= (u64)data2[3] << 24;
		case 3: h ^= (u64)data2[2] << 16;
		case 2: h ^= (u64)data2[1] << 8;
		case 1: h ^= (u64)data2[0];
			h *= m;
	}

	h ^= h >> r;
	h *= m;
	h ^= h >> r;

	return h;
}

static u64 get_scope_id(const char* id)
{
	return hash(&id, sizeof id, scope_stack_size == 0 ? 0 : scope_stack[scope_stack_size-1]);
}

void lsl_scope_push_data(const void* data, size_t sz)
{
	assert(scope_stack_size < STACK_MAX);
	u64 prev = scope_stack_size == 0 ? 0 : scope_stack[scope_stack_size-1];
	scope_stack[scope_stack_size++] = hash(data, sz, prev);
}

void lsl_scope_push_static(const void* ptr)
{
	lsl_scope_push_data(&ptr, sizeof ptr);
}

void lsl_scope_pop()
{
	assert(scope_stack_size > 0);
	scope_stack_size--;
}

u32 lsl_getch()
{
	struct wglobal* wg = wglobal_get();
	if (wg->text_length <= 0) return -1;
	u32 code = wg->text[0];
	wg->text_length--;
	memmove(wg->text, wg->text+1, wg->text_length * sizeof wg->text);
	return code;
}

struct atls_glyph* get_codepoint_glyph(int codepoint)
{
	struct atls_glyph* glyph = atls_glyph_table_lookup(active_glyph_table, codepoint);
	if (glyph == NULL) glyph = &active_glyph_table->glyphs[0];
	return glyph;
}

int lsl_get_text_width(char* str, int n)
{
	if (active_glyph_table == NULL) return 0;

	char* c = str;
	int width = 0;
	while (n > 0) {
		int codepoint = utf8_decode(&c, &n);
		if (codepoint == -1) return -1;
		if (codepoint == '\n') return width;
		struct atls_glyph* glyph = get_codepoint_glyph(codepoint);
		width += (glyph->w - 1);
	}
	return width;
}

int lsl_get_text_height()
{
	assert(active_glyph_table != NULL);
	return active_glyph_table->height;
}

void lsl_putch(int codepoint)
{
	if (active_glyph_table == NULL) return;

	if (codepoint == '\n') {
		cursor_x = cursor_x0;
		cursor_y += active_glyph_table->height;
		return;
	}

	struct atls_glyph* glyph = get_codepoint_glyph(codepoint);
	struct rect grect = {
		.p0 = { .x = cursor_x, .y = cursor_y - active_glyph_table->height },
		.dim = { .w = glyph->w, .h = glyph->h }
	};
	fill_rect(grect, text_bg_color);
	draw_glyph(glyph);
	/* XXX is the "off-by-1" required due to padding in mkatls? if so, is
	 * it that way by convention, or is it something I ought to throw into
	 * the .atls file? */
	cursor_x += glyph->w - 1;
}

void lsl_set_atls(struct atls* a)
{
	assert(a != NULL);
	active_atls = a;

	int ri = atls_get_glyph_table_index(a, "lsl_reserved");
	assert(ri >= 0);
	struct atls_glyph_table* r = &a->glyph_tables[ri];
	rgly_dot = atls_glyph_table_lookup(r, 1);
	assert(rgly_dot != NULL);

	// paint reserved stuff; dot
	for (int y = rgly_dot->y; y < (rgly_dot->y+rgly_dot->h); y++) {
		for (int x = rgly_dot->x; x < (rgly_dot->x+rgly_dot->w); x++) {
			a->atlas_bitmap[x + y * a->atlas_width] = 255;
		}
	}
}

void lsl_set_type_index(unsigned int index)
{
	assert(index >= 0);
	assert(index < active_atls->n_glyph_tables);
	active_glyph_table = &active_atls->glyph_tables[index];
}

struct atls_cell_table* lsl_set_cell_table(unsigned int index)
{
	assert(index >= 0);
	assert(index < active_atls->n_cell_tables);
	active_cell_table = &active_atls->cell_tables[index];

	return active_cell_table;
}

void lsl_set_cursor(int x, int y)
{
	cursor_x = cursor_x0 = x;
	cursor_y = y;
}

void lsl_get_cursor(int* x, int* y)
{
	if (x) *x = cursor_x;
	if (y) *y = cursor_y;
}

union vec4 lsl_eval(int atls_prg_id)
{
	float out[4];
	if (atls_prg_id >= 0 && atls_eval(active_atls, atls_prg_id, out) >= 0) {
		return (union vec4) { .r = out[0], .g = out[1], .b = out[2], .a = out[3]};
	} else {
		return (union vec4) { .r = 1, .g = 0, .b = 1, .a = 1};
	}
}



void lsl_set_gradient(union vec4 color0, union vec4 color1)
{
	draw_color0 = color0;
	draw_color1 = color1;
}

void lsl_set_color(union vec4 color)
{
	lsl_set_gradient(color, color);
}

void lsl_clear_text_bg_color()
{
	text_bg_color = (union vec4) { .r=0, .g=0, .b=0, .a=0 };
}

void lsl_set_text_bg_color(union vec4 color)
{
	text_bg_color = color;
}

int lsl_write(char* buf, int n)
{
	char* c = buf;
	while (n > 0) {
		int codepoint = utf8_decode(&c, &n);
		if (codepoint == -1) return -1;
		lsl_putch(codepoint);
	}
	return 0;
}

int lsl_printf(const char* fmt, ...)
{
	char buf[8192];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);

	if (n < 0) return -1;
	return lsl_write(buf, n);
}

int lsl_mpos_vec2(union vec2* mpos)
{
	struct wframe* top = wframe_top();
	if (mpos) *mpos = top->mpos;
	return top->minside;
}

int lsl_mpos(int* mx, int* my)
{
	union vec2 mpos;
	int retval = lsl_mpos_vec2(&mpos);
	if (mx) *mx = mpos.x;
	if (my) *my = mpos.y;
	return retval;
}

int lsl_mdelta_vec2(union vec2* mdelta)
{
	if (mdelta) *mdelta = wglobal_get()->mdelta;
	return wframe_top()->minside;
}

int lsl_mdelta(int* dx, int* dy)
{
	union vec2 mdelta;
	int retval = lsl_mdelta_vec2(&mdelta);
	if (dx) *dx = mdelta.x;
	if (dy) *dy = mdelta.y;
	return retval;
}

int lsl_press(const char* id, int activatable, int pointer)
{
	struct wframe* f = wframe_top();
	struct wglobal* wg = wglobal_get();

	int minside = f->minside;
	int pressed = 0;
	int press = 0;
	int modmask = 0;
	for (int button = 0; button < 3; button++) {
		int p = minside && wg->button[button];
		pressed |= p;
		press |= (p && wg->button_cycles[button]);
		if (p) modmask |= (LSL_LMB << button);
	}

	int retval = 0;

	if (wg->press_active) {
		if (wg->press_active_id != get_scope_id(id)) return 0;
		if (pointer != 0) lsl_set_pointer(pointer);

		int dx, dy;
		lsl_mdelta(&dx, &dy);
		if (dx || dy) wg->press_active_modmask |= LSL_DRAGGED;

		if (pressed) {
			retval = LSL_ACTIVE;
		} else {
			lsl_set_pointer(0);
			wg->press_active = 0;
			if (activatable) {
				retval = LSL_CLICK | LSL_RELEASE;
			} else {
				retval = LSL_CANCEL | LSL_RELEASE;
			}
		}
	} else if (!wg->press_active && activatable) {
		if (pointer != 0) lsl_set_pointer(pointer);

		if (press) {
			wg->press_active = 1;
			wg->press_active_id = get_scope_id(id);
			wg->press_active_modmask = modmask | wg->mod;
			retval = LSL_PRESS | LSL_ACTIVE;
		}
	}

	return retval == 0 ? 0 : (retval | wg->press_active_modmask);
}

void lsl_clip_push(struct rect* r)
{
	struct wframe* src = &wframe_stack[wframe_stack_top_index];

	assert_valid_wframe_stack_top(++wframe_stack_top_index);
	struct wframe* dst = &wframe_stack[wframe_stack_top_index];
	memcpy(dst, src, sizeof *dst);
	dst->rect = (struct rect) { .p0 = vec2_add(src->rect.p0, r->p0), .dim = r->dim };

	dst->mpos = vec2_sub(dst->mpos, r->p0);
	if (!rect_contains_point(r, src->mpos)) {
		dst->minside = 0;
	}
}

void lsl_clip_pop()
{
	assert_valid_wframe_stack_top(--wframe_stack_top_index);
}

void lsl_bezier(float thickness, union vec2 p0, union vec2 p1, union vec2 p2, union vec2 p3)
{
	const int N = 50;
	for (int i = 0; i < N; i++) {
		float t0 = (float)i / (float)N;
		float t1 = (float)(i+1) / (float)N;
		lsl_line(
			thickness,
			bezier2(t0, p0, p1, p2, p3),
			bezier2(t1, p0, p1, p2, p3));
	}
}


void lsl_textedit_reset(struct lsl_textedit* te)
{
	memset(te, 0, sizeof *te); // XXX no reason to waste time clearing te->buffer actually?
}

static int txt_delete_at(struct lsl_textedit* t, int pos)
{
	if (pos < 0 || pos > t->buffer_len) return 0;
	assert(pos <= t->buffer_len);
	if (t->buffer_len > 0) {
		int to_move = t->buffer_len - pos;
		if (to_move) {
			if (to_move > 0) memmove(&t->buffer[pos], &t->buffer[pos+1], (t->buffer_len - pos) * sizeof *t->buffer);
			t->buffer_len--;
			return 1;
		} else {
			return 0;
		}
	} else {
		return 0;
	}
}

static int txt_delete_selection(struct lsl_textedit* t)
{
	if (t->select_start == t->select_end) return 0;
	int select_min = t->select_start < t->select_end ? t->select_start : t->select_end;
	int select_max = t->select_start > t->select_end ? t->select_start : t->select_end;
	memmove(&t->buffer[select_min], &t->buffer[select_max], (t->buffer_len - select_max) * sizeof *t->buffer);
	t->buffer_len -= (select_max - select_min);
	t->cursor = t->select_start = t->select_end = select_min;
	return 1;
}

static int txt_insert_at(struct lsl_textedit* t, int pos, u32 ch)
{
	if (pos < 0 || pos > t->buffer_len) return 0;
	if (t->buffer_len < LSL_TEXTEDIT_BUFSZ) {
		int to_move = t->buffer_len - pos;
		if (to_move > 0) memmove(&t->buffer[pos+1], &t->buffer[pos], (t->buffer_len - pos) * sizeof *t->buffer);
		t->buffer[pos] = ch;
		t->buffer_len++;
		return 1;
	} else {
		return 0;
	}
}

static void txt_seek_word_boundary(struct lsl_textedit* t, int d)
{
	assert(!"TODO");
	/*
	XXX what are the rules?
	*/
	int pos = t->cursor;
	for (;;) {
		pos += d;
		if (pos < 0) {
			pos = 0;
			break;
		} else if (pos > t->buffer_len) {
			pos = t->buffer_len;
			break;
		}
	}
	t->cursor = pos;
}

int lsl_textedit_io(struct lsl_textedit* t)
{
	int ch;
	while ((ch = lsl_getch()) != -1) {
		int cp = ch & LSL_CH_CODEPOINT_MASK;

		int shift = ch & LSL_MOD_SHIFT;
		int ctrl = ch & LSL_MOD_CTRL;

		int is_movement = 0;

		if (cp == 27) { // ESC
			return LSL_CANCEL;
		} else if (cp == LSLK_ARROW_LEFT) {
			is_movement = 1;
			if (ctrl) txt_seek_word_boundary(t, -1); else t->cursor--;
		} else if (cp == LSLK_ARROW_RIGHT) {
			is_movement = 1;
			if (ctrl) txt_seek_word_boundary(t, 1);  else t->cursor++;
		} else if (cp == LSLK_HOME) {
			is_movement = 1;
			t->cursor = 0;
		} else if (cp == LSLK_END) {
			is_movement = 1;
			t->cursor = t->buffer_len;
		} else if (cp == 127) { // DEL
			if (!txt_delete_selection(t)) txt_delete_at(t, t->cursor);
		} else if (cp == '\b') {
			if (!txt_delete_selection(t)) txt_delete_at(t, --t->cursor);
		} else if (cp == '\n') {
			return LSL_COMMIT;
		} else if (cp == '\t') {
			assert(!"TODO complete?");
		} else if (cp >= ' ') {
			txt_delete_selection(t);
			txt_insert_at(t, t->cursor++, cp);
		}

		// clamp cursor
		if (t->cursor < 0) t->cursor = 0;
		if (t->cursor > t->buffer_len) t->cursor = t->buffer_len;

		// set selection range
		t->select_end = t->cursor;
		if (!is_movement || !shift) t->select_start = t->select_end;
	}

	#if 0
	//printf("["); fwrite(t->buffer, 1, t->buffer_len, stdout); printf("]\n");
	printf("[");
	for (int i = 0; i < t->buffer_len; i++) printf("%c", t->buffer[i]);
	printf("]\n");
	#endif

	return LSL_ACTIVE;
}


#ifdef USE_GLX11

	#include "lsl_gl_header.h"
	#include "lsl_gl_x11.h"
	#include "lsl_gl.h"

#elif USE_GL_WIN

	#include "lsl_gl_header.h"
	#include "lsl_gl_win.h"
	#include "lsl_gl.h"

#else
	#error "missing lsl USE_* define/implementation (2)"
#endif
