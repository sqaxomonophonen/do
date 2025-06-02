#ifndef BUFSTREAM_H

// https://fgiesen.wordpress.com/2011/11/21/buffer-centric-io/

#include <stdint.h>
#include <stddef.h>
#include <assert.h>

#include "jio.h"
#include "binary.h"
#include "leb128.h"

enum bufstream_type {
	BUFSTREAM_GENERIC=0,
	BUFSTREAM_JIO,
};

struct bufstream {
	const uint8_t* start;
	const uint8_t* end;
	const uint8_t* cursor;
	int64_t offset;
	// offset is incremented for each u8 read. bufstream_init_*() functions
	// with an `offset` argument set this value, but it's not otherwise
	// internally used for anything, so you can set it to what you want (for
	// housekeeping)
	int error;
	int (*refill)(struct bufstream*);
	enum bufstream_type type;
	union {
		struct {
			struct jio* jio;
			int64_t offset;
			void* buf;
			size_t bufsize;
		} jio;
	};
};

void bufstream_init_from_memory(struct bufstream*, const void*, int64_t);
void bufstream_init_from_jio(struct bufstream* bs, struct jio*, int64_t offset, void* buf, size_t bufsize);

static inline uint8_t bufstream_read_u8(struct bufstream* bs)
{
	if (bs->cursor < bs->end) {
		++bs->offset;
		return *(bs->cursor++);
	}
	assert(bs->cursor == bs->end);
	bs->refill(bs);
	assert(bs->cursor < bs->end);
	++bs->offset;
	return *(bs->cursor++);
}

static inline void bufstream_read(struct bufstream* bs, uint8_t* data, size_t count)
{
	for (size_t i=0; i<count; ++i) data[i] = bufstream_read_u8(bs);
}

static inline void bufstream_skip(struct bufstream* bs, size_t count)
{
	for (size_t i=0; i<count; ++i) bufstream_read_u8(bs);
}

static inline uint16_t bufstream_read_leu16(struct bufstream* bs)
{
	uint8_t raw[2];
	bufstream_read(bs, raw, sizeof raw);
	const uint8_t* p = raw;
	uint16_t value = leu16_pdecode(&p);
	assert(p == (raw+2));
	return value;
}

static inline int64_t bufstream_read_leu64(struct bufstream* bs)
{
	uint8_t raw[8];
	bufstream_read(bs, raw, sizeof raw);
	const uint8_t* p = raw;
	uint64_t value = leu64_pdecode(&p);
	assert(p == (raw+8));
	return value;
}

static uint8_t bufstream_read1_fn(void* userdata)
{
	return bufstream_read_u8((struct bufstream*)userdata);
}

static inline int64_t bufstream_read_leb128(struct bufstream* bs)
{
	return leb128_decode_int64(bufstream_read1_fn, bs);
}

// shorthands
static inline void bs_read(struct bufstream* bs, uint8_t* data, size_t count) { bufstream_read(bs, data, count); }
static inline uint16_t bs_read_u8(struct bufstream* bs)     { return bufstream_read_u8(bs); }
static inline uint16_t bs_read_leu16(struct bufstream* bs)  { return bufstream_read_leu16(bs); }
static inline int64_t  bs_read_leu64(struct bufstream* bs)  { return bufstream_read_leu64(bs); }
static inline int64_t  bs_read_leb128(struct bufstream* bs) { return bufstream_read_leb128(bs); }


#define BUFSTREAM_H
#endif
