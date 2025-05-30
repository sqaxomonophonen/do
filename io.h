#ifndef IO_H

#include <stdint.h>

enum io_open_mode {
	IO_OPEN_RDONLY = 1,
	IO_OPEN,
	IO_OPEN_OR_CREATE,
	IO_CREATE,
};

#define LIST_OF_IO_ERRORS \
	X(IO_ERROR             , "an error occurred"       , -21000) \
	X(IO_NOT_FOUND         , "file not found"          , -21001) \
	X(IO_ALREADY_EXISTS    , "file already exists"     , -21002) \
	X(IO_NOT_PERMITTED     , "operation not permitted" , -21003) \
	X(IO_READ_ERROR        , "read error"              , -21004) \
	X(IO_READ_OUT_OF_RANGE , "read out of range"       , -21005) \
	X(IO_PENDING           , "work remaining"          , -21006) \
	X(IO_BUFFER_FULL       , "buffer is full"          , -21007)

enum io_error {
	#define X(ENUM,_MSG,ID) ENUM=ID,
	LIST_OF_IO_ERRORS
	#undef X
};

typedef union {
	uint64_t u64;
	int64_t  i64;
	void*    ptr;
	intptr_t intptr;
	struct {
		uint32_t ua32;
		uint32_t ub32;
	};
} io_echo;

struct io_event {
	io_echo echo;
	int status;
};

const char* io_error_to_string(int error);
// returns static error string for error id, or NULL if not one of the ones in
// the LIST_OF_IO_ERRORS X macros

static inline const char* io_error_to_string_safe(int error)
{
	const char* s = io_error_to_string(error);
	return s ? s : "(not a io error)";
}

int io_open(const char* path, enum io_open_mode, int64_t* out_filesize);
int io_close(int file_id);

int io_pread(int file_id, void* ptr, int64_t count, int64_t offset);

int io_listen_tcp(int bind_port, int port_id, io_echo);
// start listening on :bind_port (TODO addr?).
// connections can be received on the port_id where the echo is what you set it
// to, and status is the file id for the new connection

void io_addr(int file_id);

int io_port_create(void);
int io_port_poll(int port_id, struct io_event*);

void io_port_read(int port_id, io_echo echo, int file_id, void* ptr, int64_t count);
void io_port_write(int port_id, io_echo echo, int file_id, const void* ptr, int64_t count);
void io_port_writeall(int port_id, io_echo echo, int file_id, const void* ptr, int64_t count);

void io_port_pread(int port_id, io_echo echo, int file_id, void* ptr, int64_t count, int64_t offset);
void io_port_pwrite(int port_id, io_echo echo, int file_id, const void* ptr, int64_t count, int64_t offset);
void io_port_sendfile(int port_id, io_echo echo, int dst_file_id, int src_file_id, int64_t count, int64_t src_offset);
void io_port_sendfileall(int port_id, io_echo echo, int dst_file_id, int src_file_id, int64_t count, int64_t src_offset);
// sendfile is (probably?) only guaranteed to work properly when dst_file_id is
// a socket, and src_file_id is a file.

void io_init(void);
int io_tick(void);

#define IO_H
#endif
