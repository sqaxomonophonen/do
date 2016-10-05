#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>

#include "atls.h"
#include "atlsvm.h"

#define MAX_CONTEXTS (32)

int error;
char* error_str;

static int read_fourcc(FILE* f, char* out_fourcc /*char[5]!*/)
{
	out_fourcc[4] = 0;
	return fread(out_fourcc, 4, 1, f) == 1;
}

static unsigned short read_u16(FILE* f)
{
	unsigned char b[2];
	if (fread(b, 2, 1, f) != 1) {
		error = 1;
		error_str = "premature end-of-file";
		return 0;
	}
	return b[0] + (b[1]<<8);
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

static float read_f32(FILE* f)
{
	union {
		unsigned int u;
		float f;
	} b;
	b.u = read_u32(f);
	return b.f;
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

char* resolve_string(char* strlst, int sz, int offset)
{
	return offset < sz ? (strlst + offset) : NULL;
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
	int n_ptrs = 0;
	int n_ints = 0;
	int n_floats = 0;
	int n_colors_per_colorscheme = 0;
	int n_colorschemes = 0;
	int n_programs = 0;
	int n_constants = 0;
	int n_ctx_keys = 0;
	int n_ops = 0;
	int atlas_width = 0;
	int atlas_height = 0;
	int strlst_sz = 0;

	int got_stri = 0;
	int got_btmp = 0;
	int got_prog = 0;

	struct atls* r_atls = NULL;
	struct atls_glyph* r_gly = NULL;
	struct atls_cell* r_cel = NULL;
	void** r_ptrs = NULL;
	int* r_ints = NULL;
	float* r_floats = NULL;
	atls_op* r_ops = NULL;
	char* r_strlst = NULL;

	int i_glt = 0;
	int i_clt = 0;
	int i_gly = 0;
	int i_cel = 0;
	int i_colorscheme = 0;
	int i_ptr = 0;
	int i_int = 0;
	int i_float = 0;
	int i_ops = 0;

	for (int pass = 0; pass < 2; pass++) {
		rewind(f);

		if (pass == 1) {
			if (!got_stri || !got_btmp || !got_prog) {
				fclose(f);
				error = 1;
				error_str = "file missing blocks";
				return NULL;
			}

			size_t sz = 0;
			size_t offset_atls = reserve(&sz, 1, sizeof(struct atls));
			size_t offset_glt = reserve(&sz, n_glyph_tables, sizeof(struct atls_glyph_table));
			size_t offset_gly = reserve(&sz, n_glyphs_total, sizeof(struct atls_glyph));
			size_t offset_clt = reserve(&sz, n_cell_tables, sizeof(struct atls_cell_table));
			size_t offset_cel = reserve(&sz, n_cells_total, sizeof(struct atls_cell));
			size_t offset_ptrs = reserve(&sz, n_ptrs, sizeof(void*));
			size_t offset_ints = reserve(&sz, n_ints, sizeof(int));
			size_t offset_floats = reserve(&sz, n_floats, sizeof(int));
			size_t offset_ops = reserve(&sz, n_ops, sizeof(atls_op));
			size_t offset_colorschemes = reserve(&sz, n_colorschemes, sizeof(struct atls_colorscheme));
			size_t offset_btmp = reserve(&sz, atlas_width * atlas_height, 1);
			size_t offset_strlst = reserve(&sz, strlst_sz, 1);
			size_t offset_contexts = reserve(&sz, MAX_CONTEXTS, sizeof(struct atls_ctx));

			void* data = calloc(1, sz);
			assert(data != NULL);

			r_atls = data + offset_atls;
			r_gly = data + offset_gly;
			r_cel = data + offset_cel;
			r_ptrs = data + offset_ptrs;
			r_ints = data + offset_ints;
			r_floats = data + offset_floats;
			r_ops = data + offset_ops;

			r_strlst = data + offset_strlst;

			r_atls->glyph_table_keys = (char**)(r_ptrs + i_ptr);
			i_ptr += n_glyph_tables;
			r_atls->n_glyph_tables = n_glyph_tables;
			r_atls->glyph_tables = data + offset_glt;

			r_atls->cell_table_keys = (char**)(r_ptrs + i_ptr);
			i_ptr += n_cell_tables;
			r_atls->n_cell_tables = n_cell_tables;
			r_atls->cell_tables = data + offset_clt;

			r_atls->n_colors_per_colorscheme = n_colors_per_colorscheme;
			r_atls->color_keys = (char**)(r_ptrs + i_ptr);
			i_ptr += n_colors_per_colorscheme;

			r_atls->n_ctx_keys = n_ctx_keys;
			r_atls->ctx_keys = (char**)(r_ptrs + i_ptr);
			i_ptr += n_ctx_keys;

			r_atls->n_programs = n_programs;
			r_atls->n_constants = n_constants;
			r_atls->program_keys = (char**)(r_ptrs + i_ptr);
			i_ptr += n_programs;
			r_atls->program_op_offsets = (r_ints + i_int);
			i_int += n_programs;
			r_atls->constants = (r_floats + i_float);
			i_float += n_constants;
			r_atls->programs = r_ops + i_ops;
			i_ops += n_ops;
			r_atls->program_storage = n_ops;

			r_atls->n_colorschemes = n_colorschemes;
			r_atls->colorschemes = data + offset_colorschemes;

			r_atls->atlas_width = atlas_width;
			r_atls->atlas_height = atlas_height;
			r_atls->atlas_bitmap = data + offset_btmp;

			r_atls->n_contexts = 0;
			r_atls->contexts = data + offset_contexts;
		}

		int state = 0;
		char fourcc[5];
		for (;;) {
			int got_fourcc = read_fourcc(f, fourcc);
			if (!got_fourcc) break;

			unsigned int blksize = read_u32(f);
			long nextblk = ftell(f) + blksize;

			if (state == 0) {
				if (strcmp("ATLS", fourcc) == 0) {
					unsigned int version = read_u32(f);
					if (version != 5) {
						fclose(f);
						error = 1;
						error_str = "invalid ATLS version";
						return NULL;
					}

					state = 1;
				} else {
					fclose(f);
					error = 1;
					error_str = "file does not begin with ATLS block";
					return NULL;
				}
			}

			if (state == 1) {
				if (strcmp("GLYT", fourcc) == 0) {
					unsigned int name_offset = read_u32(f);
					unsigned int n_glyphs = read_u32(f);
					unsigned int height = read_u32(f);
					unsigned int baseline = read_u32(f);

					if (pass == 0) {
						n_glyph_tables++;
						n_ptrs++;
						n_ints += n_glyphs;
						n_glyphs_total += n_glyphs;
					} else {
						int glt_index = i_glt++;
						assert(glt_index <= n_glyph_tables);
						struct atls_glyph_table* glt = &r_atls->glyph_tables[glt_index];

						r_atls->glyph_table_keys[glt_index] = resolve_string(r_strlst, strlst_sz, name_offset);

						glt->n_glyphs = n_glyphs;
						glt->height = height;
						glt->baseline = baseline;
						glt->codepoints = r_ints + i_int;
						i_int += n_glyphs;
						glt->glyphs = r_gly + i_gly;
						i_gly += n_glyphs;
						assert(i_gly <= n_glyphs_total);
						for (int i = 0; i < n_glyphs; i++) {
							struct atls_glyph* g = &glt->glyphs[i];
							glt->codepoints[i] = read_u32(f);
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
						n_ptrs++;
						n_cells_total += n_cells;
						n_ints += n_layers + n_columns + n_rows;
					} else {
						int clt_index = i_clt++;
						struct atls_cell_table* clt = &r_atls->cell_tables[clt_index];
						assert(i_clt <= n_cell_tables);

						r_atls->cell_table_keys[clt_index] = resolve_string(r_strlst, strlst_sz, name_offset);
						clt->n_columns = n_columns;
						clt->n_rows = n_rows;
						clt->n_layers = n_layers;

						clt->layer_prg_ids = r_ints + i_int;
						i_int += n_layers;

						clt->widths = r_ints + i_int;
						i_int += n_columns;
						clt->heights = r_ints + i_int;
						i_int += n_rows;
						assert(i_int <= n_ints);

						clt->cells = r_cel + i_cel;
						i_cel += n_cells;
						assert(i_cel <= n_cells_total);

						for (int i = 0; i < n_layers; i++) {
							clt->layer_prg_ids[i] = read_u32(f);
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
				} else if (strcmp("PROG", fourcc) == 0) {
					if (pass == 0 && got_prog) {
						fclose(f);
						error = 1;
						error_str = "more than one PROG block";
						return NULL;
					}
					got_prog = 1;
					n_programs = read_u32(f);
					n_constants = read_u32(f);
					n_ctx_keys = read_u32(f);
					n_colors_per_colorscheme = read_u32(f);

					if (pass == 0) {
						n_ptrs += n_programs;
						n_ptrs += n_ctx_keys;
						n_ptrs += n_colors_per_colorscheme;
						n_ints += n_programs;
						n_floats += n_constants;
						n_ops += (blksize - (4*4) - (8*n_programs) - (4*n_constants) - (4*n_ctx_keys) - (4*n_colors_per_colorscheme)) / 2;
					} else {
						for (int i = 0; i < n_programs; i++) {
							r_atls->program_keys[i] = resolve_string(r_strlst, strlst_sz, read_u32(f));
							r_atls->program_op_offsets[i] = read_u32(f);
						}
						for (int i = 0; i < n_constants; i++) {
							r_atls->constants[i] = read_f32(f);
						}
						for (int i = 0; i < n_ctx_keys; i++) {
							r_atls->ctx_keys[i] = resolve_string(r_strlst, strlst_sz, read_u32(f));
						}
						for (int i = 0; i < n_colors_per_colorscheme; i++) {
							r_atls->color_keys[i] = resolve_string(r_strlst, strlst_sz, read_u32(f));
						}
						for (int i = 0; i < n_ops; i++) {
							r_atls->programs[i] = read_u16(f);
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
				} else if (strcmp("COLS", fourcc) == 0) {
					int n_colors = (blksize-4)/4;
					if (n_colors != n_colors_per_colorscheme) {
						fclose(f);
						error = 1;
						error_str = "invalid COLS block size";
						return NULL;
					}
					if (pass == 0) {
						n_colorschemes++;
						n_ints += n_colors;
					} else {
						struct atls_colorscheme* cs = &r_atls->colorschemes[i_colorscheme++];
						cs->name = resolve_string(r_strlst, strlst_sz, read_u32(f));
						cs->colors = (unsigned int*) &r_ints[i_int];
						i_int += n_colors;
						for (int i = 0; i < n_colors; i++) {
							cs->colors[i] = read_u32(f);
						}
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
		if (strcmp(a->glyph_table_keys[i], name) == 0) return i;
	}
	return -1;
}

int atls_get_cell_table_index(struct atls* a, char* name)
{
	for (int i = 0; i < a->n_cell_tables; i++) {
		if (strcmp(a->cell_table_keys[i], name) == 0) return i;
	}
	return -1;
}

int atls_use_colorscheme(struct atls* a, char* name)
{
	for (int i = 0; i < a->n_colorschemes; i++) {
		struct atls_colorscheme* t = &a->colorschemes[i];
		if (strcmp(t->name, name) == 0) {
			a->active_colorscheme = t;
			return i;
		}
	}
	return -1;
}

int atls_get_prg_id(struct atls* a, char* prg)
{
	for (int i = 0; i < a->n_programs; i++) {
		if (strcmp(a->program_keys[i], prg) == 0) return a->program_op_offsets[i];
	}
	return -1;
}


static float ctx_get(struct atls* a, int index)
{
	assert(index >= 0);
	assert(index < a->n_ctx_keys);
	int ctxid = a->n_contexts - 1;
	assert(ctxid < MAX_CONTEXTS);
	int bi0 = index>>5;
	int bi1 = index&0x20;
	while (ctxid >= 0) {
		struct atls_ctx* c = &a->contexts[ctxid];
		if ((c->bitmask[bi0] >> bi1) & 1) return c->values[index];
		ctxid--;
	}
	return 0;
}

static void cols_get(struct atls* a, int index, float* out)
{
	assert(a->active_colorscheme != NULL);
	assert(index >= 0);
	assert(index < a->n_colors_per_colorscheme);
	unsigned int rgba = a->active_colorscheme->colors[index];
	for (int i = 0; i < 4; i++) out[i] = ((rgba>>(i<<3)) & 0xff) / (float)0xff;
}

static float fcf_lerp(float t, float a, float b)
{
	return a + (b-a) * t;
}

static int eval(struct atls* a, int opi, int depth, float* output)
{
	if (depth > 2048) return -1;

	assert(opi >= 0);
	if (opi >= a->program_storage) return -1;
	atls_op op = a->programs[opi];

	if (op & 1) {
		// load constant
		int constant_index = op >> 2;
		if (constant_index < 0) return -1;
		if (op & 2) {
			// load float constant
			if (constant_index >= a->n_constants) return -1;
			float v = a->constants[constant_index];
			for (int i = 0; i < 4; i++) output[i] = v;
		} else {
			// load vec4 constant
			if (constant_index > (a->n_constants - 4)) return -1;
			for (int i = 0; i < 4; i++) output[i] = a->constants[constant_index + i];
		}
		return opi + 1;
	} else if ((op & 3) == 2) {
		// load var
		int var_index = op >> 3;
		if (op & 4) {
			// load colorscheme value
			if (var_index < 0 || var_index >= a->n_colors_per_colorscheme) return -1;
			cols_get(a, var_index, output);
		} else {
			// load ctx var
			if (var_index < 0 || var_index >= a->n_ctx_keys) return -1;
			float v = ctx_get(a, var_index);
			for (int i = 0; i < 4; i++) output[i] = v;
		}
		return opi + 1;
	} else if ((op & 63) == 4) {
		// select
		int n_val_exprs = op >> 6;
		(opi)++;
		atls_op* jmptbl = &a->programs[opi];
		(opi) += n_val_exprs + 1;
		int jmpbase = opi;

		float selector[4];
		if (eval(a, opi, depth+1, selector) < 0) return -1;

		int selector_index = (int)selector[0];
		if (selector_index < 0 || selector_index >= n_val_exprs) {
			for (int i = 0; i < 4; i++) output[i] = 0;
		} else {
			int new_opi = jmpbase + jmptbl[selector_index];
			if (eval(a, new_opi, depth+1, output) < 0) return -1;
		}
		return jmpbase + jmptbl[n_val_exprs];
	} else {
		// op/function
		enum atlsvm_fcode fc = op >> 3;

		// determine operand count
		int n_operands = 0;
		switch (fc) {
		#define ATLSVM_DEF(t,n,fn) case t: n_operands = n; break;
		ATLSVM_FCODE_LST
		#undef ATLSVM_DEF
		default:
			return -1;
		}

		// eval args
		float values[3*4];
		for (int i = 0; i < n_operands; i++) {
			opi = eval(a, opi, depth+1, values + i*4);
			if (opi < 0) return -1;
		}

		// execute function with args
		switch (fc) {

		#define ZVAL(expr) \
			for (int i = 0; i < 4; i++) output[i] = (expr);
		#define ATLSVM_UNOP(op) \
			ZVAL(op values[0+i]);
		#define ATLSVM_BINOP(op) \
			ZVAL(values[0+i] op values[4+i]);
		#define ATLSVM_FN1(fn) \
			ZVAL(fn(values[0+i]));
		#define ATLSVM_FN2(fn) \
			ZVAL(fn(values[0+i], values[4+i]));
		#define ATLSVM_FN3(fn) \
			ZVAL(fn(values[0+i], values[4+i], values[8+i]));

		#define ATLSVM_DEF(t,n,fn) case t: fn; break;
		ATLSVM_FCODE_LST
		#undef ATLSVM_DEF

		default:
			return -1;

		#undef ATLSVM_FN3
		#undef ATLSVM_FN2
		#undef ATLSVM_FN1
		#undef ATLSVM_BINOP
		#undef ATLSVM_UNOP
		#undef ZVAL
		}

		return opi;
	}
}

int atls_eval(struct atls* a, int prg_id, float* output)
{
	assert(prg_id >= 0 && prg_id < a->program_storage);
	return eval(a, prg_id, 0, output);
}

int atls_glyph_table_index(struct atls_glyph_table* t, int codepoint)
{
	int l = 0;
	int r = t->n_glyphs-1;
	while (l <= r) {
		int m = (l+r) >> 1;
		int cmp = t->codepoints[m] - codepoint;
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
