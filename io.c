#include <assert.h>
#include <string.h>
#include <stdint.h>
#include <threads.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>

#include "io.h"
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
	_Atomic(int64_t) size;
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

int64_t io_get_size(struct io* io, union io64 handle)
{
	struct files* f = &io->files;
	assert((0 <= handle.i64) && (handle.i64 < f->cap));
	struct file* ff = &f->files[handle.i64];
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

static void complete(struct io* io, union io64 echo, union io64 result)
{
	unsigned head = io->ev_head;
	struct io_event ev = { .echo=echo, .result=result };
	const unsigned index = head & get_index_mask(io);
	io->evs[index] = ev;
	++head;
	atomic_store(&io->ev_head, head);
}

static union io64 io64i(int i) { return ((union io64){.i64=i}); }

void io_open(struct io* io, struct iosub_open sub)
{
	submit(io, ((struct iosub){.type=OPEN, .open=sub}));
}

union io64 io_open_now(struct io* io, struct iosub_open sub)
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
	int64_t r = 0;
	if (fd == -1) {
		switch (errno) {
		case ENOENT : r=IOERR_NOT_FOUND   ; break;
		case EACCES : r=IOERR_NOT_ALLOWED ; break;
		default     : r=IOERR_ERROR       ; break;
		}
	} else {
		const int64_t size = lseek(fd, 0, SEEK_END);
		if (size == -1) {
			close(fd);
			r=IOERR_ERROR;
		} else {
			r = alloc_file(io);
			if (r == -1) {
				close(fd);
				r = IOERR_TOO_MANY_FILES;
			} else {
				assert((0 <= r) && (r <= INT_MAX));
				struct file* file = get_file(io, (int)r);
				file->fd = fd;
				file->size = size;
			}
		}
	}
	if (sub.free_path_after_use) free(sub.path);
	return io64i(r);
}

void io_close(struct io* io, struct iosub_close sub)
{
	submit(io, ((struct iosub){.type=CLOSE, .close=sub}));
}

union io64 io_close_now(struct io* io, struct iosub_close sub)
{
	struct file* f = get_file(io, (int)sub.handle.i64);
	return io64i(close(f->fd) == -1 ? IOERR_ERROR : 0);
}

void io_pwrite(struct io* io, struct iosub_pwrite sub)
{
	submit(io, ((struct iosub){.type=PWRITE, .pwrite=sub}));
}

union io64 io_pwrite_now(struct io* io, struct iosub_pwrite sub)
{
	struct file* f = get_file(io, (int)sub.handle.i64);
	int64_t offset = sub.offset;
	assert((0L <= offset) && (offset <= f->size));
	const int64_t size_increase = (offset + sub.size) - f->size;
	const int e = pwrite(f->fd, sub.data, sub.size, offset);
	if (size_increase > 0) f->size += size_increase;
	if (sub.free_data_after_use) {
		free(sub.data);
	}
	int64_t r=0;
	if (e == -1) {
		r = IOERR_ERROR;
	} else {
		if (sub.sync) {
			const int e2 = fdatasync(f->fd);
			if (e2 == -1) r = IOERR_ERROR;
		}
	}
	return io64i(r);
}

void io_pread(struct io* io, struct iosub_pread sub)
{
	submit(io, ((struct iosub){.type=PREAD, .pread=sub}));
}

union io64 io_pread_now(struct io* io, struct iosub_pread sub)
{
	struct file* f = get_file(io, (int)sub.handle.i64);
	assert((sub.data != NULL) && "should we have alloc on demand here?");
	const int e = pread(f->fd, sub.data, sub.size, sub.offset);
	int64_t r=0;
	if (e == -1) r = IOERR_ERROR;
	return io64i(r);
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
			complete(io, s.echo, io_open_now(io, s));
		}	break;

		case CLOSE: {
			struct iosub_close s = sub->close;
			complete(io, s.echo, io_close_now(io, s));
		}	break;

		case PWRITE: {
			struct iosub_pwrite s = sub->pwrite;
			union io64 r = io_pwrite_now(io, s);
			if (!s.no_reply) complete(io, s.echo, r);
		}	break;

		case PREAD: {
			struct iosub_pread s = sub->pread;
			complete(io, s.echo, io_pread_now(io, s));
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
