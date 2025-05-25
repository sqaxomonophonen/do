#ifndef JIO_H

// journal I/O:
//  - can only write at end (jio_append())
//  - can read from anywhere (jio_pread())
//  - writes are typically non-blocking, but may block until there's enough
//    space in the ring buffer
//  - reads are blocking unless the requested data is already in memory.
//    special care is taken to read from the ring buffer, so that recently
//    appended data is immediately visible to jio_pread()

// TODO blocking pwrite()-like writes are useful for streaming .wav-renders
// (because the size must be written into the .wav-header). but it's rare
// enough that we shouldn't optimize for it (figure out something that doesn't
// complicate jio.c much)

// TODO tunable/settable integrity/fdatasync()? DO_JAM_JOURNAL wants frequent
// syncs, but snapshotcache.data is fine with none, even though both are
// superficially the same usecase.

#include <stdint.h>
#include <stddef.h>

#include "leb128.h"
#include "binary.h"

enum jio_open_mode {
	JIO_OPEN = 1,
	JIO_OPEN_OR_CREATE,
	JIO_CREATE,
};

#define LIST_OF_JIO_ERRORS \
	X(JIO_ERROR             , "an error occurred"       , -21000) \
	X(JIO_NOT_FOUND         , "file not found"          , -21001) \
	X(JIO_ALREADY_EXISTS    , "file already exists"     , -21002) \
	X(JIO_NOT_PERMITTED     , "operation not permitted" , -21003) \
	X(JIO_READ_ERROR        , "read error"              , -21004) \
	X(JIO_READ_OUT_OF_RANGE , "read out of range"       , -21005)

enum jio_error {
	#define X(ENUM,_MSG,ID) ENUM=ID,
	LIST_OF_JIO_ERRORS
	#undef X
};
struct jio;

const char* jio_error_to_string(int error);
// returns static error string for error id, or NULL if not one of the ones in
// the LIST_OF_JIO_ERRORS X macros

static inline const char* jio_error_to_string_safe(int error)
{
	const char* s = jio_error_to_string(error);
	return s ? s : "(not a jio error)";
}

struct jio* jio_open(const char* path, enum jio_open_mode, int ringbuf_size_log2, int* out_error);
int jio_close(struct jio*);
int64_t jio_get_size(struct jio*);
void jio_append(struct jio*, const void* ptr, int64_t size);
int jio_pread(struct jio*, void* ptr, int64_t size, int64_t offset);
int jio_get_error(struct jio*);
int jio_get_num_block_sleeps(struct jio*);

static inline void jio_append_u8(struct jio* jio, uint8_t v)
{
	jio_append(jio, &v, 1);
}

static inline void jio_append_leu64(struct jio* jio, int64_t v)
{
	uint8_t buf[8];
	leu64_encode(buf, v);
	jio_append(jio, buf, 8);
}

static inline void jio_append_leb128(struct jio* jio, int64_t v)
{
	uint8_t buf0[LEB128_MAX_LENGTH];
	uint8_t* buf1 = leb128_encode_int64_buf(buf0, v);
	jio_append(jio, buf0, buf1-buf0);
}

static inline int jio_ppread(struct jio* jio, void* ptr, int64_t size, int64_t* offset)
{
	int err = jio_get_error(jio);
	if (err < 0) return err;
	err = jio_pread(jio, ptr, size, *offset);
	if (err >= 0) *offset += size;
	return err;
}

static inline int64_t jio_ppread_leu64(struct jio* jio, int64_t* offset)
{
	if (jio_get_error(jio) < 0) return 0;
	uint8_t buf[8];
	jio_pread(jio, buf, 8, *offset);
	(*offset) += 8;
	return leu64_decode(buf);
}

static inline int64_t jio_pread_leu64(struct jio* jio, int64_t offset)
{
	return jio_ppread_leu64(jio, &offset);
}

struct jio_read1_ctx {
	struct jio* jio;
	int64_t offset;
};

static uint8_t jio_read1_fn(void* userdata)
{
	struct jio_read1_ctx* ctx = userdata;
	if (jio_get_error(ctx->jio) < 0) return 0;
	uint8_t b;
	jio_pread(ctx->jio, &b, 1, ctx->offset);
	++ctx->offset;
	return b;
}

// XXX even assuming buffered reads, this isn't very efficient?
static inline int64_t jio_ppread_leb128(struct jio* jio, int64_t* offset)
{
	struct jio_read1_ctx ctx = {.jio=jio , .offset=*offset};
	const int64_t v = leb128_decode_int64(jio_read1_fn, &ctx);
	*offset = ctx.offset;
	return v;
}

static inline int64_t jio_pread_leb128(struct jio* jio, int64_t offset)
{
	return jio_ppread_leb128(jio, &offset);
}

static inline uint8_t jio_ppread_u8(struct jio* jio, int64_t* offset)
{
	if (jio_get_error(jio) < 0) return 0;
	uint8_t b;
	jio_pread(jio, &b, 1, *offset);
	++(*offset);
	return b;
}

static inline uint8_t jio_pread_u8(struct jio* jio, int64_t offset)
{
	return jio_ppread_u8(jio, &offset);
}


void jio_init(void);
void jio_thread_run(void);

#define JIO_H
#endif
