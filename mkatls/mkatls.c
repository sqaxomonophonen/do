#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define STB_RECT_PACK_IMPLEMENTATION
#include "stb_rect_pack.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

struct glyph {
	struct stbrp_rect rect;
	int all_rects_index;
	int codepoint;
	long bitmap_offset;
	int xoff;
	int yoff;
};

struct glyph_table {
	struct glyph* glyphs;
	int n_glyphs;
	int height;
	int baseline;
	char* filename;
	int is_dummy;
};

struct cell {
	struct stbrp_rect* rects;
	int all_rects_index;
	int x0, y0;
};

struct cell_table {
	struct cell* cells;
	int n_columns;
	int n_rows;
	int *vertical;
	int *horizontal;
	int n_layers;
	int* layer_names_sid;
	stbi_uc** layer_bitmaps;
	int width;
	int height;
};

enum table_type {
	TT_NONE = 0,
	TT_GLYPH,
	TT_CELL,
};

struct table {
	int name_sid;
	enum table_type type;
	union {
		struct glyph_table glyph;
		struct cell_table cell;
	};
};


static void fread_line(FILE* f, char* buf, size_t sz)
{
	int n = 0;
	while (!feof(f)) {
		if (fread(buf, 1, 1, f) == 0 || (n+1) >= sz || *buf == '\n') {
			*buf = 0;
			return;
		}
		n++;
		buf++;
	}
	assert(0);
}

static int split2(char* str, char** part2)
{
	while (*str) {
		if (*str == ' ') {
			*str = 0;
			*part2 = str+1;
			return 1;
		}
		str++;
	}
	return 0;
}

static void hex2pixels(char* str, unsigned char* out, size_t sz)
{
	for (;;) {
		char ch = *str;
		if (!ch) break;
		int value = (ch >= '0' && ch <= '9') ? (ch-'0') : (ch >= 'A' && ch <= 'F') ? (10+ch-'A') : -1;
		for (int i = 3; i >= 0; i--) {
			*out++ = (value & (1<<i)) ? 255 : 0;
			sz--;
			if (sz == 0) return;
		}
		str++;
	}
}


static int glyph_codepoint_compar(const void* va, const void* vb)
{
	const struct glyph* a = va;
	const struct glyph* b = vb;
	return a->codepoint - b->codepoint;
}

static void blit(unsigned char* dst, int dst_stride, unsigned char* src, int w, int h, int x0, int y0)
{
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			dst[x + x0 + (y + y0)*dst_stride] = src[x + y*w];
		}
	}
}

static void blit4(unsigned char* dst, int dst_stride, unsigned char* src, int src_width, int w, int h, int dst_x0, int dst_y0, int src_x0, int src_y0)
{
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			dst[x + dst_x0 + (y + dst_y0)*dst_stride] = src[(x + src_x0 + (y + src_y0)*src_width)*4+3];
		}
	}
}


static int endswith(char* str, const char* suffix)
{
	size_t str_len = strlen(str);
	size_t suffix_len = strlen(suffix);
	if (str_len < suffix_len) return 0;
	return strcmp(str + (str_len - suffix_len), suffix) == 0;
}

static char** explode_strings(char* s, int* out_n)
{
	int n = 1;
	char* p = s;

	while (*p) {
		if (*p == ' ') n++;
		p++;
	}

	char** lst = malloc(n * sizeof(char*));
	assert(lst != NULL);

	p = s;
	for (int i = 0; i < n; i++) {
		char* p0 = p;
		while (*p) {
			if (*p == ' ') break;
			p++;
		}
		*p = 0;
		p++;
		lst[i] = strdup(p0); // hohum
		assert(lst[i] != NULL);
	}

	if (out_n != NULL) *out_n = n;

	return lst;
}

static int* explode_integers(char* s, int* out_n)
{
	int n;
	char** ss = explode_strings(s, &n); // hehe
	if (out_n != NULL) *out_n = n;

	int* lst = malloc(n * sizeof(int));
	assert(lst != NULL);

	for (int i = 0; i < n; i++) {
		lst[i] = atoi(ss[i]);
	}

	free(ss);

	return lst;
}

#define MAX_N (2048)

struct table tables[MAX_N];
int n_tables;

struct table* table_alloc(enum table_type tt, int name_sid)
{
	assert(n_tables < MAX_N);
	struct table* t = &tables[n_tables++];
	memset(t, 0, sizeof(*t));
	t->type = tt;
	t->name_sid = name_sid;
	return t;
}

static void table_reset()
{
	n_tables = 0;
}

static int alpha_sum_rect(stbi_uc* bitmap, int stride, int x0, int y0, int w, int h)
{
	int sum = 0;
	for (int y = y0; y < (y0+h); y++) {
		for (int x = x0; x < (x0+w); x++) {
			stbi_uc* pixel = &bitmap[(x + y * stride) << 2];
			sum += pixel[3];
		}
	}
	return sum;
}

struct blk {
	char type[4];
	int cap_log2;
	int data_sz;
	char* data;
	struct blk* next;
};

void blk_append(struct blk* b, unsigned char* data, int sz)
{
	int dst_offset = b->data_sz;
	b->data_sz += sz;
	{ // grow buffer if necessary
		int old_cap_log2 = b->cap_log2;
		while ((1<<b->cap_log2) < b->data_sz) b->cap_log2++;
		if (b->cap_log2 != old_cap_log2) {
			b->data = realloc(b->data, 1<<b->cap_log2);
			assert(b->data != NULL);
		}
	}
	memcpy(&b->data[dst_offset], data, sz);
}

struct blk* blk_new(char* type)
{
	assert(strlen(type) == 4);
	struct blk* b = calloc(1, sizeof(*b));
	assert(b != NULL);
	memcpy(b->type, type, 4);
	return b;
}

void encode_u32(unsigned int v, unsigned char* out)
{
	out[0] = v & 255;
	out[1] = (v>>8) & 255;
	out[2] = (v>>16) & 255;
	out[3] = (v>>24) & 255;
}

void blk_finalize(struct blk* b, FILE* f)
{
	fwrite(&b->type, sizeof(b->type), 1, f);

	unsigned char c32[4];
	encode_u32(b->data_sz, c32);
	fwrite(c32, sizeof(c32), 1, f);

	fwrite(b->data, b->data_sz, 1, f);
	free(b->data);
	free(b);
}

void blk_u32(struct blk* b, unsigned int v)
{
	unsigned char c32[4];
	encode_u32(v, c32);
	blk_append(b, c32, sizeof(c32));
}

void blk_s32(struct blk* b, int v)
{
	blk_u32(b, (unsigned int)v);
}

struct strtbl {
	struct blk* strings;
};

void strtbl_init(struct strtbl* t)
{
	t->strings = blk_new("STRI");
}

unsigned int strtbl_add(struct strtbl* t, char* str)
{
	int cursor = 0;
	while (cursor < t->strings->data_sz) {
		char* s = t->strings->data + cursor;
		if (strcmp(s, str) == 0) {
			return cursor;
		}
		cursor += strlen(s)+1;
	}

	int p = t->strings->data_sz;
	blk_append(t->strings, (unsigned char*)str, strlen(str)+1);
	return p;
}

void strtbl_finalize(struct strtbl* t, FILE* f)
{
	blk_finalize(t->strings, f);
}

static char* penultimate(char* name)
{
	size_t len = strlen(name);
	char* retval = name;
	for (int o = (len-1); o >= 0; o--) {
		char ch = name[o];
		if (ch == '.') {
			name[o] = 0;
			break;
		}
	}
	len = strlen(name);
	for (int o = (len-1); o >= 0; o--) {
		char ch = name[o];
		if (ch == '.') {
			retval = &name[o+1];
			break;
		}
	}
	return retval;
}

struct userblk {
	int type_sid;
	int name_sid;
	char* srcpath;
	struct userblk* next;
};

static int atoi8(char* s)
{
	int v = atoi(s);
	assert(v >= 0);
	assert(v < 256);
	return v;
}

static int decode_s32(const char* v)
{
	return v[0] + (v[1]<<8) + (v[2]<<16) + (v[3]<<24);
}

static int col_compar(const void* va, const void* vb)
{
	int d0 = decode_s32(va) - decode_s32(vb);
	if (d0 != 0) return d0;

	int d1 = decode_s32(va+4) - decode_s32(vb+4);
	return d1;
}

static int try_sz(int sz, char* outfile, int n, char** filenames)
{
	assert(n < MAX_N);

	table_reset();

	int atlas_width = sz;
	int atlas_height = sz;

	printf("trying %d×%d...\n", atlas_width, atlas_height);

	char line[4096];

	int n_total_rects = 0;

	struct userblk* userblks = NULL;
	struct userblk** userblk_cursor = &userblks;

	struct blk* xblks = NULL;
	struct blk** xblk_cursor = &xblks;

	struct strtbl strtbl;
	strtbl_init(&strtbl);

	for (int i = 0; i < n; i++) {
		char* filename = filenames[i];

		char* name = strdup(filename);
		assert(name != NULL);

		int is_user = 0;
		char* user_type = NULL;
		if (name[0] == '@') {
			is_user = 1;
			size_t len = strlen(name);
			for (int o = 0; o < len; o++) {
				char ch = name[o];
				if (ch == ':') {
					name[o] = 0;
					user_type = &name[1];
					name = &name[o+1];
					filename = name;
					break;
				}
			}
			assert("USER arg contains :-delimiter" && user_type != NULL);
		}

		{
			int get_name_from_filename = 1;
			size_t len = strlen(name);
			for (int o = 0; o < len; o++) {
				char ch = name[o];
				if (ch == ':') {
					name[o] = 0;
					filename = &filename[o+1];
					get_name_from_filename = 0;
					break;
				}
			}

			if (get_name_from_filename) {
				name = penultimate(name);
			}
		}

		if (is_user) {
			struct userblk* u = calloc(1, sizeof(*u));
			assert(u != NULL);
			u->type_sid = strtbl_add(&strtbl, user_type);
			u->name_sid = strtbl_add(&strtbl, name);
			u->srcpath = filename;
			*userblk_cursor = u;
			userblk_cursor = &u->next;
		} else if (endswith(filename, ".atls")) {
			assert(!"TODO merge with .atls file");
		} else if (endswith(filename, ".tbl")) {
			struct table* tbl = table_alloc(TT_CELL, strtbl_add(&strtbl, name));
			struct cell_table* clt = &tbl->cell;

			char** layer_names = NULL;
			FILE* f = fopen(filename, "r");
			for (;;) {
				fread_line(f, line, sizeof(line));
				if (strcmp(line, "END") == 0) break;

				char* tail;
				split2(line, &tail);

				if (strcmp(line, "LAYERS") == 0) {
					assert(layer_names == NULL);
					layer_names = explode_strings(tail, &clt->n_layers);
				} else if (strcmp(line, "VERTICAL") == 0) {
					assert(clt->vertical == NULL);
					clt->vertical = explode_integers(tail, &clt->n_columns);
				} else if (strcmp(line, "HORIZONTAL") == 0) {
					assert(clt->horizontal == NULL);
					clt->horizontal = explode_integers(tail, &clt->n_rows);
				} else {
					continue;
				}
			}
			fclose(f);
			assert(layer_names != NULL);

			clt->layer_bitmaps = calloc(clt->n_layers, sizeof(*clt->layer_bitmaps));
			assert(clt->layer_bitmaps != NULL);

			for (int j = 0; j < clt->n_layers; j++) {
				int comp = 4;
				clt->layer_bitmaps[j] = stbi_load(layer_names[j], &clt->width, &clt->height, &comp, comp);
				assert(clt->layer_bitmaps[j] != NULL);
				assert("image is RGBA" && comp == 4);
			}

			clt->layer_names_sid = calloc(clt->n_layers, sizeof(*clt->layer_names_sid));
			assert(clt->layer_names_sid != NULL);
			for (int j = 0; j < clt->n_layers; j++) {
				clt->layer_names_sid[j] = strtbl_add(&strtbl, penultimate(layer_names[j]));
			}

			int n_cells = clt->n_columns * clt->n_rows;
			clt->cells = calloc(n_cells, sizeof(*clt->cells));
			assert(clt->cells != NULL);

			int j = 0;
			int y0 = 0;
			for (int row = 0; row < clt->n_rows; row++) {
				int x0 = 0;
				for (int column = 0; column < clt->n_columns; column++) {
					struct cell* c = &clt->cells[j++];
					c->rects = calloc(clt->n_layers, sizeof(*c->rects));
					assert(c->rects != NULL);
					c->x0 = x0;
					c->y0 = y0;
					int w = clt->vertical[column];
					int h = clt->horizontal[row];
					for (int k = 0; k < clt->n_layers; k++) {
						if (alpha_sum_rect(clt->layer_bitmaps[k], clt->width, x0, y0, w, h) == 0) {
							c->rects[k].w = 0;
							c->rects[k].h = 0;
						} else {
							c->rects[k].w = w;
							c->rects[k].h = h;
						}
					}
					x0 += clt->vertical[column];
				}
				y0 += clt->horizontal[row];
			}

			n_total_rects += n_cells * clt->n_layers;

		} else if (endswith(filename, ".rects")) {
			struct table* tbl = table_alloc(TT_GLYPH, strtbl_add(&strtbl, name));
			struct glyph_table* glt = &tbl->glyph;

			glt->is_dummy = 1; // no bitmaps associated

			FILE* f = fopen(filename, "r");
			assert(f);

			glt->n_glyphs = 0;

			for (;;) {
				fread_line(f, line, sizeof(line));
				if (strcmp(line, "END") == 0) break;

				char* arg0;
				split2(line, &arg0);

				if (strcmp(line, "RECT") == 0) {
					glt->n_glyphs++;
				} else {
					continue;
				}
			}

			rewind(f);

			n_total_rects += glt->n_glyphs;
			glt->glyphs = calloc(glt->n_glyphs, sizeof(*glt->glyphs));

			int i = 0;
			for (;;) {
				fread_line(f, line, sizeof(line));
				if (strcmp(line, "END") == 0) break;

				char* arg0;
				split2(line, &arg0);

				if (strcmp(line, "RECT") == 0) {
					char* ws;
					char* hs;
					split2(arg0, &ws);
					split2(ws, &hs);

					int w = atoi(ws);
					int h = atoi(hs);
					int codepoint = atoi(arg0);

					assert(w > 0);
					assert(h > 0);
					assert(codepoint > 0);

					struct glyph* gly = &glt->glyphs[i++];
					gly->rect.w = w;
					gly->rect.h = h;
					gly->codepoint = codepoint;
				} else {
					continue;
				}
			}
		} else if (endswith(filename, ".bdf")) {
			struct table* tbl = table_alloc(TT_GLYPH, strtbl_add(&strtbl, name));
			struct glyph_table* glt = &tbl->glyph;
			glt->filename = filename;

			FILE* f = fopen(filename, "r");
			assert(f);

			{
				fread_line(f, line, sizeof(line));

				char* arg;
				split2(line, &arg);

				assert(strcmp(line, "STARTFONT") == 0);
				assert(strcmp(arg, "2.1") <= 0);
			}

			for (;;) {
				fread_line(f, line, sizeof(line));
				char* arg;
				split2(line, &arg);

				if (strcmp(line, "FONTBOUNDINGBOX") == 0) {
					char* arg2;
					char* arg3;
					char* arg4;
					split2(arg, &arg2);
					split2(arg2, &arg3);
					split2(arg3, &arg4);
					glt->height = atoi(arg2);
					glt->baseline = glt->height + atoi(arg4);
					break;
				}
			}

			glt->n_glyphs = 0;

			for (;;) {
				fread_line(f, line, sizeof(line));
				char* arg;
				split2(line, &arg);

				if (strcmp(line, "CHARS") == 0) {
					glt->n_glyphs = atoi(arg);
					break;
				}
			}

			assert(glt->n_glyphs);
			n_total_rects += glt->n_glyphs;

			glt->glyphs = calloc(glt->n_glyphs, sizeof(*glt->glyphs));

			for (int j = 0; j < glt->n_glyphs; j++) {
				struct glyph* glyph = &glt->glyphs[j];

				for (;;) {
					fread_line(f, line, sizeof(line));
					char* arg;
					split2(line, &arg);

					if (strcmp(line, "BITMAP") == 0) {
						glyph->bitmap_offset = ftell(f);
						break;
					} else if (strcmp(line, "ENCODING") == 0) {
						glyph->codepoint = atoi(arg);
					} else if (strcmp(line, "BBX") == 0) {
						char* arg2;
						char* arg3;
						char* arg4;
						split2(arg, &arg2);
						split2(arg2, &arg3);
						split2(arg3, &arg4);
						glyph->rect.w = atoi(arg) + 1;
						glyph->rect.h = atoi(arg2) + 1;
						glyph->xoff = atoi(arg3);
						glyph->yoff = -glyph->rect.h - atoi(arg4);
					}
				}
			}

			fclose(f);
		} else if (endswith(filename, ".cols")) {
			FILE* f = fopen(filename, "r");
			assert(f);

			struct blk* cb = blk_new("COLS");
			blk_u32(cb, strtbl_add(&strtbl, name));

			int i0 = cb->data_sz;

			int n = 0;
			while (!feof(f)) {
				fread_line(f, line, sizeof(line));
				char* layer;
				char* red;
				char* green;
				char* blue;
				char* alpha;
				if (
					!split2(line, &layer) ||
					!split2(layer, &red) ||
					!split2(red, &green) ||
					!split2(green, &blue) ||
					!split2(blue, &alpha)) continue;

				blk_u32(cb, strtbl_add(&strtbl, line)); // name
				blk_u32(cb, strtbl_add(&strtbl, layer)); // layer name
				blk_u32(cb, // rgba
					atoi8(red) |
					(atoi8(green)<<8) |
					(atoi8(blue)<<16) |
					(atoi8(alpha)<<24));
				n++;
			}

			int sz = cb->data_sz - i0;
			if (sz > 0) {
				qsort(cb->data + i0, n, 12, col_compar);
				*xblk_cursor = cb;
				xblk_cursor = &cb->next;
			}

			fclose(f);
		} else {
			fprintf(stderr, "unknown extension in '%s'\n", filename);
			exit(EXIT_FAILURE);
		}
	}

	// codepoints are used for binary search; make sure they're sorted
	for (int i = 0; i < n_tables; i++) {
		struct table* t = &tables[i];
		if (t->type != TT_GLYPH) continue;
		struct glyph_table* glt = &t->glyph;
		qsort(glt->glyphs, glt->n_glyphs, sizeof(*glt->glyphs), glyph_codepoint_compar);
	}

	// zip rects into all_rects array
	struct stbrp_rect* all_rects = calloc(1, n_total_rects * sizeof(*all_rects));
	assert(all_rects != NULL);
	int offset = 0;
	for (int i = 0; i < n_tables; i++) {
		struct table* t = &tables[i];
		switch (t->type) {
		case TT_GLYPH: {
			struct glyph_table* glt = &t->glyph;
			for (int j = 0; j < glt->n_glyphs; j++) {
				memcpy(all_rects + offset, &glt->glyphs[j].rect, sizeof(stbrp_rect));
				glt->glyphs[j].all_rects_index = offset;
				offset++;
			}
		} break;
		case TT_CELL: {
			struct cell_table* clt = &t->cell;
			int n_cells = clt->n_columns*clt->n_rows;
			for (int j = 0; j < n_cells; j++) {
				clt->cells[j].all_rects_index = offset;
				for (int k = 0; k < clt->n_layers; k++) {
					memcpy(all_rects + offset, &clt->cells[j].rects[k], sizeof(stbrp_rect));
					offset++;
				}
			}
		} break;
		default:
			assert(!"unknown type");
		}
	}

	// pack rects using stb_rect_pack.h
	int n_nodes = atlas_width;
	stbrp_node* nodes = calloc(1, n_nodes * sizeof(*nodes));
	assert(nodes);
	stbrp_context ctx;
	stbrp_init_target(&ctx, atlas_width, atlas_height, nodes, n_nodes);
	stbrp_pack_rects(&ctx, all_rects, n_total_rects);

	// bail if not all rects were packed
	for (int i = 0; i < n_total_rects; i++) if (all_rects[i].w > 0 && !all_rects[i].was_packed) return 0;

	unsigned char* bitmap = calloc(atlas_width*atlas_height, 1);
	assert(bitmap);

	int n_cell_tables = 0;
	int n_glyph_tables = 0;

	// blit source bitmaps into atlas
	for (int i = 0; i < n_tables; i++) {
		struct table* t = &tables[i];
		switch (t->type) {
		case TT_GLYPH: {
			n_glyph_tables++;

			struct glyph_table* glt = &t->glyph;

			if (glt->is_dummy) {
				continue;
			}

			FILE* f = fopen(glt->filename, "r");
			assert(f);

			for (int j = 0; j < glt->n_glyphs; j++) {
				struct glyph* gly = &glt->glyphs[j];

				struct stbrp_rect* prect = &all_rects[gly->all_rects_index];
				assert(fseek(f, gly->bitmap_offset, SEEK_SET) == 0);

				for (int k = 0; k < (prect->h-1); k++) {
					fread_line(f, line, sizeof(line));
					unsigned char row[1024];
					hex2pixels(line, row, sizeof(row));
					blit(bitmap, atlas_width, row, (prect->w-1), 1, prect->x, prect->y + k);
				}
			}

			fclose(f);
		} break;
		case TT_CELL: {
			n_cell_tables++;

			struct cell_table* clt = &t->cell;
			int n_cells = clt->n_columns*clt->n_rows;

			for (int j = 0; j < n_cells; j++) {
				struct cell* c = &clt->cells[j];
				for (int k = 0; k < clt->n_layers; k++) {
					struct stbrp_rect* prect = &all_rects[c->all_rects_index + k];
					if (prect->w == 0 || prect->h == 0) continue;

					blit4(
						bitmap,
						atlas_width,

						clt->layer_bitmaps[k],
						clt->width,

						prect->w, prect->h,
						prect->x, prect->y,
						c->x0, c->y0);
				}
			}
		} break;
		default:
			assert(!"unknown type");
		}
	}

	if (endswith(outfile, ".png")) {
		int comp = 3;

		char* atlas = calloc(atlas_width*atlas_height, comp);
		assert(atlas != NULL);

		// put atlas bitmap into green color
		for (int y = 0; y < atlas_height; y++) {
			for (int x = 0; x < atlas_width; x++) {
				atlas[(x + y*atlas_width) * comp + 1] = bitmap[x + y*atlas_width];
			}
		}

		// paint rects red/blue "checkerboard"
		for (int i = 0; i < n_total_rects; i++) {
			struct stbrp_rect* r = &all_rects[i];
			if (r->w == 0 || r->h == 0) continue;

			int d = i&1 ? 0 : 2;
			int v = i&1 ? 60 : 90;

			for (int y = r->y; y < (r->y + r->h); y++) {
				for (int x = r->x; x < (r->x + r->w); x++) {
					int v2 = (x==r->x||y==r->y) ? v*2 : (x==r->x+r->w-1||y==r->y+r->h-1) ? v/2 : v;
					atlas[(x + y*atlas_width) * comp + d] = v2;
				}
			}
		}

		assert(stbi_write_png(outfile, atlas_width, atlas_height, comp, atlas, atlas_width * comp));
	} else if (endswith(outfile, ".atls")) {
		FILE* f = fopen(outfile, "wb");

		{
			struct blk* b = blk_new("ATLS");
			blk_u32(b, 3); // version
			blk_finalize(b, f);
		}

		for (int i = 0; i < n_tables; i++) {
			struct table* t = &tables[i];
			struct blk* b = NULL;
			switch (t->type) {
			case TT_GLYPH: {
				struct glyph_table* glt = &t->glyph;
				b = blk_new("GLYT");
				blk_u32(b, t->name_sid);
				blk_u32(b, glt->n_glyphs);
				blk_u32(b, glt->height);
				blk_u32(b, glt->baseline);

				for (int i = 0; i < glt->n_glyphs; i++) {
					struct glyph* gly = &glt->glyphs[i];
					blk_u32(b, gly->codepoint);

					struct stbrp_rect* r = &all_rects[gly->all_rects_index];
					blk_u32(b, r->x);
					blk_u32(b, r->y);
					blk_u32(b, r->w);
					blk_u32(b, r->h);
					blk_u32(b, gly->xoff);
					blk_u32(b, gly->yoff);
				}
			} break;
			case TT_CELL: {
				struct cell_table* clt = &t->cell;
				b = blk_new("CELT");
				blk_u32(b, t->name_sid);
				blk_u32(b, clt->n_columns);
				blk_u32(b, clt->n_rows);
				blk_u32(b, clt->n_layers);

				for (int j = 0; j < clt->n_layers; j++) {
					blk_u32(b, clt->layer_names_sid[j]);
				}
				for (int j = 0; j < clt->n_columns; j++) {
					blk_u32(b, clt->vertical[j]);
				}
				for (int j = 0; j < clt->n_rows; j++) {
					blk_u32(b, clt->horizontal[j]);
				}

				int idx = 0;
				for (int y = 0; y < clt->n_rows; y++) {
					for (int x = 0; x < clt->n_columns; x++) {
						struct cell* c = &clt->cells[idx++];
						for (int j = 0; j < clt->n_layers; j++) {
							struct stbrp_rect* r = &all_rects[c->all_rects_index + j];
							if (r->w == 0 || r->h == 0) {
								blk_s32(b, -1);
								blk_s32(b, -1);
							} else {
								blk_s32(b, r->x);
								blk_s32(b, r->y);
							}
						}
					}
				}
			} break;
			default:
				assert(!"unknown type");
			}
			assert(b != NULL);
			blk_finalize(b, f);
		}

		{
			struct blk* b = blk_new("BTMP");
			blk_u32(b, atlas_width);
			blk_u32(b, atlas_height);
			blk_u32(b, 1); // uncompressed
			blk_append(b, bitmap, atlas_width*atlas_height);
			blk_finalize(b, f);
		}

		for (struct blk* xb = xblks; xb != NULL; xb = xb->next) blk_finalize(xb, f);

		for (struct userblk* ub = userblks; ub != NULL; ub = ub->next) {
			struct blk* b = blk_new("USER");
			blk_u32(b, ub->type_sid);
			blk_u32(b, ub->name_sid);
			unsigned char buffer[65536];
			FILE* sf = fopen(ub->srcpath, "r");
			assert(sf != NULL);
			while (!feof(sf)) {
				size_t read = fread(buffer, 1, sizeof(buffer), sf);
				blk_append(b, buffer, read);
			}
			fclose(sf);
			blk_finalize(b, f);
		}

		strtbl_finalize(&strtbl, f);

		/*
		TODO
		 - glyph/cell table aliases
		*/

		fclose(f);
	} else {
		assert(!"unhandled outfile extension");
	}
	return 1;
}

int main(int argc, char** argv)
{
	if (argc < 3) {
		fprintf(stderr, "usage: %s <out.atls> <file> [file]...\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	int n = argc - 2;

	for (int sz = 512; sz <= 8192; sz <<= 1) {
		if (try_sz(sz, argv[1], n, argv + 2)) {
			exit(EXIT_SUCCESS);
		}
	}

	exit(EXIT_FAILURE);
}
