#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "lsl_prg.h"

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
#define MAX_PALETTE_LENGTH (256)
union vec4 cell_table_palette[MAX_PALETTE_LENGTH];
int cell_table_palette_length;
int cursor_x;
int cursor_x0;
int cursor_y;

union vec4 draw_color0;
union vec4 draw_color1;

#define FRAME_STACK_MAX (16)
struct lsl_frame frame_stack[FRAME_STACK_MAX];
int frame_stack_top_index;

static void frame_stack_reset(struct lsl_frame* f)
{
	frame_stack_top_index = 0;
	frame_stack[0] = *f;
}

static void assert_valid_frame_stack_top(int i)
{
	assert(i >= 0 && i < FRAME_STACK_MAX);
}

struct lsl_frame* lsl_frame_top()
{
	return &frame_stack[frame_stack_top_index];
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

static void draw_glyph(struct atls_glyph*);

void lsl_putch(int codepoint)
{
	if (active_glyph_table == NULL) return;

	if (codepoint == '\n') {
		cursor_x = cursor_x0;
		cursor_y += active_glyph_table->height;
		return;
	}

	struct atls_glyph* glyph = atls_glyph_table_lookup(active_glyph_table, codepoint);
	if (glyph == NULL) glyph = &active_glyph_table->glyphs[0];
	draw_glyph(glyph);
	cursor_x += glyph->w;
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

struct atls_cell_table* lsl_set_cell_table(unsigned int index, struct atls_colorscheme* palette)
{
	assert(index >= 0);
	assert(index < active_atls->n_cell_tables);
	active_cell_table = &active_atls->cell_tables[index];

	for (int i = 0; i < active_cell_table->n_layers && i < MAX_PALETTE_LENGTH; i++) {
		struct atls_color* color = atls_colorscheme_layer_lookup(palette, active_cell_table->layer_names[i].id);
		if (color == NULL) {
			cell_table_palette[i] = (union vec4) { .r=1, .g=0, .b=1, .a=1 };
		} else {
			atls_color_rgba(color,
				&cell_table_palette[i].r,
				&cell_table_palette[i].g,
				&cell_table_palette[i].b,
				&cell_table_palette[i].a);
		}
	}

	return active_cell_table;
}

void lsl_set_cursor(int x, int y)
{
	cursor_x = cursor_x0 = x;
	cursor_y = y;
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

int lsl_printf(const char* fmt, ...)
{
	char buf[8192];
	va_list ap;
	va_start(ap, fmt);
	int nret = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	int n = nret;
	char* c = buf;
	while (n > 0) {
		int codepoint = utf8_decode(&c, &n);
		if (codepoint == -1) return -1;
		lsl_putch(codepoint);
	}

	return nret;
}

static void set_vh_pointer(int xp, int yp)
{
	if (xp && !yp) {
		lsl_set_pointer(LSL_POINTER_HORIZONTAL);
	} else if (!xp && yp) {
		lsl_set_pointer(LSL_POINTER_VERTICAL);
	} else if (xp && yp) {
		lsl_set_pointer(LSL_POINTER_4WAY);
	}
}

int drag_active_id;
int drag_initial_x;
int drag_initial_y;
int drag_initial_mx;
int drag_initial_my;

int lsl_drag(struct rect* r, int drag_id, int* x, int* y, int fx, int fy)
{
	if (x == NULL && y == NULL) return 0;

	struct lsl_frame* f = lsl_frame_top();

	int btn = f->button[0];
	int retval = 0;

	if (drag_active_id) {
		if (drag_active_id != drag_id) return 0;

		set_vh_pointer(x != NULL, y != NULL);

		retval = LSL_DRAG_CONT;
		if (!btn) {
			lsl_set_pointer(0);
			drag_active_id = 0;
			retval = LSL_DRAG_STOP;
		}
		if (x != NULL) *x = drag_initial_x + (f->mpos.x - drag_initial_mx) * fx;
		if (y != NULL) *y = drag_initial_y + (f->mpos.y - drag_initial_my) * fy;
	} else if (r == NULL || rect_contains_point(r, f->mpos)) {
		if (r != NULL) set_vh_pointer(x != NULL, y != NULL);

		if (btn) {
			drag_active_id = drag_id;
			retval = LSL_DRAG_START;
			if (x != NULL) drag_initial_x = *x;
			if (y != NULL) drag_initial_y = *y;
			drag_initial_mx = f->mpos.x;
			drag_initial_my = f->mpos.y;
		}
	}
	return retval;
}

void lsl_frame_push_clip(struct rect* r)
{
	struct lsl_frame* src = &frame_stack[frame_stack_top_index];

	assert_valid_frame_stack_top(++frame_stack_top_index);
	struct lsl_frame* dst = &frame_stack[frame_stack_top_index];
	memcpy(dst, src, sizeof(*dst));
	dst->rect = (struct rect) { .p0 = vec2_add(src->rect.p0, r->p0), .dim = r->dim };

	dst->mpos = vec2_sub(dst->mpos, r->p0);
	if (!rect_contains_point(r, src->mpos)) {
		// XXX what about dragging?
		dst->minside = 0;
		memset(&dst->button, 0, sizeof(dst->button));
		memset(&dst->button_cycles, 0, sizeof(dst->button_cycles));
	}
}

void lsl_frame_pop()
{
	assert_valid_frame_stack_top(--frame_stack_top_index);
}

union vec4 lsl_white()
{
	return (union vec4) { .r = 1, .g = 1, .b = 1, .a = 1 };
}

union vec4 lsl_black()
{
	return (union vec4) { .r = 0, .g = 0, .b = 0, .a = 1 };
}

#ifdef USE_GLX11
#include "lsl_prg_glx11.h"
#else
#error "missing lsl define/implementation (2)"
#endif
