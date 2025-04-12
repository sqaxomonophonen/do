#include <assert.h>
#include <stdio.h>

#include "fonts.h"
#include "font0.h"

static struct font font0;

static void init_font(struct font* font)
{
	assert(stbtt_InitFont(&font->fontinfo, font->data, stbtt_GetFontOffsetForIndex(font->data, 0)));
	font->is_ready = 1;
	printf("font \"%s\", num glyphs: %d\n", font->name, font->fontinfo.numGlyphs);
}

void fonts_init(void)
{
	font0.name = font0_name;
	font0.data = font0_data;
	font0.data_size = font0_size;
	init_font(&font0);
}

int get_num_fonts(void)
{
	return 1;
}

struct font* get_font(int index)
{
	assert(index == 0);
	return &font0;
}
