#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "lsl.h"
#include "dtypes.h"

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

union vec4 draw_color0;
union vec4 draw_color1;

struct wglobal {
	union vec2 mdelta;

	int button[LSL_MAX_BUTTONS];
	int button_cycles[LSL_MAX_BUTTONS];

	int mod;

	int text_length;
	char text[LSL_MAX_TEXT_LENGTH + 1]; /* UTF-8 */

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

static inline int utf8_decode(char** c0z, int* n)
{
	unsigned char** c0 = (unsigned char**)c0z;
	if (*n <= 0) return -1;
	unsigned char c = **c0;
	(*n)--;
	(*c0)++;
	if ((c & 0x80) == 0) return c & 0x7f;
	int mask = 192;
	for (int d = 1; d <= 3; d++) {
		int match = mask;
		mask = (mask >> 1) | 0x80;
		if ((c & mask) == match) {
			int codepoint = (c & ~mask) << (6*d);
			while (d > 0 && *n > 0) {
				c = **c0;
				if ((c & 192) != 128) return -1;
				(*c0)++;
				(*n)--;
				d--;
				codepoint += (c & 63) << (6*d);
			}
			return d == 0 ? codepoint : -1;
		}
	}
	return -1;
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
	return hash(&id, sizeof(id), scope_stack_size == 0 ? 0 : scope_stack[scope_stack_size-1]);
}

void lsl_scope_push_data(const void* data, size_t sz)
{
	assert(scope_stack_size < STACK_MAX);
	u64 prev = scope_stack_size == 0 ? 0 : scope_stack[scope_stack_size-1];
	scope_stack[scope_stack_size++] = hash(data, sz, prev);
}

void lsl_scope_push_static(const void* ptr)
{
	lsl_scope_push_data(&ptr, sizeof(ptr));
}

void lsl_scope_pop()
{
	assert(scope_stack_size > 0);
	scope_stack_size--;
}

int lsl_accept(int codepoint)
{
	struct wglobal* wg = wglobal_get();
	char* p = wg->text;
	int n = wg->text_length;
	if (utf8_decode(&p, &n) == codepoint) {
		memmove(wg->text, wg->text + (wg->text_length - n), n);
		wg->text_length = n;
		return 1;
	} else {
		return 0;
	}
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

static void draw_glyph(struct atls_glyph*);

void lsl_putch(int codepoint)
{
	if (active_glyph_table == NULL) return;

	if (codepoint == '\n') {
		cursor_x = cursor_x0;
		cursor_y += active_glyph_table->height;
		return;
	}

	struct atls_glyph* glyph = get_codepoint_glyph(codepoint);
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
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
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
		if (pressed) modmask |= (LSL_LMB << button);
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
	memcpy(dst, src, sizeof(*dst));
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
