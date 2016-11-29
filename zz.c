#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "zz.h"

const u64 err_value = 0xdeadbeefcafebabe;

/*
NOTE (TODO) compile zz.c with ZZ_IDENT_DEBUG defined to enable identifier debugging;
written values are prefixed with an identifier ("ident" argument), and when
reading values the identifier is checked ("expected_ident" argument). this also
means that files written with ZZ_IDENT_DEBUG cannot be read without, and vice
versa.
*/
#ifdef ZZ_IDENT_DEBUG
static void write_ident()
{
	// XXX TODO
}
static void expect_ident()
{
	// XXX TODO
}
#else
static void write_ident()
{
}
static void expect_ident()
{
}
#endif




static u8 read_u8(struct zz* zz)
{
	if (zz_error(zz)) return err_value;

	u8 value;
	if (fread(&value, 1, 1, zz->file) == 0) {
		zz->read_error = 1;
		return err_value;
	}
	return value;
}

static u64 read_vu(struct zz* zz)
{
	if (zz_error(zz)) return err_value;
	u64 value = 0;
	u8 byte;
	int more;
	int shift = 0;
	do {
		if (fread(&byte, 1, 1, zz->file) == 0) {
			zz->read_error = 1;
			return err_value;
		}
		value |= (byte & 0x7f) << shift;
		more = byte & 0x80;
		shift += 7;
	} while (more);
	return value;
}

static void write_data(struct zz* zz, void* data, u64 data_sz)
{
	if (zz_error(zz) || data_sz == 0) return;
	if (fwrite(data, data_sz, 1, zz->file) == 0) zz->write_error = 1;
}

static void write_u8(struct zz* zz, u8 value)
{
	if (zz_error(zz)) return;
	if (fwrite(&value, 1, 1, zz->file) == 0) zz->write_error = 1;
}

static void write_vu(struct zz* zz, u64 value)
{
	if (zz_error(zz)) return;
	int more;
	do {
		more = value >= 0x80;
		u8 wval = (value & 0x7f) | (more << 7);
		if (fwrite(&wval, 1, 1, zz->file) == 0) {
			zz->write_error = 1;
			return;
		}
		value >>= 7;
	} while (more);
}

int zz_open(struct zz* zz, char* path, int mode, struct zz_header* header)
{
	memset(zz, 0, sizeof *zz);

	char* fmode = NULL;
	if (mode == ZZ_MODE_READONLY) {
		fmode = "rb";
		zz->read = 1;
	} else if (mode == ZZ_MODE_TRUNC) {
		fmode = "wb";
		zz->write = 1;
	} else if (mode == ZZ_MODE_PATCH) {
		fmode = "ab";
		zz->read = 1;
		zz->write = 1;
	} else {
		assert(!"invalid mode");
	}
	assert(fmode != NULL);

	zz->file = fopen(path, fmode);
	if (zz->file == NULL) return -1;

	fseek(zz->file, 0, SEEK_END);
	zz->file_sz = ftell(zz->file);
	if (zz->file_sz > 0) {
		fseek(zz->file, 0, SEEK_SET);
		header->twocc[0] = read_u8(zz);
		header->twocc[1] = read_u8(zz);
		header->version = read_vu(zz);
		header->xcc = read_u8(zz);
		if (zz_error(zz)) {
			fclose(zz->file);
			return -1;
		}
	} else if (zz->write) {
		assert(ftell(zz->file) == 0);
		write_u8(zz, header->twocc[0]);
		write_u8(zz, header->twocc[1]);
		write_vu(zz, header->version);
		write_u8(zz, header->xcc);
		if (zz_error(zz)) {
			fclose(zz->file);
			return -1;
		}
	} else {
		// readonly and sz==0
		fclose(zz->file);
		return -1;
	}

	zz->first_blk = ftell(zz->file);

	return 0;
}

int zz_close(struct zz* zz)
{
	assert(zz->file != NULL);
	fclose(zz->file);
	memset(zz, 0, sizeof *zz);
	return 0;
}

int zz_error(struct zz* zz)
{
	return zz->read_error || zz->write_error;
}

void zz_rblk_iter_init(struct zz* zz, struct zz_rblk_iter* it)
{
	memset(it, 0, sizeof *it);
	it->parent = zz;
	it->cursor = zz->first_blk;
}

int zz_rblk_iter_next(struct zz_rblk_iter* it, struct zz_rblk* rblk)
{
	struct zz* zz = it->parent;
	if (it->cursor >= zz->file_sz) {
		if (it->cursor > zz->file_sz) zz->read_error = 1;
		return 0;
	}

	fseek(zz->file, it->cursor, SEEK_SET);

	memset(rblk, 0, sizeof *rblk);
	rblk->parent = zz;
	rblk->usrtype = read_vu(zz);
	rblk->flags = read_vu(zz);
	rblk->_internal_size = read_vu(zz);
	rblk->data_offset = ftell(zz->file);
	if (rblk->flags == 0) {
		rblk->size = rblk->_internal_size;
	} else {
		assert(!"TODO decompression");
	}

	it->cursor = rblk->data_offset + rblk->_internal_size;

	return 1;
}

u8 rblk_u8(struct zz_rblk* rblk)
{
	if (rblk->error) return err_value;
	if (rblk->cursor >= rblk->size) {
		rblk->error = 1;
		return err_value;
	}
	fseek(rblk->parent->file, rblk->data_offset + rblk->cursor, SEEK_SET); // XXX is fseek expensive? cache position?
	rblk->cursor++;
	return read_u8(rblk->parent);
}



u8 zz_rblk_u8(struct zz_rblk* rblk, char* expected_ident)
{
	expect_ident(rblk, expected_ident);
	return rblk_u8(rblk);
}

int zz_rblk_u8a(struct zz_rblk* rblk, u8* ary, u64 n, char* expected_ident)
{
	expect_ident(rblk, expected_ident);
	for (int i = 0; i < n; i++) ary[i] = rblk_u8(rblk);
	return rblk->error ? -1 : 0;
}

u16 zz_rblk_u16(struct zz_rblk* rblk, char* expected_ident)
{
	expect_ident(rblk, expected_ident);
	return rblk_u8(rblk) | (rblk_u8(rblk) << 8);
}

u32 zz_rblk_u32(struct zz_rblk* rblk, char* expected_ident)
{
	expect_ident(rblk, expected_ident);
	return rblk_u8(rblk) | (rblk_u8(rblk) << 8) | (rblk_u8(rblk) << 16) | (rblk_u8(rblk) << 24);
}

float zz_rblk_f32(struct zz_rblk* rblk, char* expected_ident)
{
	expect_ident(rblk, expected_ident);
	// XXX I don't think this is safe, god damn floats..
	union {
		u32 i;
		float f;
	} both;
	both.i = zz_rblk_u32(rblk, expected_ident);
	return both.f;
}

s64 zz_rblk_vs(struct zz_rblk* rblk, char* expected_ident)
{
	expect_ident(rblk, expected_ident);
	u64 uval = 0;
	u8 byte;
	int more;
	int shift = 0;
	do {
		if (rblk->error) return err_value;
		byte = rblk_u8(rblk);
		uval |= (byte & 0x7f) << shift;
		more = byte & 0x80;
		shift += 7;
	} while (more);
	if (uval >> (shift-1)) uval |= (((u64)-1) ^ ((1 << shift) - 1)); // sign-extend
	return (s64)uval;
}

u64 zz_rblk_vu(struct zz_rblk* rblk, char* expected_ident)
{
	expect_ident(rblk, expected_ident);
	u64 value = 0;
	u8 byte;
	int more;
	int shift = 0;
	do {
		byte = rblk_u8(rblk);
		value |= (byte & 0x7f) << shift;
		more = byte & 0x80;
		shift += 7;
	} while (more);
	return value;
}

int zz_rblk_is_eob(struct zz_rblk* rblk)
{
	return rblk->cursor == rblk->size;
}

static void new_blk(struct zz* zz, u64 usrtype, int compression, u64 data_sz)
{
	fseek(zz->file, 0, SEEK_END); // TODO look for free block that can contain data_sz
	write_vu(zz, usrtype);
	u64 flags = 0;
	//if (compression) flags |= 1; // TODO
	write_vu(zz, flags);
	write_vu(zz, data_sz);
}


#if 0
int zz_emit_data_blk(struct zz* zz, u64 usrtype, void* data, u64 data_sz, int compression)
{
	// TODO compression
	new_blk(zz, usrtype, compression, data_sz);
	write_data(zz, data, data_sz);
	return zz_error(zz) ? -1 : 0;
}
#endif

static int wblk_realloc(struct zz_wblk* wblk)
{
	if (wblk->buf == NULL && wblk->buf_cap == 0) return 0;
	void* p = realloc(wblk->buf, wblk->buf_cap);
	if (p == NULL) return -1;
	wblk->buf = p;
	return 0;
}

static int wblk_chk_cap(struct zz_wblk* wblk)
{
	int do_realloc = 0;
	while (wblk->buf_sz > wblk->buf_cap) {
		wblk->buf_cap <<= 2;
		do_realloc = 1;
	}
	if (do_realloc) {
		return wblk_realloc(wblk);
	} else {
		return 0;
	}
}

int zz_wblk_create(struct zz_wblk* wblk, u64 usrtype, u64 capacity, int compression)
{
	memset(wblk, 0, sizeof *wblk);
	wblk->usrtype = usrtype;
	wblk->compression = compression;
	wblk->buf_cap = capacity;
	if (wblk_realloc(wblk) == -1) return -1;
	return 0;
}

void zz_wblk_destroy(struct zz_wblk* wblk)
{
	if (wblk->buf) free(wblk->buf);
	memset(wblk, 0, sizeof *wblk);
}

#if 0
static void wblk_u8a(struct zz_wblk* wblk, u8* val, int n)
{
	for (int i = 0; i < n; i++) {
		if (wblk->error) return;
		if (wblk->cursor == wblk->buf_sz) {
			wblk->buf_sz++;
			if (wblk_chk_cap(wblk) == -1) {
				wblk->error = 1;
				return;
			}
		}
		assert(wblk->cursor < wblk->buf_sz);
		((u8*)wblk->buf)[wblk->cursor] = val[i];
		wblk->cursor++;
	}
}
#endif

static void wblk_le(struct zz_wblk* wblk, u64 val, int n_bytes)
{
	for (int i = 0; i < n_bytes; i++) {
		if (wblk->error) return;
		if (wblk->cursor == wblk->buf_sz) {
			wblk->buf_sz++;
			if (wblk_chk_cap(wblk) == -1) {
				wblk->error = 1;
				return;
			}
		}
		assert(wblk->cursor < wblk->buf_sz);
		((u8*)wblk->buf)[wblk->cursor] = val & 0xff;
		val >>= 8;
		wblk->cursor++;
	}
}

static void wblk_u8(struct zz_wblk* wblk, u8 val)
{
	wblk_le(wblk, val, 1);
}

void zz_wblk_u8(struct zz_wblk* wblk, u8 val, char* ident)
{
	write_ident(wblk, ident);
	wblk_u8(wblk, val);
}

void zz_wblk_u8a(struct zz_wblk* wblk, u8* ary, u64 n, char* ident)
{
	write_ident(wblk, ident);
	for (u64 i = 0; i < n; i++) wblk_u8(wblk, ary[i]);
}

void zz_wblk_u16(struct zz_wblk* wblk, u16 val, char* ident)
{
	write_ident(wblk, ident);
	wblk_le(wblk, val, 2);
}

void zz_wblk_u32(struct zz_wblk* wblk, u32 val, char* ident)
{
	write_ident(wblk, ident);
	wblk_le(wblk, val, 4);
}

void zz_wblk_f32(struct zz_wblk* wblk, float val, char* ident)
{
	write_ident(wblk, ident);
	// XXX I don't think this is safe, god damn floats..
	union {
		u32 i;
		float f;
	} both;
	both.f = val;
	zz_wblk_u32(wblk, both.i, ident);
}


void zz_wblk_vs(struct zz_wblk* wblk, s64 val, char* ident)
{
	write_ident(wblk, ident);
	for (;;) {
		u8 b7 = val & 0x7f;
		val /= 0x80; // right-shift for negative values is implementation-defined :-/
		wblk_u8(wblk, b7 | (val ? 0x80 : 0));
		if (!val) break;
	}
}

void zz_wblk_vu(struct zz_wblk* wblk, u64 val, char* ident)
{
	write_ident(wblk, ident);
	for (;;) {
		u8 b7 = val & 0x7f;
		val >>= 7;
		wblk_u8(wblk, b7 | (val ? 0x80 : 0));
		if (!val) break;
	}
}

int zz_write_wblk(struct zz* zz, struct zz_wblk* wblk)
{
	if (wblk->error) return -1;
	new_blk(zz, wblk->usrtype, wblk->compression, wblk->buf_sz);
	write_data(zz, wblk->buf, wblk->buf_sz);
	return zz_error(zz) ? -1 : 0;
}



int zz_strtbl_create(struct zz_strtbl* t)
{
	memset(t, 0, sizeof *t);
	return 0;
}

void zz_strtbl_destroy(struct zz_strtbl* t)
{
	if (t->strings) free(t->strings);
	memset(t, 0, sizeof *t);
}

int zz_strtbl_addn(struct zz_strtbl* t, char* str, int n)
{
	int required = t->size + n + 1;
	int prev_cap = t->_capacity;
	while (t->_capacity < required) {
		if (t->_capacity == 0) {
			t->_capacity = 4096;
		} else {
			t->_capacity <<= 2;
		}
	}
	if (t->_capacity != prev_cap) {
		void* np = realloc(t->strings, t->_capacity);
		if (np == NULL) {
			if (t->strings) free(t->strings);
			return -1;
		}
		t->strings = np;
	}
	memcpy(t->strings + t->size, str, n);
	t->strings[t->size + n] = 0;
	t->size += (n + 1);
	return 0;
}

int zz_strtbl_add(struct zz_strtbl* t, char* str)
{
	return zz_strtbl_addn(t, str, strlen(str));
}

int zz_strtbl_optimize(struct zz_strtbl* t)
{
	/* TODO order strings by add-count so that more frequent strings have a
	 * lower index (filesize optimization through variable width integers)
	 * */
	return 0;
}

int zz_strtbl_find(struct zz_strtbl* t, char* str)
{
	/* XXX assert table has been optimized? doesn't matter currently, but
	 * it might matter after implementing zz_strtbl_optimize */
	size_t n = strlen(str);
	int cursor = 0;
	while (cursor < t->size) {
		int remaining = t->size - cursor;
		if (n >= remaining) return -1;
		char* c = (char*)t->strings + cursor;
		size_t cn = strnlen(c, remaining);
		if (n == cn && memcmp(c, str, n) == 0) return cursor;
		cursor += cn + 1;
	}
	return -1;
}
