#ifndef IO_H

#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <assert.h>

struct io;

enum ioerr {
	IOERR_ERROR = -30000,
	IOERR_NOT_FOUND,
	IOERR_NOT_ALLOWED,
	IOERR_TOO_MANY_FILES,
};

union io64 {
	int64_t  i64;
	uint64_t u64;
	size_t   size;
	intptr_t intptr;
	void*    ptr;
};
static_assert(sizeof(union io64)==sizeof(int64_t),"");

struct iosub_open {
	union io64 echo;
	char* path;

	unsigned  read   :1;
	unsigned  write  :1;
	unsigned  create :1;
	// open with read/write/create mode

	unsigned  free_path_after_use :1;
	// backend must `free(path)` after use (transfer of ownership)
};

struct iosub_close {
	union io64 echo;
	union io64 handle;
};

struct iosub_pwrite {
	union io64 echo;
	union io64 handle;
	void*  data;
	int64_t size;
	int64_t offset;

	unsigned      no_reply :1;
	// backend will not reply with an io_event (suggested use: log files)

	unsigned      free_data_after_use :1;
	// backend must `free(data)` after use (transfer of ownership)

	unsigned      sync :1;
	// sync data to disk before replying
};

struct iosub_pread {
	union io64 echo;
	union io64 handle;
	void*  data;
	size_t size;
	size_t offset;
	// XXX do we want a mode where the backend allocates the data pointer?
};

struct io_event {
	union io64 echo; // `echo` is passed as-is from struct iosub_* submissions
	union io64 result;
};

// sync
int64_t io_get_size(struct io*, union io64 handle);

void io_open(struct io*, struct iosub_open);
// io_event.result is negative on error (IOERR_*), otherwise a handle

void io_close(struct io*, struct iosub_close);

void io_pwrite(struct io*, struct iosub_pwrite);
void io_pread(struct io*, struct iosub_pread);

int io_poll(struct io*, struct io_event*);

void io_init(int max_num_ios);
struct io* io_new(int ringbuf_size_log2, int max_num_files);
void io_run(void);

#define IO_H
#endif
