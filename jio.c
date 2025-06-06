#ifdef __EMSCRIPTEN__
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#define BLOCKING
const char* io_error_to_string(int error)
{
	return strerror(errno);
}
#endif

#include <limits.h>

#include "jio.h"
#include "main.h"
#include "stb_ds_sysalloc.h"

struct inflight {
	uint32_t tail;
	unsigned is_done :1;
};

struct jio {
	int file_id;
	int port_id;

	int error;
	// negative if an error occurred

	int64_t filesize;
	// filesize of file, includes non-committed writes, so new size is
	// available immediately after jio_append(). used from client thread.

	long head, tail;
	// head/tail cursors for ringbuf.

	int ringbuf_size_log2;
	uint8_t* ringbuf;
	// ring buffer size and storage

	struct inflight* inflight_arr;
	unsigned tag;
};
//static_assert(sizeof(((struct jio*)0)->head)==8,"noo");

static struct {
	unsigned tag_sequence;
} g;

struct jio* jio_open(const char* path, enum io_open_mode mode, int port_id, int ringbuf_size_log2, int* out_error)
{
	assert((0 <= ringbuf_size_log2) && (ringbuf_size_log2 <= 30));

	int64_t filesize;

	#ifdef BLOCKING
	// XXX a bit of unfortunate copy-pasta from io.c, but it's awkward to share
	// (depends on posix but io.h/jio.h don't)
	int oflags;
	int omode = 0;
	switch (mode) {
	case IO_OPEN_RDONLY: {
		oflags = (O_RDONLY);
	}	break;
	case IO_OPEN: {
		oflags = (O_RDWR);
	}	break;
	case IO_OPEN_OR_CREATE: {
		oflags = (O_RDWR | O_CREAT);
		omode = 0666;
	}	break;
	case IO_CREATE: {
		oflags = (O_RDWR | O_CREAT | O_EXCL);
		omode = 0666;
	}	break;
	default: assert(!"unhandled mode");
	}
	const int file_id = open(path, oflags, omode);
	if (file_id == -1) {
		if (out_error) *out_error = -abs(errno);
		return NULL;
	}
	off_t e = lseek(file_id, 0, SEEK_END);
	if (e == -1) {
		if (out_error) *out_error = -abs(errno);
		return NULL;
	}
	filesize = e;
	#else
	const int file_id = io_open(path, mode, &filesize);
	if (file_id < 0) {
		if (out_error) *out_error = file_id;
		return NULL;
	}
	#endif

	struct jio* jio = calloc(1, sizeof *jio);
	jio->port_id = port_id;
	jio->file_id = file_id;
	jio->filesize = filesize;
	jio->ringbuf_size_log2 = ringbuf_size_log2;
	jio->ringbuf = calloc(1L << jio->ringbuf_size_log2, sizeof *jio->ringbuf);
	jio->tag = ++g.tag_sequence;

	if (filesize > 0) {
		const int ringbuf_size_log2 = jio->ringbuf_size_log2;
		const int64_t ringbuf_size = (1L << ringbuf_size_log2);
		const int64_t mask = (ringbuf_size-1);
		const int64_t i = filesize & mask;
		const int64_t o0 = filesize & ~mask;

		int e0=0;
		if (i>0) {
			#ifdef BLOCKING
			e0 = pread(file_id, &jio->ringbuf[0], i, o0);
			#else
			e0 = io_pread(file_id, &jio->ringbuf[0], i, o0);
			#endif
		}

		if ((e0==0) && (o0>0)) {
			const int64_t n = ringbuf_size - i;
			assert(n > 0);
			#ifdef BLOCKING
			e0 = pread(file_id, &jio->ringbuf[i], n, o0 - ringbuf_size + i);
			#else
			e0 = io_pread(file_id, &jio->ringbuf[i], n, o0 - ringbuf_size + i);
			#endif
		}

		if (e0 < 0) {
			if (out_error) *out_error = e0;
			#ifdef BLOCKING
			const int e1 = close(file_id);
			#else
			const int e1 = io_close(file_id);
			#endif
			if (e1 < 0) {
				if (out_error) *out_error = e1;
				fprintf(stderr, "WARNING: close-error: %s / %s\n", io_error_to_string(e0), io_error_to_string(e1));
			}
			return NULL;
		}
		jio->head = filesize;
	}

	return jio;
}

int jio_close(struct jio* jio)
{
	if (jio->error < 0) return jio->error;
	const unsigned head = jio->head;
	if (jio->tail != head) return IO_PENDING;

	#ifdef BLOCKING
	const int e = close(jio->file_id);
	#else
	const int e = io_close(jio->file_id);
	#endif
	int has_error = 0;
	if (e < 0) {
		fprintf(stderr, "WARNING: close-error: %s\n", io_error_to_string(e));
		has_error = 1;
	}

	free(jio->ringbuf);
	arrfree(jio->inflight_arr);
	free(jio);

	return has_error ? IO_ERROR : 0;
}

int64_t jio_get_size(struct jio* jio)
{
	return jio->filesize;
}

int jio_ack(struct jio* jio, io_echo echo)
{
	if (echo.ua32 != jio->tag) return 0;
	int num_inflight = arrlen(jio->inflight_arr);
	int fill = 0;
	int match = 0;
	for (int i=0; i<num_inflight; ++i) {
		struct inflight* inflight = &jio->inflight_arr[i];
		int apply = 0;
		if (!fill) {
			if (inflight->tail == echo.ub32) {
				match = 1;
				if (i == 0) {
					assert((!inflight->is_done) && "double ack?");
					apply = 1;
					fill = 1;
				} else {
					inflight->is_done = 1;
					return 1;
				}
			}
		} else {
			if (inflight->is_done) {
				apply = 1;
			} else {
				break;
			}
		}
		if (apply) {
			jio->tail = inflight->tail;
			assert(i == 0);
			arrdel(jio->inflight_arr, 0);
			--i;
			--num_inflight;
		}
	}
	assert(match);
	return 1;
}

static void our_pwrite(struct jio* jio, io_echo echo, int file_id, void* buf, int64_t count, int64_t offset, unsigned tail)
{
	#ifdef BLOCKING
	pwrite(file_id, buf, count, offset);
	arrput(jio->inflight_arr, ((struct inflight){ .tail = tail }));
	jio_ack(jio, echo);
	#else
	io_port_pwrite(jio->port_id, echo, file_id, buf, count, offset);
	arrput(jio->inflight_arr, ((struct inflight){ .tail = tail }));
	#endif
}

int jio_append(struct jio* jio, const void* ptr, int64_t size)
{
	// ignore writes if an error has been signalled
	if (jio->error < 0) return jio->error;
	if (size == 0) return 0;
	assert(size>0);
	const int ringbuf_size_log2 = jio->ringbuf_size_log2;
	const int64_t ringbuf_size = (1L << ringbuf_size_log2);
	assert((size <= ringbuf_size) && "data does not fit in ringbuf");
	const unsigned head = jio->head;
	const unsigned new_head = (head + size);

	if ((new_head - jio->tail) > ringbuf_size) {
		jio->error = IO_BUFFER_FULL;
		return jio->error;
	}

	const unsigned cycle0 = (head         >> ringbuf_size_log2);
	const unsigned cycle1 = ((new_head-1) >> ringbuf_size_log2);
	const unsigned mask = (ringbuf_size-1);
	const int file_id = jio->file_id;
	if (cycle0 == cycle1) {
		void* dst = &jio->ringbuf[head & mask];
		memcpy(dst, ptr, size);
		io_echo echo = {
			.ua32 = jio->tag,
			.ub32 = new_head,
		};
		our_pwrite(jio, echo, file_id, dst, size, jio->filesize, new_head);

	} else {
		const unsigned remain = (cycle1 << ringbuf_size_log2) - head;

		void* dst0 = &jio->ringbuf[head & mask];
		memcpy(dst0, ptr, remain);
		const unsigned head0 = new_head - (size-remain);
		io_echo echo0 = {
			.ua32 = jio->tag,
			.ub32 = head0,
		};
		our_pwrite(jio, echo0, file_id, dst0, remain, jio->filesize, head0);

		void* dst1 = jio->ringbuf;
		memcpy(dst1, ptr+remain, size-remain);
		const unsigned head1 = new_head;
		io_echo echo1 = {
			.ua32 = jio->tag,
			.ub32 = head1,
		};
		our_pwrite(jio, echo1, file_id, dst1, size, jio->filesize+remain, head1);
	}

	jio->filesize += size;

	jio->head = new_head;

	return 0;
}

// XXX memonly never set currently; change pread_ex() back into jio_pread() if
// it sticks?
int pread_ex(struct jio* jio, void* ptr, int64_t size, int64_t offset, int memonly)
{
	// ignore read if an error has been signalled
	if (jio->error < 0) return jio->error;
	if (!((0L <= offset) && (offset <= jio->filesize))) {
		jio->error = IO_READ_OUT_OF_RANGE;
		return jio->error;
	}
	if (size == 0) return 0;

	if ((offset+size) > jio->filesize) {
		size = (jio->filesize - offset);
	}
	assert(size >= 0);
	assert(size <= INT_MAX);

	const int ringbuf_size_log2 = jio->ringbuf_size_log2;
	const int64_t ringbuf_size = 1L << ringbuf_size_log2;
	// ring buffer file position interval [rb0;rb1[
	int64_t rb0 = 0;
	const int64_t rb1 = jio->filesize;
	{
		const int64_t rbn = (rb1-rb0);
		assert(rbn >= 0);
		if (rbn > ringbuf_size) {
			// adjust rb0 for ringbuffer size
			rb0 = (rb1-ringbuf_size);
		}
	}
	assert((rb1-rb0) <= ringbuf_size);

	// requested read interval [rq0;rq1[
	const int64_t rq0 = offset;
	const int64_t rq1 = offset+size;
	assert(rq1 <= rb1);
	int copy_from_ringbuf = 0;
	int read_from_backend = 0;
	int64_t cc0=0,cc1=0,rr0=0,rr1=0;
	uint8_t *ccp=NULL,*rrp=NULL;
	if (rq0 >= rb0) {
		cc0=rq0;
		cc1=rq1;
		ccp=ptr;
		copy_from_ringbuf = 1;
	} else if (rq1 > rb0) {
		rr0=rq0;
		rr1=rb0;
		rrp=ptr;
		read_from_backend = 1;
		cc0=rb0;
		cc1=rq1;
		ccp=rrp+(rr1-rr0);
		copy_from_ringbuf = 1;
	} else {
		rr0=rq0;
		rr1=rq1;
		rrp=ptr;
		read_from_backend = 1;
	}

	if (read_from_backend && memonly) {
		jio->error = IO_READ_ERROR;
		return jio->error;
	}

	if (read_from_backend) {
		assert(!memonly);
		assert(rr1>rr0);
		#ifdef BLOCKING
		if (-1 == pread(jio->file_id, rrp, (rr1-rr0), rr0)) {
			jio->error = IO_READ_ERROR;
			return jio->error;
		}
		#else
		if (0 > io_pread(jio->file_id, rrp, (rr1-rr0), rr0)) {
			jio->error = IO_READ_ERROR;
			return jio->error;
		}
		#endif
	}
	if (copy_from_ringbuf) {
		assert(cc1>cc0);
		const int64_t cc0cycle = ( cc0    >> ringbuf_size_log2);
		const int64_t cc1cycle = ((cc1-1) >> ringbuf_size_log2);
		assert(ringbuf_size == (1<<ringbuf_size_log2));
		const int64_t mask = (ringbuf_size-1);
		assert((cc1-cc0)==size || read_from_backend);
		const int64_t cc0mask = cc0&mask;
		if (cc0cycle == cc1cycle) {
			memcpy(ccp, &jio->ringbuf[cc0mask], (cc1-cc0));
		} else {
			const int64_t n0 = (ringbuf_size - cc0mask);
			memcpy(ccp,   &jio->ringbuf[cc0mask], n0);
			memcpy(ccp+n0, jio->ringbuf,    size-n0);
		}
	}

	return size;
}

int jio_pread(struct jio* jio, void* ptr, int64_t size, int64_t offset)
{
	return pread_ex(jio, ptr, size, offset, 0);
}

#if 0
int jio_pread_memonly(struct jio* jio, void* ptr, int64_t size, int64_t offset)
{
	return pread_ex(jio, ptr, size, offset, 1);
}
#endif

int jio_get_error(struct jio* jio)
{
	return jio->error;
}

void jio_clear_error(struct jio* jio)
{
	jio->error = 0;
}
