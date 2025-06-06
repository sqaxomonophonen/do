#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "stb_ds_sysalloc.h"
#include "stb_rect_pack.h"
#include "stb_truetype.h"
#include "stb_image_write.h"
#include "stb_image_resize2.h"

#include "main.h"
#include "util.h"
#include "gui.h"
#include "sep2dconv.h"
#include "gig.h"
#include "fonts.h"
#include "utf8.h"

#define ATLAS_MIN_SIZE_LOG2 (8)
#define ATLAS_MAX_SIZE_LOG2 (13)

enum special_codepoint {
	SPECIAL_CODEPOINT0 = (1<<22),
	SPECIAL_CODEPOINT_MISSING_CHARACTER,
	SPECIAL_CODEPOINT_BLOCK,
	SPECIAL_CODEPOINT_CARET,
	SPECIAL_CODEPOINT1,
};

enum pane_type {
	CODE = 1,
	WIZARD,
	SETTINGS,
	VIDEOSYNTH,
};

// panes are "sub windows" inside your parent window
struct pane {
	enum pane_type type;
	float u0,v0,u1,v1;
	union {
		struct {
			int session_id;
			int focus_id;
			int splash4_comp, splash4_cache;
			int cpick_on;
			// TODO presentation shader id? (can be built-in, or user-defined?)
		} code;
		struct {
			int output_id;
		} videosynth;
	};
};

struct blur_level {
	float radius;
	float variance;
	float scale; // must be 1.0 or less
	float kernel_scalar;
	float post_scalar;
};

struct rect {
	int x,y,w,h;
};
static inline struct rect make_rect(int x, int y, int w, int h)
{
	return (struct rect){
		.x=x, .y=y,
		.w=w, .h=h,
	};
}

struct atlas_lut_info {
	int rect_index0;
	struct rect rect;
};

struct resize {
	struct rect src, dst;
};

static int resize_dim_compar(const struct resize* a, const struct resize* b)
{
	const int d0 = a->src.w - b->src.w;
	if (d0!=0) return d0;

	const int d1 = a->src.h - b->src.h;
	if (d1!=0) return d1;

	const int d2 = a->dst.w - b->dst.w;
	if (d2!=0) return d2;

	const int d3 = a->dst.h - b->dst.h;
	return d3;
}

static int resize_compar(const void* va, const void* vb)
{
	const int dd = resize_dim_compar(va,vb);
	if (dd!=0) return dd;

	const struct resize* a = va;
	const struct resize* b = vb;

	const int d0 = a->src.x - b->src.x;
	if (d0!=0) return d0;

	const int d1 = a->src.y - b->src.y;
	if (d1!=0) return d1;

	const int d2 = a->dst.x - b->dst.x;
	if (d2!=0) return d2;

	const int d3 = a->dst.y - b->dst.y;
	return d3;
}

struct atlas_lut_key {
	uint8_t font_spec_index;
	uint8_t y_stretch_index;
	uint8_t _padding[2];
	unsigned codepoint;
};
static_assert(sizeof(struct atlas_lut_key)==8, "unexpected size");

struct font_spec {
	int font_index;
	int px;
	unsigned uses_y_stretch:1;

	// derived
	float _px_scale;
	int _x_spacing;
	//int _y_spacing;
	int _ascent, _descent, _line_gap;
};

struct font_config {
	int* codepoint_range_pair_arr;
	float y_scalar_min, y_scalar_max;
	int num_y_stretch_levels; // XXX maybe calculate automatically?
	int num_blur_levels;
	const struct blur_level* blur_levels;
	int num_font_specs;
	struct font_spec* font_specs;
};

static float font_config_get_y_stretch_level_scale(struct font_config* fc, int index)
{
	const int n = fc->num_y_stretch_levels;
	assert(0<=index && index<n);
	if (n == 1) return fc->y_scalar_max;
	return fc->y_scalar_min + (fc->y_scalar_max - fc->y_scalar_min) * ((float)index / (float)(n-1));
}

struct draw_state {
	int is_dimming;
	int cursor_x0, cursor_x, cursor_y;
	float current_color[3];
	int current_font_spec_index;
	int current_y_stretch_index;
};

static struct {
	struct window* window_arr;
	struct pane* pane_arr;
	struct font_config font_config;
	stbrp_context rect_pack_context;
	stbrp_node rect_pack_nodes[1 << ATLAS_MAX_SIZE_LOG2];
	stbrp_rect* atlas_pack_rect_arr;

	struct {
		//int key;
		struct atlas_lut_key key;
		struct atlas_lut_info value;
	}* atlas_lut;
	int atlas_texture_id;
	struct draw_list* draw_list_arr;
	struct vertex* vertex_arr;
	vertex_index* vertex_index_arr;
	int64_t last_frame_time;
	float fps;
	struct render_mode render_mode;
	struct window* current_window;
	int focus_sequence;
	int current_focus_id;
	int* key_buffer_arr;
	char* text_buffer_arr;
	int is_dragging;
	struct draw_state state, save_state;

	struct document  doc_copy;
	struct mim_state ms_copy;
} g;

static inline int make_focus_id(void)
{
	return ++g.focus_sequence;
}

static void focus(int focus_id)
{
	assert(focus_id>0);
	g.current_focus_id = focus_id;
}

#if 0
static void unfocus(void)
{
	g.current_focus_id = 0;
}
#endif

static struct font_spec* get_current_font_spec(void)
{
	return &g.font_config.font_specs[g.state.current_font_spec_index];
}

static float get_current_y_stretch_scale(void)
{
	struct font_spec* spec = get_current_font_spec();
	if (!spec->uses_y_stretch) return 1.0f;
	return font_config_get_y_stretch_level_scale(&g.font_config, g.state.current_y_stretch_index);
}

int get_num_windows(void)
{
	return arrlen(g.window_arr);
}

struct window* get_window(int index)
{
	return arrchkptr(g.window_arr, index);
}

static int next_window_id=1;
void open_window(void)
{
	arrput(g.window_arr, ((struct window){
		.state = WINDOW_IS_NEW,
		.id = next_window_id++,
	}));
}

void remove_closed_windows(void)
{
	for (int i=0; i<get_num_windows(); ++i) {
		struct window* w = get_window(i);
		if (w->state == WINDOW_IS_CLOSING) {
			assert((w->backend_extra == NULL) && "we expected the backend to have cleaned up by this point?");
			arrdel(g.window_arr, i);
			--i;
		}
	}
}

struct blur_level_metrics {
	int inner_width;
	int inner_height;
	int width;
	int height;
	int blurpx;
	int padding;
};

// returns 1 if a/b are the same "render mode".
static int render_mode_same(struct render_mode* a, struct render_mode* b)
{
	int num_tex = 0;
	switch (a->type) {
	case MESH_TRIANGLES: num_tex=1; break;
	default: assert(!"unhandled case");
	}

	return
		// compare simple fields
		   (a->blend_mode == b->blend_mode)
		&& (a->type       == b->type)
		&& (a->do_scissor == b->do_scissor)

		// compare scissor rect if enabled
		&& (!a->do_scissor || (
			   (a->scissor_x == b->scissor_x)
			&& (a->scissor_y == b->scissor_y)
			&& (a->scissor_w == b->scissor_w)
			&& (a->scissor_h == b->scissor_h)
		))

		// compare textures if enabled
		&& (num_tex==0 || (a->texture_id == b->texture_id))
	;
}

static int get_blurpx(const struct blur_level* bl)
{
	return (int)ceilf(bl->radius * bl->scale);
}

static struct blur_level_metrics get_blur_level_metrics(const struct blur_level* bl, int w, int h)
{
	const int inner_width  = (int)ceilf((float)w*bl->scale);
	const int inner_height = (int)ceilf((float)h*bl->scale);
	const int blurpx = get_blurpx(bl);
	const int padding = 1;
	const int width  = inner_width  + 2*blurpx + padding;
	const int height = inner_height + 2*blurpx + padding;
	return (struct blur_level_metrics) {
		.inner_width=inner_width, .inner_height=inner_height,
		.width=width, .height=height,
		.blurpx=blurpx,
		.padding=padding,
	};
}

// gaussian bell curve at x for variance v and mean=0
static inline double gaussian(double v, double x)
{
	return exp(-(x*x)/(2.0*v*v)) / sqrt(6.283185307179586 * v*v);
}

static int build_atlas(void)
{
	const int64_t t0 = get_nanoseconds();

	struct font_config* fc = &g.font_config;

	const int nc = arrlen(fc->codepoint_range_pair_arr);
	assert(nc%2==0);
	const int num_codepoint_ranges = nc/2;
	assert(num_codepoint_ranges > 0);
	assert(fc->num_y_stretch_levels > 0);
	assert(fc->num_blur_levels > 0);

	// check that codepoint ranges are ordered and don't overlap
	for (int i=0; i<num_codepoint_ranges; ++i) {
		const int r0 = arrchkget(fc->codepoint_range_pair_arr, i*2+0);
		const int r1 = arrchkget(fc->codepoint_range_pair_arr, i*2+1);
		assert((r0<=r1) && "bad range");
		assert((i==0 || r0>arrchkget(fc->codepoint_range_pair_arr, i*2-1)) && "range not ordered");
	}

	for (int i=0; i<fc->num_font_specs; ++i) {
		struct font_spec* spec = &fc->font_specs[i];
		struct font* font = get_font(spec->font_index);
		spec->_px_scale = stbtt_ScaleForPixelHeight(&font->fontinfo, spec->px);
	}

	if (g.atlas_lut != NULL) hmfree(g.atlas_lut);
	assert(g.atlas_lut == NULL);
	hminit(g.atlas_lut, &system_allocator);

	arrreset(g.atlas_pack_rect_arr);

	static int* glyph_index_arr = NULL;
	arrreset(glyph_index_arr);

	// gather rectangles for each glyph/codepoint/graphic, each y-stretch
	// level, each blur level...
	for (int fsi=0; fsi<fc->num_font_specs; ++fsi) {
		struct font_spec* spec = &fc->font_specs[fsi];
		struct font* font = get_font(spec->font_index);
		stbtt_fontinfo* fontinfo = &font->fontinfo;
		int max_advance = 0;

		// XXX "special codepoints" need a bbox related to the font, so we
		// steal it from a "boxy character", but this isn't guaranteed to be
		// present in the font? :)
		const int boxy_codepoints[] = {'W'};
		int boxy_glyph_index = -1;
		for (int i=0; i<ARRAY_LENGTH(boxy_codepoints); ++i) {
			const int index = stbtt_FindGlyphIndex(fontinfo, boxy_codepoints[i]);
			if (index <= 0) continue;
			boxy_glyph_index = index;
			break;
		}
		assert((boxy_glyph_index > 0) && "found no 'boxy' glyph; maybe fix by adding more to `boxy_codepoints[]`?");

		for (int ci=0; ci<num_codepoint_ranges; ++ci) {
			const int cp0 = arrchkget(fc->codepoint_range_pair_arr, 2*ci);
			const int cp1 = arrchkget(fc->codepoint_range_pair_arr, 1+2*ci);
			for (int codepoint=cp0; codepoint<=cp1; ++codepoint) {
				int glyph_index = -1;
				if (codepoint < SPECIAL_CODEPOINT0) {
					glyph_index = stbtt_FindGlyphIndex(fontinfo, codepoint);
					if (glyph_index > 0) {
						int advance,lsb;
						stbtt_GetGlyphHMetrics(fontinfo, glyph_index, &advance, &lsb);
						//printf("i=%d, adv=%d px=%f\n", glyph_index, advance, advance*g.font_px_scale);
						if (advance > max_advance) max_advance = advance;
					}
				}
				arrput(glyph_index_arr, glyph_index);

				const int ny = spec->uses_y_stretch ? fc->num_y_stretch_levels : 1;
				for (int ysi=0; ysi<ny; ++ysi) {
					const float y_scale = spec->uses_y_stretch ? font_config_get_y_stretch_level_scale(fc, ysi) : 1.0f;

					int x0=0,y0=0,x1=0,y1=0;
					if (codepoint < SPECIAL_CODEPOINT0) {
						if (glyph_index > 0) {
							stbtt_GetGlyphBitmapBox(fontinfo, glyph_index, spec->_px_scale, y_scale * spec->_px_scale, &x0, &y0, &x1, &y1);
						}
					} else {
						assert((SPECIAL_CODEPOINT0 < codepoint) && (codepoint < SPECIAL_CODEPOINT1));
						stbtt_GetGlyphBitmapBox(
							fontinfo,
							boxy_glyph_index,
							spec->_px_scale, y_scale * spec->_px_scale,
							&x0, &y0, &x1, &y1);
						switch (codepoint) {
						case SPECIAL_CODEPOINT_MISSING_CHARACTER:
							// use boxy glyph bbox as-is
							break;
						case SPECIAL_CODEPOINT_BLOCK:
							// expand boxy bbox (XXX not sure this is "right"
							// but it seems to work?)
							--x0; --y0;
							++x1; ++y1;
							break;
						case SPECIAL_CODEPOINT_CARET:
							// use boxy height, but our own width for caret
							// (XXX should the width be some fraction of the
							// height?)
							x0=0;
							x1=2;
							--y0; ++y1;
							break;
						default: assert(!"unhandled case");
						}
					}

					const int w0 = x1-x0;
					const int h0 = y1-y0;

					const int has_pixels = (w0>0) && (h0>0);

					struct atlas_lut_key key = {
						.font_spec_index = fsi,
						.y_stretch_index = ysi,
						.codepoint = codepoint,
					};
					hmput(g.atlas_lut, key, ((struct atlas_lut_info){
						.rect_index0 = !has_pixels ? -1 : arrlen(g.atlas_pack_rect_arr),
						.rect = make_rect(x0,y0,w0,h0),
					}));

					if (!has_pixels) continue;

					for (int blur_index=0; blur_index<fc->num_blur_levels; ++blur_index) {
						const struct blur_level* bl = &fc->blur_levels[blur_index];
						const struct blur_level_metrics m = get_blur_level_metrics(bl, w0, h0);
						arrput(g.atlas_pack_rect_arr, ((stbrp_rect){
							.w=m.width,
							.h=m.height,
						}));
					}
				}
			}
		}
		spec->_x_spacing = ceilf(max_advance * spec->_px_scale);
		stbtt_GetFontVMetrics(fontinfo, &spec->_ascent, &spec->_descent, &spec->_line_gap);
	}

	// pack rectangles; give them positions in the atlas image.
	int atlas_width_log2  = ATLAS_MIN_SIZE_LOG2;
	int atlas_height_log2 = ATLAS_MIN_SIZE_LOG2;
	for (;;) {
		if ((atlas_width_log2 > ATLAS_MAX_SIZE_LOG2) || (atlas_height_log2 > ATLAS_MAX_SIZE_LOG2)) {
			printf("warning: font atlas rect pack failed\n");
			return 0; // XXX TODO check: are there any leaks?
		}

		const int w = 1 << atlas_width_log2;
		const int h = 1 << atlas_height_log2;

		stbrp_init_target(&g.rect_pack_context, w, h, g.rect_pack_nodes, w);

		if (stbrp_pack_rects(&g.rect_pack_context, g.atlas_pack_rect_arr, arrlen(g.atlas_pack_rect_arr))) {
			break;
		}

		// packing failed: try again with a larger atlas; prefer wide over high
		if (atlas_width_log2 <= atlas_height_log2) {
			++atlas_width_log2;
		} else {
			++atlas_height_log2;
		}
	}

	const int atlas_width = 1 << atlas_width_log2;
	const int atlas_height = 1 << atlas_height_log2;
	uint8_t* atlas_bitmap = calloc(atlas_width*atlas_height,1);

	// render main glyphs/graphics
	int gi=0;
	for (int fsi=0; fsi<fc->num_font_specs; ++fsi) {
		struct font_spec* spec = &fc->font_specs[fsi];
		struct font* font = get_font(spec->font_index);
		stbtt_fontinfo* fontinfo = &font->fontinfo;
		for (int ci=0; ci<num_codepoint_ranges; ++ci) {
			const int cp0 = arrchkget(fc->codepoint_range_pair_arr, 2*ci);
			const int cp1 = arrchkget(fc->codepoint_range_pair_arr, 1+2*ci);
			for (int codepoint=cp0; codepoint<=cp1; ++codepoint) {
				const int ny = spec->uses_y_stretch ? fc->num_y_stretch_levels : 1;
				const int glyph_index = arrchkget(glyph_index_arr, gi);
				++gi;
				if (glyph_index == 0) continue;

				for (int ysi=0; ysi<ny; ++ysi) {
					const float y_scale = spec->uses_y_stretch ? font_config_get_y_stretch_level_scale(fc, ysi) : 1.0f;
					struct atlas_lut_key key = {
						.font_spec_index = fsi,
						.y_stretch_index = ysi,
						.codepoint = codepoint,
					};
					const struct atlas_lut_info info = hmget(g.atlas_lut, key);
					if (info.rect_index0 == -1) continue;
					assert(info.rect_index0 >= 0);
					const stbrp_rect r = arrchkget(g.atlas_pack_rect_arr, info.rect_index0);
					uint8_t* p = &atlas_bitmap[r.x+(r.y << atlas_width_log2)];
					const int stride = atlas_width;
					if (codepoint < SPECIAL_CODEPOINT0) {
						assert(glyph_index >= 0);
						stbtt_MakeGlyphBitmap(
							fontinfo,
							p,
							r.w, r.h, // XXX these have margins? should I subtract them?
							stride,
							spec->_px_scale, y_scale*spec->_px_scale,
							glyph_index);
					} else {
						assert((SPECIAL_CODEPOINT0 < codepoint) && (codepoint < SPECIAL_CODEPOINT1));
						const int w = r.w-1;
						const int h = r.h-1;
						switch (codepoint) {
						case SPECIAL_CODEPOINT_MISSING_CHARACTER: {
							const int thickness = 1;
							for (int y=0; y<h; ++y) {
								uint8_t* next_p = p+stride;
								for (int x=0; x<w; ++x) {
									const int is_border =
										   x<thickness
										|| y<thickness
										|| x>=(w-thickness)
										|| y>=(h-thickness)
										;
									*(p++) = is_border ? 255 : 0;
								}
								p = next_p;
							}
						}	break;
						case SPECIAL_CODEPOINT_BLOCK:
						case SPECIAL_CODEPOINT_CARET: {
							for (int y=0; y<h; ++y) {
								uint8_t* next_p = p+stride;
								for (int x=0; x<w; ++x) {
									*(p++) = 255;
								}
								p = next_p;
							}
						}	break;
						default: assert(!"unhandled case");
						}
					}
				}
			}
		}
	}

	static struct resize* resize_arr = NULL;
	arrreset(resize_arr);

	// prepare resizes
	for (int fsi=0; fsi<fc->num_font_specs; ++fsi) {
		struct font_spec* spec = &fc->font_specs[fsi];
		for (int ci=0; ci<num_codepoint_ranges; ++ci) {
			const int cp0 = arrchkget(fc->codepoint_range_pair_arr, 2*ci);
			const int cp1 = arrchkget(fc->codepoint_range_pair_arr, 1+2*ci);
			for (int codepoint=cp0; codepoint<=cp1; ++codepoint) {
				struct atlas_lut_key key0 = {
					.font_spec_index = fsi,
					.y_stretch_index = 0,
					.codepoint = codepoint,
				};
				const struct atlas_lut_info info0 = hmget(g.atlas_lut, key0);
				if (info0.rect_index0 == -1) continue;
				if (info0.rect.w==0 || info0.rect.h==0) continue;

				const int ny = spec->uses_y_stretch ? fc->num_y_stretch_levels : 1;
				for (int ysi=0; ysi<ny; ++ysi) {
					struct atlas_lut_key key = {
						.font_spec_index = fsi,
						.y_stretch_index = ysi,
						.codepoint = codepoint,
					};
					const struct atlas_lut_info info = ysi==0 ? info0 : hmget(g.atlas_lut, key);
					assert(info.rect_index0 >= 0);
					const stbrp_rect r0 = arrchkget(g.atlas_pack_rect_arr, info.rect_index0);

					const int src_w=info.rect.w;
					const int src_h=info.rect.h;
					assert(src_w>0 && src_h>0);
					const int src_x=r0.x;
					const int src_y=r0.y;

					for (int blur_index=1; blur_index<fc->num_blur_levels; ++blur_index) {
						const struct blur_level* bl = &fc->blur_levels[blur_index];
						const stbrp_rect rn = arrchkget(g.atlas_pack_rect_arr, info.rect_index0+blur_index);

						const struct blur_level_metrics m = get_blur_level_metrics(bl, src_w, src_h);

						const int dst_w=m.inner_width;
						const int dst_h=m.inner_height;
						const int dst_x=rn.x + m.blurpx;
						const int dst_y=rn.y + m.blurpx;

						arrput(resize_arr, ((struct resize){
							.src = make_rect(src_x, src_y, src_w, src_h),
							.dst = make_rect(dst_x, dst_y, dst_w, dst_h),
						}));
					}
				}
			}
		}
	}

	// execute resizes
	const int num_resizes = arrlen(resize_arr);
	qsort(resize_arr, num_resizes, sizeof(*resize_arr), resize_compar);
	STBIR_RESIZE re={0};
	int num_samplers=0;
	for (int i=0; i<num_resizes; ++i) {
		struct resize rz = arrchkget(resize_arr, i);
		if (i==0 || resize_dim_compar(arrchkptr(resize_arr, i-1), &rz) != 0) {
			assert(rz.src.w>0 && rz.src.h>0 && rz.dst.w>0 && rz.dst.h>0);
			stbir_free_samplers(&re); // safe when zero-initialized
			stbir_resize_init(
				&re,
				NULL, rz.src.w, rz.src.h, -1,
				NULL, rz.dst.w, rz.dst.h, -1,
				STBIR_1CHANNEL, STBIR_TYPE_UINT8);
			stbir_set_edgemodes(&re, STBIR_EDGE_ZERO, STBIR_EDGE_ZERO);
			stbir_build_samplers(&re);
			++num_samplers;
		}

		assert((0 <= rz.src.x) && (rz.src.x < atlas_width));
		assert((0 <= rz.src.y) && (rz.src.y < atlas_height));
		assert((0 <= (rz.src.x+rz.src.w)) && ((rz.src.x+rz.src.w) <= atlas_width));
		assert((0 <= (rz.src.y+rz.src.h)) && ((rz.src.y+rz.src.h) <= atlas_height));

		stbir_set_buffer_ptrs(
			&re,
			atlas_bitmap + rz.src.x + rz.src.y*atlas_width, atlas_width,
			atlas_bitmap + rz.dst.x + rz.dst.y*atlas_width, atlas_width);
		stbir_resize_extended(&re);
	}
	stbir_free_samplers(&re);

	// do gaussian blurs
	static float* coefficient_arr;
	for (int blur_index=1; blur_index<fc->num_blur_levels; ++blur_index) {
		const struct blur_level* bl = &fc->blur_levels[blur_index];
		const int blurpx = get_blurpx(bl);
		const int kernel_size = 1+2*blurpx;
		arrsetlen(coefficient_arr, kernel_size);
		for (int i=0; i<=blurpx; ++i) {
			const double x = ((double)(-blurpx+i)/(double)blurpx)*3.0;
			//printf("y[%d]=%f\n", i, x);
			const float y = (float)gaussian(bl->variance, x) * bl->kernel_scalar;
			assert((0 <= i) && (i < kernel_size));
			coefficient_arr[arrchk(coefficient_arr,i)] = y;
			assert((0 <= (kernel_size-i-1)) && ((kernel_size-i-1) < kernel_size));
			coefficient_arr[arrchk(coefficient_arr,kernel_size-i-1)] = y;
		}

		#if 0
		float z=0;
		for (int i=0; i<kernel_size; ++i) {
			printf("x[%d]=%f\n", i, kernel_arr[i]);
			z += kernel_arr[i];
		}
		printf("kernel sum: %f\n", z);
		printf("predicted: %f\n", bl->variance*sqrtf(6.283185307179586f));
		#endif

		struct sep2dconv_kernel kernel = {
			.radius = blurpx,
			.coefficients = coefficient_arr,
		};

		for (int fsi=0; fsi<fc->num_font_specs; ++fsi) {
			struct font_spec* spec = &fc->font_specs[fsi];
			for (int ci=0; ci<num_codepoint_ranges; ++ci) {
				const int cp0 = arrchkget(fc->codepoint_range_pair_arr, 2*ci);
				const int cp1 = arrchkget(fc->codepoint_range_pair_arr, 1+2*ci);
				for (int codepoint=cp0; codepoint<=cp1; ++codepoint) {
					struct atlas_lut_key key0 = {
						.font_spec_index = fsi,
						.y_stretch_index = 0,
						.codepoint = codepoint,
					};
					const struct atlas_lut_info info0 = hmget(g.atlas_lut, key0);
					if (info0.rect_index0 == -1) continue;
					if (info0.rect.w==0 || info0.rect.h==0) continue;
					const int ny = spec->uses_y_stretch ? fc->num_y_stretch_levels : 1;
					for (int ysi=0; ysi<ny; ++ysi) {
						struct atlas_lut_key key = {
							.font_spec_index = fsi,
							.y_stretch_index = ysi,
							.codepoint = codepoint,
						};
						const struct atlas_lut_info info = hmget(g.atlas_lut, key);
						const stbrp_rect rn = arrchkget(g.atlas_pack_rect_arr, info.rect_index0+blur_index);

						sep2dconv_execute(
							&kernel,
							atlas_bitmap + rn.x + rn.y * atlas_width,
							rn.w-1,
							rn.h-1,
							atlas_width);
					}
				}
			}
		}
	}

	const int64_t dt = get_nanoseconds()-t0;

	printf("atlas %d×%d, %zd rects, %d samplers, %d resizes, built in %.5fs\n",
		atlas_width, atlas_height,
		arrlen(g.atlas_pack_rect_arr),
		num_samplers, num_resizes,
		(double)dt*1e-9);

	#if 0
	stbi_write_png("atlas.png", atlas_width, atlas_height, 1, atlas_bitmap, atlas_width);
	#endif

	if (g.atlas_texture_id >= 0) destroy_texture(g.atlas_texture_id);
	g.atlas_texture_id = create_texture(TT_LUMEN8 | TT_SMOOTH | TT_STATIC, atlas_width, atlas_height);
	update_texture(g.atlas_texture_id, 0, atlas_width, atlas_height, atlas_bitmap);
	free(atlas_bitmap);

	return 1;
}

static const struct blur_level default_blur_levels[] = {
	{
		.scale = 1.0f,
		.post_scalar = 1.0f,
	},
	{
		.scale = 0.6f,
		.radius = 4.0f,
		.variance = 1.0f,
		.kernel_scalar = 1.0f,
		.post_scalar = 0.7f,
	},
	{
		.scale = 0.4f,
		.radius = 10.0f,
		.variance = 1.0f,
		.kernel_scalar = 1.0f,
		.post_scalar = 0.5f,
	},
	{
		.scale = 0.2f,
		.radius = 32.0f,
		.variance = 1.0f,
		.kernel_scalar = 1.0f,
		.post_scalar = 0.3f,
	},
};

static const float default_y_scalar_min = 0.6f;
static const float default_y_scalar_max = 1.4f;

static struct font_spec default_font_specs[] = {
	{
		.font_index=0,
		.px=40,
		.uses_y_stretch=1,
	},
};

static void fc_add_codepoint_range(struct font_config* fc, int first, int last)
{
	arrput(fc->codepoint_range_pair_arr, first);
	arrput(fc->codepoint_range_pair_arr, last);
}

static void fc_add_latin1(struct font_config* fc)
{
	fc_add_codepoint_range(fc, 0x20, 0xff);
}

static void fc_add_special(struct font_config* fc)
{
	fc_add_codepoint_range(fc, (SPECIAL_CODEPOINT0+1), (SPECIAL_CODEPOINT1-1));
}

void gui_init(void)
{
	fonts_init();

	g.atlas_texture_id = -1;

	// XXX a lot of this code is bad "getting started code" that shouldn't
	// survive for too long
	//assert((get_num_documents() > 0) && "the following code is probably not the best if/when this changes");
	const int focus_id = make_focus_id();
	arrput(g.pane_arr, ((struct pane){
		.type = CODE,
		.u0=0, .v0=0,
		.u1=1, .v1=1,
		.code = {
			.focus_id = focus_id,
		},
	}));
	focus(focus_id);

	struct font_config* fc = &g.font_config;
	arrreset(fc->codepoint_range_pair_arr);
	fc_add_latin1(fc);
	fc_add_special(fc);
	fc->y_scalar_min = default_y_scalar_min;
	fc->y_scalar_max = default_y_scalar_max;
	fc->num_y_stretch_levels = 4;
	fc->num_blur_levels = ARRAY_LENGTH(default_blur_levels);
	fc->blur_levels = default_blur_levels;
	fc->num_font_specs = ARRAY_LENGTH(default_font_specs);
	fc->font_specs = default_font_specs;

	gui_setup_gpu_resources();
}

// XXX gui_setup_gpu_resources() is intended to be called once during init, or
// if the GPU context is lost, however this is mysterious subject fraught with
// subtleties and krakens. And therefore also not well-tested. Notes:
//  - Some GL's seem to have a "create context robustness": the SDL code has a
//    lot of this. Does that mean you won't lose context?
//  - Maybe Vulkan has "robustness" features too? I'm not too familiar with it,
//    just git-grepping the SDL source code here...
//  - WebGL can fire a "webglcontextlost" event on the <canvas> element. In
//    practice I've only seen this in Chrome which has a hardcoded context
//    limit (16). Something like a render stall (taking too long) might also
//    cause it? The event contains no "reason for loss of context".
//  - SDL_Renderer can fire SDL_RenderEvents: SDL_EVENT_RENDER_DEVICE_RESET
//    and SDL_EVENT_RENDER_DEVICE_LOST (also SDL_EVENT_RENDER_TARGETS_RESET?)
void gui_setup_gpu_resources(void)
{
	build_atlas();
}

void gui_on_key(int keycode)
{
	arrput(g.key_buffer_arr, keycode);
}

void gui_on_text(const char* text)
{
	const size_t n = strlen(text);
	assert((arrlen(g.text_buffer_arr) > 0) && "empty array; length should always be 1 or longer");
	assert((0 == arrpop(g.text_buffer_arr)) && "expected to pop cstr nul-terminator");
	char* p = arraddnptr(g.text_buffer_arr, n);
	memcpy(p, text, n);
	arrput(g.text_buffer_arr, 0);
	assert(arrlen(g.text_buffer_arr) == (1+strlen(g.text_buffer_arr)));
}

static void set_blend_mode(enum blend_mode blend_mode)
{
	g.render_mode.blend_mode = blend_mode;
}

static void set_scissor_rect(struct rect* r)
{
	g.render_mode.do_scissor = 1;
	g.render_mode.scissor_x = r->x;
	g.render_mode.scissor_y = r->y;
	g.render_mode.scissor_w = r->w;
	g.render_mode.scissor_h = r->h;
}

#if 0
static void set_no_scissor(void)
{
	g.render_mode.do_scissor = 0;
}
#endif

static void set_texture(int id)
{
	g.render_mode.texture_id = id;
}

static struct draw_list* continue_draw_list(int num_vertices, int num_indices)
{
	const int max_vert = (sizeof(vertex_index)==2) ? (1<<16) : 0;
	assert((max_vert>0) && "unhandled config?");
	assert(num_vertices <= max_vert);

	const int n = arrlen(g.draw_list_arr);
	if (n==0) return NULL;
	struct draw_list* list = arrchkptr(g.draw_list_arr, n-1);

	if (!render_mode_same(&g.render_mode, &list->render_mode)) return NULL;

	if (sizeof(vertex_index) == 2) {
		if ((list->mesh.num_vertices + num_vertices) > max_vert) return NULL;
		return list;
	} else {
		assert(!"FIXME unhandled sizeof(vertex_index)");
	}
	assert(!"unreachable");
}


static void alloc_mesh(int num_vertices, int num_indices, struct vertex** out_vertices, vertex_index** out_indices)
{
	struct draw_list* list = continue_draw_list(num_vertices, num_indices);
	if (list == NULL) {
		list = arraddnptr(g.draw_list_arr, 1);
		memcpy(&list->render_mode, &g.render_mode, sizeof g.render_mode);
		list->mesh._vertices_offset = arrlen(g.vertex_arr);
		list->mesh.num_vertices = 0;
		list->mesh._indices_offset = arrlen(g.vertex_index_arr);
		list->mesh.num_indices = 0;
	}
	*out_vertices = arraddnptr(g.vertex_arr, num_vertices);
	*out_indices  = arraddnptr(g.vertex_index_arr, num_indices);
	const int i0=list->mesh.num_vertices;
	for (int i=0; i<num_indices; ++i) (*out_indices)[i] = i0;
	list->mesh.num_vertices += num_vertices;
	list->mesh.num_indices += num_indices;
}

static struct vertex* alloc_mesh_quad(void)
{
	struct vertex* vertices;
	vertex_index*  indices;
	alloc_mesh(4, 6, &vertices, &indices);
	indices[0]+=0; indices[1]+=1; indices[2]+=2;
	indices[3]+=0; indices[4]+=2; indices[5]+=3;
	return vertices;
}

static void push_mesh_quad(float dx, float dy, float dw, float dh, float sx, float sy, float sw, float sh, uint32_t rgba)
{
	int tw, th;
	get_texture_dim(g.render_mode.texture_id, &tw, &th);
	const float mu = 1.0f / (float)tw;
	const float mv = 1.0f / (float)th;

	struct vertex* vertices = alloc_mesh_quad();
	vertices[0] = ((struct vertex) {
		.x=dx    , .y=dy    ,
		.u=mu*sx , .v=mv*sy ,
		.rgba=rgba,
	});
	vertices[1] = ((struct vertex) {
		.x=dx+dw      , .y=dy    ,
		.u=mu*(sx+sw) , .v=mv*sy ,
		.rgba=rgba,
	});
	vertices[2] = ((struct vertex) {
		.x=dx+dw      , .y=dy+dh      ,
		.u=mu*(sx+sw) , .v=mv*(sy+sh) ,
		.rgba=rgba,
	});
	vertices[3] = ((struct vertex) {
		.x=dx    , .y=dy+dh      ,
		.u=mu*sx , .v=mv*(sy+sh) ,
		.rgba=rgba,
	});
}

static inline int m33i(int a, int b)
{
	return b+a*3;
}

static void v3_mul_m33(float* out_v3, float* v3, const float* m33)
{
	float x0 = v3[0]*m33[m33i(0,0)] + v3[1]*m33[m33i(0,1)] + v3[2]*m33[m33i(0,2)];
	float x1 = v3[0]*m33[m33i(1,0)] + v3[1]*m33[m33i(1,1)] + v3[2]*m33[m33i(1,2)];
	float x2 = v3[0]*m33[m33i(2,0)] + v3[1]*m33[m33i(2,1)] + v3[2]*m33[m33i(2,2)];
	out_v3[0] = x0;
	out_v3[1] = x1;
	out_v3[2] = x2;
}

static void tonemap(float* in_out_color)
{
	const float ACES_input_transform[] = {
		 0.59719f ,  0.35458f ,  0.04823f ,
		 0.07600f ,  0.90834f ,  0.01566f ,
		 0.02840f ,  0.13383f ,  0.83777f ,
	};
	const float ACES_output_transform[] = {
		 1.60475f , -0.53108f , -0.07367f ,
		-0.10208f ,  1.10813f , -0.00605f ,
		-0.00327f , -0.07276f ,  1.07602f ,
	};
	v3_mul_m33(in_out_color, in_out_color, ACES_input_transform);
	for (int i=0; i<3; ++i) {
		float x = in_out_color[i];
		x = (x * (x + 0.0245786f) - 0.000090537f) / (x * (0.983729f * x + 0.4329510f) + 0.238081f);
		in_out_color[i] = x;
	}
	v3_mul_m33(in_out_color, in_out_color, ACES_output_transform);
	for (int i=0; i<3; ++i) {
		float x = in_out_color[i];
		x = fminf(1.0f, fmaxf(0.0f, x));
		in_out_color[i] = x;
	}
}

static uint32_t make_hdr_rgba(int blur_level_index, float* color)
{
	struct font_config* fc = &g.font_config;
	assert((0 <= blur_level_index) && (blur_level_index < fc->num_blur_levels));
	const struct blur_level* bl = &fc->blur_levels[blur_level_index];
	float c[3];
	for (int i=0; i<3; ++i) {
		float v = color[i] * bl->post_scalar;
		if (g.state.is_dimming) {
			// XXX this is a bit of a hack...
			float t = (float)blur_level_index / (float)(fc->num_blur_levels - 1);
			v = v*powf(t, 3.0f);
			v = tanhf(v*20.0f)/5.0f;
		}
		c[i] = v;
	}
	tonemap(c);
	uint32_t cu = 0;
	for (int i=0; i<3; ++i) {
		cu += (uint8_t)fminf(255.0f, fmaxf(0.0f, floorf(c[i]*256.0f))) << (i*8);
	}
	return 0xff000000 | cu;
}

#if 0
static void get_current_line_metrics(float* out_ascent, float* out_descent, float* out_line_gap)
{
	struct font_spec* spec = get_current_font_spec();
	const float scale = spec->_px_scale * get_current_y_stretch_scale();
	if (out_ascent) *out_ascent = (float)spec->_ascent * scale;
	if (out_descent) *out_descent = (float)spec->_descent * scale;
	if (out_line_gap) *out_line_gap = (float)spec->_line_gap * scale;
}
#endif

static float get_y_advance(void)
{
	struct font_spec* spec = get_current_font_spec();
	const float scale = spec->_px_scale * get_current_y_stretch_scale();
	float ascent   = (float)spec->_ascent * scale;
	float descent  = (float)spec->_descent * scale;
	const float m = 0.9f; // XXX less line spacing; formalise this?
	return (ascent - descent) * m;
}

static void put_char(int codepoint)
{
	struct font_config* fc = &g.font_config;
	if (codepoint < ' ') {
		// XXX I might want a mode that handles '\n' here? The problem is that
		// if font parameters (size, y stretch, etc) have a tendency to change
		// in the middle of a line, then we need look-ahead because line
		// spacing might be changed by the characters ahead. So for now we
		// don't handle '\n' here.
		return;
	}
	struct atlas_lut_key key = {
		.font_spec_index = g.state.current_font_spec_index,
		.y_stretch_index = g.state.current_y_stretch_index,
		.codepoint = codepoint,
	};
	const struct atlas_lut_info info = hmget(g.atlas_lut, key);

	const int has_pixels = (info.rect_index0 >= 0);
	if (has_pixels) {
		const float w = info.rect.w;
		const float h = info.rect.h;

		for (int i=0; i<fc->num_blur_levels; ++i) {
			const struct blur_level* b = &fc->blur_levels[i];
			const stbrp_rect rect = arrchkget(g.atlas_pack_rect_arr, i+info.rect_index0);
			const float r = b->radius;
			push_mesh_quad(
				g.state.cursor_x + ((float)info.rect.x) - r, // XXX should have `- ascent` too, but looks wrong?
				g.state.cursor_y + ((float)info.rect.y) - r,
				w+r*2   , h+r*2 ,
				rect.x   , rect.y ,
				rect.w-1 , rect.h-1,
				make_hdr_rgba(i, g.state.current_color)
			);
		}
	}

	struct font_spec* spec = &fc->font_specs[g.state.current_font_spec_index];
	g.state.cursor_x += spec->_x_spacing;
}

static void set_y_stretch_index(int index)
{
	struct font_config* fc = &g.font_config;
	assert((0 <= index) && (index < fc->num_y_stretch_levels));
	g.state.current_y_stretch_index = index;
}

static void set_colorv(float color[3])
{
	for (int i=0; i<3; ++i) g.state.current_color[i] = color[i];
}

static void set_color3f(float red, float green, float blue)
{
	set_colorv((float[]){red,green,blue});
}

static inline void splash4_explode(int splash4, int* out_red, int* out_green, int* out_blue, int* out_shake)
{
	const int red   = (splash4 / 1000) % 10;
	const int green = (splash4 / 100 ) % 10;
	const int blue  = (splash4 / 10  ) % 10;
	const int shake = (splash4       ) % 10;
	if (out_red)   *out_red   = red;
	if (out_green) *out_green = green;
	if (out_blue)  *out_blue  = blue;
	if (out_shake) *out_shake = shake;
}

static inline int splash_constrain(int c)
{
	if (c<0) return 0;
	if (c>9) return 9;
	return c;
}

static inline int splash4_implode(int red, int green, int blue, int shake)
{
	red   = splash_constrain(red);
	green = splash_constrain(green);
	blue  = splash_constrain(blue);
	shake = splash_constrain(shake);
	return (red*1000) + (green*100) + (blue*10) + shake;
}

static inline int splash4_comp_delta(int splash4, int comp, int delta)
{
	int red,green,blue,shake;
	splash4_explode(splash4, &red, &green, &blue, &shake);
	switch (comp) {
	case 0: red   += delta; break;
	case 1: green += delta; break;
	case 2: blue  += delta; break;
	case 3: shake += delta; break;
	}
	return splash4_implode(red, green, blue, shake);
}

static inline int splash4_comp_set(int splash4, int comp, int value)
{
	int red,green,blue,shake;
	splash4_explode(splash4, &red, &green, &blue, &shake);
	switch (comp) {
	case 0: red   = value; break;
	case 1: green = value; break;
	case 2: blue  = value; break;
	case 3: shake = value; break;
	}
	return splash4_implode(red, green, blue, shake);
}

static float splashc2f(int c)
{
	const float f0 = 0.25f;
	const float f1 = 2.5f;
	const float t = (float)c * (1.0f / 9.0f);
	const float tx = powf(t, 1.5f);
	return fminf(f1, fmaxf(f0, f0 + (f1-f0) * tx));
}

static void set_color_splash4_mul(uint16_t splash4, float mul)
{
	int red,green,blue;
	splash4_explode(splash4, &red, &green, &blue, NULL);
	set_color3f(mul*splashc2f(red), mul*splashc2f(green), mul*splashc2f(blue));
}

static void set_color_splash4(uint16_t splash4)
{
	set_color_splash4_mul(splash4, 1.0f);
}

static void update_fps(void)
{
	const int64_t t  = get_nanoseconds();
	const int64_t dt = t - g.last_frame_time;
	g.fps = 1.0f / ((float)dt * 1e-9f);
	g.last_frame_time = t;
	#if 0
	printf("%f\n", g.fps);
	#endif
}

static struct rect get_pane_rect(struct pane* pane)
{
	const int base_width = g.current_window->true_width;
	const int base_height = g.current_window->true_height;
	const int x0 = (int)floorf(pane->u0 * (float)base_width);
	const int y0 = (int)floorf(pane->v0 * (float)base_height);
	const int x1 = (int)ceilf(pane->u1 * (float)base_width);
	const int y1 = (int)ceilf(pane->v1 * (float)base_height);
	const int w = x1-x0;
	const int h = y1-y0;
	return make_rect(x0,y0,w,h);
}

#define HAS_FOCUS    (1<<1)
#define WAS_CLICKED  (1<<2)

static int keyboard_input_area(struct rect* r, int focus_id)
{
	int flags = 0;
	if (focus_id == g.current_focus_id) flags |= HAS_FOCUS;
	// TODO set flags |= WAS_CLICKED if it was
	return flags;
}

void gui_begin_frame(void)
{
	//peer_set_artificial_mim_latency(.5, .1);
	peer_tick();
	update_fps();
	arrreset(g.key_buffer_arr);
	arrreset(g.text_buffer_arr);
	arrput(g.text_buffer_arr, 0); // terminate cstr
}

static void cpick_on(struct pane* p, int on)
{
	assert(p->type == CODE);
	p->code.cpick_on = on;
}

static void cpick_select(struct pane* p, int d)
{
	assert(p->type == CODE);
	int c = p->code.splash4_comp;
	c += d;
	#if 0
	// wrap
	while (c <  0) c += 4;
	while (c >= 4) c -= 4;
	#else
	// clamp
	if (c<0) c=0;
	if (c>3) c=3;
	#endif
	assert((0 <= c) && (c < 4));
	p->code.splash4_comp = c;
}

static void cpick_add_comp(struct pane* p, int comp, int delta)
{
	assert(p->type == CODE);
	p->code.splash4_cache = splash4_comp_delta(p->code.splash4_cache, comp, delta);
	mimf("%d~", p->code.splash4_cache);
}

static void cpick_set_comp(struct pane* p, int comp, int value)
{
	assert(p->type == CODE);
	p->code.splash4_cache = splash4_comp_set(p->code.splash4_cache, comp, value);
	mimf("%d~", p->code.splash4_cache);
}

static void cpick_add(struct pane* p, int delta)
{
	assert(p->type == CODE);
	cpick_add_comp(p, p->code.splash4_comp, delta);
}

static void cpick_set(struct pane* p, int value)
{
	assert(p->type == CODE);
	cpick_set_comp(p, p->code.splash4_comp, value);
}

static void cpick_add_rgb(struct pane* p, int delta)
{
	assert(p->type == CODE);
	cpick_add_comp(p, 0, delta);
	cpick_add_comp(p, 1, delta);
	cpick_add_comp(p, 2, delta);
}


static void handle_editor_input(struct pane* pane)
{
	assert(pane->type == CODE);
	peer_begin_mim(pane->code.session_id);
	int last_mod = 0;
	for (int i=0; i<arrlen(g.key_buffer_arr); ++i) {
		const int key = arrchkget(g.key_buffer_arr, i);
		const int down = get_key_down(key);
		const int mod = get_key_mod(key);
		last_mod = mod;
		const int code = get_key_code(key);
		if (down && mod==0) {
			switch (code) {
			// XXX
			case KEY_ARROW_LEFT  : mimf("0Mh"); break;
			case KEY_ARROW_RIGHT : mimf("0Ml"); break;
			case KEY_ARROW_UP    : mimf("0Mk"); break;
			case KEY_ARROW_DOWN  : mimf("0Mj"); break;
			case KEY_ENTER       : mimi(0,"\n"); break;
			case KEY_BACKSPACE   : mimf("0X"); break;
			case KEY_DELETE      : mimf("0x"); break;
			//case KEY_ESCAPE       mimf("\033"); break;
			}
		}

		if (down && mod==MOD_SHIFT) {
			switch (code) {
			case KEY_ARROW_LEFT  : mimf("0Sh"); break;
			case KEY_ARROW_RIGHT : mimf("0Sl"); break;
			case KEY_ARROW_UP    : mimf("0Sk"); break;
			case KEY_ARROW_DOWN  : mimf("0Sj"); break;
			}
		}

		const int is_cpick_mod = (mod == (MOD_CONTROL | MOD_ALT));
		cpick_on(pane, is_cpick_mod);
		if (down && is_cpick_mod) {
			switch (code) {
			case KEY_ARROW_LEFT : cpick_select(pane  , -1); break;
			case KEY_ARROW_RIGHT: cpick_select(pane  ,  1); break;
			case KEY_ARROW_UP   : cpick_add(pane     ,  1); break;
			case KEY_ARROW_DOWN : cpick_add(pane     , -1); break;
			case KEY_PAGE_UP    : cpick_add_rgb(pane ,  1); break;
			case KEY_PAGE_DOWN  : cpick_add_rgb(pane , -1); break;
			}
			if (('0' <= code) && (code <= '9')) {
				cpick_set(pane, code-'0');
			}
		}

		if (down && mod==MOD_CONTROL) {
			if (code==KEY_ENTER) mimf("0!"); // commit
			if (code==' ')       mimf("0/"); // cancel
			if (code=='P')       mimf("0P"); // paint
		}
	}

	if (!(last_mod & MOD_CONTROL)) {
		const int num_chars = utf8_strlen(g.text_buffer_arr);
		if (num_chars > 0) {
			const int num_bytes = arrlen(g.text_buffer_arr) - 1;
			mimf("0,%di", num_bytes);
			for (int i=0; i<num_bytes; ++i) mim8(g.text_buffer_arr[i]);
		}
	}

	peer_end_mim();
}

static void save(void)
{
	g.save_state = g.state;
}

static void restore(void)
{
	g.state = g.save_state;
}

static inline int has_light(float color[3])
{
	return color[0]>0 || color[1]>0 || color[2]>0;
}

static float randf(float v0, float v1)
{
	const float f = (float)rand() / (float)RAND_MAX;
	return v0 + f*(v1-v0);
}

static void draw_code_pane(struct pane* pane)
{
	assert(pane->type == CODE);

	struct mim_state* ms  = &g.ms_copy;
	if (pane->code.session_id == 0) {
		pane->code.session_id = 1; // XXX no allocate?
		int do_setdoc=0;
		int do_addcar=0;
		if (get_copy_of_state(pane->code.session_id, ms)) {
			if ((ms->book_id == 0) || (ms->doc_id == 0)) do_setdoc=1;
			if (arrlen(ms->caret_arr) == 0)              do_addcar=1;
		} else {
			do_setdoc=1;
			do_addcar=1;
		}
		if (do_addcar || do_setdoc) {
			peer_begin_mim(pane->code.session_id);
			if (do_setdoc) mimex("setdoc 1 50");
			if (do_addcar) mimf("0,1,1c");
			peer_end_mim();
		}
	}

	g.state.is_dimming = pane->code.cpick_on;

	struct rect pr = get_pane_rect(pane);

	const int flags = keyboard_input_area(&pr, pane->code.focus_id);
	if (flags & WAS_CLICKED) {
		focus(pane->code.focus_id);
	}
	if (flags & HAS_FOCUS) {
		handle_editor_input(pane);
	}

	set_blend_mode(ADDITIVE);
	set_texture(g.atlas_texture_id);

	const int x0 = pr.x+30;
	g.state.cursor_x0 = g.state.cursor_x = x0;
	g.state.cursor_y = pr.y+20;

	set_y_stretch_index(0);
	//set_color3f(.7, 2.7, .7);
	set_color3f(.9,2.6,.9);

	struct document*  doc = &g.doc_copy;
	if (!get_copy_of_state_and_doc(pane->code.session_id, ms, doc)) {
		return;
	}
	pane->code.splash4_cache = ms->splash4;

	const int num_carets = arrlen(ms->caret_arr);

	static int* caret_coord_arr;
	arrreset(caret_coord_arr);

	struct doc_iterator it = doc_iterator(doc);
	while (doc_iterator_next(&it)) {
		int draw_caret = 0;

		float bg_color[3] = {0,0,0};

		int min_y_dist = -1;
		for (int i=0; i<num_carets; ++i) {
			struct caret* c = arrchkptr(ms->caret_arr, i);
			struct location* loc0 = &c->caret_loc;
			struct location* loc1 = &c->anchor_loc;
			location_sort2(&loc0, &loc1);
			struct location* crloc = &c->caret_loc;
			const int d0 = location_line_distance(loc0 , &it.location);
			const int d1 = location_line_distance(loc1 , &it.location);
			if (min_y_dist < 0 || d0 < min_y_dist) min_y_dist = d0;
			if (min_y_dist < 0 || d1 < min_y_dist) min_y_dist = d1;
			const int cmp0  = location_compare(&it.location, loc0);
			const int cmp1  = location_compare(&it.location, loc1);
			const int cmp1r = location_compare(&it.location, crloc);
			const int same  = (0 == location_compare(loc0, loc1));
			if (cmp1r==0) draw_caret = 1;
			if (!same && cmp0>=0 && cmp1<0) {
				bg_color[1] += 0.1f;
				bg_color[2] += 0.3f;
			}
		}

		{
			int ylvl;
			if (min_y_dist < 0) {
				ylvl = 0;
			} else {
				ylvl = g.font_config.num_y_stretch_levels-1-min_y_dist;
				if (ylvl < 0) ylvl = 0;
			}
			set_y_stretch_index(ylvl);
		}

		const float y_height = get_y_advance();

		if (draw_caret) {
			save();
			set_color_splash4_mul(ms->splash4, 1.5f);
			g.state.cursor_y += (int)y_height;
			arrput(caret_coord_arr, g.state.cursor_x);
			arrput(caret_coord_arr, g.state.cursor_y);
			put_char(SPECIAL_CODEPOINT_CARET);
			g.state.cursor_y -= (int)y_height;
			restore();
		}

		struct docchar* fc = it.docchar;
		const unsigned cp = fc != NULL ? fc->colorchar.codepoint : 0;
		int splash4 = 0;

		if (fc != NULL) {
			if (fc->flags & FC_IS_INSERT) bg_color[1] += 0.2f;
			if (fc->flags & FC_IS_DELETE) bg_color[0] += 0.3f;
			splash4 = fc->colorchar.splash4;
		}
		const int shake = splash4 % 10;

		if (has_light(bg_color) && cp >= ' ') {
			save();
			set_colorv(bg_color);
			g.state.cursor_y += (int)y_height;
			put_char(SPECIAL_CODEPOINT_BLOCK);
			g.state.cursor_y -= (int)y_height;
			restore();
		}

		if (cp == 0) continue;

		if (cp == '\n') {
			g.state.cursor_x = g.state.cursor_x0;
			g.state.cursor_y += y_height;
		}

		set_color_splash4(splash4);

		// XXX ostensibly I also need to subtract "ascent" from y? but it looks
		// wrong... text formatting is hard!
		if (shake > 0) {
			// FIXME quick'n'dirty shake viz that ought to be improved:
			//  - render multiple chars to make "motion blur", especially
			//    for higher shake levels?
			//  - shake radius should probably not be a quad like it is
			//    here, and should also be proportional to text size
			//    (currently it's 0-9 pixels)
			//  - kinda want to do rotational transforms too (currently not
			//    supported by push_mesh_quad())
			//  - maybe even spark particles at high shake levels? :D
			save();
			const float m = (float)shake;
			g.state.cursor_x += randf(-m,m);
			g.state.cursor_y += randf(-m,m);
			const float x0 = g.state.cursor_x;
			const float y0 = g.state.cursor_y;
			g.state.cursor_y += (int)y_height;
			put_char(cp);
			g.state.cursor_y -= (int)y_height;
			const float dx = g.state.cursor_x - x0;
			const float dy = g.state.cursor_y - y0;
			restore();
			g.state.cursor_x += dx;
			g.state.cursor_y += dy;
		} else {
			g.state.cursor_y += (int)y_height;
			put_char(cp);
			g.state.cursor_y -= (int)y_height;
		}
	}

	#if 0
	g.cursor_x = g.cursor_x0;
	g.cursor_y += 40;
	set_color3f(.8,.8,.8);
	for (int i=0;i<4;++i) put_char(SPECIAL_CODEPOINT_MISSING_CHARACTER);
	set_color3f(2,2,2);
	for (int i=0;i<4;++i) put_char(SPECIAL_CODEPOINT_MISSING_CHARACTER);
	set_color3f(9,9,9);
	for (int i=0;i<4;++i) put_char(SPECIAL_CODEPOINT_MISSING_CHARACTER);
	set_color3f(1,1,1);
	for (int i=0;i<2;++i) put_char(' ');
	put_char('W'); put_char(':');
	for (int i=0;i<4;++i) put_char(SPECIAL_CODEPOINT_CARET);

	for (int j=0;j<5;j++) {
		g.cursor_x = g.cursor_x0;
		g.cursor_y += 40;
		set_color3f(.9,j,.9);
		for (int i=0;i<4;++i) put_char(SPECIAL_CODEPOINT_BLOCK);
	}
	#endif

	g.state.is_dimming = 0;

	if (pane->code.cpick_on) {
		assert(arrlen(caret_coord_arr) == (2*num_carets));
		for (int i=0; i<num_carets; ++i) {
			const int cx = caret_coord_arr[i*2];
			const int cy = caret_coord_arr[i*2+1];

			const int x0 = cx;
			const int y0 = cy;
			g.state.cursor_x = x0;
			g.state.cursor_y = y0;
			set_y_stretch_index(2);
			set_color3f(1,1,1);
			int red,green,blue,shake;
			splash4_explode(pane->code.splash4_cache,&red,&green,&blue,&shake);
			set_color_splash4(splash4_implode(red,0,0,0));
			put_char('0'+red);
			set_color_splash4(splash4_implode(0,green,0,0));
			put_char('0'+green);
			set_color_splash4(splash4_implode(0,0,blue,0));
			put_char('0'+blue);
			set_color_splash4(splash4_implode(3,3,3,shake));
			put_char('0'+shake);
			g.state.cursor_x = x0;
			g.state.cursor_y = y0+30;
			set_color_splash4(splash4_implode(red,green,blue,shake));
			const int comp = pane->code.splash4_comp;
			for (int i=0; i<4; ++i) {
				put_char(i==comp?'^':' ');
			}
		}
	}

}

static void gui_draw1(void)
{
	for (int i=0; i<arrlen(g.pane_arr); ++i) {
		struct pane* pane = arrchkptr(g.pane_arr, i);

		struct rect pr = get_pane_rect(pane);
		if (pr.w<=0 || pr.h<=0) continue;

		// TODO render pane underlay? (e.g. something that darkens the
		// background or changes color)

		set_scissor_rect(&pr);

		switch (pane->type) {
		case CODE:
			draw_code_pane(pane);
			break;
		default: assert(!"unhandled pane type");
		}

		// TODO render pane overlay? (e.g. pane border lines)
	}
}

void gui_draw(struct window* window)
{
	assert(window->state == WINDOW_IS_OPEN);
	assert((g.current_window == NULL) && "wasn't properly cleaned up previously?");
	g.current_window = window;

	// reset draw lists
	arrreset(g.draw_list_arr);
	arrreset(g.vertex_arr);
	arrreset(g.vertex_index_arr);

	memset(&g.render_mode, 0, sizeof g.render_mode);

	// actually draw
	gui_draw1();

	// replace the "_"-prefixed variables in struct draw_list inner unions
	// with corresponding pointer values in the same unions. we can't easily
	// set the pointer values as we go because they must point at offsets in a
	// larger dynamic array, and so a realloc() can potentially move the
	// pointer and corrupt any number of draw_lists. this "fixup" happens last,
	// when it it's safe to point into the dynamic array which won't be written
	// to while it's being rendered.
	const int n_lists = arrlen(g.draw_list_arr);
	for (int i=0; i<n_lists; ++i) {
		struct draw_list* list = arrchkptr(g.draw_list_arr, i);
		switch (list->render_mode.type) {
		case MESH_TRIANGLES: {
			list->mesh.vertices = arrchkptr(g.vertex_arr       , list->mesh._vertices_offset);
			list->mesh.indices  = arrchkptr(g.vertex_index_arr , list->mesh._indices_offset );
		}	break;
		default: assert(!"unhandled draw list type");
		}
	}

	g.current_window = NULL;
}

int gui_get_num_draw_lists(void)
{
	return arrlen(g.draw_list_arr);
}

struct draw_list* gui_get_draw_list(int index)
{
	return arrchkptr(g.draw_list_arr, index);
}

void gui_set_dragging(struct window* w, int is_dragging)
{
	if (g.is_dragging != is_dragging) {
		// TODO! fun idea: anticipate the drop with love. you're about to drop
		// the best .wav ever! (so particle effects?)
		printf("dragging %d => %d\n", g.is_dragging, is_dragging);
		g.is_dragging = is_dragging;
	}
}

void gui_drop_file(const char* name, size_t num_bytes, uint8_t* bytes)
{
	printf("TODO file drop name=[%s] num_bytes=%zd data:", name, num_bytes);
	for (size_t i=0; (i<10) && (i<num_bytes); ++i) {
		printf(" %.2x", bytes[i]);
	}
	printf("\n");
	// TODO!?
	//  - should probably always hash file
	//  - NOTE: bytes might be freed after we return?
}

// TODO

// - secondary atlas for dynamic software rendered graphics (e.g. inline curves
//   in document); all entries have a resource id and dimensions (the same
//   resource id can be rendered at different scales), plus blur variants for
//   HDR. to render, do a "gather pass" to find the required resources and
//   dimensions; then do a lookup in the atlas to see which are missing (hash
//   table similar to primary atlas?); if any, pack and render them; if atlas
//   overflows, clear it and try again; if atlas still overflows, make the
//   atlas bigger and try again. consider blitting graphics from the old atlas
//   when rebuilding it. also, resource are proably "immutable", so resource
//   edits mean new resource ids?

