#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <threads.h>
#include <stdatomic.h>

#include "jio.h"
#include "main.h"
#include "stb_ds_sysalloc.h"

struct jio {
	int fd;
	// underlying file descriptor

	_Atomic(int) error;
	// negative if an error occurred

	int64_t filesize;
	// filesize of file, includes non-committed writes, so new size is
	// available immediately after jio_append(). used from client thread.

	int64_t filesize0;
	int64_t appended;
	// filesize0 is the size of the file when it was opened. appended is how
	// much data has been added to the ringbuf since opening. in combination
	// this is used to map the ringbuf to actual file positions (useful for
	// jio_pread()ing directly from ringbuf)

	int64_t disk_cursor;
	// where data is being written to. can also be regarded as a "delayed
	// filesize" (matches filesize when no writes are pending). used in io
	// thread.

	_Atomic(unsigned) head;
	_Atomic(unsigned) tail;
	// head/tail cursors for ringbuf. these should be able to wrap around (XXX
	// is this tested&verified?)

	int ringbuf_size_log2;
	uint8_t* ringbuf;
	// ring buffer size and storage

	int num_block_sleeps;
};

static struct {
	struct jio** jio_arr;
	mtx_t jio_arr_mutex;
} g;

static void block_sleep(struct jio* jio)
{
	if (jio != NULL) ++jio->num_block_sleeps;
	sleep_nanoseconds(100000L); // 100µs
}

const char* jio_error_to_string(int error)
{
	switch (error) {
	#define X(ENUM,MSG,_ID) case ENUM: return #MSG;
	LIST_OF_JIO_ERRORS
	#undef X
	default: return NULL;
	}
}

struct jio* jio_open(const char* path, enum jio_open_mode mode, int ringbuf_size_log2, int* out_error)
{
	assert((0 <= ringbuf_size_log2) && (ringbuf_size_log2 <= 30));

	int oflags;
	int omode = 0;
	switch (mode) {

	case JIO_OPEN: {
		oflags = (O_RDWR);
	}	break;

	case JIO_OPEN_OR_CREATE: {
		oflags = (O_RDWR | O_CREAT);
		omode = 0666;
	}	break;

	case JIO_CREATE: {
		oflags = (O_RDWR | O_CREAT | O_EXCL);
		omode = 0666;
	}	break;

	default: assert(!"unhandled mode");

	}
	const int fd = open(path, oflags, omode);
	if (fd == -1) {
		int err = JIO_ERROR;
		switch (errno) {
		case EACCES: err = JIO_NOT_PERMITTED  ; break;
		case EEXIST: err = JIO_ALREADY_EXISTS ; break;
		case ENOENT: err = JIO_NOT_FOUND      ; break;
		}
		if (out_error) *out_error = err;
		return NULL;
	}

	const off_t o = lseek(fd, 0, SEEK_END);
	if (o < 0) {
		(void)close(fd);
		if (out_error) *out_error = JIO_ERROR;
		return NULL;
	}

	struct jio* jio = calloc(1, sizeof *jio);
	jio->fd = fd;
	jio->filesize = o;
	jio->filesize0 = o;
	jio->disk_cursor = o;
	jio->ringbuf_size_log2 = ringbuf_size_log2;
	jio->ringbuf = calloc(1L << jio->ringbuf_size_log2, sizeof *jio->ringbuf);

	assert(thrd_success == mtx_lock(&g.jio_arr_mutex));
	arrput(g.jio_arr, jio);
	assert(thrd_success == mtx_unlock(&g.jio_arr_mutex));

	return jio;
}

int jio_close(struct jio* jio)
{
	// wait for pending writes to finish (or an error to occur)
	const unsigned head = jio->head;
	while ((atomic_load(&jio->tail) != head) && (atomic_load(&jio->error) == 0)) {
		block_sleep(jio);
	}

	int has_error = 0;
	if (atomic_load(&jio->error) != 0) has_error = 1;

	// close file descriptor
	const int e = close(jio->fd);
	if (e == -1) {
		fprintf(stderr, "WARNING: close-error: %s\n", strerror(errno));
		has_error = 1;
	}

	// remove jio from jio_arr
	assert(thrd_success == mtx_lock(&g.jio_arr_mutex));
	const int num = arrlen(g.jio_arr);
	int did_remove = 0;
	for (int i=0; i<num; ++i) {
		if (g.jio_arr[i]->fd == jio->fd) {
			arrdel(g.jio_arr, i);
			did_remove = 1;
			break;
		}
	}
	assert(did_remove);
	assert(thrd_success == mtx_unlock(&g.jio_arr_mutex));

	free(jio->ringbuf);
	free(jio);

	return has_error ? JIO_ERROR : 0;
}

int64_t jio_get_size(struct jio* jio)
{
	return jio->filesize;
}

void jio_append(struct jio* jio, const void* ptr, int64_t size)
{
	// ignore writes if an error has been signalled
	if (atomic_load(&jio->error) < 0) return;

	if (size == 0) return;
	assert(size>0);
	const int ringbuf_size_log2 = jio->ringbuf_size_log2;
	const int64_t ringbuf_size = (1L << ringbuf_size_log2);
	assert((size <= ringbuf_size) && "data does not fit in ringbuf");
	const unsigned head = jio->head;
	const unsigned new_head = (head + size);
	while ((new_head - atomic_load(&jio->tail)) > ringbuf_size) {
		// block until there's room in the ringbuf
		block_sleep(jio);
	}

	const unsigned turn0 = (head >> ringbuf_size_log2);
	const unsigned turn1 = (new_head >> ringbuf_size_log2);
	const unsigned mask = (ringbuf_size-1);
	if (turn0 == turn1) {
		memcpy(&jio->ringbuf[head & mask], ptr, size);
	} else {
		const unsigned remain = (turn1 << ringbuf_size_log2) - head;
		memcpy(&jio->ringbuf[head & mask], ptr, remain);
		memcpy(jio->ringbuf, ptr+remain, size-remain);
	}

	jio->filesize += size;
	jio->appended += size;

	atomic_store(&jio->head, new_head);
}

static int pwriten(int fd, const void* buf, int64_t n, int64_t offset)
{
	int64_t remaining = n;
	const void* p = buf;
	while (remaining > 0) {
		const int64_t n = pwrite(fd, p, remaining, offset);
		if (n == -1) {
			if (errno == EINTR) continue;
			return -1;
		}
		assert(n >= 0);
		p += n;
		remaining -= n;
		offset += n;
	}
	assert(remaining == 0);
	return 0;
}

static int preadn(int fd, void* buf, int64_t n, int64_t offset)
{
	int64_t remaining = n;
	void* p = buf;
	while (remaining > 0) {
		const int64_t n = pread(fd, p, remaining, offset);
		if (n == -1) {
			if (errno == EINTR) continue;
			return -1;
		}
		assert(n >= 0);
		p += n;
		remaining -= n;
		offset += n;
	}
	assert(remaining == 0);
	return 0;
}

int jio_pread(struct jio* jio, void* ptr, int64_t size, int64_t offset)
{
	// ignore read if an error has been signalled
	int error = atomic_load(&jio->error);
	if (error < 0) return error;
	if (!((0L <= offset) && ((offset+size) <= jio->filesize))) {
		error = JIO_READ_OUT_OF_RANGE;
		atomic_store(&jio->error, error);
		return error;
	}
	if (size == 0) return 0;

	const int ringbuf_size_log2 = jio->ringbuf_size_log2;
	const int64_t ringbuf_size = 1L << ringbuf_size_log2;
	// ring buffer file position interval [rb0;rb1[
	int64_t rb0 = jio->filesize0;
	const int64_t rb1 = rb0 + jio->appended;
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

	if (read_from_backend) {
		assert(rr1>rr0);
		if (-1 == preadn(jio->fd, rrp, (rr1-rr0), rr0)) {
			error = JIO_READ_ERROR;
			atomic_store(&jio->error, error);
			return error;
		}
	}
	if (copy_from_ringbuf) {
		assert(cc1>cc0);
		const int64_t i0 = (cc0 - jio->filesize0);
		const int64_t i0turn = (i0 >> ringbuf_size_log2);
		const int64_t i1 = (cc1 - jio->filesize0);
		const int64_t i1turn = (i1 >> ringbuf_size_log2);
		assert(ringbuf_size == (1<<ringbuf_size_log2));
		const int64_t mask = (ringbuf_size-1);
		assert((i1-i0)==size || read_from_backend);
		const int64_t i0mask = i0&mask;
		if (i0turn == i1turn) {
			memcpy(ccp, &jio->ringbuf[i0mask], (i1-i0));
		} else {
			const int64_t n0 = (ringbuf_size - i0mask);
			memcpy(ccp,   &jio->ringbuf[i0mask], n0);
			memcpy(ccp+n0, jio->ringbuf,    size-n0);
		}
	}

	return 0;
}

int jio_get_error(struct jio* jio)
{
	return atomic_load(&jio->error);
}

int jio_get_num_block_sleeps(struct jio* jio)
{
	return jio->num_block_sleeps;
}

void jio_init(void)
{
	assert(thrd_success == mtx_init(&g.jio_arr_mutex, mtx_plain));
}

void jio_thread_run(void)
{
	int cursor = 0;
	for (;;) {
		int did_wrap = 0;
		int did_work = 0;
		struct jio* jio = NULL;
		assert(thrd_success == mtx_lock(&g.jio_arr_mutex));
		const int num = arrlen(g.jio_arr);
		if (num > 0) {
			if (cursor >= num) {
				cursor = 0;
				did_wrap = 1;
			}
			assert((0 <= cursor) && (cursor < num));
			jio = g.jio_arr[cursor];
			++cursor;
		} else {
			cursor = 0;
		}
		assert(thrd_success == mtx_unlock(&g.jio_arr_mutex));

		if (jio != NULL) {
			const unsigned head = atomic_load(&jio->head);
			const unsigned tail = jio->tail;
			if (tail != head) {
				const int ringbuf_size_log2 = jio->ringbuf_size_log2;
				const unsigned head_turn = (head >> ringbuf_size_log2);
				const unsigned tail_turn = (tail >> ringbuf_size_log2);
				const unsigned ringbuf_size = 1L << ringbuf_size_log2;
				const unsigned mask = (ringbuf_size-1);
				const unsigned size = (head-tail);
				if (head_turn == tail_turn) {
					pwriten(jio->fd, &jio->ringbuf[tail&mask], size, jio->disk_cursor);
				} else {
					const unsigned remain = ringbuf_size - (tail&mask);
					assert((0 < remain) && (remain < ringbuf_size));
					pwriten(jio->fd, &jio->ringbuf[tail&mask], remain, jio->disk_cursor);
					pwriten(jio->fd, jio->ringbuf, size-remain, jio->disk_cursor+remain);
				}
				jio->disk_cursor += size;
				atomic_store(&jio->tail, head);
			}
		}

		if (!did_work && ((num==0) || did_wrap)) {
			block_sleep(NULL);
		}
	}
}
