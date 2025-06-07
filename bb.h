#ifndef BB_H // binary builder

// NOTE: depends on stb_ds.h, but it can be included+configured as either
// stb_ds.h or stb_ds_sysalloc.h making it problematic to include it here.
// instead you should include bb.h /after/ stb_ds.h or stb_ds_sysalloc.h

#include <stdint.h>
#include <assert.h>
#include <string.h>

#include "binary.h"
#include "leb128.h"

static inline void bb_append(uint8_t** bbarr, void* data, size_t sz)
{
	uint8_t* p0 = arraddnptr(*bbarr, sz);
	memcpy(p0, data, sz);
}

static inline void bb_append_u8(uint8_t** bbarr, uint8_t value)
{
	arrput(*bbarr, value);
}

static inline void bb_append_leu16(uint8_t** bbarr, uint16_t value)
{
	uint8_t* p0 = arraddnptr(*bbarr, 2);
	uint8_t* p = p0;
	leu16_pencode(&p, value);
	assert(p == (p0+2));
}

static inline void bb_append_leu64(uint8_t** bbarr, uint64_t value)
{
	uint8_t* p0 = arraddnptr(*bbarr, 8);
	uint8_t* p = p0;
	leu64_pencode(&p, value);
	assert(p == (p0+8));
}

static inline void bb_append_leb128(uint8_t** bbarr, int64_t value)
{
	uint8_t buf0[LEB128_MAX_LENGTH];
	uint8_t* buf1 = leb128_encode_int64_buf(buf0, value);
	bb_append(bbarr, buf0, buf1-buf0);
}

static inline void* bb_dup2plain(uint8_t** bbarr)
{
	const size_t s = arrlen(*bbarr);
	void* mem = malloc(s);
	memcpy(mem, *bbarr, s);
	return mem;
}

#define BB_H
#endif
