#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <threads.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include "io.h"
#include "leb128.h"
#include "util.h"
#include "main.h"

#define MAX_SIZE_LOG2 (28)

enum iosub_type { OPEN, CLOSE, PWRITE, PREAD, };

struct iosub {
	enum iosub_type type;
	union {
		struct iosub_open   open;
		struct iosub_close  close;
		struct iosub_pwrite pwrite;
		struct iosub_pread  pread;
	};
};

struct file {
	int fd;
	_Atomic(size_t) size;
};

struct files {
	int cap, freelist_len, next_id;
	struct file* files;
	int* freelist;
};

struct io {
	_Atomic(unsigned) sub_head, sub_tail;
	struct iosub* subs;

	_Atomic(unsigned) ev_head, ev_tail;
	struct io_event* evs;

	int ringbuf_size_log2;

	struct files files;
};

static struct {
	int cap;
	struct io* ios;
	int* freelist;
	_Atomic(int)* uset;
	int freelist_len;
	int next_id;
	mtx_t alloc_mutex;
} g;

void io_init(int max_num_ios)
{
	g.cap = max_num_ios;
	g.ios = calloc(g.cap, sizeof *g.ios);
	g.freelist = calloc(g.cap, sizeof *g.freelist);
	g.uset = calloc(g.cap, sizeof *g.uset);
	assert(thrd_success == mtx_init(&g.alloc_mutex, mtx_plain));
}

static inline unsigned get_index_mask(struct io* io)
{
	return (1U << io->ringbuf_size_log2) - 1;
}

static int alloc_file(struct io* io)
{
	struct files* f = &io->files;
	if (f->freelist_len > 0) {
		--f->freelist_len;
		assert(f->freelist_len >= 0);
		return f->freelist[f->freelist_len];
	}
	const int id = f->next_id;
	if (id >= f->cap) {
		// no free file
		return -1;
	}
	++f->next_id;
	return id;
}

static struct file* get_file(struct io* io, int id)
{
	assert((0 <= id) && (id <= io->files.next_id));
	return &io->files.files[id];
}

static void submit(struct io* io, struct iosub sub)
{
	unsigned head = io->sub_head;
	io->subs[head & get_index_mask(io)] = sub;
	++head;
	atomic_store(&io->sub_head, head);
}

size_t io_get_size(struct io* io, io_handle handle)
{
	struct files* f = &io->files;
	assert((0 <= handle) && (handle < f->cap));
	struct file* ff = &f->files[handle];
	return atomic_load(&ff->size);
}

struct io* io_new(int ringbuf_size_log2, int max_num_files)
{
	assert((0 <= ringbuf_size_log2) && (ringbuf_size_log2 <= MAX_SIZE_LOG2));
	assert(thrd_success == mtx_lock(&g.alloc_mutex));
	int id = -1;
	if (g.freelist_len > 0) {
		--g.freelist_len;
		assert(g.freelist_len >= 0);
		id = g.freelist[g.freelist_len];
	} else {
		id = (g.next_id++);
	}
	assert(id != -1);
	assert((id < g.cap) && "no free io (maybe increase max_num_ios in io_init()?)");
	struct io* io = &g.ios[id];
	memset(io, 0, sizeof *io);
	io->ringbuf_size_log2 = ringbuf_size_log2;
	io->subs = calloc(1 << ringbuf_size_log2, sizeof *io->subs);
	io->evs  = calloc(1 << ringbuf_size_log2, sizeof *io->evs);
	struct files* f = &io->files;
	f->cap = max_num_files;
	f->files = calloc(max_num_files, sizeof *f->files);
	f->freelist = calloc(max_num_files, sizeof *f->freelist);
	assert(thrd_success == mtx_unlock(&g.alloc_mutex));
	atomic_store(&g.uset[id], 1);
	return io;
}

static void complete(struct io* io, struct io_echo echo, io_handle handle, int32_t status)
{
	unsigned head = io->ev_head;
	struct io_event ev = {
		.echo=echo,
		.handle=handle,
		.status=status,
	};
	const unsigned index = head & get_index_mask(io);
	io->evs[index] = ev;
	++head;
	atomic_store(&io->ev_head, head);
}

void io_open(struct io* io, struct iosub_open sub)
{
	submit(io, ((struct iosub){.type=OPEN, .open=sub}));
}

struct io_event io_open_now(struct io* io, struct iosub_open sub)
{
	int flags = 0;
	if (sub.read && !sub.write) {
		flags |= O_RDONLY;
	} else if (!sub.read && sub.write) {
		flags |= O_WRONLY;
	} else if (sub.read && sub.write) {
		flags |= O_RDWR;
	} else {
		assert(!"open() submission has neither read nor write flag?");
	}
	int mode = 0;
	if (sub.create) {
		assert(sub.write && "create without write");
		flags |= O_CREAT;
		mode = 0666;
	}
	int fd = open(sub.path, flags, mode);
	io_status status = 0;
	io_handle handle = 0;
	if (fd == -1) {
		switch (errno) {
		case ENOENT : status=IOERR_NOT_FOUND   ; break;
		case EACCES : status=IOERR_NOT_ALLOWED ; break;
		default     : status=IOERR_ERROR       ; break;
		}
	} else {
		const int64_t size = lseek(fd, 0, SEEK_END);
		if (size == -1) {
			close(fd);
			status=IOERR_ERROR;
		} else {
			handle = alloc_file(io);
			if (handle == -1) {
				close(fd);
				status = IOERR_TOO_MANY_FILES;
			} else {
				struct file* file = get_file(io, handle);
				file->fd = fd;
				file->size = size;
			}
		}
	}
	if (sub.free_path_after_use) free(sub.path);
	return ((struct io_event){
		.echo = sub.echo,
		.handle = handle,
		.status = status,
	});
}

void io_close(struct io* io, struct iosub_close sub)
{
	submit(io, ((struct iosub){.type=CLOSE, .close=sub}));
}

io_status io_close_now(struct io* io, struct iosub_close sub)
{
	struct file* f = get_file(io, sub.handle);
	return close(f->fd) == -1 ? IOERR_ERROR : 0;
}

void io_pwrite(struct io* io, struct iosub_pwrite sub)
{
	submit(io, ((struct iosub){.type=PWRITE, .pwrite=sub}));
}

io_status io_pwrite_now(struct io* io, struct iosub_pwrite sub)
{
	struct file* f = get_file(io, sub.handle);
	int64_t offset = sub.offset;
	assert((0L <= offset) && (offset <= f->size));
	const int64_t size_increase = (offset + sub.size) - f->size;
	const int e = pwrite(f->fd, sub.data, sub.size, offset);
	if (size_increase > 0) f->size += size_increase;
	if (sub.free_data_after_use) {
		free(sub.data);
	}
	io_status status = 0;
	if (e == -1) {
		status = IOERR_ERROR;
	} else {
		if (sub.sync) {
			const int e2 = fdatasync(f->fd);
			if (e2 == -1) status = IOERR_ERROR;
		}
	}
	return status;
}

void io_pread(struct io* io, struct iosub_pread sub)
{
	submit(io, ((struct iosub){.type=PREAD, .pread=sub}));
}

io_status io_pread_now(struct io* io, struct iosub_pread sub)
{
	struct file* f = get_file(io, sub.handle);
	assert((sub.data != NULL) && "should we have alloc on demand here?");
	const int e = pread(f->fd, sub.data, sub.size, sub.offset);
	io_status status=0;
	if (e == -1) status = IOERR_ERROR;
	return status;
}

static void spool(struct io* io)
{
	const unsigned index_mask = get_index_mask(io);
	const unsigned head = atomic_load(&io->sub_head);
	unsigned tail = atomic_load(&io->sub_tail);
	while (tail < head) {
		const struct iosub* sub = &io->subs[tail & index_mask];
		switch (sub->type) {

		case OPEN: {
			struct iosub_open s = sub->open;
			struct io_event ev = io_open_now(io, s);
			complete(io, s.echo, ev.handle, ev.status);
		}	break;

		case CLOSE: {
			struct iosub_close s = sub->close;
			complete(io, s.echo, s.handle, io_close_now(io, s));
		}	break;

		case PWRITE: {
			struct iosub_pwrite s = sub->pwrite;
			io_status status = io_pwrite_now(io, s);
			if (!s.no_reply) complete(io, s.echo, s.handle, status);
		}	break;

		case PREAD: {
			struct iosub_pread s = sub->pread;
			complete(io, s.echo, s.handle, io_pread_now(io, s));
		}	break;

		default: assert(!"unhandled opcode");
		}
		++tail;
	}
	atomic_store(&io->sub_tail, tail);
}


void io_run(void)
{
	const int n = g.cap;
	for (;;) {
		for (int i=0; i<n; ++i) {
			const int in_use = atomic_load(&g.uset[i]);
			if (!in_use) continue;
			spool(&g.ios[i]);
		}
		sleep_nanoseconds(100000L); // 100µs
	}
}

int io_poll(struct io* io, struct io_event* ev)
{
	const unsigned head = atomic_load(&io->ev_head);
	unsigned tail = io->ev_tail;
	if (tail == head) return 0;
	memcpy(ev, &io->evs[tail], sizeof *ev);
	++tail;
	io->ev_tail = tail;
	return 1;
}

void io_appender_init(struct io_appender* a, struct io* io, io_handle handle, int ringbuf_cap_log2, int inflight_cap)
{
	memset(a, 0, sizeof *a);
	a->io = io;
	a->handle = handle;
	a->file_cursor = io_get_size(io, handle);
	a->ringbuf_cap_log2 = ringbuf_cap_log2;
	a->ringbuf = calloc(1L << ringbuf_cap_log2, sizeof *a->ringbuf);
	a->inflight_cap = inflight_cap;
	a->inflight_buf = calloc(inflight_cap, sizeof *a->inflight_buf);
}

int io_appender_write_raw(struct io_appender* a, void* data, size_t size)
{
	assert(io_appender_is_initialized(a));
	const int cap_log2 = a->ringbuf_cap_log2;
	const int64_t cap = (1L << cap_log2);
	const int64_t new_head = (a->head + size);
	const int64_t limit    = (a->tail + cap);
	if (new_head > limit) {
		// not write now! (would overwrite data that has not yet been flushed /
		// written to disk)
		return 0;
	}
	const int64_t turn0 = (a->head  >> cap_log2);
	const int64_t turn1 = (new_head >> cap_log2);
	const int64_t index_mask = (cap-1);
	if (turn1 == turn0) {
		// write doesn't cross end of ringbuf; a single memcpy() is enough:
		memcpy(&a->ringbuf[a->head & index_mask], data, size);
	} else {
		// write crosses end of ringbuf; two memcpy()'s needed:
		const int64_t end_of_turn = (turn1 << cap_log2);
		const int64_t until_end = (end_of_turn - a->head);
		assert(until_end > 0);
		assert(until_end < size);
		memcpy(&a->ringbuf[a->head & index_mask], data, until_end);
		memcpy(&a->ringbuf, data + until_end, size - until_end);
	}
	a->head = new_head;
	return 1;
}

int io_appender_write_leb128(struct io_appender* a, int64_t value)
{
	assert(io_appender_is_initialized(a));
	uint8_t buf[LEB128_MAX_LENGTH];
	uint8_t* end = leb128_encode_int64_buf(buf, value);
	const size_t n = (end - buf);
	assert(n <= LEB128_MAX_LENGTH);
	return io_appender_write_raw(a, buf, n);
}

static void appender_push_inflight(struct io_appender* a, int64_t size, int32_t echo_ib32)
{
	assert(io_appender_is_initialized(a));
	assert((a->num_inflights < a->inflight_cap) && "inflight buffer is full!");
	struct io_inflight* ii = &a->inflight_buf[a->num_inflights];
	++a->num_inflights;
	memset(ii, 0, sizeof *ii);
	ii->size = size;
	ii->echo_ib32 = echo_ib32;
}

static int appender_flush(struct io_appender* a, int now)
{
	assert(io_appender_is_initialized(a));
	const int64_t head = a->head;
	const int64_t tail = a->tail;
	if (head == tail) return 0; // nothing to flush, nothing to do
	const int cap_log2 = a->ringbuf_cap_log2;
	const int64_t cap = (1L << cap_log2);
	const int64_t index_mask = (cap-1);
	assert(head > tail);
	assert(head <= (tail+cap));
	const int64_t head_turn = (head >> cap_log2);
	const int64_t tail_turn = (tail >> cap_log2);
	int r = 0;
	const int inflight_avail = (a->inflight_cap - a->num_inflights);
	assert(inflight_avail >= 0);
	const int64_t size = (head-tail);
	// if the entire flush is within the same "turn" (the ring buffer being a
	// "ring") then we can flush with 1 request/write, otherwise it must be
	// split into 2
	const int num_req = (head_turn == tail_turn) ? 1 : 2;
	if (num_req == 1) {
		struct iosub_pwrite w = {
			.handle = a->handle,
			.sync = a->sync,
			.data = &a->ringbuf[tail & index_mask],
			.size = size,
			.offset = a->file_cursor,
		};
		assert(w.size > 0);
		a->file_cursor += size;
		if (now) {
			r = io_pwrite_now(a->io, w);
		} else {
			assert(num_req == 1);
			if (inflight_avail < num_req) {
				// not enough inflight slots available, abort
				return -1; // XXX return code?
			}
			w.echo = (struct io_echo) {
				.ia32 = IO_IA32_APPENDER,
				.ib32 = (a->ack_ib32_sequence++),
			};
			io_pwrite(a->io, w);
			appender_push_inflight(a, w.size, w.echo.ib32);
		}
	} else {
		assert(num_req == 2);
		const int index0 = tail & index_mask;
		const int64_t size0 = (cap-index0);
		assert(size0 > 0);
		assert(size0 < size);
		const int64_t size1 = size - size0;
		assert(size1 > 0);
		struct iosub_pwrite w0 = {
			.handle = a->handle,
			.sync = a->sync,
			.data = &a->ringbuf[index0],
			.size = size0,
			.offset = a->file_cursor,
		};
		struct iosub_pwrite w1 = {
			.handle = a->handle,
			.sync = a->sync,
			.data = &a->ringbuf,
			.size = size1,
			.offset = a->file_cursor + size0,
		};
		a->file_cursor += (size0 + size1);
		if (now) {
			r = io_pwrite_now(a->io, w0);
			if (r >= 0) {
				r = io_pwrite_now(a->io, w1);
			}
		} else {
			assert(num_req == 2);
			if (inflight_avail < num_req) {
				// not enough inflight slots available, abort
				return -1; // XXX return code?
			}
			w0.echo = (struct io_echo) {
				.ia32 = IO_IA32_APPENDER,
				.ib32 = (a->ack_ib32_sequence++),
			};
			w1.echo = (struct io_echo) {
				.ia32 = IO_IA32_APPENDER,
				.ib32 = (a->ack_ib32_sequence++),
			};
			io_pwrite(a->io, w0);
			io_pwrite(a->io, w1);
			appender_push_inflight(a, w0.size, w0.echo.ib32);
			appender_push_inflight(a, w1.size, w1.echo.ib32);
		}
	}
	if (now && r >= 0) {
		a->tail = a->head;
	}
	return r;
}

int io_appender_flush_now(struct io_appender* a)
{
	assert(io_appender_is_initialized(a));
	return appender_flush(a, 1);
}

void io_appender_flush(struct io_appender* a)
{
	assert(io_appender_is_initialized(a));
	assert(appender_flush(a, 0) == 0);
}

int io_appender_ack(struct io_appender* a, struct io_event* ev)
{
	assert(io_appender_is_initialized(a));
	if (ev->echo.ia32 != IO_IA32_APPENDER) return 0;
	const int num_inflights = a->num_inflights;
	const uint32_t ev_ib32 = ev->echo.ib32;
	for (int i0=0; i0<num_inflights; ++i0) {
		struct io_inflight* if0 = &a->inflight_buf[i0];
		if (if0->echo_ib32 != ev_ib32) continue;
		assert((if0->ack == 0) && "double ack?");
		if (i0 == 0) {
			int num_remove = 1;
			int64_t advance = if0->size;
			for (int i1=1; i1<num_inflights; ++i1) {
				struct io_inflight* if1 = &a->inflight_buf[i1];
				if (!if1->ack) break;
				++num_remove;
				advance += if1->size;
			}
			assert(num_remove <= num_inflights);
			const int num_move = num_inflights-num_remove;
			memmove(a->inflight_buf, &a->inflight_buf[num_remove], sizeof(*a->inflight_buf)*num_move);
			a->num_inflights -= num_remove;
			assert(a->num_inflights >= 0);
			a->tail += advance;
		} else {
			if0->ack = 1;
		}
	}
	return 1;
}

io_status io_bufread_read(struct io_bufread* b, void* dst, size_t sz)
{
	void* dp = dst;
	size_t remain_read = sz;
	const size_t filesize = io_get_size(b->io, b->handle);
	while (remain_read > 0) {
		if (b->error < 0) return b->error;
		if (b->end_of_file) return 0;
		if (b->_bufcur == NULL) {
			const size_t remain_file = (filesize - b->file_cursor);
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
			if (b->file_cursor == filesize) {
				b->end_of_file = 1;
				return 0;
			}
		}
	}
	return 0;
}

/*
list problems to solve, all without blocking until completion?

files:
 - append data to a journal file, snapshot file, log file, .wav file?
 - read part of file, like part of the journal file
 - read entire file? (could be handled on the outside as small reads, which
   might be better)
 - read a bunch of files (e.g. a sample pack)
 - open, close, get size?

network (covers HTTP, WS, UDP-protocol, OSC,...?)
 - send UDP packets
 - receive UDP packets
 - send TCP data
 - receive TCP data
 - connect to TCP address
 - listen on TCP address
 - copy from TCP directly into local file?

problems NOT to solve:
 - protocols and file formats? not entirely sure: io.c-side decompression might
   not be the worst idea, but io.c-side HTTP parsing seems bad?
*/
