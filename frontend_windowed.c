#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "util.h"
#include "frontend_windowed.h"
#include "editor_client.h"
#include "font0.h"
#include "stb_rect_pack.h"
#include "stb_truetype.h"
#include "stb_ds.h"

#define ATLAS_MAX_SIZE_LOG2 (13)
#define ATLAS_MAX_GLYPHS (1<<16)

struct y_expand_level {
	float y_scale;
	// TODO?
	//float x_bold;
	//float y_bold;
	//float brightness_modifier;
};

#define MAX_Y_EXPAND_LEVELS (16)

struct temp_glyph {
	int index;
	int width;
	int height;
};

static struct {
	int font_data_is_on_the_heap_and_owned_by_us;
	stbtt_fontinfo fontinfo;
	stbrp_context rect_pack_context;
	stbrp_node rect_pack_nodes[1 << ATLAS_MAX_SIZE_LOG2];
	stbrp_rect rect_pack_rects[ATLAS_MAX_GLYPHS];
	int num_y_expand_levels;
	struct y_expand_level y_expand_levels[MAX_Y_EXPAND_LEVELS];

	struct temp_glyph* temp_glyphs;
	uint8_t* temp_font_pixels;
} g;

union glyph_key {
	uint32_t key;
	struct {
		uint32_t codepoint :21;
		uint32_t yexpand   :2;
		uint32_t layer     :2;
		uint32_t _reserved :7;
	};
};
static_assert(4==sizeof(union glyph_key), "expected u32 size");

struct atlas_rect {
	uint16_t x,y,w,h;
};

struct glyph_atlas_rect {
	union glyph_key key;
	struct atlas_rect rect;
};
static_assert(12==sizeof(struct glyph_atlas_rect), "bad size");

struct dynamic_atlas_rect {
	uint32_t key;
	struct atlas_rect rect;
};
static_assert(12==sizeof(struct dynamic_atlas_rect), "bad size");

static int set_font(
	uint8_t* maybe_font_data,
	int transfer_data_ownership,
	int px,
	const int* codepoint_ranges,
	int num_y_expand_levels,
	const struct y_expand_level* y_expand_levels
) {
	assert((num_y_expand_levels > 0) && "there must be at least one y-expand level");
	assert((num_y_expand_levels <= MAX_Y_EXPAND_LEVELS) && "too many levels; fix elsewhere?");

	float max_y_scale = 0.0f;
	for (int i=0; i<num_y_expand_levels; ++i) {
		const struct y_expand_level* level = &y_expand_levels[i];
		const float ys = level->y_scale;
		assert((ys > 0) && "level y-scale must be positive");
		if (ys > max_y_scale) max_y_scale = ys;
	}

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

	g.num_y_expand_levels = num_y_expand_levels;
	memcpy(g.y_expand_levels, y_expand_levels, g.num_y_expand_levels*sizeof(g.y_expand_levels[0]));


	const float px_scale = stbtt_ScaleForPixelHeight(&g.fontinfo, px);
	int num_codepoints_requested = 0;
	int num_codepoints_in_font = 0;
	int num_pixels = 0;
	arrsetlen(g.temp_glyphs, 0);
	for (const int* p=codepoint_ranges; *p>=0; p+=2) {
		for (int codepoint = p[0]; codepoint <= p[1]; ++codepoint) {
			++num_codepoints_requested;
			const int glyph_index = stbtt_FindGlyphIndex(&g.fontinfo, codepoint);
			if (glyph_index == 0) continue;
			++num_codepoints_in_font;
			int x0,y0,x1,y1;
			stbtt_GetGlyphBitmapBox(&g.fontinfo, glyph_index, px_scale, max_y_scale*px_scale, &x0, &y0, &x1, &y1);
			const int w = x1-x0;
			const int h = y1-y0;
			arrput(g.temp_glyphs, ((struct temp_glyph){
				.index  = glyph_index,
				.width  = w,
				.height = h,
			}));
			num_pixels += w*h;
		}
	}
	//printf("npx%d\n", num_pixels);
	arrsetlen(g.temp_font_pixels, num_pixels);
	assert(arrlen(g.temp_glyphs) == num_codepoints_in_font);

	for (int i=0; i<num_codepoints_in_font; ++i) {
	}

	#if 0
	int atlas_width_log2  = 8;
	int atlas_height_log2 = 8;

	for (;;) {
		if ((atlas_width_log2 > ATLAS_MAX_SIZE_LOG2) || (atlas_height_log2 > ATLAS_MAX_SIZE_LOG2)) {
			printf("warning: font atlas rect pack failed\n");
			return 0;
		}

		const int w = 1 << atlas_width_log2;
		const int h = 1 << atlas_height_log2;

		stbrp_init_target(&g.rect_pack_context, w, h, g.rect_pack_nodes, w);

		int num_codepoints_requested = 0;
		int num_codepoints_in_font = 0;
		for (const int* p=codepoint_ranges; *p>=0; p+=2) {
			for (int codepoint = p[0]; codepoint <= p[1]; ++codepoint) {
				++num_codepoints_requested;
				const int glyph_index = stbtt_FindGlyphIndex(&g.fontinfo, codepoint);
				if (glyph_index == 0) continue;
				stbrp_rect* rect = &g.rect_pack_rects[num_codepoints_in_font++];
				int x0,y0,x1,y1;
				stbtt_GetGlyphBitmapBox(&g.fontinfo, glyph_index, scale, scale, &x0, &y0, &x1, &y1);

				rect->id = codepoint;
				rect->w = x1-x0;
				rect->h = y1-y0;
			}
		}

		if (!stbrp_pack_rects(&g.rect_pack_context, g.rect_pack_rects, num_codepoints_in_font)) {
			// packing failed: try again with a larger atlas; prefer wide over high
			if (atlas_width_log2 <= atlas_height_log2) {
				++atlas_width_log2;
			} else {
				++atlas_height_log2;
			}
			continue;
		}

		printf("codepoints, font/requested: %d/%d\n", num_codepoints_in_font, num_codepoints_requested);
		printf("atlas: %d×%d\n", w, h);

		// TODO rasterize!

		return 1;
	}
	#endif

	assert(!"UNREACHABLE");
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

void frontend_init(void)
{
	assert(set_font(
		font0_data, 0,
		20,
		codepoint_ranges_latin1,
		ARRAY_LENGTH(default_y_expand_levels), default_y_expand_levels));
}

void frontend_emit_keypress_event(int keycode)
{
	// TODO
}
