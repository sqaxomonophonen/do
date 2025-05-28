// RFCs:
//   RFC2616: Hypertext Transfer Protocol -- HTTP/1.1
//   RFC6455: The WebSocket Protocol
// (NOTE: RFC2616 is supposedly "obsoleted", and replaced by RFC7230 thru
// RFC7235, and later RFC9110. If these are used to guide this implementation
// include them in the list above. I'm currently avoiding them because RFC723x
// splits the standard into multiple docs (whyy) and RFC9110 includes HTTP/2
// and beyond (ew))

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "httpserver.h"
#include "io.h"
#include "util.h"

#define IGNORE_ECHO (-100)
#define LISTEN_ECHO (-101)

#define BUFFER_SIZE_LOG2     (14)
#define BUFFER_SIZE          (1L << (BUFFER_SIZE_LOG2))
#define MAX_CONN_COUNT_LOG2  (8)
#define MAX_CONN_COUNT       (1L << (MAX_CONN_COUNT_LOG2))

// canned responses (these can be sent cheaply in a fire'n'forget manner
// because the memory is static and we close the connection)

#define BLAHBLAHBLAH \
	"Content-Type: text/plain; charset=utf-8\r\n" \
	"Connection: close\r\n" \

static const char R404[]=
	"HTTP/1.1 404 Not Found\r\n"
	BLAHBLAHBLAH
	"Content-Length: 13\r\n"
	"\r\n"
	"404 Not Found"
	;

static const char R413[]=
	"HTTP/1.1 413 Payload Too Large\r\n"
	BLAHBLAHBLAH
	"Content-Length: 21\r\n"
	"\r\n"
	"413 Payload Too Large"
	;

static const char R503[]=
	"HTTP/1.1 503 Service Unavailable\r\n"
	BLAHBLAHBLAH
	"Content-Length: 23\r\n"
	"\r\n"
	"503 Service Unavailable"
	;

enum conn_state {
	REQUEST = 1,
};

struct conn {
	enum conn_state state;
	int file_id;
};

static struct {
	int port_id;
	int listen_file_id;

	uint8_t* buffer_storage;
	struct conn* conns;
	int* freelist;
	int num_free;
	int next;
} g;

static int alloc_conn(void)
{
	assert((0 <= g.num_free) && (g.num_free <= MAX_CONN_COUNT));
	if (g.num_free > 0) {
		--g.num_free;
		assert(g.num_free >= 0);
		return g.freelist[g.num_free];
	}
	if (g.next == MAX_CONN_COUNT) return -1;
	assert((0 <= g.next) && (g.next < MAX_CONN_COUNT));
	return g.next++;
}

static void free_conn(int id)
{
	assert((0 <= id) && (id < MAX_CONN_COUNT));
	assert(g.num_free < MAX_CONN_COUNT);
	g.freelist[g.num_free++] = id;
}

static uint8_t* get_conn_buffer(int id)
{
	assert((0 <= id) && (id < MAX_CONN_COUNT));
	return &g.buffer_storage[id << BUFFER_SIZE_LOG2];
}

static struct conn* get_conn(int id)
{
	assert((0 <= id) && (id < MAX_CONN_COUNT));
	return &g.conns[id];
}

void httpserver_init(void)
{
	g.port_id = io_port_create();
	io_echo echo0 = { .i64 = LISTEN_ECHO };
	g.listen_file_id = io_listen_tcp(6510, g.port_id, echo0);
	g.buffer_storage = calloc(1L << (MAX_CONN_COUNT_LOG2 + BUFFER_SIZE_LOG2), sizeof *g.buffer_storage);
	g.conns    = calloc(1L << MAX_CONN_COUNT_LOG2, sizeof *g.conns);
	g.freelist = calloc(1L << MAX_CONN_COUNT_LOG2, sizeof *g.freelist);
}

static void serve_static_and_close(int file_id, const void* data, size_t size)
{
	io_echo echo = { .i64 = IGNORE_ECHO };
	io_port_write(g.port_id, echo, file_id, data, size);
	// XXX ... and close?
}

// use this with the "canned responses", R404 etc. assumes that sizeof(R) is
// one larger that the response due to NUL-terminator
#define SERVE_STATIC_AND_CLOSE(FILE_ID,R) serve_static_and_close(FILE_ID,R,sizeof(R)-1)

// parses HTTP/1.1 request between pstart/pend. the memory is modified.
static void parse_request(uint8_t* pstart, uint8_t* pend)
{
	uint8_t* p=pstart;
	uint8_t* method0=p;
	for (; p<pend && *p>' '; ++p) {}
	int err = (*p != ' ');
	uint8_t* method1=p;

	enum http_method m = UNKNOWN;

	#define X(ENUM) \
	if (m==UNKNOWN) { \
		static const char METHOD[] = #ENUM; \
		const size_t n = sizeof(METHOD)-1; \
		if ((n == (method1-method0)) && memcmp(method0,METHOD,n)==0) { \
			m = ENUM; \
		} \
	}
	LIST_OF_METHODS
	#undef X

	++p;
	uint8_t* path0=p;
	for (; p<pend && *p>' '; ++p) {}
	err |= (*p != ' ');
	uint8_t* path1=p;
	*path1 = 0; // insert NUL-terminator

	++p;
	uint8_t* proto0=p;
	for (; p<(pend-1) && *p>' '; ++p) {}
	err |= (p[0] != '\r');
	err |= (p[1] != '\n');
	uint8_t* proto1=p;

	static const char PROTO[] = "HTTP/1.1";
	err |= ((proto1-proto0) != (sizeof(PROTO)-1) || memcmp(proto0,PROTO,proto1-proto0) != 0);

	printf("m=%d path=[%s]/%zd err=%d\n", m, path0, path1-path0, err);
	// TODO do "routing" here?

	p+=2;

	int end_of_header = 0;
	while ((p<pend) && !end_of_header) {
		uint8_t* head0=p;
		uint8_t* headcolon=NULL;
		for (; p<(pend-1) && *p!='\r'; ++p) {
			if ((headcolon==NULL) && (*p==':')) {
				headcolon=p;
			}
		}
		uint8_t* head1=p;
		if ((p[0]=='\r') && (head1==head0)) {
			end_of_header = 1;
		}
		err |= (p[0] != '\r');
		err |= (p[1] != '\n');
		if (!end_of_header) {
			err |= (headcolon==NULL);
			*head1 = 0; // insert NUL-terminator
			printf("header [%s] err=%d\n", head0, err);
		}
		p+=2;
	}
}

int httpserver_tick(void)
{
	int did_work = 0;
	struct io_event ev;
	while (io_port_poll(g.port_id, &ev)) {
		did_work = 1;
		if (ev.echo.i64 == IGNORE_ECHO) {
			// ok
		} else if (ev.echo.i64 == LISTEN_ECHO) {
			printf("http ev status=%d!\n", ev.status);
			if (ev.status >= 0) {
				io_addr(ev.status);
			}
			const int id = alloc_conn();
			if (id == -1) {
				SERVE_STATIC_AND_CLOSE(ev.status, R503);
			} else {
				struct conn* conn = get_conn(id);
				memset(conn, 0, sizeof *conn);
				conn->file_id = ev.status;
				conn->state = REQUEST;
				uint8_t* buf = get_conn_buffer(id);
				io_echo echo = { .i64 = id };
				io_port_read(g.port_id, echo, conn->file_id, buf, BUFFER_SIZE);
			}
		} else {
			const int64_t id64 = ev.echo.i64;
			assert((0 <= id64) && (id64 < MAX_CONN_COUNT));
			const int id = id64;
			struct conn* conn = get_conn(id);

			const int num_bytes = ev.status;
			uint8_t* buf = get_conn_buffer(id);
			parse_request(buf, buf+num_bytes);

			SERVE_STATIC_AND_CLOSE(conn->file_id, R404); // XXX
		}
	}
	return did_work;
}
