#include <string.h>

#include "bufstream.h"

static int bufstream_refill_zeroes(struct bufstream* bs)
{
	static const uint8_t zeroes[1<<8] = {0};
	bs->cursor = bs->start = zeroes;
	bs->end = zeroes + sizeof zeroes;
	return bs->error;
}

static int bufstream_no_refill(struct bufstream* bs)
{
	bs->error = -1;
	bs->refill = bufstream_refill_zeroes;
	return bufstream_refill_zeroes(bs);
}

void bufstream_init_from_memory(struct bufstream* bs, const void* data, int64_t size)
{
	memset(bs, 0, sizeof *bs);
	bs->cursor = bs->start = data;
	bs->end = bs->start + size;
	bs->refill = bufstream_no_refill;
}

static int bufstream_refill_jio(struct bufstream* bs)
{
	assert(bs->type == BUFSTREAM_JIO);
	const int64_t count = bs->jio.bufsize;
	const int n = jio_pread(bs->jio.jio, bs->jio.buf, count, bs->jio.offset);
	if (n < 0) {
		//printf("bufstream read error %d - count=%zd offset=%zd\n", n, count, bs->jio.offset);
		bs->error = n;
		bs->refill = bufstream_refill_zeroes;
		return bufstream_refill_zeroes(bs);
	}
	bs->jio.offset += n;
	bs->cursor = bs->start = bs->jio.buf;
	assert(n <= bs->jio.bufsize);
	bs->end = bs->jio.buf + n;
	return bs->error;
}

void bufstream_init_from_jio(struct bufstream* bs, struct jio* jio, int64_t offset, void* buf, size_t bufsize)
{
	memset(bs, 0, sizeof *bs);
	bs->type = BUFSTREAM_JIO;
	bs->jio.jio = jio;
	bs->jio.offset = offset;
	bs->offset = offset;
	assert(buf != NULL);
	bs->jio.buf = buf;
	assert(bufsize > 0);
	bs->jio.bufsize = bufsize;
	bs->refill = bufstream_refill_jio;
	bs->refill(bs);
}
