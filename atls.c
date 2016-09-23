#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "atls.h"

int error;
char* error_str;

static int read_fourcc(FILE* f, char* out_fourcc /*char[5]!*/)
{
	out_fourcc[4] = 0;
	return fread(out_fourcc, 4, 1, f) == 1;
}

static unsigned int read_u32(FILE* f)
{
	unsigned char b[4];
	if (fread(b, 4, 1, f) != 1) {
		error = 1;
		error_str = "premature end-of-file";
		return 0;
	}
	return b[0] + (b[1]<<8) + (b[2]<<16) + (b[3]<<24);
}

static int read_data(FILE* f, void* dst, size_t n)
{
	return fread(dst, n, 1, f) == 1;
}

static size_t reserve(size_t* csz, int n, size_t esz)
{
	size_t p = *csz;

	const int alignment = 8;
	*csz += n*esz;
	if ((*csz) & (alignment-1)) *csz += (alignment - ((*csz) & (alignment-1)));

	return p;
}

struct atls* atls_load_from_file(char* file)
{
	error = 0;
	error_str = "";

	FILE* f = fopen(file, "r");
	if (f == NULL) {
		error = 1;
		error_str = "no such file";
		return NULL;
	}

	int n_glyph_tables = 0;
	int n_cell_tables = 0;
	int n_glyphs_total = 0;
	int n_cells_total = 0;
	int n_charptrs = 0;
	int n_ints = 0;
	int atlas_width = 0;
	int atlas_height = 0;
	int strlst_sz = 0;
	int got_stri = 0;
	int got_btmp = 0;

	struct atls* r_atls = NULL;
	struct atls_glyph* r_gly = NULL;
	struct atls_cell* r_cel = NULL;
	int* r_ints = NULL;
	char** r_charptrs = NULL;
	char* r_strlst = NULL;

	int i_glt = 0;
	int i_clt = 0;
	int i_gly = 0;
	int i_cel = 0;
	int i_int = 0;
	int i_charptr = 0;

	for (int pass = 0; pass < 2; pass++) {
		rewind(f);

		if (pass == 1) {
			// TODO alloc
			if (!got_stri) {
				fclose(f);
				error = 1;
				error_str = "file contains no STRI block";
				return NULL;
			}
			if (!got_btmp) {
				fclose(f);
				error = 1;
				error_str = "file contains no BTMP block";
				return NULL;
			}

			size_t sz = 0;
			size_t offset_atls = reserve(&sz, 1, sizeof(struct atls));
			size_t offset_glt = reserve(&sz, n_glyph_tables, sizeof(struct atls_glyph_table));
			size_t offset_gly = reserve(&sz, n_glyphs_total, sizeof(struct atls_glyph));
			size_t offset_clt = reserve(&sz, n_cell_tables, sizeof(struct atls_cell_table));
			size_t offset_cel = reserve(&sz, n_cells_total, sizeof(struct atls_cell));
			size_t offset_ints = reserve(&sz, n_ints, sizeof(int));
			size_t offset_charptrs = reserve(&sz, n_charptrs, sizeof(char*));
			size_t offset_btmp = reserve(&sz, atlas_width * atlas_height, 1);
			size_t offset_strlst = reserve(&sz, strlst_sz, 1);

			void* data = calloc(1, sz);
			assert(data != NULL);

			r_atls = data + offset_atls;
			r_atls->glyph_tables = data + offset_glt;
			r_gly = data + offset_gly;
			r_atls->cell_tables = data + offset_clt;
			r_cel = data + offset_cel;
			r_ints = data + offset_ints;
			r_charptrs = data + offset_charptrs;
			r_atls->atlas_bitmap = data + offset_btmp;
			r_strlst = data + offset_strlst;

			r_atls->n_glyph_tables = n_glyph_tables;
			r_atls->n_cell_tables = n_cell_tables;
			r_atls->atlas_width = atlas_width;
			r_atls->atlas_height = atlas_height;
		}

		int state = 0;
		char fourcc[5];
		for (;;) {

			int got_fourcc = read_fourcc(f, fourcc);

			if (state == 0) {
				if (strcmp("ATLS", fourcc) == 0) {
					state = 1;
				} else {
					fclose(f);
					error = 1;
					error_str = "file does not begin with ATLS block";
					return NULL;
				}
			}

			if (state == 1) {
				if (!got_fourcc) break;

				unsigned int blksize = read_u32(f); // XXX TODO check error

				long nextblk = ftell(f) + blksize;

				if (strcmp("GLYT", fourcc) == 0) {
					unsigned int name_offset = read_u32(f);
					unsigned int n_glyphs = read_u32(f);
					unsigned int height = read_u32(f);
					unsigned int baseline = read_u32(f);

					if (pass == 0) {
						n_glyph_tables++;
						n_glyphs_total += n_glyphs;
					} else {
						struct atls_glyph_table* glt = &r_atls->glyph_tables[i_glt++];
						assert(i_glt <= n_glyph_tables);

						glt->name = r_strlst + name_offset;
						glt->n_glyphs = n_glyphs;
						glt->height = height;
						glt->baseline = baseline;
						glt->glyphs = r_gly + i_gly;
						i_gly += n_glyphs;
						assert(i_gly <= n_glyphs_total);
						for (int i = 0; i < n_glyphs; i++) {
							struct atls_glyph* g = &glt->glyphs[i];
							g->codepoint = read_u32(f);
							g->x = read_u32(f);
							g->y = read_u32(f);
							g->w = read_u32(f);
							g->h = read_u32(f);
							g->xoff = read_u32(f);
							g->yoff = read_u32(f);
						}
					}
				} else if (strcmp("CELT", fourcc) == 0) {
					unsigned int name_offset = read_u32(f);
					unsigned int n_columns = read_u32(f);
					unsigned int n_rows = read_u32(f);
					unsigned int n_layers = read_u32(f);
					int n_cells = n_columns * n_rows * n_layers;
					if (pass == 0) {
						n_cell_tables++;
						n_cells_total += n_cells;
						n_charptrs += n_layers;
						n_ints += n_columns + n_rows;
					} else {
						struct atls_cell_table* clt = &r_atls->cell_tables[i_clt++];
						assert(i_clt <= n_cell_tables);

						clt->name = r_strlst + name_offset;
						clt->n_columns = n_columns;
						clt->n_rows = n_rows;
						clt->n_layers = n_layers;

						clt->layer_names = r_charptrs + i_charptr;
						i_charptr += n_layers;
						assert(i_charptr <= n_charptrs);

						clt->widths = r_ints + i_int;
						i_int += n_columns;
						clt->heights = r_ints + i_int;
						i_int += n_rows;
						assert(i_int <= n_ints);

						clt->cells = r_cel + i_cel;
						i_cel += n_cells;
						assert(i_cel <= n_cells_total);

						for (int i = 0; i < n_layers; i++) {
							clt->layer_names[i] = r_strlst + read_u32(f);
						}
						for (int i = 0; i < n_columns; i++) {
							clt->widths[i] = read_u32(f);
						}
						for (int i = 0; i < n_rows; i++) {
							clt->heights[i] = read_u32(f);
						}
						for (int i = 0; i < n_cells; i++) {
							struct atls_cell* c = &clt->cells[i];
							c->x = read_u32(f);
							c->y = read_u32(f);
						}
					}
				} else if (strcmp("BTMP", fourcc) == 0) {
					if (pass == 0 && got_btmp) {
						fclose(f);
						error = 1;
						error_str = "more than one BTMP block";
						return NULL;
					}
					got_btmp = 1;

					atlas_width = read_u32(f);
					atlas_height = read_u32(f);
					unsigned int encoding = read_u32(f);
					if (encoding != 1) {
						error = 1;
						error_str = "unsupported BTMP encoding";
						fclose(f);
						return NULL;
					}
					if (pass == 1) {
						read_data(f, r_atls->atlas_bitmap, atlas_width*atlas_height);
					}
				} else if (strcmp("STRI", fourcc) == 0) {
					if (pass == 0 && got_stri) {
						fclose(f);
						error = 1;
						error_str = "more than one STRI block";
						return NULL;
					}
					got_stri = 1;

					if (pass == 0) {
						strlst_sz += blksize;
					} else {
						read_data(f, r_strlst, blksize);
					}
				}

				if (error) {
					fclose(f);
					return NULL;
				}

				fseek(f, nextblk, SEEK_SET);
			}
		}
	}

	fclose(f);

	return r_atls;
}

void atls_free(struct atls* a)
{
	free(a);
}

int atls_get_glyph_table_index(struct atls* a, char* name)
{
	for (int i = 0; i < a->n_glyph_tables; i++) {
		struct atls_glyph_table* t = &a->glyph_tables[i];
		if (strcmp(t->name, name) == 0) return i;
	}
	return -1;
}

int atls_get_cell_table_index(struct atls* a, char* name)
{
	for (int i = 0; i < a->n_cell_tables; i++) {
		struct atls_cell_table* t = &a->cell_tables[i];
		if (strcmp(t->name, name) == 0) return i;
	}
	return -1;
}


int atls_glyph_table_index(struct atls_glyph_table* t, int codepoint)
{
	int l = 0;
	int r = t->n_glyphs-1;
	while (l <= r) {
		int m = (l+r) >> 1;
		struct atls_glyph* g = &t->glyphs[m];
		int cmp = g->codepoint - codepoint;
		if (cmp < 0) {
			l = m + 1;
		} else if (cmp > 0) {
			r = m - 1;
		} else {
			return m;
		}
	}
	return -1;
}

struct atls_glyph* atls_glyph_table_lookup(struct atls_glyph_table* t, int codepoint)
{
	int idx = atls_glyph_table_index(t, codepoint);
	return idx < 0 ? NULL : &t->glyphs[idx];
}

struct atls_cell* atls_cell_table_lookup(struct atls_cell_table* t, int x, int y, int layer)
{
	assert(x >= 0);
	assert(x < t->n_columns);
	assert(y >= 0);
	assert(y < t->n_rows);
	assert(layer >= 0);
	assert(layer < t->n_layers);

	int index =
		  (y * t->n_columns * t->n_layers)
		+ (x * t->n_layers)
		+ layer;
	struct atls_cell* cell = &t->cells[index];
	if (cell->x == -1) return NULL;
	return cell;
}

char* atls_get_error()
{
	return error_str;
}
