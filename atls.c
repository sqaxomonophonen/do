#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <math.h>

#include "zz.h"
#include "atls.h"
#include "atlsvm.h"
#include "atlsfmt.h"

#define MAX_CONTEXTS (32)

int error;
char* error_str;

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

	struct zz_header header;
	struct zz zz;
	if (zz_open(&zz, file, ZZ_MODE_READONLY, &header) == -1) {
		error = 1;
		error_str = "no such file";
		return NULL;
	}

	{
		int bad_header = 0;
		bad_header |= (header.twocc[0] != 'A');
		bad_header |= (header.twocc[1] != 'T');
		bad_header |= (header.xcc != 'L');
		if (bad_header) {
			error = 1;
			error_str = "invalid file (bad CCs)";
			zz_close(&zz);
			return NULL;
		}

		bad_header |= (header.version != 1);
		if (bad_header) {
			error = 1;
			error_str = "invalid version (not 1)";
			zz_close(&zz);
			return NULL;
		}
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
		if (pass == 1) {
			if (!got_stri || !got_btmp || !got_prog) {
				zz_close(&zz);
				error = 1;
				error_str = "file missing blocks";
				return NULL;
			}

			// reserve space for contexts
			int ctx_storage = MAX_CONTEXTS * n_ctx_keys;
			n_floats += ctx_storage;
			int ctx_bitmask_storage = (ctx_storage+31) >> 5;
			n_ints += ctx_bitmask_storage;

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
			r_atls->_ctx_bitmask = r_ints + i_int;
			i_int += ctx_bitmask_storage;
			r_atls->_ctx_value = r_floats + i_float;
			i_float += ctx_storage;
		}

		struct zz_rblk_iter zzi;
		struct zz_rblk rblk;
		zz_rblk_iter_init(&zz, &zzi);
		while (zz_rblk_iter_next(&zzi, &rblk)) {
			if (rblk.usrtype == ATLSFMT_GLYPH_TABLE) {
				unsigned int name_stridx = zz_rblk_vu(&rblk, "glt_name_stridx");
				unsigned int n_glyphs = zz_rblk_vu(&rblk, "n_glyphs");
				unsigned int height = zz_rblk_vu(&rblk, "height");
				unsigned int baseline = zz_rblk_vu(&rblk, "baseline");

				if (pass == 0) {
					n_glyph_tables++;
					n_ptrs++;
					n_ints += n_glyphs;
					n_glyphs_total += n_glyphs;
				} else {
					int glt_index = i_glt++;
					assert(glt_index <= n_glyph_tables);
					struct atls_glyph_table* glt = &r_atls->glyph_tables[glt_index];

					r_atls->glyph_table_keys[glt_index] = resolve_string(r_strlst, strlst_sz, name_stridx);

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
						glt->codepoints[i] = zz_rblk_vu(&rblk, "codepoint");
						g->x = zz_rblk_vu(&rblk, "x");
						g->y = zz_rblk_vu(&rblk, "y");
						g->w = zz_rblk_vu(&rblk, "w");
						g->h = zz_rblk_vu(&rblk, "h");
						g->xoff = zz_rblk_vs(&rblk, "xoff");
						g->yoff = zz_rblk_vs(&rblk, "yoff");
					}
				}
			} else if (rblk.usrtype == ATLSFMT_CELL_TABLE) {
				unsigned int name_stridx = zz_rblk_vu(&rblk, "clt_name_stridx");
				unsigned int n_columns = zz_rblk_vu(&rblk, "n_columns");
				unsigned int n_rows = zz_rblk_vu(&rblk, "n_rows");
				unsigned int n_layers = zz_rblk_vu(&rblk, "n_layers");
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

					r_atls->cell_table_keys[clt_index] = resolve_string(r_strlst, strlst_sz, name_stridx);
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
						clt->layer_prg_ids[i] = zz_rblk_vu(&rblk, "program_id");
					}
					for (int i = 0; i < n_columns; i++) {
						clt->widths[i] = zz_rblk_vu(&rblk, "column_width");
					}
					for (int i = 0; i < n_rows; i++) {
						clt->heights[i] = zz_rblk_vu(&rblk, "row_height");
					}
					for (int i = 0; i < n_cells; i++) {
						struct atls_cell* c = &clt->cells[i];
						c->x = zz_rblk_vs(&rblk, "xoff");
						c->y = zz_rblk_vs(&rblk, "yoff");
					}
				}
			} else if (rblk.usrtype == ATLSFMT_PROGRAMS) {
				if (pass == 0 && got_prog) {
					zz_close(&zz);
					error = 1;
					error_str = "more than one programs block";
					return NULL;
				}
				got_prog = 1;
				n_programs = zz_rblk_vu(&rblk, "n_programs");
				n_constants = zz_rblk_vu(&rblk, "n_constants");
				n_ctx_keys = zz_rblk_vu(&rblk, "n_ctx_keys");
				n_colors_per_colorscheme = zz_rblk_vu(&rblk, "n_colors_per_colorscheme");
				n_ops = zz_rblk_vu(&rblk, "n_ops");

				if (pass == 0) {
					n_ptrs += n_programs;
					n_ptrs += n_ctx_keys;
					n_ptrs += n_colors_per_colorscheme;
					n_ints += n_programs;
					n_floats += n_constants;
				} else {
					for (int i = 0; i < n_programs; i++) {
						r_atls->program_keys[i] = resolve_string(r_strlst, strlst_sz, zz_rblk_vu(&rblk, "program_key_stridx"));
						r_atls->program_op_offsets[i] = zz_rblk_vu(&rblk, "program_offset");
					}
					for (int i = 0; i < n_constants; i++) {
						r_atls->constants[i] = zz_rblk_f32(&rblk, "f32_constant");
					}
					for (int i = 0; i < n_ctx_keys; i++) {
						r_atls->ctx_keys[i] = resolve_string(r_strlst, strlst_sz, zz_rblk_vu(&rblk, "ctx_key_stridx"));
					}
					for (int i = 0; i < n_colors_per_colorscheme; i++) {
						r_atls->color_keys[i] = resolve_string(r_strlst, strlst_sz, zz_rblk_vu(&rblk, "color_key_striddx"));
					}
					for (int i = 0; i < n_ops; i++) {
						r_atls->programs[i] = zz_rblk_u16(&rblk, "opcode");
					}
				}
			} else if (rblk.usrtype == ATLSFMT_BITMAP) {
				if (pass == 0 && got_btmp) {
					zz_close(&zz);
					error = 1;
					error_str = "more than one bitmap block";
					return NULL;
				}
				got_btmp = 1;

				atlas_width = zz_rblk_vu(&rblk, "width");
				atlas_height = zz_rblk_vu(&rblk, "height");
				if (pass == 1) {
					zz_rblk_u8a(&rblk, r_atls->atlas_bitmap, atlas_width*atlas_height, "bitmap");
				}
			} else if (rblk.usrtype == ATLSFMT_STRTABLE) {
				if (pass == 0 && got_stri) {
					zz_close(&zz);
					error = 1;
					error_str = "more than one string table block";
					return NULL;
				}
				got_stri = 1;

				if (pass == 0) {
					strlst_sz += rblk.size;
				} else {
					zz_rblk_u8a(&rblk, (u8*)r_strlst, rblk.size, "strtbl");
				}
			} else if (rblk.usrtype == ATLSFMT_COLORSCHEME) {
				if (pass == 0) {
					n_colorschemes++;
					n_ints += n_colors_per_colorscheme;
				} else {
					struct atls_colorscheme* cs = &r_atls->colorschemes[i_colorscheme++];
					cs->name = resolve_string(r_strlst, strlst_sz, zz_rblk_vu(&rblk, "colorscheme_stridx"));
					cs->colors = (unsigned int*) &r_ints[i_int];
					i_int += n_colors_per_colorscheme;
					for (int i = 0; i < n_colors_per_colorscheme; i++) {
						cs->colors[i] = zz_rblk_vu(&rblk, "rbga");
					}
					if (rblk.error) {
						zz_close(&zz);
						error = 1;
						error_str = "read error";
						return NULL;
					}
					if (!zz_rblk_is_eob(&rblk)) {
						zz_close(&zz);
						error = 1;
						error_str = "read error (block larger than expected)";
						return NULL;
					}
				}
			}

			if (error) {
				zz_close(&zz);
				return NULL;
			}
		}
	}

	zz_close(&zz);

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

int atls_get_ctxkey_id(struct atls* a, char* key)
{
	for (int i = 0; i < a->n_ctx_keys; i++) {
		if (strcmp(a->ctx_keys[i], key) == 0) return i;
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

void atls_enter_ctx(struct atls* a)
{
	assert(a->n_contexts >= 0);
	a->n_contexts++;
	assert(a->n_contexts <= MAX_CONTEXTS);

	int i0 = (a->n_contexts-1) * a->n_ctx_keys;
	int i1 = a->n_contexts * a->n_ctx_keys;
	for (int i = i0; i < i1; i++) {
		int bi0 = i>>5;
		int bi1 = i&31;
		a->_ctx_bitmask[bi0] &= ~(1<<bi1);
	}
}

void atls_leave_ctx(struct atls* a)
{
	a->n_contexts--;
	assert(a->n_contexts >= 0);
}

void atls_ctx_set(struct atls* a, int ctxkey_id, float value)
{
	if (ctxkey_id < 0) return; // no assert; -1 means key not found
	assert(ctxkey_id < a->n_ctx_keys);
	assert(a->n_contexts > 0);
	assert(a->n_contexts <= MAX_CONTEXTS);

	int i = (a->n_contexts-1) * a->n_ctx_keys + ctxkey_id;
	a->_ctx_value[i] = value;

	// flag bit
	int bi0 = i>>5;
	int bi1 = i&31;
	a->_ctx_bitmask[bi0] |= (1<<bi1);
}

static float ctx_get(struct atls* a, int ctxkey_id)
{
	assert(ctxkey_id >= 0);
	assert(ctxkey_id < a->n_ctx_keys);

	if (a->n_contexts <= 0) return 0.0f;
	assert(a->n_contexts <= MAX_CONTEXTS);

	int ci = a->n_contexts-1;
	while (ci >= 0) {
		int i = ci * a->n_ctx_keys + ctxkey_id;
		int bi0 = i>>5;
		int bi1 = i&31;
		if ((a->_ctx_bitmask[bi0] >> bi1) & 1) {
			return a->_ctx_value[i];
		}
		ci--;
	}
	return 0.0f;
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

////#define EVAL_TRACE
#ifdef EVAL_TRACE
static char* indent(int n)
{
	char* spaaace = "                                                                          ";
	size_t m = strlen(spaaace);
	if (n > m) {
		return spaaace;
	} else {
		return spaaace+m-n;
	}
}
#endif

static int eval(struct atls* a, int opi, int depth, float* output)
{
	if (depth > 2048) return -1;

	assert(opi >= 0);
	if (opi >= a->program_storage) return -2;
	atls_op op = a->programs[opi];

	#ifdef EVAL_TRACE
	printf("%s (opi=%d / op=%d) ", indent(depth), opi, op);
	#endif

	if (op & 1) {
		// load constant
		int constant_index = op >> 2;
		if (constant_index < 0) return -3;
		if (op & 2) {
			// load vec4 constant
			if (constant_index > (a->n_constants - 4)) return -5;
			for (int i = 0; i < 4; i++) output[i] = a->constants[constant_index + i];
			#ifdef EVAL_TRACE
			printf("LOAD VEC4 CONST (%.3f, %.3f, %.3f, %.3f)\n", output[0], output[1], output[2], output[3]);
			#endif
		} else {
			// load float constant
			if (constant_index >= a->n_constants) return -4;
			float v = a->constants[constant_index];
			for (int i = 0; i < 4; i++) output[i] = v;
			#ifdef EVAL_TRACE
			printf("LOAD FLOAT CONST %.3f\n", v);
			#endif
		}
		return opi + 1;
	} else if ((op & 3) == 2) {
		// load var
		int var_index = op >> 3;
		if (op & 4) {
			// load colorscheme value
			if (var_index < 0 || var_index >= a->n_colors_per_colorscheme) return -6;
			cols_get(a, var_index, output);
			#ifdef EVAL_TRACE
			printf("LOAD COLS (%.3f, %.3f, %.3f, %.3f)\n", output[0], output[1], output[2], output[3]);
			#endif
		} else {
			// load ctx var
			if (var_index < 0 || var_index >= a->n_ctx_keys) return -7;
			float v = ctx_get(a, var_index);
			for (int i = 0; i < 4; i++) output[i] = v;
			#ifdef EVAL_TRACE
			printf("LOAD CTX VAR %.3f\n", v);
			#endif
		}
		return opi + 1;
	} else if ((op & 63) == 4) {
		// select
		int n_val_exprs = op >> 6;
		opi++;
		atls_op* jmptbl = &a->programs[opi];
		opi += n_val_exprs + 1;
		int jmpbase = opi;

		float selector[4];
		if (eval(a, opi, depth+1, selector) < 0) return -8;

		int selector_index = (int)selector[0];
		if (selector_index < 0 || selector_index >= n_val_exprs) {
			for (int i = 0; i < 4; i++) output[i] = 0;
		} else {
			int new_opi = jmpbase + jmptbl[selector_index];
			if (eval(a, new_opi, depth+1, output) < 0) return -9;
		}
		return jmpbase + jmptbl[n_val_exprs];
	} else if ((op & 7) == 0) {
		// op/function
		enum atlsvm_fcode fc = op >> 3;

		#ifdef EVAL_TRACE
		printf("OP %s\n", atlsvm_fcode_str(fc));
		#endif
		opi++;

		// determine operand count
		int n_operands = 0;
		switch (fc) {
		#define ATLSVM_DEF(t,n,fn) case t: n_operands = n; break;
		ATLSVM_FCODE_LST
		#undef ATLSVM_DEF
		default:
			return -10;
		}

		// eval args
		float values[3*4];
		for (int i = 0; i < n_operands; i++) {
			opi = eval(a, opi, depth+1, values + i*4);
			if (opi < 0) return opi;
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
			return -12;

		#undef ATLSVM_FN3
		#undef ATLSVM_FN2
		#undef ATLSVM_FN1
		#undef ATLSVM_BINOP
		#undef ATLSVM_UNOP
		#undef ZVAL
		}

		return opi;
	} else {
		return -13;
	}
}

int atls_eval(struct atls* a, int prg_id, float* output)
{
	/* TODO cache results? */
	#ifdef EVAL_TRACE
	printf("EVAL\n");
	#endif
	assert(prg_id >= 0 && prg_id < a->program_storage);
	int r = eval(a, prg_id, 0, output);
	if (r < 0) printf("%d\n", r);
	return r;
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
