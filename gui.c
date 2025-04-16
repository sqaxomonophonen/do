#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "util.h"
#include "gui.h"
#include "stb_rect_pack.h"
#include "stb_truetype.h"
#include "stb_image_write.h"
#include "stb_image_resize2.h"
#include "stb_ds.h"
#include "sep2dconv.h"
#include "gig.h"
#include "fonts.h"

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
			int document_id;
			int focus_id;
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

#if 0
struct font {
	#if 0
	int font_data_is_on_the_heap_and_owned_by_us;
	stbtt_fontinfo fontinfo;
	float font_px_scale;
	int font_spacing_x, font_spacing_y;
	#endif
	int font_data_is_on_the_heap_and_owned_by_us;
	stbtt_fontinfo fontinfo;
	float px_scale;
	int spacing_x, spacing_y;
};
#endif

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
	int* codepoint_range_pairs_arr;
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
	int cursor_x0, cursor_x, cursor_y;
	struct render_mode render_mode;
	int current_font_spec_index;
	int current_y_stretch_index;
	float current_color[4];
	struct window* current_window;
	int focus_sequence;
	int current_focus_id;
	int* key_buffer_arr;
	char* text_buffer_arr;
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

static void unfocus(void)
{
	g.current_focus_id = 0;
}

static struct font_spec* get_current_font_spec(void)
{
	return &g.font_config.font_specs[g.current_font_spec_index];
}

static float get_current_y_stretch_scale(void)
{
	struct font_spec* spec = get_current_font_spec();
	if (!spec->uses_y_stretch) return 1.0f;
	return font_config_get_y_stretch_level_scale(&g.font_config, g.current_y_stretch_index);
}

int get_num_windows(void)
{
	return arrlen(g.window_arr);
}

struct window* get_window(int index)
{
	assert((0 <= index) && (index < get_num_windows()));
	return &g.window_arr[index];
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

	const int nc = arrlen(fc->codepoint_range_pairs_arr);
	assert(nc%2==0);
	const int num_codepoint_ranges = nc/2;
	assert(num_codepoint_ranges > 0);
	assert(fc->num_y_stretch_levels > 0);
	assert(fc->num_blur_levels > 0);

	// check that codepoint ranges are ordered and don't overlap
	for (int i=0; i<num_codepoint_ranges; ++i) {
		const int r0 = fc->codepoint_range_pairs_arr[i*2+0];
		const int r1 = fc->codepoint_range_pairs_arr[i*2+1];
		assert((r0<=r1) && "bad range");
		assert((i==0 || r0>fc->codepoint_range_pairs_arr[i*2-1]) && "range not ordered");
	}

	for (int i=0; i<fc->num_font_specs; ++i) {
		struct font_spec* spec = &fc->font_specs[i];
		struct font* font = get_font(spec->font_index);
		spec->_px_scale = stbtt_ScaleForPixelHeight(&font->fontinfo, spec->px);
	}

	if (g.atlas_lut != NULL) {
		hmfree(g.atlas_lut);
	}
	assert(g.atlas_lut == NULL);

	arrsetlen(g.atlas_pack_rect_arr, 0);
	static int* glyph_index_arr = NULL;
	arrsetlen(glyph_index_arr, 0);

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
			for (int codepoint=fc->codepoint_range_pairs_arr[2*ci]; codepoint<=fc->codepoint_range_pairs_arr[1+2*ci]; ++codepoint) {

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
						case SPECIAL_CODEPOINT_BLOCK:
							// ok: use boxy glyph bbox
							break;
						case SPECIAL_CODEPOINT_CARET:
							// use boxy height, but our own width for caret
							// (XXX should the width be some fraction of the
							// height?)
							x0=0;
							x1=2;
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
						.rect_index0 = !has_pixels ? -1 : (int)arrlen(g.atlas_pack_rect_arr),
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

		const int n = arrlen(g.atlas_pack_rect_arr);
		if (stbrp_pack_rects(&g.rect_pack_context, g.atlas_pack_rect_arr, n)) {
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

	int* pgi = glyph_index_arr;
	for (int fsi=0; fsi<fc->num_font_specs; ++fsi) {
		struct font_spec* spec = &fc->font_specs[fsi];
		struct font* font = get_font(spec->font_index);
		stbtt_fontinfo* fontinfo = &font->fontinfo;
		for (int ci=0; ci<num_codepoint_ranges; ++ci) {
			for (int codepoint=fc->codepoint_range_pairs_arr[2*ci]; codepoint<=fc->codepoint_range_pairs_arr[1+2*ci]; ++codepoint) {
				const int ny = spec->uses_y_stretch ? fc->num_y_stretch_levels : 1;
				const int glyph_index = *(pgi++);
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
					const stbrp_rect r = g.atlas_pack_rect_arr[info.rect_index0];
					uint8_t* p = &atlas_bitmap[r.x+(r.y << atlas_width_log2)];
					const int stride = atlas_width;
					if (codepoint < SPECIAL_CODEPOINT0) {
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
	arrsetlen(resize_arr, 0);
	for (int fsi=0; fsi<fc->num_font_specs; ++fsi) {
		struct font_spec* spec = &fc->font_specs[fsi];
		for (int ci=0; ci<num_codepoint_ranges; ++ci) {
			for (int codepoint=fc->codepoint_range_pairs_arr[2*ci]; codepoint<=fc->codepoint_range_pairs_arr[1+2*ci]; ++codepoint) {
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
					const stbrp_rect r0 = g.atlas_pack_rect_arr[info.rect_index0];

					const int src_w=info.rect.w;
					const int src_h=info.rect.h;
					assert(src_w>0 && src_h>0);
					const int src_x=r0.x;
					const int src_y=r0.y;

					for (int blur_index=1; blur_index<fc->num_blur_levels; ++blur_index) {
						const struct blur_level* bl = &fc->blur_levels[blur_index];
						const stbrp_rect rn = g.atlas_pack_rect_arr[info.rect_index0+blur_index];

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

	const int num_resizes = arrlen(resize_arr);
	qsort(resize_arr, num_resizes, sizeof(resize_arr[0]), resize_compar);
	STBIR_RESIZE re={0};
	int num_samplers=0;
	for (int i=0; i<num_resizes; ++i) {
		struct resize rz = resize_arr[i];
		if (i==0 || resize_dim_compar(&resize_arr[i-1], &rz) != 0) {
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

	static float* kernel_arr = NULL;
	for (int blur_index=1; blur_index<fc->num_blur_levels; ++blur_index) {
		const struct blur_level* bl = &fc->blur_levels[blur_index];
		const int blurpx = get_blurpx(bl);
		const int kernel_size = 1+2*blurpx;
		arrsetlen(kernel_arr, kernel_size);
		for (int i=0; i<=blurpx; ++i) {
			const double x = ((double)(-blurpx+i)/(double)blurpx)*3.0;
			//printf("y[%d]=%f\n", i, x);
			const float y = (float)gaussian(bl->variance, x) * bl->kernel_scalar;
			assert((0 <= i) && (i < kernel_size));
			kernel_arr[i] = y;
			assert((0 <= (kernel_size-i-1)) && ((kernel_size-i-1) < kernel_size));
			kernel_arr[kernel_size-i-1] = y;
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
			.coefficients = kernel_arr,
		};

		for (int fsi=0; fsi<fc->num_font_specs; ++fsi) {
			struct font_spec* spec = &fc->font_specs[fsi];
			for (int ci=0; ci<num_codepoint_ranges; ++ci) {
				for (int codepoint=fc->codepoint_range_pairs_arr[2*ci]; codepoint<=fc->codepoint_range_pairs_arr[1+2*ci]; ++codepoint) {
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
						const stbrp_rect rn = g.atlas_pack_rect_arr[info.rect_index0+blur_index];

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

	printf("atlas %d×%d, %d rects, %d samplers, %d resizes, built in %.5fs\n",
		atlas_width, atlas_height,
		(int)arrlen(g.atlas_pack_rect_arr),
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

static const float default_y_scalar_min = 0.7f;
static const float default_y_scalar_max = 1.3f;

static struct font_spec default_font_specs[] = {
	{
		.font_index=0,
		.px=40,
		.uses_y_stretch=1,
	},
};

static void fc_add_codepoint_range(struct font_config* fc, int first, int last)
{
	arrput(fc->codepoint_range_pairs_arr, first);
	arrput(fc->codepoint_range_pairs_arr, last);
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
	g.atlas_texture_id = -1;

	assert((get_num_documents() > 0) && "the following code is probably not the best if/when this changes");

	const int focus_id = make_focus_id();
	arrput(g.pane_arr, ((struct pane){
		.type = CODE,
		.u0=0, .v0=0,
		.u1=1, .v1=1,
		.code = {
			.focus_id = focus_id,
			.document_id = get_document_by_index(0)->id,
		},
	}));
	focus(focus_id);

	struct font_config* fc = &g.font_config;
	arrsetlen(fc->codepoint_range_pairs_arr, 0);
	fc_add_latin1(fc);
	fc_add_special(fc);
	fc->y_scalar_min = default_y_scalar_min;
	fc->y_scalar_max = default_y_scalar_max;
	fc->num_y_stretch_levels = 3;
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

static void set_no_scissor(void)
{
	g.render_mode.do_scissor = 0;
}

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
	struct draw_list* list = &g.draw_list_arr[n-1];

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
	*out_indices = arraddnptr(g.vertex_index_arr, num_indices);
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
	c[0]=color[0]; c[1]=color[1]; c[2]=color[2];
	for (int i=0; i<3; ++i) c[i] *= bl->post_scalar;
	tonemap(c);
	uint32_t cu = 0;
	for (int i=0; i<3; ++i) {
		cu += (uint8_t)fminf(255.0f, fmaxf(0.0f, floorf(c[i]*256.0f))) << (i*8);
	}
	return 0xff000000 | cu;
}

static void get_current_line_metrics(float* out_ascent, float* out_descent, float* out_line_gap)
{
	struct font_spec* spec = get_current_font_spec();
	const float scale = spec->_px_scale * get_current_y_stretch_scale();
	if (out_ascent) *out_ascent = (float)spec->_ascent * scale;
	if (out_descent) *out_descent = (float)spec->_descent * scale;
	if (out_line_gap) *out_line_gap = (float)spec->_line_gap * scale;
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
		.font_spec_index = g.current_font_spec_index,
		.y_stretch_index = g.current_y_stretch_index,
		.codepoint = codepoint,
	};
	const struct atlas_lut_info info = hmget(g.atlas_lut, key);

	const int has_pixels = (info.rect_index0 >= 0);
	if (has_pixels) {
		const float w = info.rect.w;
		const float h = info.rect.h;

		for (int i=0; i<fc->num_blur_levels; ++i) {
			const struct blur_level* b = &fc->blur_levels[i];
			const stbrp_rect rect = g.atlas_pack_rect_arr[i+info.rect_index0];
			const float r = b->radius;
			push_mesh_quad(
				g.cursor_x + ((float)info.rect.x) - r, // XXX should have `- ascent` too, but looks wrong?
				g.cursor_y + ((float)info.rect.y) - r,
				w+r*2   , h+r*2 ,
				rect.x   , rect.y ,
				rect.w-1 , rect.h-1,
				make_hdr_rgba(i, g.current_color)
			);
		}
	}

	struct font_spec* spec = &fc->font_specs[g.current_font_spec_index];
	g.cursor_x += spec->_x_spacing;
}

static void set_y_stretch_index(int index)
{
	struct font_config* fc = &g.font_config;
	assert((0 <= index) && (index < fc->num_y_stretch_levels));
	g.current_y_stretch_index = index;
}

static void set_color3f(float red, float green, float blue)
{
	g.current_color[0] = red;
	g.current_color[1] = green;
	g.current_color[2] = blue;
	g.current_color[3] = 1;
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
	gig_spool();
	update_fps();
	arrsetlen(g.key_buffer_arr, 0);
	arrsetlen(g.text_buffer_arr, 0);
	arrput(g.text_buffer_arr, 0); // terminate cstr
}

static void move_caret(int delta_column, int delta_line)
{
	struct command base = {
		.type = COMMAND_MOVE_CARET,
		.move_caret = {
			.set_selection_begin = 1,
			.set_selection_end = 1,
		},
	};

	if (delta_column != 0) {
		struct command c = base;
		c.move_caret.target.type = TARGET_RELATIVE_COLUMN;
		c.move_caret.target.relative_column.delta = delta_column;
		ed_command(&c);
	}

	if (delta_line != 0) {
		struct command c = base;
		c.move_caret.target.type = TARGET_RELATIVE_LINE;
		c.move_caret.target.relative_line.delta = delta_line;
		ed_command(&c);
	}
}

static void handle_editor_input(struct pane* pane)
{
	assert(pane->type == CODE);
	for (int i=0; i<arrlen(g.key_buffer_arr); ++i) {
		const int key = g.key_buffer_arr[i];
		const int down = get_key_down(key);
		const int mod = get_key_mod(key);
		const int code = get_key_code(key);
		if (down && mod==0) {
			switch (code) {
			case KEY_ARROW_LEFT:  move_caret(-1,0); break;
			case KEY_ARROW_RIGHT: move_caret(1,0); break;
			case KEY_ARROW_UP:    move_caret(0,-1); break;
			case KEY_ARROW_DOWN:  move_caret(0,1); break;
			default: break;
			}
		}
		//if (get_key_down(key) && get_key_mod(key)==0 && get_key_code(key) == KEY_ARROW_LEFT
	}

	if (strlen(g.text_buffer_arr) > 0) {
		printf("TODO focused text [%s]\n", g.text_buffer_arr); // TODO
	}
}

static void draw_code_pane(struct pane* pane)
{
	assert(pane->type == CODE);

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
	g.cursor_x0 = g.cursor_x = x0;
	g.cursor_y = pr.y+50;

	set_y_stretch_index(0);
	//set_color3f(.7, 2.7, .7);
	set_color3f(.9,2.6,.9);

	struct document* doc = get_document_by_id(pane->code.document_id);
	const int num_chars = arrlen(doc->fat_char_arr);
	int line_index = 0;
	for (int i=0; i<num_chars; ++i) {
		struct fat_char* fc = &doc->fat_char_arr[i];
		const unsigned c = fc->codepoint;
		if (c == '\n') {
			float /*ascent0,*/ descent0,   line_gap0;
			float   ascent1, /*descent1,*/ line_gap1;
			get_current_line_metrics(/*&ascent0*/NULL, &descent0, &line_gap0);
			set_y_stretch_index((line_index/2) % 3); // XXX look up proper stretch index for current line
			get_current_line_metrics(&ascent1, /*&descent1*/NULL, &line_gap1);
			const float y_advance = ascent1 - descent0 + (line_gap0 + line_gap1)*.5f;
			set_color3f(.9,line_index%4,.9);
			line_index++;
			g.cursor_x = g.cursor_x0;
			g.cursor_y += (int)ceilf(y_advance);
		}

		// XXX ostensibly I also need to subtract "ascent" from y? but it looks
		// wrong... text formatting is hard!
		put_char(c);
	}

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
}

static void gui_draw1(void)
{
	// TODO render video synth background if configured?

	for (int i=0; i<arrlen(g.pane_arr); ++i) {
		struct pane* pane = &g.pane_arr[i];

		//int x0,y0,w,h;
		//get_pane_position(pane, &x0, &y0, NULL, NULL, &w, &h);
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
	arrsetlen(g.draw_list_arr, 0);
	arrsetlen(g.vertex_arr, 0);
	arrsetlen(g.vertex_index_arr, 0);

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
	for (int i=0; i<arrlen(g.draw_list_arr); ++i) {
		struct draw_list* list = &g.draw_list_arr[i];
		switch (list->render_mode.type) {
		case MESH_TRIANGLES: {
			list->mesh.vertices = g.vertex_arr       + list->mesh._vertices_offset;
			list->mesh.indices  = g.vertex_index_arr + list->mesh._indices_offset;
		}	break;
		default: assert(!"unhandled draw list type");
		}
	}

	g.current_window = NULL;
}

struct draw_list* gui_get_draw_list(int index)
{
	assert(index >= 0);
	if (index >= arrlen(g.draw_list_arr)) return NULL;
	return &g.draw_list_arr[index];
}

struct draw_char {
	unsigned codepoint;
	unsigned foreground_color[4];
	unsigned background_color[4];
	unsigned focus;
	// TODO particle effects?
	// TODO shakes/tremors or other effects based on affine transforms?
};
