#ifndef IO_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <assert.h>
#include <string.h>

#include "binary.h"
#include "leb128.h"

struct io;

typedef int32_t io_handle;
typedef int32_t io_status;

enum ioerr {
	IOERR_ERROR = -20000,
	IOERR_NOT_FOUND,
	IOERR_ALREADY_EXISTS,
	IOERR_NOT_ALLOWED,
	IOERR_TOO_MANY_FILES,
};

struct io_echo {
	int32_t ia32;
	int32_t ib32;
};

struct iosub_open {
	struct io_echo echo;
	char* path;

	unsigned  read   :1;
	unsigned  write  :1;
	unsigned  create :1;
	// open with read/write/create mode

	unsigned  free_path_after_use :1;
	// backend must `free(path)` after use (transfer of ownership)
};

struct iosub_close {
	struct io_echo echo;
	io_handle handle;
};

struct iosub_pwrite {
	struct io_echo echo;
	io_handle handle;
	void*  data;
	size_t size;
	size_t offset;

	unsigned      no_reply :1;
	// backend will not reply with an io_event. suggested for log files. has no
	// effect with io_pwrite_now().

	unsigned      free_data_after_use :1;
	// backend must `free(data)` after use (transfer of ownership)

	unsigned      sync :1;
	// sync data to disk before replying
};

struct iosub_pread {
	struct io_echo echo;
	io_handle handle;
	void*  data;
	size_t size;
	size_t offset;
	// XXX do we want a mode where the backend allocates the data pointer?
};

struct io_event {
	struct io_echo echo; // `echo` is passed as-is from struct iosub_* submissions
	io_handle handle;
	io_status status;
};

size_t io_get_size(struct io*, io_handle handle);


// functions with `_now()` suffix are "synchronous" / "blocking": they execute
// the operation immediately and returns the status (sometimes io_event). they
// work exactly like their asynchronous counterparts.

void io_open(struct io*, struct iosub_open);
struct io_event io_open_now(struct io*, struct iosub_open);
// status is negative on error (IOERR_*), otherwise 0

void io_close(struct io*, struct iosub_close);
io_status io_close_now(struct io*, struct iosub_close);
static inline io_status ioi_close_now(struct io* io, io_handle h)
{
	return io_close_now(io, ((struct iosub_close){ .handle = h }));
}
// status is negative on error (IOERR_*), otherwise 0

void io_pwrite(struct io*, struct iosub_pwrite);
io_status io_pwrite_now(struct io*, struct iosub_pwrite);
// status is negative on error (IOERR_*), otherwise 0

void io_pread(struct io*, struct iosub_pread);
io_status io_pread_now(struct io*, struct iosub_pread);
// status is negative on error (IOERR_*), otherwise 0

int io_poll(struct io*, struct io_event*);

void io_init(int max_num_ios);
struct io* io_new(int ringbuf_size_log2, int max_num_files);
void io_run(void);

// when using io_appender async, this value is reserved for the echo.ia32 field
#define IO_IA32_APPENDER (-501010)

struct io_inflight {
	int64_t size;
	int32_t echo_ib32;  // see `struct io_echo` / `ack_ib32_sequence`
	unsigned    ack :1; // set if ack'd out of order (otherwise it's just removed)
};

struct io_appender {
	struct io* io;
	io_handle handle;
	size_t file_cursor;
	int64_t head, tail;
	int num_inflights, inflight_cap, ack_ib32_sequence;
	struct io_inflight* inflight_buf;
	uint8_t* ringbuf;
	int ringbuf_cap_log2;
	unsigned  flush_on_buffer_full  :1;
	unsigned  panic_on_error        :1;
	unsigned  sync                  :1; // passed into iosub_pwrite.sync
	// these flags can be changed after io_appender_init()
};

static inline int io_appender_is_initialized(struct io_appender* a)
{
	return (a->ringbuf != NULL);
}

void io_appender_init(struct io_appender*, struct io*, io_handle handle, int ringbuf_cap_log2, int inflight_cap);
int  io_appender_flush_now(struct io_appender*);
void io_appender_flush(struct io_appender*);
int  io_appender_write_raw(struct io_appender*, void* data, size_t size);
static inline int io_appender_write_u8(struct io_appender* a, uint8_t value)
{
	uint8_t data[1] = {value};
	return io_appender_write_raw(a, data, sizeof data);
}
int  io_appender_write_leu64(struct io_appender*, uint64_t value);
int  io_appender_write_leb128(struct io_appender*, int64_t value);
int  io_appender_ack(struct io_appender*, struct io_event*);
// returns 1 if event contained something acknowledgable (ia32==IO_IA32_APPENDER
// for starters), otherwise 0 (suggesting you should handle the event
// elsewhere)


struct io_bufread {
	struct io* io;
	io_handle handle;
	size_t bufsize;
	uint8_t* buf;
	size_t file_cursor;
	// internal/feedback
	io_status error;
	unsigned end_of_file :1;
	uint8_t *bufcur, *bufend;
};

void io_bufread_init(struct io_bufread*, struct io*, size_t bufsize, io_handle);

io_status io_bufread_raw(struct io_bufread* b, void* dst, size_t sz);

static inline uint8_t io_bufread_u8(struct io_bufread* b)
{
	uint8_t v=0;
	io_bufread_raw(b, &v, 1);
	return v;
}

static uint8_t io_bufread_u8_wrap(void* userdata)
{
	return io_bufread_u8((struct io_bufread*)userdata);
}

void io_bufread_seek(struct io_bufread*, int64_t pos);
int64_t io_bufread_tell(struct io_bufread*);

static inline int64_t io_bufread_leb128_i64(struct io_bufread* b)
{
	return leb128_decode_int64(io_bufread_u8_wrap, b);
}

static inline uint64_t io_bufread_leu64(struct io_bufread* b)
{
	uint8_t buf[8];
	io_bufread_raw(b, buf, sizeof buf);
	return leu64_decode(buf);
}

const char* ioerrstr(enum ioerr);

#define IO_H
#endif
