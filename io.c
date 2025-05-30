#include <assert.h>
#include <poll.h>
#include <unistd.h>
#include <threads.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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
	SUBMISSION_READ=1,
	SUBMISSION_WRITE,
	SUBMISSION_WRITEALL,
	SUBMISSION_PREAD,
	SUBMISSION_PWRITE,
	SUBMISSION_SENDFILE,
	SUBMISSION_SENDFILEALL,
	// TODO send/recv?
	INTERNAL_ACCEPT,
};

struct submission {
	int port_id;
	enum submission_type type;
	io_echo echo;
	union {
		struct {
			void* ptr;
			int64_t count;
		} read;
		struct {
			const void* ptr;
			int64_t count;
		} write;
		struct {
			const void* ptr;
			int64_t count;
		} writeall;
		struct {
			void* ptr;
			int64_t count;
			int64_t offset;
		} pread;
		struct {
			const void* ptr;
			int64_t count;
			int64_t offset;
		} pwrite;
		struct {
			int src_file_id;
			int64_t count;
			int64_t src_offset;
		} sendfile;
		struct {
			int src_file_id;
			int64_t count;
			int64_t src_offset;
		} sendfileall;
	};
};

enum file_type {
	DISK,
	LISTEN,
	SOCKET,
};

struct file {
	enum file_type type;
	int file_id;
	int posix_fd;
	struct submission* submission_arr;
	struct sockaddr_in addr;
	int is_pipe_destination;
	int num_pipe_destinations;
};

struct listen {
	io_echo echo;
	int posix_fd;
};

struct port {
	int port_id;
	struct io_event* event_arr;
	struct listen* listen_arr;
};

struct fire {
	int posix_fd;
	int status;
	struct submission sub;
	unsigned did_resub  :1;
};

static struct {
	int port_id_sequence;
	int file_id_sequence;
	mtx_t mutex;
	struct file* file_arr;
	struct port* port_arr;
} g;

static int alloc_file_id(void)
{
	return ++g.file_id_sequence;
}

static void G_LOCK(void)
{
	assert(thrd_success == mtx_lock(&g.mutex));
}

static void G_UNLOCK(void)
{
	assert(thrd_success == mtx_unlock(&g.mutex));
}


int io_open(const char* path, enum io_open_mode mode, int64_t* out_filesize)
{
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
	const int file_id = alloc_file_id();
	arrput(g.file_arr, ((struct file) {
		.type = DISK,
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

static struct file* get_file_by_posix_fd(int posix_fd)
{
	const int num_files = arrlen(g.file_arr);
	for (int ii=0; ii<num_files; ++ii) {
		struct file* f = &g.file_arr[ii];
		if (f->posix_fd != posix_fd) continue;
		return f;
	}
	assert(!"posix fd not found?");
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


int io_listen_tcp(int bind_port, int port_id, io_echo echo)
{
	const int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_fd == -1) {
		fprintf(stderr, "socket(AF_INET, SOCK_STREAM, 0): %s\n", strerror(errno));
		abort();
	}

	int yes = 1;
	setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

	struct sockaddr_in my_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = INADDR_ANY, // XXX make overridable?
		.sin_port = htons(bind_port),
	};

	int e;
	e = bind(listen_fd, (struct sockaddr*)&my_addr, sizeof my_addr);
	if (e == -1) {
		fprintf(stderr, "bind(): %s\n", strerror(errno));
		abort();
	}

	const int backlog = 25;
	e = listen(listen_fd, backlog);
	if (e == -1) {
		fprintf(stderr, "listen(...,%d): %s\n", backlog, strerror(errno));
		abort();
	}

	G_LOCK();
	const int file_id = alloc_file_id();
	arrput(g.file_arr, ((struct file) {
		.type = LISTEN,
		.file_id = file_id,
		.posix_fd = listen_fd,
	}));

	const int num_ports = arrlen(g.port_arr);
	int found_port = 0;
	for (int i=0; i<num_ports; ++i) {
		struct port* port = &g.port_arr[i];
		if (port->port_id != port_id) continue;
		arrput(port->listen_arr, ((struct listen) {
			.echo = echo,
			.posix_fd = listen_fd,
		}));
		found_port = 1;
		break;
	}
	assert(found_port);
	G_UNLOCK();

	return file_id;
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

void io_port_read(int port_id, io_echo echo, int file_id, void* ptr, int64_t count)
{
	struct submission s = {
		.port_id = port_id,
		.type = SUBMISSION_READ,
		.echo = echo,
		.read = {
			.ptr = ptr,
			.count = count,
		},
	};
	submit(file_id, &s);
}

void io_port_write(int port_id, io_echo echo, int file_id, const void* ptr, int64_t count)
{
	struct submission s = {
		.port_id = port_id,
		.type = SUBMISSION_WRITE,
		.echo = echo,
		.write = {
			.ptr = ptr,
			.count = count,
		},
	};
	submit(file_id, &s);
}

void io_port_writeall(int port_id, io_echo echo, int file_id, const void* ptr, int64_t count)
{
	struct submission s = {
		.port_id = port_id,
		.type = SUBMISSION_WRITEALL,
		.echo = echo,
		.writeall = {
			.ptr = ptr,
			.count = count,
		},
	};
	submit(file_id, &s);
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

void io_port_pwrite(int port_id, io_echo echo, int file_id, const void* ptr, int64_t count, int64_t offset)
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

void io_port_sendfile(int port_id, io_echo echo, int dst_file_id, int src_file_id, int64_t count, int64_t src_offset)
{
	struct submission s = {
		.port_id = port_id,
		.type = SUBMISSION_SENDFILE,
		.echo = echo,
		.sendfile = {
			.src_file_id=src_file_id,
			.count = count,
			.src_offset = src_offset,
		},
	};
	submit(dst_file_id, &s);
}

void io_port_sendfileall(int port_id, io_echo echo, int dst_file_id, int src_file_id, int64_t count, int64_t src_offset)
{
	struct submission s = {
		.port_id = port_id,
		.type = SUBMISSION_SENDFILEALL,
		.echo = echo,
		.sendfileall = {
			.src_file_id=src_file_id,
			.count = count,
			.src_offset = src_offset,
		},
	};
	submit(dst_file_id, &s);
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

void io_addr(int file_id)
{
	struct file* file = get_file(file_id);
	assert(file->type == SOCKET);
	struct sockaddr_in a = file->addr;
	const int port = ntohs(a.sin_port);
	const uint32_t addr = ntohl(a.sin_addr.s_addr);
	printf("addr %d.%d.%d.%d %d\n",
		(addr >> 24) & 0xff,
		(addr >> 16) & 0xff,
		(addr >> 8 ) & 0xff,
		(addr      ) & 0xff,
		port);
}

struct listenecho {
	int port_id;
	io_echo echo;
};



int io_tick(void)
{
	static struct pollfd* pollfd_arr;
	arrreset(pollfd_arr);

	static struct listenecho* listenecho_arr;
	arrreset(listenecho_arr);

	int first_listen_index = -1;

	// populate pollfd_arr with fds/events corresponding to submission types
	G_LOCK();
	{
		const int num_files = arrlen(g.file_arr);

		// clear "piping info" on files (sendfile)
		for (int i=0; i<num_files; ++i) {
			struct file* file = &g.file_arr[i];
			file->is_pipe_destination = 0;
			file->num_pipe_destinations = 0;
		}

		// figure out which files are sendfile sources
		for (int i=0; i<num_files; ++i) {
			struct file* file = &g.file_arr[i];
			const int num_subs = arrlen(file->submission_arr);
			for (int ii=0; ii<num_subs; ++ii) {
				struct submission* sub = &file->submission_arr[ii];
				int src_file_id = -1;

				switch (sub->type) {
				case SUBMISSION_SENDFILE    : src_file_id = sub->sendfile.src_file_id    ; break;
				case SUBMISSION_SENDFILEALL : src_file_id = sub->sendfileall.src_file_id ; break;
				default: break;
				}

				if (src_file_id >= 0) {
					file->is_pipe_destination = 1;
					++get_file(src_file_id)->num_pipe_destinations;
				}
			}
		}

		// populate pollfd data for poll(2). 3 categories:
		// - common I/O; reads, writes...
		// - pipes between files
		// - tcp/ip sockets listening for new connections
		for (int i=0; i<num_files; ++i) {
			struct file* file = &g.file_arr[i];
			int events = 0;
			if (file->num_pipe_destinations) {
				events |= POLLIN;
				arrput(pollfd_arr, ((struct pollfd) {
					.fd = file->posix_fd,
					.events = events,
				}));
			}

			const int num_subs = arrlen(file->submission_arr);
			if (num_subs == 0) continue;

			// handle common I/O
			for (int ii=0; ii<num_subs; ++ii) {
				struct submission* sub = &file->submission_arr[ii];
				int add=0;

				switch (sub->type) {

				case SUBMISSION_READ:
				case SUBMISSION_PREAD:
					add = POLLIN;
					break;

				case SUBMISSION_WRITE:
				case SUBMISSION_WRITEALL:
				case SUBMISSION_PWRITE:
				case SUBMISSION_SENDFILE:    // handling sendfile destinations here
				case SUBMISSION_SENDFILEALL: // sources are handled above
					add = POLLOUT;
					break;

				default: assert(!"unhandled submission type");
				}

				assert(add);
				if (!(events & add)) {
					if (!events) {
						events |= add;
						arrput(pollfd_arr, ((struct pollfd) {
							.fd = file->posix_fd,
							.events = events,
						}));
					} else {
						events |= add;
						struct pollfd* pf = &pollfd_arr[arrlen(pollfd_arr)-1];
						pf->events |= add;
						assert(pf->events == events);
					}
				}
			}
		}

		// handle listening sockets
		first_listen_index = arrlen(pollfd_arr);
		const int num_ports = arrlen(g.port_arr);
		for (int i=0; i<num_ports; ++i) {
			struct port* port = &g.port_arr[i];
			const int num_listen = arrlen(port->listen_arr);
			for (int ii=0; ii<num_listen; ++ii) {
				struct listen* listen = &port->listen_arr[ii];
				arrput(pollfd_arr, ((struct pollfd) {
					.fd = listen->posix_fd,
					.events = POLLIN,
				}));
				arrput(listenecho_arr, ((struct listenecho) {
					.port_id = port->port_id,
					.echo = listen->echo,
				}));
			}
		}
	}

	assert((0 <= first_listen_index) && (first_listen_index <= arrlen(pollfd_arr)));
	assert(arrlen(listenecho_arr) == (arrlen(pollfd_arr) - first_listen_index));
	G_UNLOCK();

	const int num_pollfd = arrlen(pollfd_arr);
	if (num_pollfd == 0) return 0;

	// execute poll(2)
	int e = poll(pollfd_arr, num_pollfd, 0);
	if (e == -1) {
		if (errno == EINTR) {
			return 1;
		}
		fprintf(stderr, "poll(): %s\n", strerror(errno));
		abort();
	} else if (e == 0) {
		return 0;
	}
	assert(e>0);

	static struct fire* fire_arr;
	arrreset(fire_arr);

	// convert poll() output to list of calls to make
	G_LOCK();
	for (int i=0; i<num_pollfd; ++i) {
		struct pollfd* pollfd = &pollfd_arr[i];
		int revents = pollfd->revents;
		assert(first_listen_index >= 0);

		if (i < first_listen_index) {
			assert(!(revents & POLLNVAL) && "invalid fd added?");

			if (revents & POLLERR) {
				assert(!"handle ERR");
			}
			if (revents & POLLHUP) {
				assert(!"handle HUP");
			}

			struct file* file = get_file_by_posix_fd(pollfd->fd);

			revents &= (POLLIN | POLLOUT); // only consider these events from here on
			int num_subs = arrlen(file->submission_arr);
			for (int ii=0; revents && ii<num_subs; ++ii) {
				struct submission* sub = &file->submission_arr[ii];
				int do_fire=0;
				switch (sub->type) {

				case SUBMISSION_READ:
				case SUBMISSION_PREAD:
					if (revents & POLLIN) {
						do_fire = 1;
						revents &= ~POLLIN;
					}
					break;

				case SUBMISSION_WRITE:
				case SUBMISSION_WRITEALL:
				case SUBMISSION_PWRITE:
					if (revents & POLLOUT) {
						do_fire = 1;
						revents &= ~POLLOUT;
					}
					break;

				case SUBMISSION_SENDFILE:
					if (revents & POLLOUT) {
						struct file* src_file = get_file(sub->sendfile.src_file_id);
						int src_is_ready = 0;
						for (int iii=0; iii<num_pollfd; ++iii) {
							struct pollfd* other = &pollfd_arr[iii];
							if (other->fd == src_file->posix_fd) {
								if (other->revents & POLLIN) {
									src_is_ready = 1;
								}
								break;
							} 
						}
						if (src_is_ready) {
							do_fire = 1;
							revents &= ~POLLOUT;
						}
					}
					break;

				case SUBMISSION_SENDFILEALL:
					if (revents & POLLOUT) {
						struct file* src_file = get_file(sub->sendfileall.src_file_id);
						int src_is_ready = 0;
						for (int iii=0; iii<num_pollfd; ++iii) {
							struct pollfd* other = &pollfd_arr[iii];
							if (other->fd == src_file->posix_fd) {
								if (other->revents & POLLIN) {
									src_is_ready = 1;
								}
								break;
							} 
						}
						if (src_is_ready) {
							do_fire = 1;
							revents &= ~POLLOUT;
						}
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
		} else { // i >= first_listen_index

			if (revents & POLLIN) {
				const int il = i - first_listen_index;
				const int num_listenecho = arrlen(listenecho_arr);
				assert((0 <= il) && (il < num_listenecho));
				struct listenecho* le = &listenecho_arr[il];

				arrput(fire_arr, ((struct fire) {
					.posix_fd = pollfd->fd,
					.sub = {
						.type = INTERNAL_ACCEPT,
						.port_id = le->port_id,
						.echo = le->echo,
					},
				}));
			}
		}
	}
	G_UNLOCK();

	static struct sockaddr_in* addr_arr;
	arrreset(addr_arr);

	// do I/O calls
	const int num_fire = arrlen(fire_arr);
	for (int i=0; i<num_fire; ++i) {
		struct fire* fire = &fire_arr[i];
		const int posix_fd = fire->posix_fd;
		struct submission* sub = &fire->sub;
		struct submission resub;

		int resub_fd=-1;
		switch (sub->type) {

		case SUBMISSION_READ: {
			fire->status = read(posix_fd, sub->read.ptr, sub->read.count);
		}	break;

		case SUBMISSION_WRITE: {
			fire->status = write(posix_fd, sub->write.ptr, sub->write.count);
		}	break;

		case SUBMISSION_WRITEALL: {
			fire->status = write(posix_fd, sub->writeall.ptr, sub->writeall.count);
			if (fire->status != -1) {
				if (sub->writeall.count > fire->status) {
					fire->did_resub=1;
					resub_fd=posix_fd;
					resub=*sub;
					resub.writeall.ptr   += fire->status;
					resub.writeall.count -= fire->status;
				} else {
					assert(sub->writeall.count == fire->status);
				}
			}
		}	break;

		case SUBMISSION_PREAD: {
			fire->status = pread(posix_fd, sub->pread.ptr, sub->pread.count, sub->pread.offset);
			if (fire->status != -1) {
				assert((fire->status == sub->pread.count) && "TODO resub");
			}
		}	break;

		case SUBMISSION_PWRITE: {
			fire->status = pwrite(posix_fd, sub->pwrite.ptr, sub->pwrite.count, sub->pwrite.offset);
			if (fire->status != -1) {
				assert((fire->status == sub->pwrite.count) && "TODO resub");
			}
		}	break;

		case SUBMISSION_SENDFILE: {
			const int src_fd = file_id_to_posix_fd(sub->sendfile.src_file_id);
			off_t o = sub->sendfile.src_offset;
			assert(!"XXX0");
			fire->status = sendfile(posix_fd, src_fd, &o, sub->sendfile.count);
		}	break;

		case SUBMISSION_SENDFILEALL: {
			const int src_fd = file_id_to_posix_fd(sub->sendfileall.src_file_id);
			off_t o = sub->sendfileall.src_offset;
			fire->status = sendfile(posix_fd, src_fd, &o, sub->sendfileall.count);
			if (fire->status != -1) {
				if (sub->sendfileall.count > fire->status) {
					fire->did_resub=1;
					resub_fd=posix_fd;
					resub=*sub;
					resub.sendfileall.src_offset  += fire->status;
					resub.sendfileall.count       -= fire->status;
				} else {
					assert(sub->sendfileall.count == fire->status);
				}
			}
		}	break;

		case INTERNAL_ACCEPT: {
			struct sockaddr_in addr;
			socklen_t size = sizeof addr;
			fire->status = accept(posix_fd, (struct sockaddr*)&addr, &size);
			if (fire->status >= 0) {
				const int e = fcntl(fire->status, F_SETFL, O_NONBLOCK);
				if (e == -1) {
					assert(!"XXX");
					fire->status = -1;
				} else  {
					arrput(addr_arr, addr);
				}
			}
		}	break;

		default: assert(!"unhandled submission type");
		}

		if (resub_fd>=0) {
			struct file* file = NULL;
			const int num_files = arrlen(g.file_arr);
			for (int ii=0; ii<num_files; ++ii) {
				struct file* f = &g.file_arr[ii];
				if (f->posix_fd != resub_fd) continue;
				file = f;
				break;
			}
			assert((file != NULL) && "fd not found?!");
			submit(file->file_id, &resub);
		}

	}

	// put events into ports so io_port_poll() can find them
	G_LOCK();
	{
		int addr_index = 0;
		const int num_addr = arrlen(addr_arr);
		const int num_ports = arrlen(g.port_arr);
		for (int i=0; i<num_fire; ++i) {
			struct fire* fire = &fire_arr[i];
			if (fire->did_resub) continue;

			if (fire->status == -1) {
				// TODO convert status?
				fire->status = IO_ERROR;
			} else if (fire->sub.type == INTERNAL_ACCEPT && fire->status >= 0) {
				const int file_id = alloc_file_id();
				assert((0 <= addr_index) && (addr_index < num_addr));
				arrput(g.file_arr, ((struct file) {
					.type = SOCKET,
					.file_id = file_id,
					.posix_fd = fire->status,
					.addr = addr_arr[addr_index++],
				}));
				fire->status = file_id;
			}

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
		assert(addr_index == num_addr);
	}
	G_UNLOCK();

	return 1;
}

void io_init(void)
{
	assert(thrd_success == mtx_init(&g.mutex, mtx_plain));
}
