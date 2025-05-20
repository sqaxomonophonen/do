#include <limits.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h> // XXX

#include "stb_ds_sysalloc.h" // XXX we borrow mie's scrallox; consider mandatory arrinit() for all?
#include "stb_sprintf.h"

#include "io.h"
#include "gig.h"
#include "util.h"
#include "leb128.h"
#include "utf8.h"
#include "path.h"
#include "main.h"
#include "args.h"

static void document_copy(struct document* dst, struct document* src)
{
	struct document tmp = *dst;
	*dst = *src;
	dst->fat_char_arr = tmp.fat_char_arr;
	arrcpy(dst->fat_char_arr, src->fat_char_arr);
	dst->name_arr = tmp.name_arr;
	arrcpy(dst->name_arr, src->name_arr);
}

static void mim_state_copy(struct mim_state* dst, struct mim_state* src)
{
	struct mim_state tmp = *dst;
	*dst = *src;
	dst->caret_arr = tmp.caret_arr;
	arrcpy(dst->caret_arr, src->caret_arr);
}

static int document_locate(struct document* doc, struct location* loc)
{
	struct doc_iterator it = doc_iterator(doc);
	doc_iterator_locate(&it, loc);
	return it.offset;
}

struct book {
	int id;
	int target; // TODO? should be language+backend? like mie+audiobackend(name?)
};

struct snapshot {
	struct book* book_arr;
	struct document*  document_arr;
	struct mim_state* mim_state_arr;
};

static int snapshot_get_num_documents(struct snapshot* ss)
{
	return arrlen(ss->document_arr);
}

static struct document* snapshot_get_document_by_index(struct snapshot* ss, int index)
{
	return arrchkptr(ss->document_arr, index);
}

static struct document* snapshot_find_document_by_id(struct snapshot* ss, int id)
{
	const int n = snapshot_get_num_documents(ss);
	for (int i=0; i<n; ++i) {
		struct document* doc = snapshot_get_document_by_index(ss, i);
		if (doc->id == id) return doc;
	}
	return NULL;
}

static struct document* snapshot_get_document_by_id(struct snapshot* ss, int id)
{
	struct document* doc = snapshot_find_document_by_id(ss, id);
	assert((doc != NULL) && "document not found by id");
	return doc;
}

static struct mim_state* snapshot_find_mim_state_by_ids(struct snapshot* ss, int artist_id, int session_id)
{
	const int n = arrlen(ss->mim_state_arr);
	for (int i=0; i<n; ++i) {
		struct mim_state* s = arrchkptr(ss->mim_state_arr, i);
		if ((s->artist_id == artist_id) &&  (s->session_id == session_id)) {
			return s;
		}
	}
	return NULL;
	assert(!"not found");
}

static struct mim_state* snapshot_get_mim_state_by_ids(struct snapshot* ss, int artist_id, int session_id)
{
	struct mim_state* ms = snapshot_find_mim_state_by_ids(ss, artist_id, session_id);
	assert(ms != NULL);
	return ms;
}

static struct mim_state* snapshot_get_personal_mim_state_by_id(struct snapshot* ss, int session_id)
{
	return snapshot_get_mim_state_by_ids(ss, get_my_artist_id(), session_id);
}

// "cow" stands for "copy-on-write". moo! it represents pending changes to a
// snapshot, and only contains the changed documents and mim states
struct cow_snapshot {
	struct allocator* allocator;
	struct snapshot* ref;
	struct document*  cow_document_arr;
	struct mim_state* cow_mim_state_arr;
};

static void init_cow_snapshot(struct cow_snapshot* cows, struct snapshot* ref, struct allocator* a)
{
	memset(cows, 0, sizeof *cows);
	cows->allocator = a;
	cows->ref = ref;
	arrinit(cows->cow_document_arr,  cows->allocator);
	arrinit(cows->cow_mim_state_arr, cows->allocator);
}

static struct document* cow_snapshot_find_cow_document_by_id(struct cow_snapshot* cows, int id)
{
	const int n = arrlen(cows->cow_document_arr);
	for (int i=0; i<n; ++i) {
		struct document* doc = &cows->cow_document_arr[i];
		if (doc->id == id) return doc;
	}
	return NULL;
}

static struct document* cow_snapshot_find_readonly_document_by_id(struct cow_snapshot* cows, int id)
{
	struct document* doc = cow_snapshot_find_cow_document_by_id(cows, id);
	if (doc != NULL) return doc;
	return snapshot_find_document_by_id(cows->ref, id);
}

static struct document* cow_snapshot_get_readonly_document_by_id(struct cow_snapshot* cows, int id)
{
	struct document* doc = cow_snapshot_find_readonly_document_by_id(cows, id);
	assert(doc != NULL);
	return doc;
}

static struct document* cow_snapshot_find_readwrite_document_by_id(struct cow_snapshot* cows, int id)
{
	struct document* doc = cow_snapshot_find_cow_document_by_id(cows, id);
	if (doc != NULL) return doc;

	struct document* src_doc = snapshot_find_document_by_id(cows->ref, id);
	if (src_doc == NULL) return NULL;

	if (cows->cow_document_arr == NULL) arrinit(cows->cow_document_arr, cows->allocator);

	struct document* dst_doc = arraddnptr(cows->cow_document_arr, 1);
	memcpy(dst_doc, src_doc, sizeof *dst_doc);

	dst_doc->name_arr = NULL;
	arrinit(dst_doc->name_arr, cows->allocator);
	arrcpy(dst_doc->name_arr, src_doc->name_arr);

	dst_doc->fat_char_arr = NULL;
	arrinit(dst_doc->fat_char_arr, cows->allocator);
	arrcpy(dst_doc->fat_char_arr, src_doc->fat_char_arr);

	return dst_doc;
}

static struct document* cow_snapshot_get_readwrite_document_by_id(struct cow_snapshot* cows, int id)
{
	struct document* doc = cow_snapshot_find_readwrite_document_by_id(cows, id);
	assert(doc != NULL);
	return doc;
}

static struct mim_state* cow_snapshot_find_cow_mim_state_by_ids(struct cow_snapshot* cows, int artist_id, int session_id)
{
	const int n = arrlen(cows->cow_mim_state_arr);
	for (int i=0; i<n; ++i) {
		struct mim_state* ms = &cows->cow_mim_state_arr[i];
		if ((ms->artist_id == artist_id) && (ms->session_id == session_id)) return ms;
	}
	return NULL;
}

static struct mim_state* cow_snapshot_find_readonly_mim_state_by_ids(struct cow_snapshot* cows, int artist_id, int session_id)
{
	struct mim_state* ms = cow_snapshot_find_cow_mim_state_by_ids(cows, artist_id, session_id);
	if (ms != NULL) return ms;
	return snapshot_find_mim_state_by_ids(cows->ref, artist_id, session_id);
}

static struct mim_state* cow_snapshot_find_readwrite_mim_state_by_ids(struct cow_snapshot* cows, int artist_id, int session_id)
{
	struct mim_state* ms = cow_snapshot_find_cow_mim_state_by_ids(cows, artist_id, session_id);
	if (ms != NULL) return ms;

	struct mim_state* src_ms = snapshot_find_mim_state_by_ids(cows->ref, artist_id, session_id);
	if (src_ms == NULL) return NULL;

	struct mim_state* dst_ms = arraddnptr(cows->cow_mim_state_arr, 1);
	memset(dst_ms, 0, sizeof *dst_ms);
	assert(dst_ms->caret_arr == NULL);
	arrinit(dst_ms->caret_arr, cows->allocator);
	assert(dst_ms->caret_arr != NULL);
	mim_state_copy(dst_ms, src_ms);

	return dst_ms;
}

static struct mim_state* cow_snapshot_get_readwrite_mim_state_by_ids(struct cow_snapshot* cows, int artist_id, int session_id)
{
	struct mim_state* ms = cow_snapshot_find_readwrite_mim_state_by_ids(cows, artist_id, session_id);
	assert(ms != NULL);
	return ms;
}

static void cow_snapshot_commit(struct cow_snapshot* cows)
{
	{
		const int n = arrlen(cows->cow_document_arr);
		for (int i=0; i<n; ++i) {
			struct document* src_doc = &cows->cow_document_arr[i];
			struct document* dst_doc = snapshot_get_document_by_id(cows->ref, src_doc->id);
			document_copy(dst_doc, src_doc);
		}
		if (n>0) arrsetlen(cows->cow_document_arr, 0);
	}

	{
		const int n = arrlen(cows->cow_mim_state_arr);
		for (int i=0; i<n; ++i) {
			struct mim_state* src_ms = &cows->cow_mim_state_arr[i];
			struct mim_state* dst_ms = snapshot_get_mim_state_by_ids(cows->ref, src_ms->artist_id, src_ms->session_id);
			mim_state_copy(dst_ms, src_ms);
		}
		if (n>0) arrsetlen(cows->cow_mim_state_arr, 0);
	}
}

static struct {
	int document_id_sequence;
	struct snapshot cool_snapshot, hot_snapshot;
	int my_artist_id;
	uint8_t* mim_buffer_arr;
	int using_mim_session_id;
	int in_mim;
	struct io* io;
	struct io_appender journal_appender;
	struct io_bufread journal_bufread;
	struct io_appender savestates_data_appender;
	struct io_appender savestates_index_appender;
	int64_t journal_timestamp_start;
} g;

void gig_spool(void)
{
	// TODO?
}

void gig_thread_tick(void)
{
	struct io_event ev;
	while (io_poll(g.io, &ev)) {
		if (io_appender_ack(&g.journal_appender, &ev)) continue;
		printf("TODO EV echo=%d h=%d st=%d!\n", ev.echo.ia32, ev.handle, ev.status);
		assert(ev.echo.ia32 == 42);
		const int64_t sz = io_get_size(g.io, ev.handle);
		printf("sz=%ld\n", sz);
	}
}

static char* get_mim_buffer_top(void)
{
	arrsetmincap(g.mim_buffer_arr, arrlen(g.mim_buffer_arr) + STB_SPRINTF_MIN);
	return (char*)g.mim_buffer_arr + arrlen(g.mim_buffer_arr);
}

static void mimsrc(uint8_t* src, size_t n)
{
	uint8_t* dst = arraddnptr(g.mim_buffer_arr, n);
	memcpy(dst, src, n);
	(void)get_mim_buffer_top();
}

static char* wrote_mim_cb(const char* buf, void* user, int len)
{
	arrsetlen(g.mim_buffer_arr, arrlen(g.mim_buffer_arr)+len);
	return get_mim_buffer_top();
}

void mimf(const char* fmt, ...)
{
	assert(g.in_mim);
	va_list va;
	va_start(va, fmt);
	stbsp_vsprintfcb(wrote_mim_cb, NULL, get_mim_buffer_top(), fmt, va);
	va_end(va);
}

void mim8(uint8_t v)
{
	uint8_t* p = (uint8_t*)get_mim_buffer_top();
	*p = v;
	arrsetlen(g.mim_buffer_arr, arrlen(g.mim_buffer_arr)+1);
}

#define DOJO_MAGIC ("DOJO0001")
#define DO_FORMAT_VERSION (1)
#define SYNC (0xfa)

static uint8_t bufread_fn(void* userdata)
{
	return io_bufread_next((struct io_bufread*)userdata);
}

static int64_t journal_read_leb128_i64(void)
{
	return leb128_decode_int64(bufread_fn, &g.journal_bufread);
}

static void journal_read_raw(void* data, size_t sz)
{
	io_bufread_read(&g.journal_bufread, data, sz);
}

static uint8_t journal_read_byte(void)
{
	return bufread_fn(&g.journal_bufread);
}

static void host_dir(const char* dir)
{
	char pathbuf[1<<14];
	path_join(pathbuf, sizeof pathbuf, dir, "DO_JOURNAL", NULL);
	struct io_event ev = io_open_now(g.io, ((struct iosub_open){
		.path = pathbuf,
		.read = 1,
		.write = 1,
		.create = 1,
	}));
	if (ev.status < 0) assert(!"TODO handle dir not found?");
	struct io_appender* a = &g.journal_appender;
	io_appender_init(&g.journal_appender, g.io, ev.handle, /*ringbuf_cap_log2=*/16, /*inflight_cap=*/8);
	int64_t sz = io_get_size(g.io, ev.handle);
	if (sz == 0) {
		io_appender_write_raw(a, DOJO_MAGIC, strlen(DOJO_MAGIC));
		io_appender_write_leb128(a, DO_FORMAT_VERSION);
		// TODO epoch timestamp?
		io_appender_flush_now(a);
		g.journal_timestamp_start = get_nanoseconds();
	} else {
		uint8_t magic[8];
		// XXX
		assert("FIXME ERROR HANDLING" && (0 == io_pread_now(g.io, ((struct iosub_pread){
			.handle = ev.handle,
			.data = magic,
			.size = 8,
			.offset = 0,
		}))));
		assert("FIXME ERROR HANDLING" && (memcmp(magic, DOJO_MAGIC, 8) == 0)); // XXX

		uint8_t buf[1<<12];
		struct io_bufread* jbr = &g.journal_bufread;
		memset(jbr, 0, sizeof *jbr);
		jbr->io = g.io;
		jbr->handle = ev.handle,
		jbr->file_cursor = 8;
		jbr->buf = buf;
		jbr->bufsize = sizeof(buf);

		const int64_t do_format_version = journal_read_leb128_i64();
		assert((do_format_version == DO_FORMAT_VERSION) && "XXX error handling");

		static uint8_t* mimbuf_arr = NULL;
		while (!jbr->end_of_file) {
			const uint8_t sync = journal_read_byte();
			assert((sync == SYNC) && "XXX error handling");
			const int64_t timestamp_us = journal_read_leb128_i64();
			const int64_t artist_id = journal_read_leb128_i64();
			const int64_t session_id = journal_read_leb128_i64();
			const int64_t num_bytes = journal_read_leb128_i64();
			#if 1
			printf("%ld %ld %ld %ld\n",
				timestamp_us,
				artist_id,
				session_id,
				num_bytes);
			#endif
			arrsetlen(mimbuf_arr, num_bytes);
			journal_read_raw(mimbuf_arr, num_bytes);
			begin_mim(session_id);
			mimsrc(mimbuf_arr, num_bytes);
			end_mim();
			//assert(!"XXX end_mim() writes back to journal");
		}

		assert(!"TODO INIT FROM DISK");
		#if 0
		int64_t remaining = sz;
		int64_t offset = 0;
		while (remaining > 0) {
			uint8_t buf[1<<12];
			const size_t bufsz = sizeof(buf);
			size_t rsize = (remaining > bufsz) ? bufsz : remaining;
			assert(0 == io_pread_now(g.io, ((struct iosub_pread){
				.handle = ev.handle,
				.data = buf,
				.size = rsize,
				.offset = offset,
			}))); // FIXME handle error
			offset += rsize;
			remaining -= rsize;
		}
		assert(remaining == 0);
		assert(!"TODO INIT FROM DISK");
		#endif
	}
	// TODO
}

void gig_init(void)
{
	//ringbuf_init(&g.command_ringbuf, 16);

	// XXX "getting started"-stuff here:
	g.my_artist_id = 1;
	const int document_id = 1;
	struct snapshot* ss = &g.cool_snapshot;
	struct document* doc = arraddnptr(ss->document_arr, 1);
	memset(doc, 0, sizeof *doc);
	doc->id = document_id;
	struct mim_state ms1 = {
		.artist_id = get_my_artist_id(),
		.session_id = 1,
		.document_id = document_id,
	};
	struct caret cr = {
		.tag=0,
		.caret_loc={.line=1,.column=1},
		.anchor_loc={.line=1,.column=1},
	};
	arrput(ms1.caret_arr, cr);
	arrput(ss->mim_state_arr, ms1);

	g.io = io_new(10, 128);

	if (arg_dir) {
		host_dir(arg_dir);
	}

	#if 0
	gig_thread_tick();
	gig_spool();
	#endif
}

int get_my_artist_id(void)
{
	return g.my_artist_id;
}

void get_state_and_doc(int session_id, struct mim_state** out_mim_state, struct document** out_doc)
{
	struct snapshot* ss = &g.cool_snapshot;
	const int artist_id = get_my_artist_id();
	const int num_states = arrlen(ss->mim_state_arr);
	for (int i=0; i<num_states; ++i) {
		struct mim_state* ms = arrchkptr(ss->mim_state_arr, i);
		if ((ms->artist_id==artist_id) && (ms->session_id==session_id)) {
			const int document_id = ms->document_id;
			assert((document_id > 0) && "invalid document id in mim state");
			struct document* doc = NULL;
			const int num_docs = arrlen(ss->document_arr);
			for (int i=0; i<num_docs; ++i) {
				struct document* d = arrchkptr(ss->document_arr, i);
				if (d->id == document_id) {
					doc = d;
					break;
				}
			}
			assert((doc != NULL) && "invalid document id (not found) in mim state");
			if (out_mim_state) *out_mim_state = ms;
			if (out_doc) *out_doc = doc;
			return;
		}
	}
	assert(!"state not found");
}

FORMATPRINTF1
static void mimerr(const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	char msg[1<<12];
	stbsp_vsnprintf(msg, sizeof msg, fmt, va);
	va_end(va);
	fprintf(stderr, "MIMERR :: [%s]\n", msg);
}

static void doc_location_constraint(struct document* doc, struct location* loc)
{
	struct doc_iterator it = doc_iterator(doc);
	doc_iterator_locate(&it, loc);
	*loc = it.location;
}

struct mimop {
	struct cow_snapshot* cows;
	int document_id;
	int artist_id, session_id;
};

static struct document* mimop_rw_doc(struct mimop* mo)
{
	return cow_snapshot_get_readwrite_document_by_id(mo->cows, mo->document_id);
}

static struct mim_state* mimop_rw_ms(struct mimop* mo)
{
	return cow_snapshot_get_readwrite_mim_state_by_ids(mo->cows, mo->artist_id, mo->session_id);
}

static void mimop_delete(struct mimop* mo, struct location* loc0, struct location* loc1)
{
	struct document* doc = mimop_rw_doc(mo);
	location_sort2(&loc0, &loc1);
	const int o0 = document_locate(doc, loc0);
	int o1 = document_locate(doc, loc1);
	for (int o=o0; o<o1; ++o) {
		struct fat_char* fc = arrchkptr(doc->fat_char_arr, o);
		if (fc->is_insert) {
			arrdel(doc->fat_char_arr, arrchk(doc->fat_char_arr, o));
			--o;
			--o1;
		} else {
			fc->is_delete = 1;
		}
	}
}

// parses a mim message, typically written by mimf()/mim8()
static int mim_spool(struct mimop* mo, const uint8_t* input, int num_bytes)
{
	const char* p = (const char*)input;
	int remaining = num_bytes;

	static int* number_stack_arr = NULL;
	arrreset(number_stack_arr);

	enum {
		COMMAND=1,
		NUMBER,
		INSERT_STRING,
		INSERT_COLOR_STRING,
		MOTION,
	};

	enum {
		UTF8,
		U32,
	};

	int mode = COMMAND;
	int datamode = UTF8;
	int previous_mode = -1;
	int number=0, number_sign=0;
	int push_chr = -1;
	int string_bytes_remaining = 0;
	int arg_tag = -1;
	int arg_num = -1;
	int motion_cmd = -1;

	const int64_t now = get_nanoseconds();

	int chr=0;
	uint32_t u32val=0;
	while ((push_chr>=0) || (remaining>0)) {
		const char* p0 = p;
		if (datamode == UTF8) {
			chr = (push_chr>=0) ? push_chr : utf8_decode(&p, &remaining);
		} else if (datamode == U32) {
			assert(push_chr == -1);
			assert(remaining >= 4);
			const uint8_t* p8 = (const uint8_t*)p;
			u32val = decode_u32(&p8);
			p = (const char*)p8;
			remaining -= 4;
			assert(!"TODO");
		} else {
			assert(!"unhandled datamode");
		}
		const char* p1 = p;
		const int num_bytes = (p1-p0);
		push_chr = -1;
		if (chr == -1) {
			mimerr("invalid UTF-8 input");
			return 0;
		}
		const int num_args = arrlen(number_stack_arr);

		switch (mode) {

		case COMMAND: {
			if (is_digit(chr) || chr=='-') {
				previous_mode = mode;
				mode = NUMBER;
				number = 0;
				if (chr == '-') {
					number_sign = -1;
				} else {
					number_sign = 1;
					push_chr = chr;
				}
			} else {
				switch (chr) {

				case ':': {
					assert(!"TODO mim ex command");
				}	break;

				case '!': // commit
				case '/': // cancel
				case '-': // defer
				case '+': // fer (XXX stops at defer, hmm...)
				case '*': // toggle fer/defer..?
				{
					if (num_args != 1) {
						mimerr("command '%c' expected 1 argument; got %d", chr, num_args);
						return 0;
					}
					arg_tag = arrchkget(number_stack_arr, 0);
					arrreset(number_stack_arr);

					struct document* doc = mimop_rw_doc(mo);
					struct mim_state* ms = mimop_rw_ms(mo); // XXX could be readonly?
					int num_chars = arrlen(doc->fat_char_arr);
					const int num_carets = arrlen(ms->caret_arr);
					for (int i=0; i<num_carets; ++i) {
						struct caret* car = arrchkptr(ms->caret_arr, i);
						if (car->tag != arg_tag) continue;
						struct location* loc0 = &car->caret_loc;
						struct location* loc1 = &car->anchor_loc;
						location_sort2(&loc0, &loc1);
						const int off0 = document_locate(doc, loc0);
						const int off1 = document_locate(doc, loc1);
						for (int off=off0; off<=off1; ++off) {
							for (int dir=0; dir<2; ++dir) {
								int d,o;
								if      (dir==0) { d= 1; o=off  ; }
								else if (dir==1) { d=-1; o=off-1; }
								else assert(!"unreachable");
								while ((0 <= o) && (o < num_chars)) {
									struct fat_char* fc = arrchkptr(doc->fat_char_arr, o);
									const int is_fillable = (fc->is_insert || fc->is_delete);
									if (fc->_fill || fc->is_defer || !is_fillable) break;
									if (is_fillable) fc->_fill = 1;
									o += d;
								}
							}
						}
						car->anchor_loc = car->caret_loc;
					}

					for (int i=0; i<num_chars; ++i) {
						struct fat_char* fc = arrchkptr(doc->fat_char_arr, i);
						if (!fc->_fill) continue;

						if (chr=='!') {
							if (fc->is_insert) { // commit
								fc->_fill=0;
								fc->is_insert=0;
							}
							if (fc->is_delete) {
								arrdel(doc->fat_char_arr, arrchk(doc->fat_char_arr, i));
								--i;
								--num_chars;
							}
						} else if (chr=='/') { // cancel
							if (fc->is_insert) {
								arrdel(doc->fat_char_arr, arrchk(doc->fat_char_arr, i));
								--i;
								--num_chars;
							}
							if (fc->is_delete) {
								fc->_fill=0;
								fc->is_delete=0;
							}
						} else {
							assert(!"unhandled command");
						}
					}

				}	break;

				#if 0
				case 'c': {
					if (num_args != 3) {
						mimerr("command 'c' expected 3 arguments; got %d", num_args);
						return 0;
					}
					assert(!"TODO c"); // TODO
					arrreset(number_stack_arr);
				}	break;

				case 'C': {
					if (num_args != 2) {
						mimerr("command 'C' expected 2 arguments; got %d", num_args);
						return 0;
					}
					assert(!"TODO C"); // TODO
					arrreset(number_stack_arr);
				}	break;
				#endif

				case 'S':
				case 'M': {
					if (num_args != 1) {
						mimerr("command '%c' expected 1 argument; got %d", chr, num_args);
						return 0;
					}
					arg_tag = arrchkget(number_stack_arr, 0);
					arrreset(number_stack_arr);
					previous_mode = mode;
					motion_cmd = chr;
					mode = MOTION;
				}	break;

				case 'i':
				case 'I': {
					if (num_args != 2) {
						mimerr("command '%c' expected 2 arguments; got %d", chr, num_args);
						return 0;
					}
					assert(string_bytes_remaining == 0);
					string_bytes_remaining = arrchkget(number_stack_arr, 1);
					arg_tag = arrchkget(number_stack_arr, 0);
					arrreset(number_stack_arr);

					previous_mode = mode;
					switch (chr) {
					case 'i': mode = INSERT_STRING; break;
					case 'I': mode = INSERT_COLOR_STRING; break;
					default: assert(!"unexpected chr");
					}
				}	break;

				case 'X': // backspace
				case 'x': // delete
				{
					if (num_args != 1) {
						mimerr("command '%c' expected 1 argument; got %d", chr, num_args);
						return 0;
					}
					arg_tag = arrchkget(number_stack_arr, 0);
					arg_num = 1; // XXX make it an optional arg?
					arrreset(number_stack_arr);

					struct document* doc = mimop_rw_doc(mo);
					struct mim_state* ms = mimop_rw_ms(mo);
					const int num_carets = arrlen(ms->caret_arr);
					for (int i=0; i<num_carets; ++i) {
						struct caret* car = arrchkptr(ms->caret_arr, i);
						if (car->tag != arg_tag) continue;
						struct location* caret_loc = &car->caret_loc;
						struct location* anchor_loc = &car->anchor_loc;
						if (0 == location_compare(caret_loc, anchor_loc)) {
							int o = document_locate(doc, caret_loc);
							int d,m0,m1;
							if (chr == 'X') { // backspace
								d=-1; m0=-1; m1=-1;
							} else if (chr == 'x') { // delete
								d=0;  m0=0;  m1=1;
							} else {
								assert(!"unexpected char");
							}

							for (int i=0; i<arg_num; ++i) {
								const int num_chars = arrlen(doc->fat_char_arr);
								const int od = o+d;
								int dc=0;
								if ((0 <= od) && (od < num_chars)) {
									struct fat_char* fc = arrchkptr(doc->fat_char_arr, od);
									if (fc->is_insert) {
										arrdel(doc->fat_char_arr, od);
										dc=m0;
									} else if (fc->is_delete) {
										dc=m1;
									} else {
										assert((!fc->is_insert) && (!fc->is_delete));
										if (fc->is_delete == 0) {
											fc->is_delete = 1;
											fc->flipped_delete = 1;
											fc->timestamp = now;
										}
										dc=m1;
									}
								}
								caret_loc->column += dc;
								doc_location_constraint(doc, caret_loc);
								o += dc;
							}
						} else {
							mimop_delete(mo, caret_loc, anchor_loc);
						}
						*anchor_loc = *caret_loc;
					}
				}	break;

				default:
					mimerr("invalid command '%c'/%d", chr, chr);
					return 0;

				}
			}
		}	break;

		case NUMBER: {
			if (is_digit(chr)) {
				number = ((10*number) + (chr-'0'));
			} else {
				number *= number_sign;
				arrput(number_stack_arr, number);
				mode = previous_mode;
				if (chr != ',') push_chr = chr;
			}
		}	break;

		case INSERT_STRING:
		case INSERT_COLOR_STRING: {
			int do_insert = 0;
			int next_datamode = datamode;
			if (mode == INSERT_STRING) {
				assert(datamode == UTF8);
				do_insert = 1;
			} else if (mode == INSERT_COLOR_STRING) {
				if (datamode == U32) {
					do_insert = 1;
					next_datamode = UTF8;
				} else {
					next_datamode = U32;
				}
			} else {
				assert(!"unreachable");
			}

			if (do_insert) {
				if (chr < ' ') {
					switch (chr) {
					case '\n':
					case '\t': // XXX consider not supporting tabs?
						// accepted control codes
						break;
					default:
						mimerr("invalid control code %d in string", chr);
						return 0;
					}
				}

				struct mim_state* ms = mimop_rw_ms(mo);

				uint8_t r,g,b,x;
				if (mode == INSERT_STRING) {
					// insert with artist mim state color
					r=ms->color[0];
					g=ms->color[1];
					b=ms->color[2];
					x=ms->color[3];
				} else if (mode == INSERT_COLOR_STRING) {
					// insert with encoded color
					r=(u32val    ) & 0xff,
					g=(u32val>>8 ) & 0xff,
					b=(u32val>>16) & 0xff,
					x=(u32val>>24) & 0xff;
				} else {
					assert(!"unreachable");
				}

				struct document* doc = mimop_rw_doc(mo);
				const int num_carets = arrlen(ms->caret_arr);
				for (int i=0; i<num_carets; ++i) {
					struct caret* car = arrchkptr(ms->caret_arr, i);
					if (car->tag != arg_tag) continue;

					struct location* loc = &car->caret_loc;
					struct location* anchor = &car->anchor_loc;
					if (0 != location_compare(loc, anchor)) mimop_delete(mo, loc, anchor);
					*anchor = *loc;

					const int off = document_locate(doc, loc);
					arrins(doc->fat_char_arr, off, ((struct fat_char){
						.thicchar = {
							.codepoint = chr,
							.color = {r,g,b,x},
						},
						.timestamp = now,
						//.artist_id = get_my_artist_id(),
						.is_insert = 1,
						.flipped_insert = 1,
					}));
					if (chr == '\n') {
						++loc->line;
						loc->column = 1;
					} else {
						++loc->column;
					}
					doc_location_constraint(doc, loc);
					*anchor = *loc;
				}
			}

			datamode = next_datamode;

			assert(string_bytes_remaining > 0);
			string_bytes_remaining -= num_bytes;
			assert(string_bytes_remaining >= 0);
			if (string_bytes_remaining == 0) {
				assert(datamode == UTF8);
				arrreset(number_stack_arr);
				assert(previous_mode == COMMAND);
				mode = previous_mode;
			}

		}	break;

		case MOTION: {

			assert(arrlen(number_stack_arr) == 0);
			// TODO read number?

			switch (chr) {

			case 'h': // left
			case 'l': // right
			case 'k': // up
			case 'j': // down
			{
				const int is_left  = (chr=='h');
				const int is_right = (chr=='l');
				const int is_up    = (chr=='k');
				const int is_down  = (chr=='j');

				struct mim_state* ms = mimop_rw_ms(mo);
				const int num_carets = arrlen(ms->caret_arr);
				struct document* doc = mimop_rw_doc(mo);
				for (int i=0; i<num_carets; ++i) {
					struct caret* car = arrchkptr(ms->caret_arr, i);
					if (car->tag != arg_tag) continue;
					struct location* loc0 = motion_cmd=='S' ? &car->caret_loc : &car->anchor_loc;
					struct location* loc1 = &car->caret_loc;
					location_sort2(&loc0, &loc1);
					if (0 == location_compare(loc0,loc1)) {
						if (is_left) {
							--loc0->column;
							doc_location_constraint(doc, loc0);
						}
						if (is_right) {
							++loc1->column;
							doc_location_constraint(doc, loc1);
						}
						if (is_up) {
							--loc0->line;
							doc_location_constraint(doc, loc0);
						}
						if (is_down) {
							++loc1->line;
							doc_location_constraint(doc, loc1);
						}
					}
					if (is_left  || is_up  ) *loc1 = *loc0;
					if (is_right || is_down) *loc0 = *loc1;
				}
			}	break;

			default:
				// XXX no: reject instead? otherwise untrusted input (mim in
				// files, over network,...) crashes the program
				assert(!"unhandled motion char");

			}

			assert(previous_mode == COMMAND);
			mode = previous_mode;

		}	break;

		default:
			assert(!"unhandled mim_spool()-mode");

		}
	}

	if (mode != COMMAND) {
		mimerr("mode (%d) not terminated", mode);
		return 0;
	}

	const int num_args = arrlen(number_stack_arr);
	if (num_args > 0) {
		mimerr("non-empty number stack (n=%d) at end of mim-input", num_args);
		return 0;
	}

	return 1;
}

void begin_mim(int session_id)
{
	assert(!g.in_mim);
	g.in_mim = 1;
	g.using_mim_session_id = session_id;
	arrreset(g.mim_buffer_arr);
}

#if 0
static void u8pp_write(uint8_t v, void* userdata)
{
	uint8_t** p = (uint8_t**)userdata;
	**p = v;
	++(*p);
}

static void write_varint64(uint8_t** pp, uint8_t* end, int64_t value)
{
	assert(((end - *pp) >= LEB128_MAX_LENGTH) && "not enough space for largest-case leb128 encoding");
	leb128_encode_int64(u8pp_write, pp, value);
}
#endif

static void document_to_thicchar_da(struct thicchar** arr, struct document* doc)
{
	const int num_src = arrlen(doc->fat_char_arr);
	arrsetmincap(*arr, num_src);
	arrreset(*arr);
	for (int i=0; i<num_src; ++i) {
		struct fat_char* fc = arrchkptr(doc->fat_char_arr, i);
		if (fc->is_insert) continue; // not yet inserted
		arrput(*arr, fc->thicchar);
	}
}

void end_mim(void)
{
	assert(g.in_mim);
	const int num_bytes = arrlen(g.mim_buffer_arr);
	g.in_mim = 0;
	if (num_bytes == 0) return;

	uint8_t* data = g.mim_buffer_arr;

	#if 0
	struct snapshot* ss = &g.cool_snapshot;

	struct mim_state* msr = snapshot_get_personal_mim_state_by_id(ss, g.using_mim_session_id);
	struct document* doc = snapshot_get_document_by_id(ss, msr->document_id);

	// make "scratch copies" of state and doc and work on these; this
	// protects the actual state in case of partially invalid input
	static struct mim_state scratch_state = {0};
	static struct document scratch_doc = {0};
	mim_state_copy(&scratch_state, msr);
	document_copy(&scratch_doc, doc);

	struct mimop mo = { .ms=&scratch_state, .doc=&scratch_doc, };
	if (!mim_spool(&mo, data, num_bytes)) {
		assert(!"mim protocol error"); // XXX? should I have a "failable" flag? eg. for human input
	}
	#endif

	struct snapshot* snapshot = &g.cool_snapshot;
	mie_begin_scrallox();

	if (0 == setjmp(*mie_prep_scrallox_jmp_buf_for_out_of_memory())) {
		struct cow_snapshot cows = {0};
		init_cow_snapshot(&cows, snapshot, mie_borrow_scrallox());

		struct mim_state* ms = snapshot_get_personal_mim_state_by_id(snapshot, g.using_mim_session_id);
		struct mimop mo = {
			.cows = &cows,
			.document_id = ms->document_id,
			.artist_id = get_my_artist_id(), // XXX?
			.session_id = g.using_mim_session_id,
		};

		if (!mim_spool(&mo, data, num_bytes)) {
			assert(!"mim protocol error"); // XXX? should I have a "failable" flag? eg. for human input
		}

		struct document* doc = cow_snapshot_get_readonly_document_by_id(&cows, ms->document_id);
		static struct thicchar* dodoc_arr = NULL;
		document_to_thicchar_da(&dodoc_arr, doc);
		const int prg = mie_compile_thicc(dodoc_arr, arrlen(dodoc_arr));
		//printf("prg=%d\n", prg);
		if (prg == -1) {
			printf("TODO compile error [%s]\n", mie_error());
		} else {
			vmie_reset(prg);
			vmie_run();
			mie_program_free(prg);
		}

		cow_snapshot_commit(&cows);
	} else {
		printf("ERROR out of scratch memory!\n"); // XXX?
	}

	#if 1
	{
		size_t allocated, capacity;
		mie_scrallox_stats(&allocated, &capacity);
		printf("scrallox scope: allocated %zd/%zd (%.1f%%)\n",
			allocated, capacity,
			(double)(allocated*100L) / (double)(capacity));
	}
	#endif

	mie_end_scrallox();

	#if 0
	mim_state_copy(msr, &scratch_state);
	document_copy(doc, &scratch_doc);
	#endif

	// XXX more assumptions that we are the host

	struct io_appender* a = &g.journal_appender;
	if (io_appender_is_initialized(a)) {
		io_appender_write_raw(a, "\xfa", 1);
		const int64_t journal_timestamp = (get_nanoseconds() - g.journal_timestamp_start)/1000LL;
		io_appender_write_leb128(a, journal_timestamp);
		io_appender_write_leb128(a, get_my_artist_id());
		io_appender_write_leb128(a, g.using_mim_session_id);
		io_appender_write_leb128(a, num_bytes);
		io_appender_write_raw(a, data, num_bytes);
		io_appender_flush(a);
	}

	#if 0
	uint8_t header[1<<8];
	uint8_t* hp = header;
	uint8_t* end = header + sizeof(header);
	write_varint64(&hp, end, g.using_mim_session_id);
	write_varint64(&hp, end, num_bytes);
	const size_t nh = hp - header;
	ringbuf_writen(&g.command_ringbuf, header, nh);
	ringbuf_writen(&g.command_ringbuf, data, num_bytes);
	ringbuf_commit(&g.command_ringbuf);
	#endif
}

void gig_selftest(void)
{
	// TODO (used to be a mim test here)
}

int doc_iterator_next(struct doc_iterator* it)
{
	assert((!it->done) && "you cannot call this function after it has returned 0");
	struct document* d = it->doc;
	const int num_chars = arrlen(d->fat_char_arr);
	if (it->last) {
		it->done = 1;
		assert(it->offset == num_chars);
		return 0;
	}

	if (it->new_line) {
		++it->location.line;
		it->location.column = 1;
		it->new_line = 0;
	} else {
		++it->location.column;
	}

	++it->offset;
	const int off = it->offset;
	if (off < num_chars) {
		it->fat_char = arrchkptr(d->fat_char_arr, off);
		if (it->fat_char->thicchar.codepoint == '\n') {
			it->new_line = 1;
		}
	} else {
		assert(off == num_chars);
		it->fat_char = NULL;
		it->last = 1;
	}

	return 1;
}
