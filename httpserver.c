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
#include <limits.h>

#include "httpserver.h"
#include "io.h"
#include "util.h"
#include "sha1.h"
#include "base64.h"
#include "stb_sprintf.h"

#define IGNORE_ECHO (-100)
#define LISTEN_ECHO (-101)

#define BUFFER_SIZE_LOG2     (14)
#define BUFFER_SIZE          (1L << (BUFFER_SIZE_LOG2))
#define MAX_CONN_COUNT_LOG2  (8)
#define MAX_CONN_COUNT       (1L << (MAX_CONN_COUNT_LOG2))

#define LIST_OF_METHODS \
	X(GET) \
	X(POST) \
	X(PUT)

enum http_method {
	UNKNOWN=0,
	#define X(ENUM) ENUM,
	LIST_OF_METHODS
	#undef X
};

// canned responses (these can be sent cheaply in a fire'n'forget manner
// because the memory is static and we close the connection)

#define BLAHBLAHBLAH \
	"Content-Type: text/plain; charset=utf-8\r\n" \
	"Connection: close\r\n" \

static const char R200test[]=
	"HTTP/1.1 200 OK\r\n"
	BLAHBLAHBLAH
	"Content-Length: 13\r\n"
	"\r\n"
	"This Is Fine!"
	;

static const char R400proto[]=
	"HTTP/1.1 400 Bad Request\r\n"
	BLAHBLAHBLAH
	"Content-Length: 32\r\n"
	"\r\n"
	"400 Bad Request (protocol error)"
	;

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
	WEBSOCKET,
	CLOSE,
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
	enum websock_state state;
	int cursor;
	int64_t payload_length;
	int64_t remaining;
	uint8_t mask_key[4];
	unsigned  fin    :1;
	unsigned  opcode :4;
};

struct conn {
	enum conn_state state;
	int file_id;
	int write_cursor;
	unsigned buffer_full :1;
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

#if 0
static void free_conn(int id)
{
	assert((0 <= id) && (id < MAX_CONN_COUNT));
	assert(g.num_free < MAX_CONN_COUNT);
	g.freelist[g.num_free++] = id;
}
#endif

static uint8_t* get_conn_buffer_by_id(int id)
{
	assert((0 <= id) && (id < MAX_CONN_COUNT));
	return &g.buffer_storage[id << BUFFER_SIZE_LOG2];
}

static uint8_t* get_conn_buffer_by_conn(struct conn* conn)
{
	int64_t id = conn - g.conns;
	assert((0 <= id) && (id < MAX_CONN_COUNT));
	return get_conn_buffer_by_id((int)id);
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
	io_port_writeall(g.port_id, echo, file_id, data, size);
	// XXX ... and close?
}

// use this with the "canned responses", R404 etc. assumes that sizeof(R) is
// one larger that the response due to NUL-terminator
#define SERVE_STATIC_AND_CLOSE(FILE_ID,R) serve_static_and_close(FILE_ID,R,sizeof(R)-1)

static int is_route(const char* route, const char* path)
{
	const size_t route_len = strlen(route);
	const size_t path_len  = strlen(path);
	if (path_len < route_len) return 0;
	if (route[route_len-1] != '/') {
		return strcmp(route,path)==0;
	} else {
		return (path_len > route_len) && (memcmp(route,path,route_len)==0);
	}
}

FORMATPRINTF2
static void conn_printf(struct conn* conn, const char* fmt, ...)
{
	int r = (BUFFER_SIZE - conn->write_cursor);
	if (r == 0) {
		assert(conn->buffer_full);
		return;
	}
	assert(r>0);
	char* p = (char*)(get_conn_buffer_by_conn(conn) + conn->write_cursor);
	va_list va;
	va_start(va, fmt);
	const int n = stbsp_vsnprintf(p, r, fmt, va);
	va_end(va);
	conn->write_cursor += n;
	if ((BUFFER_SIZE - conn->write_cursor) == 0) {
		conn->buffer_full = 1;
	}
}

static void conn_print(struct conn* conn, const char* str)
{
	int r = (BUFFER_SIZE - conn->write_cursor);
	if (r == 0) {
		assert(conn->buffer_full);
		return;
	}
	assert(r>0);
	int num = strlen(str);
	if (num > r) num = r;
	uint8_t* p = (get_conn_buffer_by_conn(conn) + conn->write_cursor);
	memcpy(p, str, num);
	conn->write_cursor += num;
}

static void conn_respond(struct conn* conn)
{
	if (conn->buffer_full) {
		// TODO log a warning?
		// should always close when this happens...
	}

	// XXX not really right
	io_echo echo = { .i64 = IGNORE_ECHO };
	uint8_t* p = get_conn_buffer_by_conn(conn);
	io_port_writeall(g.port_id, echo, conn->file_id, p, conn->write_cursor);
}

static void serve405(struct conn* conn, int allow_method_set)
{
	assert(allow_method_set != 0);
	conn_print(conn,
		"HTTP/1.1 405 Method Not Allowed\r\n"
		"Connection: close\r\n"
		"Content-Length: 0\r\n"
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
	conn_print(conn, "\r\n\r\n");

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

// parses HTTP/1.1 request between pstart/pend. the memory is modified.
static void serve(struct conn* conn, uint8_t* pstart, uint8_t* pend)
{
	assert(conn->state == REQUEST);
	const int file_id = conn->file_id;

	// skip past method
	uint8_t* p=pstart;
	uint8_t* method0=p;
	while (p<pend && *p>' ') ++p;
	int err = (*p != ' ');
	uint8_t* method1=p;

	// parse method
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

	// skip past path
	++p;
	uint8_t* path0=p;
	while (p<pend && *p>' ') ++p;
	err |= (*p != ' ');
	uint8_t* path1=p;
	*path1 = 0; // insert NUL-terminator
	const size_t plen = (path1-path0);

	// match protocol ("HTTP/1.1")
	++p;
	uint8_t* proto0=p;
	while (p<(pend-1) && *p>' ') ++p;
	err |= (p[0] != '\r');
	err |= (p[1] != '\n');
	uint8_t* proto1=p;
	p+=2;
	static const char PROTO[] = "HTTP/1.1";
	err |= ((proto1-proto0) != (sizeof(PROTO)-1) || memcmp(proto0,PROTO,proto1-proto0) != 0);

	printf("method=%d path=[%s]/%zd err=%d\n", method, path0, plen, err);
	if (err) {
		assert(!"TODO handle bad http; close connection?");
	}

	// read headers
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
			printf("header [%s] err=%d\n", head0, err);
		}
		p+=2;
	}
	uint8_t* headers1 = p;
	assert(p<=pend);

	if (err) {
		SERVE_STATIC_AND_CLOSE(file_id, R400proto);
		return;
	}

	if (!end_of_header) {
		SERVE_STATIC_AND_CLOSE(file_id, R413);
		return;
	}

	const size_t remaining = (pend-p);
	//printf("remaining: %zd\n", remaining);

	if ((remaining > 0) && method==GET) {
		SERVE_STATIC_AND_CLOSE(file_id, R400proto);
		return;
	}

	struct header_reader hr = header_begin((char*)headers0, (char*)headers1);

	int upgrade_to_websocket = 0;
	int method_set = 0;
	#define ROUTE(R) (method_set=0 , is_route(R,(char*)path0))
	#define IS(M)    (assert(!(method_set&(1<<(M)))), (method_set|=(1<<(M))), method==(M))
	#define DO405()  (assert(method_set),serve405(conn, method_set))

	if (ROUTE("/foo/bar")) { // XXX
		if (IS(GET)) {
			SERVE_STATIC_AND_CLOSE(file_id, R200test);
			return;
		} else if (IS(PUT)) {
			assert(!"TODO PUT /foo/bar"); // XXX
		} else {
			DO405();
			return;
		}

	} else if (ROUTE("/ding/dong")) { // XXX
		if (IS(GET)) {
			SERVE_STATIC_AND_CLOSE(file_id, R200test);
			return;
		} else {
			DO405();
			return;
		}

	} else if (ROUTE("/data")) {
		if (IS(GET)) {
			const size_t n = 1<<24;
			void* mem = malloc(n);
			memset(mem, 0xdd, n);
			conn_printf(conn,
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: application/octet-stream\r\n"
				"Content-Length: %zd\r\n\r\n"
				,
				n);

			conn_respond(conn);
			io_echo echo = { .i64 = IGNORE_ECHO };
			io_port_writeall(g.port_id, echo, file_id, mem, n);
			return;
		} else {
			DO405();
			return;
		}

	} else if (ROUTE("/test.html")) {
		if (IS(GET)) {
			//SERVE_STATIC_AND_CLOSE(file_id, R200test);
			//int io_open(const char* path, enum io_open_mode, int64_t* out_filesize);
			int64_t size;
			const int src_file_id = io_open("test.html", IO_OPEN_RDONLY, &size);
			assert(src_file_id >= 0);

			conn_printf(conn,
				"HTTP/1.1 200 OK\r\n"
				"Content-Type: text/html; charset=utf-8\r\n"
				"Content-Length: %ld\r\n\r\n"
				,
				size);

			conn_respond(conn);

			// XXX not really right eh...
			io_echo echo = { .i64 = IGNORE_ECHO };
			io_port_sendfileall(g.port_id, echo, conn->file_id, src_file_id, size, 0);

			return;
		} else {
			DO405();
			return;
		}

	} else if (ROUTE("/wstest")) {
		if (IS(GET)) {
			upgrade_to_websocket = 1;
		} else {
			DO405();
			return;
		}

	} else {
		SERVE_STATIC_AND_CLOSE(file_id, R404);
		return;
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
			SERVE_STATIC_AND_CLOSE(file_id, R400proto);
			return;
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

		printf("accept key [%s]\n", accept_key);

		conn_printf(conn,
			"HTTP/1.1 101 Switching Protocols\r\n"
			"Upgrade: websocket\r\n"
			"Connection: Upgrade\r\n"
			"Sec-WebSocket-Accept: %s\r\n\r\n",
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
		// (making it 24 chars "fools" our httpserver), as well as it can send:
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
		conn->state = WEBSOCKET;
		assert(conn->websock.state == 0);
		conn->websock.state = HEAD_FIN_RSV_OPCODE;
		return;
	}

	assert(!"unreachable");
}

static void websocket_drop(struct conn* conn)
{
	assert(!"TODO DROP/WS");
}

static int websocket_read_header_u8(struct conn* conn, uint8_t b)
{
	struct websock* ws = &conn->websock;
	int next_state = -1;
	switch (ws->state) {

	case HEAD_FIN_RSV_OPCODE: {
		ws->cursor = 0;
		ws->fin = !!(b&0x80);
		const int RSV123 = b & 0x70;
		if (RSV123) {
			fprintf(stderr, "RSV123=%d but expected RSV123=0\n", RSV123);
			return -1; // reserved bits must be 0
		}
		ws->opcode = b & 0xf;

		switch (ws->opcode) {
		case 0: // continuation frame
			break;
		case 1: // text frame
			// TODO?
			break;
		case 2: // binary frame
			// TODO?
			break;
		case 8: // connection close:
			websocket_drop(conn);
			return 0;
		case 9: // ping
		case 10: // pong
			assert(!"TODO ping pong");
			return -1;
		default:
			// TODO log unexpected/reserved opcode?
			fprintf(stderr, "unexpected opcode %d\n", ws->opcode);
			websocket_drop(conn);
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
			websocket_drop(conn);
			return -1;
		}
		const uint8_t pl7 = (b&0x7f);
		if (pl7 < 126) {
			ws->payload_length = pl7;
			next_state = HEAD_MASKKEY;
		} else if (pl7 == 126) {
			next_state = HEAD_PLEN16;
			ws->cursor = 0;
		} else if (pl7 == 127) {
			next_state = HEAD_PLEN64;
			ws->cursor = 0;
		} else {
			assert(!"unreachable");
		}
	}	break;

	case HEAD_PLEN16: {
		assert(ws->cursor < 2);
		ws->payload_length |= b << 8*(1 - ws->cursor);
		++ws->cursor;
		if (ws->cursor == 2) {
			next_state = HEAD_MASKKEY;
			ws->cursor = 0;
		} else {
			assert((0 < ws->cursor) && (ws->cursor < 2));
			next_state = ws->state;
		}
	}	break;

	case HEAD_PLEN64: {
		assert(ws->cursor < 8);
		ws->payload_length |= (int64_t)b << 8*(7 - ws->cursor);
		++ws->cursor;
		if (ws->cursor == 8) {
			next_state = HEAD_MASKKEY;
			ws->cursor = 0;
		} else {
			assert((0 < ws->cursor) && (ws->cursor < 8));
			next_state = ws->state;
		}
	}	break;

	case HEAD_MASKKEY: {
		assert((0 <= ws->cursor) && (ws->cursor < 4));
		ws->mask_key[ws->cursor++] = b;
		if (ws->cursor == 4) {
			next_state = PAYLOAD;
			ws->remaining = ws->payload_length;
			ws->cursor = 0;
		} else {
			assert((0 < ws->cursor) && (ws->cursor < 4));
			next_state = ws->state;
		}
	}	break;

	case PAYLOAD: assert(!"expected header state, but is in payload state");
	default:      assert(!"unhandled state");
	}
	assert(next_state>=0);
	ws->state = next_state;
	return 0;
}

static void websocket_read(struct conn* conn, uint8_t* pstart, uint8_t* pend)
{
	struct websock* ws = &conn->websock;
	uint8_t* p = pstart;
	while (p < pend) {
		if (ws->state == PAYLOAD) {
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

			for (int i=0; i<r; ++i) printf("%c", p[i]);
			printf("]\n");
			ws->remaining -= r;
			p += r;
			if (ws->remaining == 0) {
				ws->state = HEAD_FIN_RSV_OPCODE;
			} else {
				assert(ws->remaining > 0);
			}
		} else {
			if (-1 == websocket_read_header_u8(conn, *(p++))) {
				websocket_drop(conn);
				return;
			}
		}
	}
	assert(p == pend);
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
			const int conn_id = alloc_conn();
			if (conn_id == -1) {
				SERVE_STATIC_AND_CLOSE(ev.status, R503);
			} else {
				struct conn* conn = get_conn(conn_id);
				memset(conn, 0, sizeof *conn);
				conn->file_id = ev.status;
				conn->state = REQUEST;
				uint8_t* buf = get_conn_buffer_by_id(conn_id);
				io_echo echo = { .i64 = conn_id };
				io_port_read(g.port_id, echo, conn->file_id, buf, BUFFER_SIZE);
			}
		} else {
			const int64_t id64 = ev.echo.i64;
			assert((0 <= id64) && (id64 < MAX_CONN_COUNT));
			const int conn_id = id64;
			struct conn* conn = get_conn(conn_id);
			const int num_bytes = ev.status;
			uint8_t* buf = get_conn_buffer_by_id(conn_id);
			switch (conn->state) {
			case REQUEST: {
				serve(conn, buf, buf+num_bytes);
			}	break;
			case WEBSOCKET: {
				websocket_read(conn, buf, buf+num_bytes);
			}	break;
			default: assert(!"unhandled conn state");
			}

			if (conn->state != CLOSE) {
				io_echo echo = { .i64 = conn_id };
				io_port_read(g.port_id, echo, conn->file_id, buf, BUFFER_SIZE);
			}
		}
	}
	return did_work;
}

void httpserver_selftest(void)
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

		assert(header_next(&hr));
		assert(!is_header(&hr, "foo"));
		assert(!is_header(&hr, "Foo"));
		assert(is_header(&hr, "BaR"));
		assert(header_csv_contains(&hr, "xx"));
		assert(!header_csv_contains(&hr, "x"));
		assert(!header_csv_contains(&hr, "xxx"));
		assert(header_csv_contains(&hr, "yyy"));
		assert(header_csv_contains(&hr, "zzzz"));

		assert(!header_next(&hr));
	}
}
