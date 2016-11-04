/*
ZZ read/write library. see ZZ-FORMAT for a description of what ZZ is.
*/
#ifndef ZZ_H

#include <stdio.h>
#include "dtypes.h"

struct zz {
	FILE* file;
	int read:1;
	int write:1;
	int read_error;
	int write_error;
};

struct zz_head {
	char twocc[2];
	char xcc;
	u64 version;
};

struct zz_wblk {
	u64 usrtype;
	struct zz* parent;
	u64 cursor;
	u64 buf_cap, buf_sz;
	int compression;
	void* buf;
	int error;
};

struct zz_rblk {
	int usrtype;
	u64 size;
	int error; // read past EOB, corrupted compression data,...
	/* XXX transparent decompression? */
};

struct zz_rblk_iter {
	struct zz* parent;
	u64 cursor;
};

#define ZZ_MODE_READONLY 1
#define ZZ_MODE_TRUNC 2
#define ZZ_MODE_PATCH 3

int zz_open(struct zz*, char* path, int mode, struct zz_head*);
int zz_close(struct zz*);

void zz_new_rblk_iter(struct zz*, struct zz_rblk_iter*);
int zz_rblk_iter_next(struct zz_rblk_iter*, struct zz_rblk*);
int zz_rblk_free(struct zz_rblk*); // frees/deletes block in PATCH mode; will abort() if not operating in PATCH mode
//int zz_rblk2wblk(struct zz_rblk*, struct zz_wblk*); // yields fixed size wblk (can't grow). will abort() unless if in PATCH mode

u8 zz_rblk_u8(struct zz_rblk*);
u16 zz_rblk_u16(struct zz_rblk*);
u32 zz_rblk_u32(struct zz_rblk*);
s64 zz_rblk_vs(struct zz_rblk*);
u64 zz_rblk_vu(struct zz_rblk*);
int zz_rblk_is_eob(struct zz_rblk*);


int zz_new_stream_wblk(struct zz*, struct zz_wblk*, u64 usrtype, u64 size);
int zz_new_prep_wblk(struct zz*, struct zz_wblk*, u64 usrtype, u64 capacity, int compression);
int zz_emit_data_blk(struct zz*, u64 usrtype, void* data, u64 data_sz, int compression);

u64 zz_wblk_tell(struct zz_wblk*);
int zz_wblk_seek(struct zz_wblk*, u64 cursor);

void zz_wblk_u8(struct zz_wblk*, u8 val);
void zz_wblk_u16(struct zz_wblk*, u16 val);
void zz_wblk_u32(struct zz_wblk*, u32 val);
void zz_wblk_vs(struct zz_wblk*, s64 val);
void zz_wblk_vu(struct zz_wblk*, u64 val);
int zz_wblk_end(struct zz_wblk*);


#define ZZ_H
#endif
