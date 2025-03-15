#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "frontend_windowed.h"
#include "editor_client.h"
#include "font0.h"
#include "stb_rect_pack.h"
#include "stb_truetype.h"

#define ATLAS_MAX_SIZE_LOG2 (13)
#define ATLAS_MAX_GLYPHS (1<<16)

static struct {
	int font_data_is_on_the_heap_and_owned_by_us;
	stbtt_fontinfo fontinfo;
	stbrp_context rect_pack_context;
	stbrp_node rect_pack_nodes[1 << ATLAS_MAX_SIZE_LOG2];
	stbrp_rect rect_pack_rects[ATLAS_MAX_GLYPHS];
} g;

union glyph_key {
	uint32_t key;
	struct {
		uint32_t codepoint :21;
		uint32_t plane     :3;
		uint32_t level     :8;
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


static void set_font_data(uint8_t* font_data, int transfer_data_ownership)
{
	if (g.font_data_is_on_the_heap_and_owned_by_us && g.fontinfo.data != NULL) {
		free(g.fontinfo.data);
	}
	g.font_data_is_on_the_heap_and_owned_by_us = transfer_data_ownership;
	assert(stbtt_InitFont(&g.fontinfo, font_data, stbtt_GetFontOffsetForIndex(font_data, 0)));
	printf("font0, num glyphs: %d\n", g.fontinfo.numGlyphs);
}

static int set_font_params(int px, const int* codepoint_ranges)
{
	assert((g.fontinfo.data != NULL) && "font not loaded; call set_font_data() first?");

	int atlas_width_log2  = 8;
	int atlas_height_log2 = 8;

	const float scale = stbtt_ScaleForPixelHeight(&g.fontinfo, px);

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

	assert(!"UNREACHABLE");
}

static const int codepoint_ranges_latin1[] = {0x20, 0xff, -1};

void frontend_init(void)
{
	set_font_data(font0_data, 0);
	assert(set_font_params(20, codepoint_ranges_latin1));
}

void frontend_emit_keypress_event(int keycode)
{
	// TODO
}
