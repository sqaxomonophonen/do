// search this file for "ROUTE" to find the routes

// RFCs:
//   RFC2616: Hypertext Transfer Protocol -- HTTP/1.1
//   RFC6455: The WebSocket Protocol

// (NOTE: RFC2616 is supposedly "obsoleted", and replaced by RFC7230 thru
// RFC7235, and later RFC9110. If these are used to guide this implementation
// include them in the list above. I'm currently avoiding them because RFC723x
// splits the standard into multiple docs (whyy) and RFC9110 includes HTTP/2
// and beyond (corporate bloatware). Also, since HTTP/1.1 isn't about to die,
// the only thing the newer RFCs can offer is corrections and clarifications?)

#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

#include "webserv.h"
#include "io.h"
#include "util.h"
#include "sha1.h"
#include "base64.h"
#include "stb_sprintf.h"

#define CRLF "\r\n"

#define IGNORE_ECHO_TYPE (-10)
#define LISTEN_ECHO_TYPE (-11)

#define IGNORE_ECHO ((io_echo) { .ia32 = IGNORE_ECHO_TYPE })
#define LISTEN_ECHO ((io_echo) { .ia32 = LISTEN_ECHO_TYPE })

#ifndef NO_WEBPACK
#include "webpack.gen.h" // <- missing? see codegen/disable comments below:
// to codegen webpack.gen.h, run `./webpack.bash` (requires Emscripten)
// to disable "webpack" (web app at route /o) build with NO_WEBPACK defined:
// `CFLAGS="-DNO_WEBPACK" gmake -f Makefile...`

static const char* webpack_lookup(const char* name)
{
	#define X(LOCNAME,DOKNAME) if (strcmp(name,LOCNAME)==0) return DOKNAME;
	LIST_OF_WEBPACK_FILES
	#undef X
	fprintf(stderr, "webpack_lookup(\"%s\") failed\n", name);
	abort();
}
#endif

static inline int is_ignore_echo(io_echo echo)
{
	return (echo.ia32 == IGNORE_ECHO_TYPE) && (echo.ib32 == 0);
}

static inline int is_listen_echo(io_echo echo)
{
	return (echo.ia32 == LISTEN_ECHO_TYPE) && (echo.ib32 == 0);
}

static inline int is_type(io_echo echo, int type, int* out_id)
{
	if (echo.ia32!=type) return 0;
	if (out_id) *out_id = echo.ib32;
	return 1;
}

enum { CLOSE=1,READ,WRITE,SENDFILE };

static inline io_echo echo_write(int conn_id)
{
	return (io_echo) {.ia32=WRITE, .ib32=conn_id };
}

static inline int is_echo_write(io_echo echo, int* out_conn_id)
{
	return is_type(echo, WRITE, out_conn_id);
}

static inline io_echo echo_read(int conn_id)
{
	return (io_echo) {.ia32=READ, .ib32=conn_id };
}

static inline int is_echo_read(io_echo echo, int* out_conn_id)
{
	return is_type(echo, READ, out_conn_id);
}

static inline io_echo echo_close(int conn_id)
{
	return (io_echo) {.ia32=CLOSE, .ib32=conn_id };
}

static inline int is_echo_close(io_echo echo, int* out_conn_id)
{
	return is_type(echo, CLOSE, out_conn_id);
}


#define BUFFER_SIZE_LOG2     (14)
#define BUFFER_SIZE          (1L << (BUFFER_SIZE_LOG2))
#define MAX_CONN_COUNT_LOG2  (8)
#define MAX_CONN_COUNT       (1L << (MAX_CONN_COUNT_LOG2))

#define LIST_OF_METHODS \
	X(HEAD) \
	X(GET) \
	X(POST) \
	X(PUT)

enum http_method {
	UNKNOWN=0,
	#define X(ENUM) ENUM,
	LIST_OF_METHODS
	#undef X
	METHOD_COUNT
};
static_assert(METHOD_COUNT <= 31, "(1<<METHOD_COUNT) close to int-overflow -- too many http methods...");

// canned responses in static memory

static const char R404[]=
	"HTTP/1.1 404 Not Found" CRLF
	"Content-Type: text/plain; charset=utf-8" CRLF
	"Content-Length: 13" CRLF
	CRLF
	"404 Not Found"
	;

#define BLAHBLAHBLAH \
	"Content-Type: text/plain; charset=utf-8" CRLF \
	"Connection: close" CRLF \

#if 0
static const char R200test[]=
	"HTTP/1.1 200 OK" CRLF
	BLAHBLAHBLAH
	"Content-Length: 13" CRLF
	CRLF
	"This Is Fine!"
	;
#endif

static const char R400proto[]=
	"HTTP/1.1 400 Bad Request" CRLF
	BLAHBLAHBLAH
	"Content-Length: 32" CRLF
	CRLF
	"400 Bad Request (protocol error)"
	;

static const char R413[]=
	"HTTP/1.1 413 Payload Too Large" CRLF
	BLAHBLAHBLAH
	"Content-Length: 21" CRLF
	CRLF
	"413 Payload Too Large"
	;

static const char R503[]=
	"HTTP/1.1 503 Service Unavailable" CRLF
	BLAHBLAHBLAH
	"Content-Length: 23" CRLF
	CRLF
	"503 Service Unavailable"
	;

enum conn_state {
	HTTP_REQUEST = 1,
	HTTP_RESPONSE,
	WEBSOCKET,
	CLOSING,
};

enum websock_state {
	HEAD_FIN_RSV_OPCODE=1,
	HEAD_MASK_PLEN7,
	HEAD_PLEN16,
	HEAD_PLEN64,
	HEAD_MASKKEY,
	PAYLOAD,
};

struct websock {
	enum websock_state wstate;
	int header_cursor;
	int64_t payload_length;
	int64_t remaining;
	uint8_t mask_key[4];
	unsigned  fin    :1;
	unsigned  opcode :4;
};

struct conn {
	enum conn_state cstate;
	int file_id;
	int write_cursor;
	int num_writes_pending;
	int sendfile_src_file_id;
	unsigned buffer_full :1;
	unsigned enter_websocket_after_response :1;
	unsigned inflight_sendfile :1;
	unsigned inflight_write    :1; // TODO
	unsigned inflight_read     :1;
	union {
		struct websock websock;
	};
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

static int get_conn_id_by_conn(struct conn* conn)
{
	int64_t id = conn - g.conns;
	assert((0 <= id) && (id < MAX_CONN_COUNT));
	return id;
}

static uint8_t* get_conn_buffer_raw(struct conn* conn, int divindex, int bufdiv_log2, size_t* out_size)
{
	const int id = get_conn_id_by_conn(conn);
	assert((0 <= id) && (id < MAX_CONN_COUNT));
	uint8_t* base = &g.buffer_storage[id << BUFFER_SIZE_LOG2];
	const int bufdiv = (1 << bufdiv_log2);
	assert((0 <= divindex) && (divindex < bufdiv));
	int size_log2 = (BUFFER_SIZE_LOG2 - bufdiv_log2);
	const size_t size = 1L << size_log2;
	if (out_size) *out_size = size;
	return base + (divindex << size_log2);
}

#define HTTP_BUFDIV_LOG2 (0)
// HTTP is kind of "half duplex" where the connection is either reading or
// writing, never both at the same time, so we use the entire buffer for reads
// or writes.

#define WS_BUFDIV_LOG2 (1)
// WebSockets are "full duplex", so the connection buffer is split into read
// and write parts

static uint8_t* get_conn_http_read_buffer(struct conn* conn, size_t* out_size)
{
	assert(conn->cstate == HTTP_REQUEST);
	return get_conn_buffer_raw(conn, 0, HTTP_BUFDIV_LOG2, out_size);
}

static uint8_t* get_conn_http_write_buffer(struct conn* conn, size_t* out_size)
{
	assert(conn->cstate == HTTP_RESPONSE);
	return get_conn_buffer_raw(conn, 0, HTTP_BUFDIV_LOG2, out_size);
}

static uint8_t* get_conn_ws_read_buffer(struct conn* conn, size_t* out_size)
{
	assert(conn->cstate == WEBSOCKET);
	return get_conn_buffer_raw(conn, 0, WS_BUFDIV_LOG2, out_size);
}

static uint8_t* get_conn_ws_write_buffer(struct conn* conn, size_t* out_size)
{
	assert(conn->cstate == WEBSOCKET);
	return get_conn_buffer_raw(conn, 1, WS_BUFDIV_LOG2, out_size);
}

static struct conn* get_conn(int id)
{
	assert((0 <= id) && (id < MAX_CONN_COUNT));
	return &g.conns[id];
}

static void free_conn(struct conn* conn)
{
	const int id = get_conn_id_by_conn(conn);
	assert((0 <= id) && (id < MAX_CONN_COUNT));
	assert(g.num_free < MAX_CONN_COUNT);
	g.freelist[g.num_free++] = id;
}

static void conn_enter(struct conn* conn, enum conn_state state)
{
	assert(state > 0);
	int no_reentry = 0;
	switch (state) {
	case HTTP_RESPONSE:
		conn->write_cursor = 0;
		no_reentry = 1;
		break;
	default:
		break;
	}
	assert((!no_reentry || (conn->cstate != state)) && "no reentry!");
	conn->cstate = state;
}

static int conn_can_write(struct conn* conn)
{
	switch (conn->cstate) {
	case HTTP_RESPONSE: return 1;
	default           : return 0;
	}
}

static void conn_read(struct conn* conn, void* buf, int64_t count)
{
	assert(!conn->inflight_read);
	io_port_read(g.port_id, echo_read(get_conn_id_by_conn(conn)), conn->file_id, buf, count);
	conn->inflight_read=1;
}

static void conn_writeall(struct conn* conn, const void* ptr, int64_t count)
{
	io_port_writeall(g.port_id, echo_write(get_conn_id_by_conn(conn)), conn->file_id, ptr, count);
	++conn->num_writes_pending;
}

static void conn_sendfileall(struct conn* conn, int src_file_id, int64_t count, int64_t src_offset)
{
	assert(conn->inflight_sendfile == 0);
	io_port_sendfileall(g.port_id, echo_write(get_conn_id_by_conn(conn)), conn->file_id, src_file_id, count, src_offset);
	conn->sendfile_src_file_id = src_file_id;
	conn->inflight_sendfile = 1;
	++conn->num_writes_pending;
}

static void conn_drop(struct conn* conn)
{
	//printf("dropping conn in state %d\n", conn->cstate);
	conn_enter(conn, CLOSING);
	io_port_close(g.port_id, echo_close(get_conn_id_by_conn(conn)), conn->file_id);
}

void webserv_init(void)
{
	g.port_id = io_port_create();
	g.listen_file_id = io_listen_tcp(6581, g.port_id, LISTEN_ECHO);
	g.buffer_storage = calloc(1L << (MAX_CONN_COUNT_LOG2 + BUFFER_SIZE_LOG2), sizeof *g.buffer_storage);
	g.conns    = calloc(1L << MAX_CONN_COUNT_LOG2, sizeof *g.conns);
	g.freelist = calloc(1L << MAX_CONN_COUNT_LOG2, sizeof *g.freelist);
}

static void serve_static(struct conn* conn, const void* data, size_t size)
{
	conn_writeall(conn, data, size);
}

// use this with the "canned responses", R404 etc. assumes that sizeof(SS) is
// one larger that the response due to NUL-terminator, so only use with
// responses defined like `static char foo[] = "..."`
#define SERVE_STATIC_AND_RETURN(CONN,SS) {serve_static(CONN,SS,sizeof(SS)-1);return;}
#define SERVE_STATIC_CLOSE_AND_RETURN(CONN,SS) {serve_static(CONN,SS,sizeof(SS)-1);conn_enter(conn,CLOSING);return;}

static int is_route(const char* route, const char* path, const char** out_tail)
{
	if (out_tail) *out_tail = NULL;
	const size_t route_len = strlen(route);
	const size_t path_len  = strlen(path);
	if (path_len < route_len) return 0;
	if (route[route_len-1] != '/') {
		return (strcmp(route,path)==0);
	} else {
		if ((path_len > route_len) && (memcmp(route,path,route_len)==0)) {
			if (out_tail) *out_tail = &path[route_len];
			return 1;
		} else {
			return 0;
		}
	}
}

FORMATPRINTF2
static void conn_printf(struct conn* conn, const char* fmt, ...)
{
	assert(conn_can_write(conn));
	size_t bufsize;
	uint8_t* buf = get_conn_http_write_buffer(conn, &bufsize);
	const int r = (bufsize - conn->write_cursor);
	if (r == 0) {
		assert(conn->buffer_full);
		return;
	}
	assert(r>0);
	char* p = (char*)(buf + conn->write_cursor);
	va_list va;
	va_start(va, fmt);
	const int n = stbsp_vsnprintf(p, r, fmt, va);
	va_end(va);
	conn->write_cursor += n;
	if ((bufsize - conn->write_cursor) == 0) {
		conn->buffer_full = 1;
	}
}

static void conn_print(struct conn* conn, const char* str)
{
	assert(conn_can_write(conn));
	size_t bufsize;
	uint8_t* buf = get_conn_http_write_buffer(conn, &bufsize);
	int r = (bufsize - conn->write_cursor);
	if (r == 0) {
		assert(conn->buffer_full);
		return;
	}
	assert(r>0);
	int num = strlen(str);
	if (num > r) num = r;
	uint8_t* p = (buf + conn->write_cursor);
	memcpy(p, str, num);
	conn->write_cursor += num;
}

static void conn_respond(struct conn* conn)
{
	if (conn->buffer_full) {
		fprintf(stderr, "buffer full\n");
		conn_drop(conn);
		return;
	}
	assert(conn_can_write(conn));
	if (conn->write_cursor == 0) return;
	size_t size;
	uint8_t* buf = get_conn_http_write_buffer(conn, &size);
	assert(conn->write_cursor <= size);
	conn_writeall(conn, buf, conn->write_cursor);
}

static void serve405(struct conn* conn, int allow_method_set)
{
	assert(allow_method_set != 0);
	conn_print(conn,
		"HTTP/1.1 405 Method Not Allowed" CRLF
		"Connection: close" CRLF
		"Content-Length: 0" CRLF
		"Allow:"
	);
	int num=0;
	#define X(ENUM) \
		if (allow_method_set & (1<<(ENUM))) { \
			conn_printf(conn, "%s%s", ((num>0) ? ", " : " "), #ENUM); \
			++num; \
		}
	LIST_OF_METHODS
	#undef X
	assert(num>0);
	conn_print(conn, CRLF CRLF);
	conn_respond(conn);
}

struct header_reader {
	char *p, *bufend, *header, *colon, *header_end;
};

static struct header_reader header_begin(char* headers0, char* headers1)
{
	struct header_reader hr = {
		.p      = headers0,
		.bufend = headers1,
	};
	return hr;
}

static int header_next(struct header_reader* hr)
{
	char* p0 = hr->p;
	char* colon = NULL;
	while ((hr->p < hr->bufend) && (*hr->p)) {
		if ((*hr->p == ':') && (colon==NULL)) {
			colon = hr->p;
		}
		++hr->p;
	}
	if (hr->p == hr->bufend) return 0;
	assert(hr->p < hr->bufend);
	assert((colon != NULL) && "this header error should've been caught earlier");
	hr->header = p0;
	hr->colon = colon;
	hr->header_end = hr->p;
	assert(*hr->p == 0);
	hr->p += 2;
	return 1;
}

static int case_insensitive_match(const char* s0, const char* s1, int n)
{
	for (int i=0; i<n; ++i) {
		char c0 = s0[i];
		char c1 = s1[i];
		assert(c0 != 0);
		assert(c1 != 0);
		// convert lowercase ASCII letters to uppercase:
		if (('a' <= c0) && (c0 <= 'z')) c0 -= 32;
		if (('a' <= c1) && (c1 <= 'z')) c1 -= 32;
		if (c0 != c1) return 0;
	}
	return 1;
}


// case-insensitive header key match
static int is_header(struct header_reader* hr, const char* key)
{
	assert(hr->header != NULL);
	assert(hr->colon != NULL);
	assert(hr->colon <= hr->header_end);
	const char* p0 = hr->header;
	const char* p0end = hr->colon;
	const char* p1 = key;
	const char* p1end = p1+strlen(p1);
	const int n = (p0end-p0);
	if (n != (p1end-p1)) return 0;
	return case_insensitive_match(p0, p1, n);
}

static int header_csv_contains(struct header_reader* hr, const char* s)
{
	const int ns = strlen(s);
	const char* p = hr->colon;
	assert(p != NULL);
	++p;
	const char* pend = hr->header_end;
	assert(pend != NULL);
	while (p<pend) {
		while (p<pend && *p==' ') ++p;
		const char* v0=p;
		while (p<pend && *p && *p!=',') ++p;
		const char* v1=p;
		while (v1>hr->colon && *v1==' ') --v1;
		if (((v1-v0) == ns) && case_insensitive_match(v0,s,ns)) {
			return 1;
		}
		++p;
	}
	return 0;
}

static int get_header_value_length(struct header_reader* hr)
{
	const char* p = hr->colon;
	assert(p != NULL);
	const char* pend = hr->header_end;
	assert(pend != NULL);
	while (p<pend && (*p==':' || *p==' ')) ++p;
	return (pend-p);
}

static void header_copy_value(struct header_reader* hr, char* dst)
{
	const int n = get_header_value_length(hr);
	memcpy(dst, (hr->header_end-n), n);
}

static const char* get_mime_from_ext(const char* ext)
{
	if (0==strcmp("wav",ext))   return "audio/wav";
	if (0==strcmp("js",ext))    return "text/javascript";
	if (0==strcmp("wasm",ext))  return "application/wasm";
	if (0==strcmp("css",ext))   return "text/css";
	if (0==strcmp("html",ext))  return "text/html; charset=UTF-8";
	return "application/octet-stream";
}

// parses HTTP/1.1 request between pstart/pend. the memory is modified.
static void http_serve(struct conn* conn, uint8_t* pstart, uint8_t* pend)
{
	assert(conn->cstate == HTTP_REQUEST);

	// parse method ([GET] /path/to/resource HTTP/1.1\r\n)
	uint8_t* p=pstart;
	uint8_t* method0=p;
	while (p<pend && *p>' ') ++p;
	int err = (*p != ' ');
	uint8_t* method1=p;
	enum http_method method = UNKNOWN;
	#define X(ENUM) \
	if (method==UNKNOWN) { \
		static const char METHOD[] = #ENUM; \
		const size_t n = sizeof(METHOD)-1; \
		if ((n == (method1-method0)) && memcmp(method0,METHOD,n)==0) { \
			method = ENUM; \
		} \
	}
	LIST_OF_METHODS
	#undef X

	// read path (GET [/path/to/resource] HTTP/1.1\r\n)
	++p;
	uint8_t* path0=p;
	while (p<pend && *p>' ') ++p;
	err |= (*p != ' ');
	uint8_t* path1=p;
	*path1 = 0; // insert NUL-terminator

	// match protocol (GET /path/to/resource [HTTP/1.1]\r\n)
	++p;
	uint8_t* proto0=p;
	while (p<(pend-1) && *p>' ') ++p;
	err |= (p[0] != '\r');
	err |= (p[1] != '\n');
	uint8_t* proto1=p;
	p+=2;
	static const char PROTO[] = "HTTP/1.1";
	err |= ((proto1-proto0) != (sizeof(PROTO)-1) || memcmp(proto0,PROTO,proto1-proto0) != 0);

	if (err) {
		// client didn't send even one line of valid HTTP so just drop the
		// connection instead of bothering with a 400-response
		conn_drop(conn);
		return;
	}

	// read headers (Header: Value\r\n)
	uint8_t* headers0 = p;
	int end_of_header = 0;
	while ((p<(pend-1)) && !end_of_header) {
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
			head1[0] = 0; // insert NUL-terminator, overwriting \r
			head1[1] = 0; // also overwrite \n
		}
		p+=2;
	}
	uint8_t* headers1 = p;
	assert(p<=pend);

	if (err) SERVE_STATIC_CLOSE_AND_RETURN(conn, R400proto)
	if (!end_of_header) SERVE_STATIC_CLOSE_AND_RETURN(conn, R413)

	const size_t remaining = (pend-p);

	if ((remaining > 0) && method==GET) SERVE_STATIC_CLOSE_AND_RETURN(conn, R400proto)

	assert((remaining == 0) && "XXX we're not handling POST/PUT bodies yet");

	struct header_reader hr = header_begin((char*)headers0, (char*)headers1);

	int upgrade_to_websocket = 0;
	int method_set;
	conn_enter(conn, HTTP_RESPONSE);

	const char* tail;
	#define ROUTE(R) (method_set=0 , is_route(R,(char*)path0,&tail))
	#define IS(M)    (assert(!(method_set&(1<<(M)))), (method_set|=(1<<(M))), method==(M))
	#define DO405_AND_RETURN {assert(method_set);serve405(conn,method_set);return;}

	if (ROUTE("/o")) {
		#ifdef NO_WEBPACK
		SERVE_STATIC_AND_RETURN(conn, R404)
		#else
		char html[1<<12];

		const int n = stbsp_snprintf(html, sizeof html,
			"<!DOCTYPE html>\n"
			"<title>%s</title>"
			"<link rel=\"stylesheet\" type=\"text/css\" href=\"/dok/%s\"/>"
			"<canvas id=\"canvas\"></canvas>"
			"<div id=\"text_input_overlay\" contenteditable>?</div>"
			"<script src=\"/dok/%s\"></script>"
			,
			"toDO",
			webpack_lookup("do.css"),
			webpack_lookup("do.js")
		);

		conn_printf(conn,
			"HTTP/1.1 200 OK" CRLF
			"Content-Type: text/html; charset=UTF-8" CRLF
			"Content-Length: %d" CRLF
			CRLF
			"%s"
			, n, html);
		conn_respond(conn);
		return;
		#endif

	} else if (ROUTE("/o/info")) {
		TODO(web/info)
		SERVE_STATIC_AND_RETURN(conn, R404)
	} else if (ROUTE("/o/websocket")) {
		if (IS(GET)) {
			upgrade_to_websocket = 1;
		} else {
			DO405_AND_RETURN
		}

	} else if (ROUTE("/o/resolv/")) {
		TODO(web/resolv)
		SERVE_STATIC_AND_RETURN(conn, R404)
	} else if (ROUTE("/dok/")) {
		if (IS(HEAD) || IS(GET)) {
			assert(tail != NULL);
			char* p = (char*)tail;
			const size_t np = strlen(p);
			// strictly match <hash>.<ext> filename in "tail" after /dok/:
			//   <hash> must be 64 lowercase hex digits (sha256)
			//   <ext> must be a supported type (see get_mime_from_ext())
			if (np < 66) {
				// too short
				SERVE_STATIC_AND_RETURN(conn, R404)
			}
			for (int i=0; i<64; ++i) {
				char c = p[i];
				if (!((('0'<=c) && (c<='9')) || ('a'<=c && c<='f'))) {
					// non-hex digit found in <hash>
					SERVE_STATIC_AND_RETURN(conn, R404)
				}
			}
			if (p[64] != '.') {
				// no dot at expected position
				SERVE_STATIC_AND_RETURN(conn, R404)
			}
			const char* ext = p+65;

			// XXX code assumes to find the same file under dok/... relative to
			// current directory, but we might want to make the dok-path
			// configurable
			p = (char*)path0;
			while (*p=='/') ++p;
			int64_t size;
			const int src_file_id = io_open(p, IO_OPEN_RDONLY, &size);
			if (src_file_id < 0) {
				// file actually not found
				SERVE_STATIC_AND_RETURN(conn, R404)
			}
			assert(src_file_id >= 0);

			// TODO range?

			conn_printf(conn,
				"HTTP/1.1 200 OK" CRLF
				"Content-Type: %s" CRLF
				"Content-Length: %ld" CRLF
				"Cache-Control: max-age=31536000, immutable" CRLF
				CRLF
				,
				get_mime_from_ext(ext),
				size);

			conn_respond(conn);
			if (method==GET) {
				conn_sendfileall(conn, src_file_id, size, 0);
			} else assert(method==HEAD);
			return;
		} else {
			// TODO PUT?
			DO405_AND_RETURN
		}

	} else {
		SERVE_STATIC_AND_RETURN(conn, R404)
	}
	#undef DO405
	#undef IS
	#undef ROUTE

	if (upgrade_to_websocket) {
		int upgrade_ok    = -1;
		int connection_ok = -1;
		int version_ok    = -1;
		int key_ok        = -1;

		char sec_websocket_key[24];

		while (header_next(&hr)) {
			if (is_header(&hr, "Upgrade")) {
				upgrade_ok = header_csv_contains(&hr, "WebSocket");
			} else if (is_header(&hr, "Connection")) {
				connection_ok = header_csv_contains(&hr, "Upgrade");
			} else if (is_header(&hr, "Sec-WebSocket-Version")) {
				version_ok = header_csv_contains(&hr, "13");
			} else if (is_header(&hr, "Sec-WebSocket-Key")) {
				if (get_header_value_length(&hr) == sizeof sec_websocket_key) {
					header_copy_value(&hr, sec_websocket_key);
					key_ok = 1;
				} else {
					key_ok = 0;
				}
			}
		}

		#if 0
		printf("OKs %d %d %d %d\n",
			upgrade_ok,
			connection_ok,
			version_ok,
			key_ok);
		#endif

		if ((upgrade_ok<1) || (connection_ok<1) || (version_ok<1) || (key_ok<1)) {
			SERVE_STATIC_CLOSE_AND_RETURN(conn, R400proto)
		}

		// the hardest problem in computer science is to hold back your sarcasm
		// when implementing the handshake part of RFC6455. JUST LOOK AT IT:

		SHA1_CTX sha1;
		// let's depend on a broken hash function, why not... "it's fine"
		// because the RFC says: "The WebSocket handshake described in this
		// document doesn't depend on any security properties of SHA-1, such as
		// collision resistance or resistance to the second pre-image attack".
		// so why are we using SHA-1 again? also, what does the "sec" in
		// "Sec-WebSocket-Key" stand for? secondhand?
		SHA1_Init(&sha1);
		SHA1_Update(&sha1, (uint8_t*) sec_websocket_key, sizeof sec_websocket_key);
		// now let's add some base64-encoded data from "Sec-WebSocket-Key"...
		// except we haven't actually verified it's base64-encoded, because
		// the RFC says: "It is not necessary for the server to base64-decode
		// the |Sec-WebSocket-Key| value.". yet the RFC also says that the
		// client MUST base64-encode it... um ok. I guess we should base64-
		// decode it for '"extra security"'? (triple-quotes for /extra/ sarcasm)
		SHA1_Update(&sha1, (uint8_t*) "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
		// now let's add a magic GUID... of course it would NOT work in
		// lowercase even though it would be the same guid? it says something
		// about the creator of this handshake, doesn't it, to have the
		// opportunity to select a sequence of 36 characters to be immortalized
		// in RFC6455, and then you choose an F'ing (I now know what the F in
		// RFC stands for) GUID!!1 also note how we've now /twice/ added the
		// ASCII representation of binary data to the SHA-1... because... mmpfh
		uint8_t digest[SHA1_DIGEST_SIZE];
		SHA1_Final(&sha1, digest);

		char accept_key[32];
		char* p = base64_encode(accept_key, digest, sizeof digest);
		assert(p < (accept_key + sizeof(accept_key)));
		*p = 0;

		// and base64-encode it for the sec-websocket-accept header. at this
		// point ima little surprised we're not base64-encoding it twice but

		//printf("accept key [%s]\n", accept_key);
		conn_printf(conn,
			"HTTP/1.1 101 Switching Protocols" CRLF
			"Upgrade: websocket" CRLF
			"Connection: Upgrade" CRLF
			"Sec-WebSocket-Accept: %s" CRLF
			CRLF
			,
			accept_key);
		// so if "Upgrade: websocket" and more headers werent't enough, we also
		// have to reply with this psuedo crypto nonsense. if you really want
		// the server to "prove" you speak websocket, why not just echo the
		// sec-websocket-key? but the RFC's justification is:

		//   "The |Sec-WebSocket-Key| header field is used in the WebSocket
		//   opening handshake.  It is sent from the client to the server to
		//   provide part of the information used by the server to prove that
		//   it received a valid WebSocket opening handshake.  This helps
		//   ensure that the server does not accept connections from
		//   non-WebSocket clients (e.g., HTTP clients) that are being abused
		//   to send data to unsuspecting WebSocket servers."

		// hello are we talking about the same protocol? you say it proves to
		// the server that the client can speak websocket, but the server is
		// doing all the work? the client can just send:
		//   Sec-WebSocket-Key: adsfasdf12341243143adfa2
		// (making it 24 chars "fools" our webserv), as well as it can send:
		//   Sec-WebSocket-Version: 13
		// which it must anyway??? (a browser can send neither due to the sec-*
		// header prefix). if the client /really/ wants to fool a
		// goody-two-shoes base64-decoding server it can always send the
		// example from the RFC:
		//   Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==
		// and then the server does all this work and sends back:
		//   Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=
		// and the client can just ignore it? how did all this help the server
		// "prove that it received a valid WebSocket opening handshake."?
		// can the RFC clarify if a server should -- sorry -- SHOULD check that
		// the client isn't reusing keys? SHOULD we do live entropy analyses to
		// see if the client /actually/ wants to speak websocket?

		// TODO make a "fast path" when Sec-WebSocket-Key is "dGhlIHNhbXBsZSBub25jZQ=="?

		conn_respond(conn);
		// ok whatever
		// from here on the protocol is actually pretty normal (?)
		assert(conn->websock.wstate == 0);
		conn->enter_websocket_after_response = 1;
		conn->websock.wstate = HEAD_FIN_RSV_OPCODE;
		return;
	}

	assert(!"unreachable");
}

enum {
	WS_CONTINUATION_FRAME = 0,
	WS_TEXT_FRAME         = 1,
	WS_BINARY_FRAME       = 2,
	WS_CONNECTION_CLOSE   = 8,
	WS_PING               = 9,
	WS_PONG               = 10,
};

static int websocket_read_header_u8(struct conn* conn, uint8_t b)
{
	struct websock* ws = &conn->websock;
	int next_state = -1;
	switch (ws->wstate) {

	case HEAD_FIN_RSV_OPCODE: {
		ws->header_cursor = 0;
		ws->fin = !!(b&0x80);
		const int RSV123 = b & 0x70;
		if (RSV123) {
			fprintf(stderr, "RSV123=0x%x but expected RSV123=0\n", RSV123);
			return -1; // reserved bits must be 0
		}
		ws->opcode = b & 0xf;

		switch (ws->opcode) {
		case WS_CONTINUATION_FRAME:
			break;
		case WS_TEXT_FRAME:
			// TODO?
			break;
		case WS_BINARY_FRAME:
			// TODO?
			break;
		case WS_CONNECTION_CLOSE:
			conn_drop(conn);
			return 0;
		case WS_PING:
			TODO(websocket ping);
			// TODO send opcode=10 (pong) back
			conn_drop(conn);
			return -1;
		default:
			fprintf(stderr, "unexpected opcode %d\n", ws->opcode);
			conn_drop(conn);
			return -1;
		}

		next_state = HEAD_MASK_PLEN7;

	}	break;

	case HEAD_MASK_PLEN7: {
		ws->payload_length = 0;
		const int mask = !!(b&0x80);
		if (!mask) {
			// MASK bit must always be 1 in data sent from client to server
			fprintf(stderr, "MASK bit not set in client data\n");
			conn_drop(conn);
			return -1;
		}
		const uint8_t pl7 = (b&0x7f);
		if (pl7 < 126) {
			ws->payload_length = pl7;
			next_state = HEAD_MASKKEY;
		} else if (pl7 == 126) {
			next_state = HEAD_PLEN16;
			ws->header_cursor = 0;
		} else if (pl7 == 127) {
			next_state = HEAD_PLEN64;
			ws->header_cursor = 0;
		} else {
			assert(!"unreachable");
		}
	}	break;

	case HEAD_PLEN16: {
		assert(ws->header_cursor < 2);
		ws->payload_length |= b << 8*(1 - ws->header_cursor);
		++ws->header_cursor;
		if (ws->header_cursor == 2) {
			next_state = HEAD_MASKKEY;
			ws->header_cursor = 0;
		} else {
			assert((0 < ws->header_cursor) && (ws->header_cursor < 2));
			next_state = ws->wstate;
		}
	}	break;

	case HEAD_PLEN64: {
		assert(ws->header_cursor < 8);
		ws->payload_length |= (int64_t)b << 8*(7 - ws->header_cursor);
		++ws->header_cursor;
		if (ws->header_cursor == 8) {
			next_state = HEAD_MASKKEY;
			ws->header_cursor = 0;
		} else {
			assert((0 < ws->header_cursor) && (ws->header_cursor < 8));
			next_state = ws->wstate;
		}
	}	break;

	case HEAD_MASKKEY: {
		assert((0 <= ws->header_cursor) && (ws->header_cursor < 4));
		ws->mask_key[ws->header_cursor++] = b;
		if (ws->header_cursor == 4) {
			next_state = PAYLOAD;
			ws->remaining = ws->payload_length;
			ws->header_cursor = 0;
		} else {
			assert((0 < ws->header_cursor) && (ws->header_cursor < 4));
			next_state = ws->wstate;
		}
	}	break;

	case PAYLOAD: assert(!"expected header state, but is in payload state");
	default:      assert(!"unhandled state");
	}
	assert(next_state>=0);
	ws->wstate = next_state;
	return 0;
}

static int websocket_send0(struct conn* conn, void* payload, int payload_size)
{
	assert(conn->cstate == WEBSOCKET);

	size_t bufsize;
	uint8_t* buf = get_conn_ws_write_buffer(conn, &bufsize);

	int header_size=0;
	int do_write_plen16=0;
	int do_write_plen64=0;
	// FIXME if payload_size is, say, 42MB, and the buffer size is less than
	// 64kB, then we still could use the 16bit "plen16" approach, but we
	// currently don't because payload_size is truncated later
	if (payload_size < 126) {
		header_size = 2;
	} else if (payload_size < 65536) {
		header_size = 2+2;
		do_write_plen16 = 1;
	} else {
		header_size = 2+8;
		do_write_plen64 = 1;
	}
	assert(header_size >= 2);

	const int payload_space = bufsize - header_size;
	assert(payload_space > 0);
	const int fin = payload_size <= payload_space;

	const int opcode = WS_TEXT_FRAME; // XXX binary?
	buf[0] = (fin ? 0x80 : 0) + opcode;

	uint8_t* p = &buf[1];
	if (!do_write_plen16 && !do_write_plen64) {
		assert(payload_size < 126);
		*(p++) = payload_size;
	} else if (do_write_plen16 && !do_write_plen64) {
		*(p++) = 126;
		for (int i=0; i<2; ++i) {
			*(p++) = (payload_size >> (8*(1-i))) & 0xff;
		}
	} else if (do_write_plen64 && !do_write_plen16) {
		*(p++) = 127;
		for (int i=0; i<8; ++i) {
			*(p++) = (payload_size >> (8*(7-i))) & 0xff;
		}
	} else {
		assert(!"unreachable");
	}
	assert((bufsize-(p-buf)) == payload_space);
	int num = payload_size;
	if (num > payload_space) num = payload_space;
	memcpy(p, payload, num);
	p+=num;

	assert((p-buf) <= bufsize);
	conn_writeall(conn, buf, p-buf);

	return num;
}

static void websocket_serve(struct conn* conn, uint8_t* pstart, uint8_t* pend)
{
	struct websock* ws = &conn->websock;
	uint8_t* p = pstart;
	while (p < pend) {
		if (ws->wstate == PAYLOAD) {
			int64_t r = (pend - p);
			if (r > ws->remaining) r = ws->remaining;
			const int64_t o0 = (ws->payload_length - ws->remaining);
			for (int64_t i=0; i<r; ++i) {
				// unmask data. RFC6455 has this to say: "The unpredictability
				// of the masking key is essential to prevent authors of
				// malicious applications from selecting the bytes that appear
				// on the wire."
				p[i] ^= ws->mask_key[(o0+i)&3];
			}

			printf("TODO websocket num_bytes=%ld fin=%d [", r, ws->fin); // TODO

			if (conn->cstate == WEBSOCKET) {
				assert(5 == websocket_send0(conn, "howdy", 5));
			}

			for (int i=0; i<r; ++i) printf("%c", p[i]);
			printf("]\n");
			ws->remaining -= r;
			p += r;
			if (ws->remaining == 0) {
				ws->wstate = HEAD_FIN_RSV_OPCODE;
			} else {
				assert(ws->remaining > 0);
			}
		} else {
			if (-1 == websocket_read_header_u8(conn, *(p++))) {
				conn_drop(conn);
				return;
			}
		}
	}
	assert(p == pend);
}

int webserv_tick(void)
{
	int did_work = 0;
	struct io_event ev;
	while (io_port_poll(g.port_id, &ev)) {
		did_work = 1;
		int conn_id = -1;
		if (is_ignore_echo(ev.echo)) {
			// ok; ignored
		} else if (is_listen_echo(ev.echo)) {
			//printf("http ev status=%d!\n", ev.status);
			if (ev.status >= 0) {
				io_addr(ev.status);
			}
			const int conn_id = alloc_conn();
			if (conn_id == -1) {
				io_port_writeall(g.port_id, IGNORE_ECHO, ev.status, R503, sizeof(R503)-1);
			} else {
				struct conn* conn = get_conn(conn_id);
				memset(conn, 0, sizeof *conn);
				conn->file_id = ev.status;
				conn_enter(conn, HTTP_REQUEST);
				size_t size;
				uint8_t* buf = get_conn_http_read_buffer(conn, &size);
				io_port_read(g.port_id, echo_read(conn_id), conn->file_id, buf, size);
			}
		} else if (is_echo_close(ev.echo, &conn_id)) {
			struct conn* conn = get_conn(conn_id);
			assert(conn->cstate == CLOSING);
			free_conn(conn);
		} else if (is_echo_read(ev.echo, &conn_id)) {
			const int conn_id = ev.echo.ib32;
			assert((0 <= conn_id) && (conn_id < MAX_CONN_COUNT));
			struct conn* conn = get_conn(conn_id);
			if (ev.status <= 0) {
				if (ev.status < 0) {
					fprintf(stderr, "conn I/O error %d for echo %d:%d\n", ev.status, ev.echo.ia32, ev.echo.ib32);
				}
				conn_drop(conn);
				continue;
			}
			conn->inflight_read=0;
			const int num_bytes = ev.status;
			assert(num_bytes > 0);
			assert(conn->cstate != CLOSING);

			switch (conn->cstate) {
			case HTTP_REQUEST: {
				size_t size;
				uint8_t* buf = get_conn_http_read_buffer(conn, &size);
				assert(num_bytes <= size);
				http_serve(conn, buf, buf+num_bytes);
			}	break;
			case WEBSOCKET: {
				size_t size;
				uint8_t* buf = get_conn_ws_read_buffer(conn, &size);
				assert(num_bytes <= size);
				websocket_serve(conn, buf, buf+num_bytes);
				conn_read(conn, buf, size);
			}	break;
			default: assert(!"unhandled conn state");
			}
			assert((conn->cstate != HTTP_REQUEST) && "unexpected state");
		} else if (is_echo_write(ev.echo, &conn_id)) {
			struct conn* conn = get_conn(conn_id);
			assert(conn->num_writes_pending > 0);
			--conn->num_writes_pending;
			if (conn->num_writes_pending == 0) {
				if (conn->cstate == HTTP_RESPONSE) {
					if (conn->inflight_sendfile) {
						io_port_close(g.port_id, IGNORE_ECHO, conn->sendfile_src_file_id);
						conn->sendfile_src_file_id = 0;
						conn->inflight_sendfile = 0;
					}
					if (conn->enter_websocket_after_response) {
						conn_enter(conn, WEBSOCKET);
						conn->enter_websocket_after_response = 0;
						size_t size;
						uint8_t* buf = get_conn_ws_read_buffer(conn, &size);
						conn_read(conn, buf, size);
					} else {
						conn_enter(conn, HTTP_REQUEST);
						size_t size;
						uint8_t* buf = get_conn_http_read_buffer(conn, &size);
						conn_read(conn, buf, size);
					}
				} else {
					assert(!conn->enter_websocket_after_response);
				}
			} else {
				assert(conn->num_writes_pending > 0);
			}
		}
	}
	return did_work;
}

void webserv_selftest(void)
{
	{
		assert(case_insensitive_match("foo", "foo", 3));
		assert(case_insensitive_match("foo", "Foo", 3));
		assert(case_insensitive_match("foo", "FOO", 3));
		assert(case_insensitive_match("fOo", "foo", 3));
		assert(!case_insensitive_match("fOo!", "foo?", 4));
		assert(!case_insensitive_match("aa0", "aa1", 3));
	}

	{
		// headers have NUL'd \r\n after the initial pass which is why these
		// headers end in \0\0
		static char h0[] =
		"Foo: 666\0\0"
		"Bar: xx, yyy, zzzz\0\0"
		;
		struct header_reader hr = header_begin(h0, h0+sizeof(h0)-1);

		assert(header_next(&hr));
		assert(is_header(&hr, "foo"));
		assert(is_header(&hr, "Foo"));
		assert(!is_header(&hr, "BaR"));
		assert(get_header_value_length(&hr) == 3);
		assert(header_csv_contains(&hr, "666"));
		assert(!header_csv_contains(&hr, "66"));
		assert(!header_csv_contains(&hr, "6666"));
		assert(!header_csv_contains(&hr, "777"));
		assert(!header_csv_contains(&hr, "foo"));
		assert(!header_csv_contains(&hr, "Foo"));
		assert(!header_csv_contains(&hr, ":"));

		assert(header_next(&hr));
		assert(!is_header(&hr, "foo"));
		assert(!is_header(&hr, "Foo"));
		assert(is_header(&hr, "BaR"));
		assert(header_csv_contains(&hr, "xx"));
		assert(!header_csv_contains(&hr, "x"));
		assert(!header_csv_contains(&hr, "xxx"));
		assert(!header_csv_contains(&hr, "bar"));
		assert(header_csv_contains(&hr, "yyy"));
		assert(header_csv_contains(&hr, "zzzz"));

		assert(!header_next(&hr));
	}
}
