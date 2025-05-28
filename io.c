#include <assert.h>
#include <poll.h>
#include <unistd.h>
#include <threads.h>
#include <errno.h>
#include <fcntl.h>

#include "stb_ds_sysalloc.h"
#include "io.h"

const char* io_error_to_string(int error)
{
	switch (error) {
	#define X(ENUM,MSG,_ID) case ENUM: return #MSG;
	LIST_OF_IO_ERRORS
	#undef X
	default: return NULL;
	}
}

enum submission_type {
	SUBMISSION_PREAD=1,
	SUBMISSION_PWRITE,
	// TODO send/recv?
};

struct submission {
	int port_id;
	enum submission_type type;
	io_echo echo;
	union {
		struct {
			void* ptr;
			int64_t count;
			int64_t offset;
		} pread;
		struct {
			void* ptr;
			int64_t count;
			int64_t offset;
		} pwrite;
	};
};

struct file {
	int file_id;
	int posix_fd;
	struct submission* submission_arr;
};

struct port {
	int port_id;
	struct io_event* event_arr;
};

struct fire {
	int posix_fd;
	int status;
	struct submission sub;
};

static struct {
	int port_id_sequence;
	int file_id_sequence;
	mtx_t mutex;
	struct file* file_arr;
	struct port* port_arr;
} g;

#define G_LOCK()   assert(thrd_success == mtx_lock(&g.mutex))
#define G_UNLOCK() assert(thrd_success == mtx_unlock(&g.mutex));

int io_open(const char* path, enum io_open_mode mode, int64_t* out_filesize)
{
	int oflags;
	int omode = 0;
	switch (mode) {
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
	const int posix_fd = open(path, oflags, omode);
	if (posix_fd == -1) {
		switch (errno) {
		case EACCES: return IO_NOT_PERMITTED  ;
		case EEXIST: return IO_ALREADY_EXISTS ;
		case ENOENT: return IO_NOT_FOUND      ;
		}
		return IO_ERROR;
	}

	if (out_filesize) {
		const off_t o = lseek(posix_fd, 0, SEEK_END);
		if (o < 0) {
			(void)close(posix_fd);
			return IO_ERROR;
		}
		*out_filesize = o;
	}

	G_LOCK();
	const int file_id = ++g.file_id_sequence;
	arrput(g.file_arr, ((struct file) {
		.file_id = file_id,
		.posix_fd = posix_fd,
	}));
	G_UNLOCK();

	return file_id;
}

static struct file* get_file(int file_id)
{
	const int num_files = arrlen(g.file_arr);
	for (int i=0; i<num_files; ++i) {
		struct file* file = &g.file_arr[i];
		if (file->file_id == file_id) return file;
	}
	assert(!"file id not found");
}

static int file_id_to_posix_fd(int file_id)
{
	return get_file(file_id)->posix_fd;
}

int io_close(int file_id)
{
	G_LOCK();
	struct file* file = get_file(file_id);
	const int num_sub = arrlen(file->submission_arr);
	G_UNLOCK();
	if (num_sub) return IO_PENDING;
	if (close(file->posix_fd) != 0) {
		return IO_ERROR;
	}
	return 0;
}

#if 0
static int pwriten(int posix_fd, const void* ptr, int64_t count, int64_t offset)
{
	int64_t remaining = count;
	const void* p = ptr;
	while (remaining > 0) {
		const int64_t count = pwrite(posix_fd, p, remaining, offset);
		if (count == -1) {
			if (errno == EINTR) continue;
			return -1;
		}
		assert(count >= 0);
		p += count;
		remaining -= count;
		offset += count;
	}
	assert(remaining == 0);
	return 0;
}
#endif

static int preadn(int posix_fd, void* ptr, int64_t count, int64_t offset)
{
	int64_t remaining = count;
	void* p = ptr;
	while (remaining > 0) {
		const int64_t count = pread(posix_fd, p, remaining, offset);
		if (count == -1) {
			if (errno == EINTR) continue;
			return -1;
		}
		assert(count >= 0);
		p += count;
		remaining -= count;
		offset += count;
	}
	assert(remaining == 0);
	return 0;
}


int io_pread(int file_id, void* ptr, int64_t count, int64_t offset)
{
	G_LOCK();
	const int posix_fd = file_id_to_posix_fd(file_id);
	G_UNLOCK();
	const int e = preadn(posix_fd, ptr, count, offset);
	if (e == -1) {
		return IO_READ_ERROR;
	} else {
		return 0;
	}
}

static void submit(int file_id, struct submission* sub)
{
	G_LOCK();
	const int num_files = arrlen(g.file_arr);
	for (int i=0; i<num_files; ++i) {
		struct file* file = &g.file_arr[i];
		if (file->file_id != file_id) continue;
		arrput(file->submission_arr, *sub);
		G_UNLOCK();
		return;
	}
	assert(!"file_id does not exist");
}

void io_port_pread(int port_id, io_echo echo, int file_id, void* ptr, int64_t count, int64_t offset)
{
	struct submission s = {
		.port_id = port_id,
		.type = SUBMISSION_PREAD,
		.echo = echo,
		.pread = {
			.ptr = ptr,
			.count = count,
			.offset = offset,
		},
	};
	submit(file_id, &s);
}

void io_port_pwrite(int port_id, io_echo echo, int file_id, void* ptr, int64_t count, int64_t offset)
{
	struct submission s = {
		.port_id = port_id,
		.type = SUBMISSION_PWRITE,
		.echo = echo,
		.pwrite = {
			.ptr = ptr,
			.count = count,
			.offset = offset,
		},
	};
	submit(file_id, &s);
}

int io_port_create(void)
{
	G_LOCK();
	const int port_id = ++g.port_id_sequence;
	arrput(g.port_arr, ((struct port) {
		.port_id = port_id,
	}));
	G_UNLOCK();
	return port_id;
}

int io_port_poll(int port_id, struct io_event* out_ev)
{
	G_LOCK();
	const int num_ports = arrlen(g.port_arr);
	for (int i=0; i<num_ports; ++i) {
		struct port* port = &g.port_arr[i];
		if (port->port_id != port_id) continue;
		const int num_events = arrlen(port->event_arr);
		int r=0;
		if (num_events > 0) {
			if (out_ev) *out_ev = port->event_arr[0];
			arrdel(port->event_arr, 0);
			r=1;
		}
		G_UNLOCK();
		return r;
	}
	assert(!"port id does not exist");
}

void io_tick(void)
{
	static struct pollfd* pollfd_arr;
	arrreset(pollfd_arr);

	// populate pollfd_arr with fds/events corresponding to submission types
	G_LOCK();
	{
		const int num_files = arrlen(g.file_arr);
		for (int i=0; i<num_files; ++i) {
			struct file* file = &g.file_arr[i];
			const int num_subs = arrlen(file->submission_arr);
			if (num_subs == 0) continue;
			int events = 0;
			for (int ii=0; ii<num_subs; ++ii) {
				struct submission* sub = &file->submission_arr[ii];
				switch (sub->type) {
				case SUBMISSION_PREAD:
					events |= POLLIN;
					break;
				case SUBMISSION_PWRITE:
					events |= POLLOUT;
					break;
				default: assert(!"unhandled submission type");
				}
			}

			if (events == 0) continue;

			arrput(pollfd_arr, ((struct pollfd) {
				.fd = file->posix_fd,
				.events = events,
			}));
		}
	}
	G_UNLOCK();

	// execute poll(2)
	const int num_pollfd = arrlen(pollfd_arr);
	int e = poll(pollfd_arr, num_pollfd, 0);
	if (e == -1) {
		if (errno == EINTR) {
			return;
		}
		fprintf(stderr, "poll(): %s\n", strerror(errno));
		abort();
	} else if (e == 0) {
		return;
	}
	assert(e>0);

	static struct fire* fire_arr;
	arrreset(fire_arr);

	// convert poll() output to list of calls to make
	G_LOCK();
	{
		const int num_files = arrlen(g.file_arr);
		for (int i=0; i<num_pollfd; ++i) {
			struct pollfd* pollfd = &pollfd_arr[i];

			int revents = pollfd->revents;
			assert(!(revents & POLLNVAL) && "invalid fd added?");

			if (revents & POLLERR) {
				assert(!"handle ERR");
			}
			if (revents & POLLHUP) {
				assert(!"handle HUP");
			}

			struct file* file = NULL;
			for (int ii=0; ii<num_files; ++ii) {
				struct file* f = &g.file_arr[ii];
				if (f->posix_fd != pollfd->fd) continue;
				file = f;
				break;
			}
			assert((file != NULL) && "fd not found?!");

			revents &= (POLLIN | POLLOUT); // only consider these events from here on
			int num_subs = arrlen(file->submission_arr);
			for (int ii=0; revents && ii<num_subs; ++ii) {
				struct submission* sub = &file->submission_arr[ii];
				int do_fire=0;
				switch (sub->type) {
				case SUBMISSION_PREAD:
					if (revents & POLLIN) {
						do_fire = 1;
						revents &= ~POLLIN;
					}
					break;
				case SUBMISSION_PWRITE:
					if (revents & POLLOUT) {
						do_fire = 1;
						revents &= ~POLLOUT;
					}
					break;
				default: assert(!"unhandled submission type");
				}
				if (!do_fire) continue;
				arrput(fire_arr, ((struct fire) {
					.posix_fd = file->posix_fd,
					.sub = *sub,
				}));
				arrdel(file->submission_arr, ii);
				--ii;
				--num_subs;
			}
		}
	}
	G_UNLOCK();

	// do I/O calls
	const int num_fire = arrlen(fire_arr);
	for (int i=0; i<num_fire; ++i) {
		struct fire* fire = &fire_arr[i];
		const int posix_fd = fire->posix_fd;
		struct submission* sub = &fire->sub;
		int e = 0;

		switch (sub->type) {

		case SUBMISSION_PREAD: {
			e = pread(posix_fd, sub->pread.ptr, sub->pread.count, sub->pread.offset);
		}	break;

		case SUBMISSION_PWRITE: {
			//printf("pwrite %p %zd %zd\n", sub->pwrite.ptr, sub->pwrite.count, sub->pwrite.offset);
			e = pwrite(posix_fd, sub->pwrite.ptr, sub->pwrite.count, sub->pwrite.offset);
		}	break;

		default: assert(!"unhandled submission type");
		}

		assert((e >= 0) && "handle error");
	}

	// put events into ports so io_port_poll() can find them
	G_LOCK();
	{
		const int num_ports = arrlen(g.port_arr);
		for (int i=0; i<num_fire; ++i) {
			struct fire* fire = &fire_arr[i];
			const int port_id = fire->sub.port_id;
			int found_port = 0;
			for (int ii=0; ii<num_ports; ++ii) {
				struct port* port = &g.port_arr[ii];
				if (port_id != port->port_id) continue;
				arrput(port->event_arr, ((struct io_event) {
					.echo = fire->sub.echo,
					.status = fire->status,
				}));
				found_port = 1;
				break;
			}
			assert(found_port);
		}
	}
	G_UNLOCK();
}

void io_init(void)
{
	assert(thrd_success == mtx_init(&g.mutex, mtx_plain));
}
