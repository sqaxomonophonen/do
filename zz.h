/*
ZZ read/write library. see ZZ-FORMAT for a description of what ZZ is.
*/
#ifndef ZZ_H

#include <stdio.h>
#include "dtypes.h"

struct zz {
	FILE* file;
	u64 first_blk, file_sz;
	int read:1;
	int write:1;
	int read_error;
	int write_error;
};

struct zz_header {
	char twocc[2];
	char xcc;
	u64 version;
};

struct zz_wblk {
	u64 usrtype;
	u64 cursor;
	u64 buf_cap, buf_sz;
	int compression;
	void* buf;
	int error;
};

struct zz_rblk {
	struct zz* parent;
	u64 usrtype;
	u64 flags;
	u64 _internal_size;
	u64 size;
	u64 data_offset;
	u64 cursor;
	int error; // read past EOB, corrupted compression data,...
	/* XXX transparent decompression? */
};

struct zz_rblk_iter {
	struct zz* parent;
	u64 cursor;
};

struct zz_strtbl {
	u8* strings;
	int size;
	int _capacity;
};

#define ZZ_MODE_READONLY 1
#define ZZ_MODE_TRUNC 2
#define ZZ_MODE_PATCH 3

int zz_open(struct zz*, char* path, int mode, struct zz_header*);
int zz_close(struct zz*);

int zz_error(struct zz*);

int zz_write_wblk(struct zz*, struct zz_wblk*);

void zz_rblk_iter_init(struct zz*, struct zz_rblk_iter*);
int zz_rblk_iter_next(struct zz_rblk_iter*, struct zz_rblk*);

u8 zz_rblk_u8(struct zz_rblk*, char* expected_ident);
int zz_rblk_u8a(struct zz_rblk*, u8* ary, u64 n, char* expected_ident);
u16 zz_rblk_u16(struct zz_rblk*, char* expected_ident);
u32 zz_rblk_u32(struct zz_rblk*, char* expected_ident);
float zz_rblk_f32(struct zz_rblk*, char* expected_ident);
s64 zz_rblk_vs(struct zz_rblk*, char* expected_ident);
u64 zz_rblk_vu(struct zz_rblk*, char* expected_ident);
int zz_rblk_is_eob(struct zz_rblk*);

int zz_wblk_create(struct zz_wblk*, u64 usrtype, u64 capacity, int compression);
void zz_wblk_destroy(struct zz_wblk*);

u64 zz_wblk_tell(struct zz_wblk*);
int zz_wblk_seek(struct zz_wblk*, u64 cursor);

void zz_wblk_u8(struct zz_wblk*, u8 val, char* ident);
void zz_wblk_u8a(struct zz_wblk*, u8* ary, u64 n, char* ident);
void zz_wblk_u16(struct zz_wblk*, u16 val, char* ident);
void zz_wblk_u32(struct zz_wblk*, u32 val, char* ident);
void zz_wblk_f32(struct zz_wblk*, float val, char* ident);
void zz_wblk_vs(struct zz_wblk*, s64 val, char* ident);
void zz_wblk_vu(struct zz_wblk*, u64 val, char* ident);

int zz_strtbl_create(struct zz_strtbl*);
void zz_strtbl_destroy(struct zz_strtbl*);
int zz_strtbl_addn(struct zz_strtbl*, char* str, int n);
int zz_strtbl_add(struct zz_strtbl*, char* str);
int zz_strtbl_optimize(struct zz_strtbl*);
int zz_strtbl_find(struct zz_strtbl*, char* str);

#define ZZ_H
#endif
