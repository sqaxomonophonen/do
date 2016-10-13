#ifndef ATLS_H

/*
atls.h: atls file support. atls files define "skins", and contain an atlas
texture, fonts, cell-based graphics, expression programs and colorschemes.
*/

struct atls_glyph {
	int w,h,x,y,xoff,yoff;
};

struct atls_glyph_table {
	int n_glyphs;
	int height;
	int baseline;
	int* codepoints;
	struct atls_glyph* glyphs;
};

struct atls_cell {
	int x,y;
};

struct atls_cell_table {
	int n_columns;
	int n_rows;
	int n_layers;
	int* layer_prg_ids;
	int* widths;
	int* heights;
	struct atls_cell* cells;
};

struct atls_colorscheme {
	char* name;
	unsigned int* colors;
};

typedef unsigned short atls_op;

struct atls {
	int n_colors_per_colorscheme;
	char** color_keys;

	int n_ctx_keys;
	char** ctx_keys;

	int n_glyph_tables;
	char** glyph_table_keys;
	struct atls_glyph_table* glyph_tables;

	int n_cell_tables;
	char** cell_table_keys;
	struct atls_cell_table* cell_tables;

	int n_colorschemes;
	struct atls_colorscheme* colorschemes;

	int n_programs;
	int n_constants;
	char** program_keys;
	int* program_op_offsets;
	float* constants;
	atls_op* programs;
	int program_storage;

	int atlas_width;
	int atlas_height;
	unsigned char* atlas_bitmap;

	// vvv state

	int n_contexts;
	int* _ctx_bitmask;
	float* _ctx_value;

	struct atls_colorscheme* active_colorscheme;
};

struct atls* atls_load_from_file(char* file);
void atls_free(struct atls*);

/* NOTE uses linear search (cache results) */
int atls_get_glyph_table_index(struct atls*, char* name);
int atls_get_cell_table_index(struct atls*, char* name);
int atls_use_colorscheme(struct atls*, char* name);
int atls_get_ctxkey_id(struct atls*, char* key);
int atls_get_prg_id(struct atls*, char* prg);

void atls_enter_ctx(struct atls*);
void atls_leave_ctx(struct atls*);
void atls_ctx_set(struct atls*, int ctxkey_id, float value);

int atls_glyph_table_index(struct atls_glyph_table*, int codepoint);
struct atls_glyph* atls_glyph_table_lookup(struct atls_glyph_table*, int codepoint);

struct atls_cell* atls_cell_table_lookup(struct atls_cell_table*, int x, int y, int layer);

/* evaluates stored program, returns RGBA as float[4]
 * result is typically affected by ctx vars and colorscheme */
int atls_eval(struct atls*, int prg_id, float* output);

char* atls_get_error();

#define ATLS_H
#endif
