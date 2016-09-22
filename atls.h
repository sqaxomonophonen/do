#ifndef ATLS_H

struct atls_glyph {
	int codepoint;
	int w,h,x,y,xoff,yoff;
};

struct atls_glyph_table {
	char* name;
	int n_glyphs;
	int height;
	int baseline;
	struct atls_glyph* glyphs;
};

struct atls_cell {
	int x,y;
};

struct atls_cell_table {
	char* name;
	int n_columns;
	int n_rows;
	int n_layers;
	char** layer_names;
	int* widths;
	int* heights;
	struct atls_cell* cells;
};

struct atls {
	int n_glyph_tables;
	struct atls_glyph_table* glyph_tables;

	int n_cell_tables;
	struct atls_cell_table* cell_tables;

	int atlas_width;
	int atlas_height;
	char* atlas_bitmap;
};

struct atls* atls_load_from_file(char* file);
void atls_free(struct atls*);

/* NOTE uses linear search */
int atls_get_glyph_table_index(struct atls*, char* name);
int atls_get_cell_table_index(struct atls*, char* name);

int atls_glyph_table_index(struct atls_glyph_table*, int codepoint);
struct atls_glyph* atls_glyph_table_lookup(struct atls_glyph_table*, int codepoint);
struct atls_cell* atls_cell_table_lookup(struct atls_cell_table*, int x, int y, int layer);

char* atls_get_error();

#define ATLS_H
#endif
