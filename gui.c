#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "util.h"
#include "gui.h"
#include "editor_client.h"
#include "font0.h"
#include "stb_rect_pack.h"
#include "stb_truetype.h"
#include "stb_image_write.h"
#include "stb_image_resize2.h"
#include "stb_ds.h"
#include "sep2dconv.h"

#define ATLAS_MIN_SIZE_LOG2 (8)
#define ATLAS_MAX_SIZE_LOG2 (13)

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
		} code;
		struct {
			int output_id;
		} videosynth;
	};
};

struct y_expand_level {
	float y_scale;
	// TODO?
	//float x_bold;
	//float y_bold;
	//float brightness_modifier;
};

struct blur_level {
	float radius;
	float variance;
	float scale; // must be 1.0 or less
	float kernel_scalar;
	float post_scalar;
};

#define MAX_Y_EXPAND_LEVELS (16)
#define MAX_BLUR_LEVELS (8)
#define MAX_CODEPOINT_RANGES (256)

struct atlas_lut_info {
	int index0;
	int glyph_index;
	int glyph_x0,glyph_y0,glyph_x1,glyph_y1;
};

struct resize {
	int src_w, src_h;
	int dst_w, dst_h;
	int src_x, src_y;
	int dst_x, dst_y;
};

static int resize_dim_compar(const struct resize* a, const struct resize* b)
{
	const int d0 = a->src_w - b->src_w;
	if (d0!=0) return d0;

	const int d1 = a->src_h - b->src_h;
	if (d1!=0) return d1;

	const int d2 = a->dst_w - b->dst_w;
	if (d2!=0) return d2;

	const int d3 = a->dst_h - b->dst_h;
	return d3;
}

static int resize_compar(const void* va, const void* vb)
{
	const int dd = resize_dim_compar(va,vb);
	if (dd!=0) return dd;

	const struct resize* a = va;
	const struct resize* b = vb;

	const int d0 = a->src_x - b->src_x;
	if (d0!=0) return d0;

	const int d1 = a->src_y - b->src_y;
	if (d1!=0) return d1;

	const int d2 = a->dst_x - b->dst_x;
	if (d2!=0) return d2;

	const int d3 = a->dst_y - b->dst_y;
	return d3;
}

static struct {
	int base_width, base_height;
	int font_data_is_on_the_heap_and_owned_by_us;
	stbtt_fontinfo fontinfo;
	float font_px_scale;
	int font_spacing_x, font_spacing_y;
	stbrp_context rect_pack_context;
	stbrp_node rect_pack_nodes[1 << ATLAS_MAX_SIZE_LOG2];
	stbrp_rect* atlas_pack_rect_arr;
	int num_codepoint_ranges;
	int codepoint_ranges[MAX_CODEPOINT_RANGES*2];
	int num_y_expand_levels;
	struct y_expand_level y_expand_levels[MAX_Y_EXPAND_LEVELS];
	int num_blur_levels;
	struct blur_level blur_levels[MAX_BLUR_LEVELS];
	float* kernel_arr;
	struct {
		int key;
		struct atlas_lut_info value;
	}* atlas_lut;
	struct resize* resize_arr;
	int atlas_texture_id;
	struct draw_list* draw_list_arr;
	struct vertex* vertex_arr;
	vertex_index* vertex_index_arr;
	int64_t last_frame_time;
	float fps;
	struct pane* pane_arr;
	int cursor_x, cursor_y;
	struct render_mode render_mode;
	int current_y_expand_index;
	float current_color[4];
} g;

#define NUM_IDS_LOG2 (32-22)

int get_atlas_lut_key(int num_y_expand_levels, int codepoint, int y_expand_index)
{
	assert(num_y_expand_levels>0);
	assert((0 <= y_expand_index) && (y_expand_index < num_y_expand_levels));
	return codepoint*num_y_expand_levels + y_expand_index;
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

static int set_font(
	uint8_t* maybe_font_data,
	int transfer_data_ownership,
	int px,
	int num_codepoint_ranges,
	const int* codepoint_ranges,
	int num_y_expand_levels,
	const struct y_expand_level* y_expand_levels,
	int num_blur_levels,
	const struct blur_level* blur_levels
) {
	const int64_t t0 = get_nanoseconds();

	if ((num_codepoint_ranges <= 0) || (num_y_expand_levels <= 0) || (num_blur_levels <= 0)) {
		printf("warning: bad request (%d/%d/%d)\n", num_codepoint_ranges, num_y_expand_levels, num_blur_levels);
		return 0;
	}

	if (num_codepoint_ranges > MAX_CODEPOINT_RANGES) {
		printf("warning: too many codepoint ranges: %d (max is %d)\n", num_codepoint_ranges, MAX_CODEPOINT_RANGES);
		return 0;
	}
	if (num_y_expand_levels > MAX_Y_EXPAND_LEVELS) {
		printf("warning: too many y-expand levels: %d (max is %d)\n", num_y_expand_levels, MAX_Y_EXPAND_LEVELS);
		return 0;
	}
	if (num_blur_levels > MAX_BLUR_LEVELS) {
		printf("warning: too many blur levels: %d (max is %d)\n", num_blur_levels, MAX_BLUR_LEVELS);
		return 0;
	}

	// check that codepoint ranges are ordered and don't overlap
	int num_codepoints_requested = 0;
	for (int i=0; i<num_codepoint_ranges; ++i) {
		const int r0 = codepoint_ranges[i*2+0];
		const int r1 = codepoint_ranges[i*2+1];
		if (r1 < r0) {
			printf("warning: invalid codepoint range [%d;%d]\n", r0, r1);
			return 0;
		}
		if (i>0 && r0<=codepoint_ranges[i*2-1]) {
			printf("warning: codepoint ranges not ordered\n");
			return 0;
		}
		num_codepoints_requested += (r1-r0+1);
	}

	const int num_id_levels = num_y_expand_levels * num_blur_levels;
	if (num_id_levels>(1<<NUM_IDS_LOG2)) {
		printf("warning: num_id_levels value %d is too high\n", num_id_levels);
		return 0;
	}

	g.num_codepoint_ranges = num_codepoint_ranges;
	memcpy(g.codepoint_ranges, codepoint_ranges, g.num_codepoint_ranges*sizeof(g.codepoint_ranges[0]));

	g.num_y_expand_levels = num_y_expand_levels;
	memcpy(g.y_expand_levels, y_expand_levels, g.num_y_expand_levels*sizeof(g.y_expand_levels[0]));

	g.num_blur_levels = num_blur_levels;
	memcpy(g.blur_levels, blur_levels, g.num_blur_levels*sizeof(g.blur_levels[0]));

	if (maybe_font_data != NULL) {
		uint8_t* font_data = maybe_font_data;
		if (g.font_data_is_on_the_heap_and_owned_by_us && g.fontinfo.data != NULL) {
			free(g.fontinfo.data);
		}
		g.font_data_is_on_the_heap_and_owned_by_us = transfer_data_ownership;
		assert(stbtt_InitFont(&g.fontinfo, font_data, stbtt_GetFontOffsetForIndex(font_data, 0)));
		printf("font0, num glyphs: %d\n", g.fontinfo.numGlyphs);
	} else {
		assert(!transfer_data_ownership && "transfer_data_ownership set without passing font data: that's weird?");
	}

	assert(g.fontinfo.data != NULL);

	g.font_px_scale = stbtt_ScaleForPixelHeight(&g.fontinfo, px);

	if (g.atlas_lut != NULL) hmfree(g.atlas_lut);
	assert(g.atlas_lut == NULL);

	arrsetcap(g.atlas_pack_rect_arr, (num_codepoints_requested * num_y_expand_levels * num_blur_levels));
	arrsetlen(g.atlas_pack_rect_arr, 0);
	int max_advance = 0;
	for (int ci=0; ci<num_codepoint_ranges; ++ci) {
		for (int codepoint=codepoint_ranges[2*ci]; codepoint<=codepoint_ranges[1+2*ci]; ++codepoint) {
			const int glyph_index = stbtt_FindGlyphIndex(&g.fontinfo, codepoint);
			if (glyph_index == 0) continue;

				int advance,lsb;
				stbtt_GetGlyphHMetrics(&g.fontinfo, glyph_index, &advance, &lsb);
				//printf("i=%d, adv=%d px=%f\n", glyph_index, advance, advance*g.font_px_scale);
				if (advance > max_advance) max_advance = advance;

			for (int y_expand_index=0; y_expand_index<num_y_expand_levels; ++y_expand_index) {
				const struct y_expand_level* yx = &g.y_expand_levels[y_expand_index];

				int x0,y0,x1,y1;
				stbtt_GetGlyphBitmapBox(&g.fontinfo, glyph_index, g.font_px_scale, yx->y_scale*g.font_px_scale, &x0, &y0, &x1, &y1);

				const int w0 = x1-x0;
				const int h0 = y1-y0;

				const int has_pixels = (w0>0) && (h0>0);

				hmput(g.atlas_lut, get_atlas_lut_key(num_y_expand_levels, codepoint, y_expand_index), ((struct atlas_lut_info){
					.index0 = !has_pixels ? -1 : (int)arrlen(g.atlas_pack_rect_arr),
					.glyph_index=glyph_index,
					.glyph_x0=x0,
					.glyph_y0=y0,
					.glyph_x1=x1,
					.glyph_y1=y1,
				}));

				if (!has_pixels) continue;

				for (int blur_index=0; blur_index<num_blur_levels; ++blur_index) {
					const struct blur_level* bl = &g.blur_levels[blur_index];
					const struct blur_level_metrics m = get_blur_level_metrics(bl, w0, h0);
					arrput(g.atlas_pack_rect_arr, ((stbrp_rect){
						.w=m.width,
						.h=m.height,
					}));
				}
			}
		}
	}
	g.font_spacing_x = ceilf(max_advance * g.font_px_scale);
	g.font_spacing_y = 1; // XXX

	int atlas_width_log2  = ATLAS_MIN_SIZE_LOG2;
	int atlas_height_log2 = ATLAS_MIN_SIZE_LOG2;
	for (;;) {
		if ((atlas_width_log2 > ATLAS_MAX_SIZE_LOG2) || (atlas_height_log2 > ATLAS_MAX_SIZE_LOG2)) {
			printf("warning: font atlas rect pack failed\n");
			return 0; // XXX are there any leaks?
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

	for (int ci=0; ci<num_codepoint_ranges; ++ci) {
		for (int codepoint=codepoint_ranges[2*ci]; codepoint<=codepoint_ranges[1+2*ci]; ++codepoint) {
			for (int y_expand_index=0; y_expand_index<num_y_expand_levels; ++y_expand_index) {
				const struct y_expand_level* yx = &g.y_expand_levels[y_expand_index];
				const struct atlas_lut_info info = hmget(g.atlas_lut, get_atlas_lut_key(num_y_expand_levels, codepoint, y_expand_index));
				if (info.index0 == -1) continue;
				assert(info.index0 >= 0);
				const stbrp_rect r = g.atlas_pack_rect_arr[info.index0];
				stbtt_MakeGlyphBitmap(
					&g.fontinfo,
					&atlas_bitmap[r.x+(r.y << atlas_width_log2)],
					r.w, r.h, // XXX these have margins? should I subtract them?
					atlas_width,
					g.font_px_scale,
					yx->y_scale*g.font_px_scale,
					info.glyph_index);
			}
		}
	}

	arrsetlen(g.resize_arr, 0);
	for (int ci=0; ci<num_codepoint_ranges; ++ci) {
		for (int codepoint=codepoint_ranges[2*ci]; codepoint<=codepoint_ranges[1+2*ci]; ++codepoint) {

		const struct atlas_lut_info info0 = hmget(g.atlas_lut, get_atlas_lut_key(num_y_expand_levels, codepoint, 0));
		if (info0.index0 == -1) continue;
		if (info0.glyph_x0==0 && info0.glyph_y0==0 && info0.glyph_x1==0 && info0.glyph_y1==0) continue;

		for (int y_expand_index=0; y_expand_index<num_y_expand_levels; ++y_expand_index) {
				//const struct y_expand_level* yx = &g.y_expand_levels[y_expand_index];
				const struct atlas_lut_info info = y_expand_index==0 ? info0 : hmget(g.atlas_lut, get_atlas_lut_key(num_y_expand_levels, codepoint, y_expand_index));
				assert(info.index0 >= 0);
				const stbrp_rect r0 = g.atlas_pack_rect_arr[info.index0];

				const int src_w=info.glyph_x1-info.glyph_x0;
				const int src_h=info.glyph_y1-info.glyph_y0;
				assert(src_w>0 && src_h>0);
				const int src_x=r0.x;
				const int src_y=r0.y;

				for (int blur_index=1; blur_index<num_blur_levels; ++blur_index) {
					const struct blur_level* bl = &g.blur_levels[blur_index];
					const stbrp_rect rn = g.atlas_pack_rect_arr[info.index0+blur_index];

					const struct blur_level_metrics m = get_blur_level_metrics(bl, src_w, src_h);

					const int dst_w=m.inner_width;
					const int dst_h=m.inner_height;
					const int dst_x=rn.x + m.blurpx;
					const int dst_y=rn.y + m.blurpx;

					arrput(g.resize_arr, ((struct resize){
						.src_w=src_w, .src_h=src_h,
						.dst_w=dst_w, .dst_h=dst_h,
						.src_x=src_x, .src_y=src_y,
						.dst_x=dst_x, .dst_y=dst_y,
					}));
				}
			}
		}
	}

	const int num_resizes = arrlen(g.resize_arr);
	qsort(g.resize_arr, num_resizes, sizeof(g.resize_arr[0]), resize_compar);
	STBIR_RESIZE re={0};
	int num_samplers=0;
	for (int i=0; i<num_resizes; ++i) {
		struct resize rz = g.resize_arr[i];
		if (i==0 || resize_dim_compar(&g.resize_arr[i-1], &rz) != 0) {
			assert(rz.src_w>0 && rz.src_h>0 && rz.dst_w>0 && rz.dst_h>0);
			stbir_free_samplers(&re); // safe when zero-initialized
			stbir_resize_init(
				&re,
				NULL, rz.src_w, rz.src_h, -1,
				NULL, rz.dst_w, rz.dst_h, -1,
				STBIR_1CHANNEL, STBIR_TYPE_UINT8);
			stbir_set_edgemodes(&re, STBIR_EDGE_ZERO, STBIR_EDGE_ZERO);
			stbir_build_samplers(&re);
			++num_samplers;
		}

		assert((0 <= rz.src_x) && (rz.src_x < atlas_width));
		assert((0 <= rz.src_y) && (rz.src_y < atlas_height));
		assert((0 <= (rz.src_x+rz.src_w)) && ((rz.src_x+rz.src_w) <= atlas_width));
		assert((0 <= (rz.src_y+rz.src_h)) && ((rz.src_y+rz.src_h) <= atlas_height));

		stbir_set_buffer_ptrs(
			&re,
			atlas_bitmap + rz.src_x + rz.src_y*atlas_width, atlas_width,
			atlas_bitmap + rz.dst_x + rz.dst_y*atlas_width, atlas_width);
		stbir_resize_extended(&re);
	}
	stbir_free_samplers(&re);

	for (int blur_index=1; blur_index<num_blur_levels; ++blur_index) {
		const struct blur_level* bl = &g.blur_levels[blur_index];
		const int blurpx = get_blurpx(bl);
		const int kernel_size = 1+2*blurpx;
		arrsetlen(g.kernel_arr, kernel_size);
		for (int i=0; i<=blurpx; ++i) {
			const double x = ((double)(-blurpx+i)/(double)blurpx)*3.0;
			//printf("y[%d]=%f\n", i, x);
			const float y = (float)gaussian(bl->variance, x) * bl->kernel_scalar;
			assert((0 <= i) && (i < kernel_size));
			g.kernel_arr[i] = y;
			assert((0 <= (kernel_size-i-1)) && ((kernel_size-i-1) < kernel_size));
			g.kernel_arr[kernel_size-i-1] = y;
		}

		#if 0
		float z=0;
		for (int i=0; i<kernel_size; ++i) {
			printf("x[%d]=%f\n", i, g.kernel_arr[i]);
			z += g.kernel_arr[i];
		}
		printf("kernel sum: %f\n", z);
		printf("predicted: %f\n", bl->variance*sqrtf(6.283185307179586f));
		#endif

		struct sep2dconv_kernel kernel = {
			.radius = blurpx,
			.coefficients = g.kernel_arr,
		};

		for (int ci=0; ci<num_codepoint_ranges; ++ci) {
			for (int codepoint=codepoint_ranges[2*ci]; codepoint<=codepoint_ranges[1+2*ci]; ++codepoint) {
				const struct atlas_lut_info info0 = hmget(g.atlas_lut, get_atlas_lut_key(num_y_expand_levels, codepoint, 0));
				if (info0.index0 == -1) continue;
				if (info0.glyph_x0==0 && info0.glyph_y0==0 && info0.glyph_x1==0 && info0.glyph_y1==0) continue;
				for (int y_expand_index=0; y_expand_index<num_y_expand_levels; ++y_expand_index) {
					const struct atlas_lut_info info = hmget(g.atlas_lut, get_atlas_lut_key(num_y_expand_levels, codepoint, y_expand_index));
					const stbrp_rect rn = g.atlas_pack_rect_arr[info.index0+blur_index];

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

static const int codepoint_ranges_latin1[] = {0x20, 0xff, -1};

static const struct y_expand_level default_y_expand_levels[] = {
	{
		.y_scale = 0.7f,
	},
	{
		.y_scale = 1.0f,
	},
	{
		.y_scale = 1.3f,
	},
};

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

void gui_init(void)
{
	g.atlas_texture_id = -1;

	arrput(g.pane_arr, ((struct pane){
		.type = CODE,
		.u0=0, .v0=0,
		.u1=1, .v1=1,
		.code = {
			.document_id = 0,
		},
	}));

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
	// XXX should be current font instead of a hardcoded one
	assert(set_font(
		font0_data, 0,
		40,
		ARRAY_LENGTH(default_y_expand_levels)/2, codepoint_ranges_latin1,
		ARRAY_LENGTH(default_y_expand_levels), default_y_expand_levels,
		ARRAY_LENGTH(default_blur_levels), default_blur_levels));
}

void gui_emit_keypress_event(int keycode)
{
	// TODO
}

static void set_blend_mode(enum blend_mode blend_mode)
{
	g.render_mode.blend_mode = blend_mode;
}

static void set_scissor(int x, int y, int w, int h)
{
	g.render_mode.do_scissor = 1;
	g.render_mode.scissor_x = x;
	g.render_mode.scissor_y = y;
	g.render_mode.scissor_w = w;
	g.render_mode.scissor_h = h;
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
	assert((0 <= blur_level_index) && (blur_level_index < g.num_blur_levels));
	const struct blur_level* bl = &g.blur_levels[blur_level_index];
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

static void put_char(int codepoint)
{
	const struct atlas_lut_info info = hmget(
		g.atlas_lut,
		get_atlas_lut_key(g.num_y_expand_levels, codepoint, g.current_y_expand_index));
	const float w = info.glyph_x1 - info.glyph_x0;
	const float h = info.glyph_y1 - info.glyph_y0;
	for (int i=0; i<g.num_blur_levels; ++i) {
		struct blur_level* b = &g.blur_levels[i];
		const stbrp_rect rect = g.atlas_pack_rect_arr[i+info.index0];
		const float r = b->radius;
		push_mesh_quad(
			g.cursor_x + ((float)info.glyph_x0) - r,
			g.cursor_y + ((float)info.glyph_y0) - r,
			w+r*2   , h+r*2 ,
			rect.x   , rect.y ,
			rect.w-1 , rect.h-1,
			make_hdr_rgba(i, g.current_color)
		);
	}
	g.cursor_x += g.font_spacing_x;
}

static void set_y_expand_index(int index)
{
	assert((0 <= index) && (index < g.num_y_expand_levels));
	g.current_y_expand_index = index;
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
}

static void render_code_pane(void)
{
	// TODO
}

static void gui_draw1(void)
{
	// TODO render video synth background if configured?

	for (int i=0; i<arrlen(g.pane_arr); ++i) {
		struct pane* pane = &g.pane_arr[i];

		const int x0 = (int)floorf(pane->u0 * (float)g.base_width);
		const int y0 = (int)floorf(pane->v0 * (float)g.base_height);
		const int x1 = (int)ceilf(pane->u1 * (float)g.base_width);
		const int y1 = (int)ceilf(pane->v1 * (float)g.base_height);
		const int w = x1-x0;
		const int h = y1-y0;
		if (w<=0 || h<=0) continue;

		set_scissor(x0,y0,w,h);

		switch (pane->type) {
		case CODE:
			render_code_pane();
			break;
		default: assert(!"unhandled pane type");
		}

		set_blend_mode(ADDITIVE);
		set_texture(g.atlas_texture_id);

		g.cursor_x = 100;
		g.cursor_y = 100;

		const float m = fabsf(sinf(get_nanoseconds() * 2e-9)) * 8.0f;

		set_y_expand_index(0);
		set_color3f(1*m,1*m,1*m);
		for (int i=0; i<26; ++i) put_char(i+'a');

		set_y_expand_index(1);
		set_color3f(1*m,.3*m,1*m);
		for (int i=0; i<26; ++i) put_char(i+'A');

		set_y_expand_index(2);
		set_color3f(1*m,.3*m,.3*m);
		for (int i=0; i<26; ++i) put_char(i+' '+1);
	}
}

void gui_draw(int width, int height)
{
	g.base_width = width;
	g.base_height = height;

	update_fps();

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
}

struct draw_list* gui_get_draw_list(int index)
{
	assert(index >= 0);
	if (index >= arrlen(g.draw_list_arr)) return NULL;
	return &g.draw_list_arr[index];
}
