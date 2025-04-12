#ifndef FONTS_H

#include "stb_truetype.h"

#include <stddef.h>
#include <stdint.h>

#define SYSTEM_FONT (0)

void fonts_init(void);
int get_num_fonts(void);
struct font* get_font(int);

struct font {
	int is_ready;
	const char* name;
	size_t data_size;
	const uint8_t* data;
	stbtt_fontinfo fontinfo;
};

#define FONTS_H
#endif
