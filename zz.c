#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "zz.h"

const u64 err_value = 0xdeadbeefcafebabe;

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

int zz_open(struct zz* zz, char* path, int mode, struct zz_head* head)
{
	memset(zz, 0, sizeof(*zz));

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
		head->twocc[0] = read_u8(zz);
		head->twocc[1] = read_u8(zz);
		head->version = read_vu(zz);
		head->xcc = read_u8(zz);
		if (zz_error(zz)) {
			fclose(zz->file);
			return -1;
		}
	} else if (zz->write) {
		assert(ftell(zz->file) == 0);
		write_u8(zz, head->twocc[0]);
		write_u8(zz, head->twocc[1]);
		write_vu(zz, head->version);
		write_u8(zz, head->xcc);
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
	memset(zz, 0, sizeof(*zz));
	return 0;
}

int zz_error(struct zz* zz)
{
	return zz->read_error || zz->write_error;
}

void zz_new_rblk_iter(struct zz* zz, struct zz_rblk_iter* it)
{
	memset(it, 0, sizeof(*it));
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

	memset(rblk, 0, sizeof(*rblk));
	rblk->parent = zz;
	rblk->usrtype = read_vu(zz);
	rblk->flags = read_vu(zz);
	rblk->data_size = read_vu(zz);
	rblk->data_offset = ftell(zz->file);
	if (rblk->flags == 0) {
		rblk->size = rblk->data_size;
	} else {
		assert(!"TODO decompression");
	}

	it->cursor = rblk->data_offset + rblk->data_size;

	return 1;
}

u8 zz_rblk_u8(struct zz_rblk* rblk)
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

int zz_rblk_u8a(struct zz_rblk* rblk, u8* ary, u64 n)
{
	for (int i = 0; i < n; i++) ary[i] = zz_rblk_u8(rblk);
	return rblk->error ? -1 : 0;
}

s64 zz_rblk_vs(struct zz_rblk* rblk)
{
	u64 uval = 0;
	u8 byte;
	int more;
	int shift = 0;
	do {
		if (rblk->error) return err_value;
		byte = zz_rblk_u8(rblk);
		uval |= (byte & 0x7f) << shift;
		more = byte & 0x80;
		shift += 7;
	} while (more);
	if (uval >> (shift-1)) uval |= (((u64)-1) ^ ((1 << shift) - 1)); // sign-extend
	return (s64)uval;
}

u64 zz_rblk_vu(struct zz_rblk* rblk)
{
	if (rblk->error) return err_value;
	u64 value = 0;
	u8 byte;
	int more;
	int shift = 0;
	do {
		byte = zz_rblk_u8(rblk);
		value |= (byte & 0x7f) << shift;
		more = byte & 0x80;
		shift += 7;
	} while (more);
	return value;
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


int zz_emit_data_blk(struct zz* zz, u64 usrtype, void* data, u64 data_sz, int compression)
{
	// TODO compression
	new_blk(zz, usrtype, compression, data_sz);
	write_data(zz, data, data_sz);
	return zz_error(zz) ? -1 : 0;
}

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

int zz_new_prep_wblk(struct zz* zz, struct zz_wblk* wblk, u64 usrtype, u64 capacity, int compression)
{
	memset(wblk, 0, sizeof(*wblk));
	wblk->usrtype = usrtype;
	wblk->compression = compression;
	wblk->parent = zz;
	wblk->buf_cap = capacity;
	if (wblk_realloc(wblk) == -1) return -1;
	return 0;
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

void zz_wblk_u8(struct zz_wblk* wblk, u8 val)
{
	wblk_le(wblk, val, 1);
}

void zz_wblk_u16(struct zz_wblk* wblk, u16 val)
{
	wblk_le(wblk, val, 2);
}

void zz_wblk_u32(struct zz_wblk* wblk, u32 val)
{
	wblk_le(wblk, val, 4);
}

void zz_wblk_vs(struct zz_wblk* wblk, s64 val)
{
	for (;;) {
		u8 b7 = val & 0x7f;
		val /= 0x80; // right-shift for negative values is implementation-defined :-/
		zz_wblk_u8(wblk, b7 | (val ? 0x80 : 0));
		if (!val) break;
	}
}

void zz_wblk_vu(struct zz_wblk* wblk, u64 val)
{
	for (;;) {
		u8 b7 = val & 0x7f;
		val >>= 7;
		zz_wblk_u8(wblk, b7 | (val ? 0x80 : 0));
		if (!val) break;
	}
}

int zz_wblk_end(struct zz_wblk* wblk)
{
	if (wblk->error) return -1;
	new_blk(wblk->parent, wblk->usrtype, wblk->compression, wblk->buf_sz);
	write_data(wblk->parent, wblk->buf, wblk->buf_sz);
	return zz_error(wblk->parent) ? -1 : 0;
}

