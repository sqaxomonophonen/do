#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "main.h"
#include "util.h"
#include"gui.h"
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
	float pre_multiplier;
};

#define MAX_Y_EXPAND_LEVELS (16)
#define MAX_BLUR_LEVELS (8)
#define MAX_CODEPOINT_RANGES (256)

struct atlas_lut_info {
	int index0;
	int glyph_index;
	int x0,y0,x1,y1;
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
	int font_data_is_on_the_heap_and_owned_by_us;
	stbtt_fontinfo fontinfo;
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

	const float px_scale = stbtt_ScaleForPixelHeight(&g.fontinfo, px);

	if (g.atlas_lut != NULL) hmfree(g.atlas_lut);

	arrsetcap(g.atlas_pack_rect_arr, (num_codepoints_requested * num_y_expand_levels * num_blur_levels));
	arrsetlen(g.atlas_pack_rect_arr, 0);
	for (int ci=0; ci<num_codepoint_ranges; ++ci) {
		for (int codepoint=codepoint_ranges[2*ci]; codepoint<=codepoint_ranges[1+2*ci]; ++codepoint) {
			const int glyph_index = stbtt_FindGlyphIndex(&g.fontinfo, codepoint);
			if (glyph_index == 0) continue;
			for (int y_expand_index=0; y_expand_index<num_y_expand_levels; ++y_expand_index) {
				const struct y_expand_level* yx = &g.y_expand_levels[y_expand_index];

				int x0,y0,x1,y1;
				stbtt_GetGlyphBitmapBox(&g.fontinfo, glyph_index, px_scale, yx->y_scale*px_scale, &x0, &y0, &x1, &y1);

				const int w0 = x1-x0;
				const int h0 = y1-y0;

				const int has_pixels = (w0>0) && (h0>0);

				hmput(g.atlas_lut, get_atlas_lut_key(num_y_expand_levels, codepoint, y_expand_index), ((struct atlas_lut_info){
					.index0 = !has_pixels ? -1 : (int)arrlen(g.atlas_pack_rect_arr),
					.glyph_index=glyph_index,
					.x0=x0,
					.y0=y0,
					.x1=x1,
					.y1=y1,
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

	//printf("nlut=%d\n", (int)hmlen(g.atlas_lut));

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
					px_scale,
					yx->y_scale*px_scale,
					info.glyph_index);
			}
		}
	}

	arrsetlen(g.resize_arr, 0);
	for (int ci=0; ci<num_codepoint_ranges; ++ci) {
		for (int codepoint=codepoint_ranges[2*ci]; codepoint<=codepoint_ranges[1+2*ci]; ++codepoint) {
		const struct atlas_lut_info info0 = hmget(g.atlas_lut, get_atlas_lut_key(num_y_expand_levels, codepoint, 0));
		if (info0.index0 == -1) continue;
		if (info0.x0==0 && info0.y0==0 && info0.x1==0 && info0.y1==0) continue;

		for (int y_expand_index=0; y_expand_index<num_y_expand_levels; ++y_expand_index) {
				//const struct y_expand_level* yx = &g.y_expand_levels[y_expand_index];
				const struct atlas_lut_info info = y_expand_index==0 ? info0 : hmget(g.atlas_lut, get_atlas_lut_key(num_y_expand_levels, codepoint, y_expand_index));
				assert(info.index0 >= 0);
				const stbrp_rect r0 = g.atlas_pack_rect_arr[info.index0];

				const int src_w=info.x1-info.x0;
				const int src_h=info.y1-info.y0;
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
			const float y = (float)gaussian(bl->variance, x) * bl->pre_multiplier;
			assert((0 <= i) && (i < kernel_size));
			g.kernel_arr[i] = y;
			assert((0 <= (kernel_size-i-1)) && ((kernel_size-i-1) < kernel_size));
			g.kernel_arr[kernel_size-i-1] = y;
		}
		struct sep2dconv_kernel kernel = {
			.radius = blurpx,
			.coefficients = g.kernel_arr,
		};

		for (int ci=0; ci<num_codepoint_ranges; ++ci) {
			for (int codepoint=codepoint_ranges[2*ci]; codepoint<=codepoint_ranges[1+2*ci]; ++codepoint) {
				const struct atlas_lut_info info0 = hmget(g.atlas_lut, get_atlas_lut_key(num_y_expand_levels, codepoint, 0));
				if (info0.index0 == -1) continue;
				if (info0.x0==0 && info0.y0==0 && info0.x1==0 && info0.y1==0) continue;
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
	},
	{
		.scale = 0.6f,
		.radius = 4.0f,
		.variance = 1.0f,
		.pre_multiplier = 1.0f,
	},
	{
		.scale = 0.4f,
		.radius = 10.0f,
		.variance = 1.0f,
		.pre_multiplier = 1.0f,
	},
	{
		.scale = 0.2f,
		.radius = 32.0f,
		.variance = 1.0f,
		.pre_multiplier = 1.0f,
	},
};

void gui_init(void)
{
	g.atlas_texture_id = -1;
	assert(set_font(
		font0_data, 0,
		50,
		ARRAY_LENGTH(default_y_expand_levels)/2, codepoint_ranges_latin1,
		ARRAY_LENGTH(default_y_expand_levels), default_y_expand_levels,
		ARRAY_LENGTH(default_blur_levels), default_blur_levels));
}

void gui_emit_keypress_event(int keycode)
{
	// TODO
}

void gui_draw(void)
{
	arrsetlen(g.draw_list_arr, 0);
	arrsetlen(g.vertex_arr, 0);
	arrsetlen(g.vertex_index_arr, 0);

	for (int i=0; i<10; ++i) {
		const uint32_t rgba0=0xff00ff00;
		const uint32_t rgba1=0xffffffff;
		const float S=24+1000*fabsf(sinf((float)get_nanoseconds()*1e-9*(float)(1+i)*1e-1));

		const intptr_t v0 = arrlen(g.vertex_arr);
		arrput(g.vertex_arr, ((struct vertex) {
			.x=0    , .y=0  ,
			.u=0    , .v=0  ,
			.rgba=rgba0,
		}));

		arrput(g.vertex_arr, ((struct vertex) {
			.x=S    , .y=0  ,
			.u=1    , .v=0  ,
			.rgba=rgba0,
		}));

		arrput(g.vertex_arr, ((struct vertex) {
			.x=S    , .y=S  ,
			.u=1    , .v=1  ,
			.rgba=rgba1,
		}));

		arrput(g.vertex_arr, ((struct vertex) {
			.x=0    , .y=S  ,
			.u=0    , .v=1  ,
			.rgba=rgba1,
		}));

		const intptr_t vi0 = arrlen(g.vertex_index_arr);
		arrput(g.vertex_index_arr,0);
		arrput(g.vertex_index_arr,1);
		arrput(g.vertex_index_arr,2);
		arrput(g.vertex_index_arr,0);
		arrput(g.vertex_index_arr,2);
		arrput(g.vertex_index_arr,3);

		struct draw_list* list = arraddnptr(g.draw_list_arr, 1);
		list->blend_mode = ADDITIVE;
		list->type = MESH_TRIANGLES;
		list->mesh.texture = g.atlas_texture_id;
		list->mesh.vertices = (struct vertex*)v0; // offset temporarily stored as ptr
		list->mesh.num_vertices = arrlen(g.vertex_arr)-v0;
		list->mesh.indices = (vertex_index*)vi0; // offset temporarily stored as ptr
		list->mesh.num_indices = arrlen(g.vertex_index_arr)-vi0;
	}


	// TODO

	// fixup pointers to dynamic arrays
	for (int i=0; i<arrlen(g.draw_list_arr); ++i) {
		struct draw_list* list = &g.draw_list_arr[i];
		switch (list->type) {
		case MESH_TRIANGLES: {
			list->mesh.vertices = g.vertex_arr + (intptr_t)list->mesh.vertices;
			list->mesh.indices  = g.vertex_index_arr + (intptr_t)list->mesh.indices;
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
