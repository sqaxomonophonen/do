#ifndef IO_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <assert.h>
#include <string.h>

struct io;

typedef int32_t io_handle;
typedef int32_t io_status;

enum ioerr {
	IOERR_ERROR = -30000,
	IOERR_NOT_FOUND,
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

void io_appender_init(struct io_appender*, struct io*, io_handle handle, int ringbuf_cap_log2, int inflight_cap);
int  io_appender_flush_now(struct io_appender*);
void io_appender_flush(struct io_appender*);
int  io_appender_write_raw(struct io_appender*, void* data, size_t size);
int  io_appender_write_leb128(struct io_appender*, int64_t value);
int  io_appender_ack(struct io_appender*, struct io_event*);
// returns 1 if event contained something acknowledgable (ia32==IO_IA32_APPENDER
// for starters), otherwise 0 (suggesting you should handle the event
// elsewhere)

struct io_bufread {
	struct io* io;
	io_handle handle;
	size_t file_cursor;
	uint8_t* buf;
	size_t bufsize;
	io_status error;
	unsigned end_of_file :1;
	// derived
	uint8_t *_bufcur, *_bufend;
};

static inline io_status io_bufread_read(struct io_bufread* b, void* dst, size_t sz)
{
	void* dp = dst;
	size_t remain_read = sz;
	while (remain_read > 0) {
		if (b->error < 0) return b->error;
		if (b->end_of_file) return 0;
		if (b->_bufcur == NULL) {
			const size_t remain_file = (io_get_size(b->io, b->handle) - b->file_cursor);
			if (remain_file == 0) {
				b->end_of_file = 1;
				return 0;
			}
			const size_t rsz = (remain_file > b->bufsize) ? b->bufsize : remain_file;
			io_status e = io_pread_now(b->io, ((struct iosub_pread){
				.handle = b->handle,
				.data = b->buf,
				.size = rsz,
				.offset = b->file_cursor,
			}));
			if (e < 0) {
				b->error = e;
				return 0;
			}
			b->file_cursor += rsz;
			b->_bufcur = b->buf;
			b->_bufend = b->buf + rsz;
		}
		const size_t can_copy = (b->_bufend - b->buf);
		const size_t n = (remain_read > can_copy) ? can_copy : remain_read;
		memcpy(dp, b->_bufcur, n);
		dp += n;
		remain_read -= n;
		b->_bufcur += n;
		assert(b->_bufcur <= b->_bufend);
		if (b->_bufcur == b->_bufend) {
			b->_bufcur = NULL;
		}
	}
	return 0;
}

static inline uint8_t io_bufread_next(struct io_bufread* b)
{
	uint8_t v=0;
	io_bufread_read(b, &v, 1);
	return v;
}

#define IO_H
#endif
