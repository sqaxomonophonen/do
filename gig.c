#include <limits.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h> // XXX

#include "stb_sprintf.h"

#include "gig.h"
#include "util.h"
#include "leb128.h"
#include "utf8.h"
#include "main.h"
#include "da.h"

struct ringbuf {
	uint8_t* data;
	int size_log2;

	//size_t read_cursor;
	size_t write_cursor;

	// fields on separate cache lines to avoid "false sharing"
	//_Alignas(CACHE_LINE_SIZE) _Atomic(size_t) acknowledge_cursor;
	_Alignas(CACHE_LINE_SIZE) _Atomic(size_t) commit_cursor;
};

static void ringbuf_init_with_pointer(struct ringbuf* rb, void* data, int size_log2)
{
	memset(rb, 0, sizeof *rb);
	rb->size_log2 = size_log2;
	rb->data = data;
}

static void ringbuf_init(struct ringbuf* rb, int size_log2)
{
	ringbuf_init_with_pointer(rb, calloc(1L<<size_log2, 1), size_log2);
}

static void ringbuf_commit(struct ringbuf* rb)
{
	atomic_store(&rb->commit_cursor, rb->write_cursor);
}


static void ringbuf_writen(struct ringbuf* rb, const void* src_data, size_t num_bytes)
{
	const size_t size = 1L << rb->size_log2;
	const size_t mask = size - 1L;
	const size_t cursor = rb->write_cursor;
	const size_t until_next_wrap_around = size - (cursor & mask);
	uint8_t* dst_data = rb->data;
	uint8_t* dst_first = dst_data + (cursor & mask);
	if (until_next_wrap_around >= num_bytes) {
		memcpy(dst_first, src_data, num_bytes);
	} else {
		memcpy(dst_first, src_data, until_next_wrap_around);
		const size_t remaining = num_bytes - until_next_wrap_around;
		assert((until_next_wrap_around + remaining) == num_bytes);
		assert((remaining>0) && "expected to write at least 1 byte");
		memcpy(dst_data, src_data + until_next_wrap_around, remaining);
	}
	rb->write_cursor = cursor + num_bytes;
}

#if 0
static void document_copy(struct document* dst, struct document* src)
{
	const struct document copy = *dst;
	memcpy(dst, src, sizeof *dst);

	// the arrays are handled differently, reusing the "_arr"s in the dst
	// document (simplifies memory management)
	dst->fat_char_arr = copy.fat_char_arr;
	//dst->caret_arr = copy.caret_arr;
	dst->mim_state_arr = copy.mim_state_arr;

	ARRCOPY(dst->fat_char_arr, src->fat_char_arr);
	ARRCOPY(dst->mim_state_arr, src->mim_state_arr);
}
#endif

struct snapshot {
	size_t journal_cursor;
	DA(struct document, documents);
};

static int snapshot_get_num_documents(struct snapshot* ss)
{
	return daLen(ss->documents);
}

struct document* snapshot_get_document_by_index(struct snapshot* ss, int index)
{
	return daPtr(ss->documents, index);
}

struct document* snapshot_find_document_by_id(struct snapshot* ss, int id)
{
	const int n = snapshot_get_num_documents(ss);
	for (int i=0; i<n; ++i) {
		struct document* doc = snapshot_get_document_by_index(ss, i);
		if (doc->id == id) return doc;
	}
	return NULL;
}

struct mim_state_da DA_STRUCT_BODY(struct mim_state);

static struct {
	struct ringbuf command_ringbuf;
	int document_id_sequence;
	struct snapshot outside, inside;
	//struct document scratch_doc;
	struct ringbuf journal_ringbuf;
	int my_artist_id;
	DA(uint8_t, mim_buffer);
	int using_personal_mim_state_id;
	int in_mim;
	struct mim_state_da hot_mim_states, my_cool_mim_states;
} g;

int get_num_documents(void)
{
	return snapshot_get_num_documents(&g.outside);
}

struct document* get_document_by_index(int index)
{
	return snapshot_get_document_by_index(&g.outside, index);
}

struct document* find_document_by_id(int id)
{
	assert((id >= 1) && "id must be at least 1");
	return snapshot_find_document_by_id(&g.outside, id);
}

struct document* get_document_by_id(int id)
{
	struct document* doc = find_document_by_id(id);
	assert((doc != NULL) && "document id not found");
	return doc;
}

static void snapshot_spool(struct snapshot* ss, struct ringbuf* journal)
{
	// TODO?
}

void gig_spool(void)
{
	snapshot_spool(&g.outside, &g.journal_ringbuf);
}

void gig_thread_tick(void)
{
	// TODO?
}

void gig_init(void)
{
	ringbuf_init(&g.command_ringbuf, 16);

	// XXX "getting started"-stuff here:
	g.my_artist_id = 1;
	const int doc_id = 1;
	struct document* doc = daAddNPtr(g.outside.documents, 1);
	memset(doc, 0, sizeof *doc);
	doc->id = doc_id;
	struct mim_state ms1 = {
		.artist_id = get_my_artist_id(),
		.personal_id = 1,
		.document_id = doc_id,
	};

	daPut(g.hot_mim_states, ms1);
	daPut(g.my_cool_mim_states, ms1);

	#if 0
	gig_thread_tick();
	gig_spool();
	#endif
}

int get_my_artist_id(void)
{
	return g.my_artist_id;
}

int get_num_mim_states(void)
{
	return daLen(g.hot_mim_states);
}

static struct mim_state* find_mim_state_by_id(struct mim_state_da* da, int id)
{
	const int n = daLen(*da);
	for (int i=0; i<n; ++i) {
		struct mim_state* ms = daPtr(*da, i);
		if (ms->personal_id == id) return ms;
	}
	return NULL;
}

struct mim_state* check_my_mim_state(struct mim_state* ms, int id)
{
	assert(ms != NULL);
	assert(ms->artist_id == get_my_artist_id());
	assert(ms->personal_id == id);
	return ms;
}

struct mim_state* get_cool_mim_state_by_personal_id(int id)
{
	return check_my_mim_state(find_mim_state_by_id(&g.my_cool_mim_states, id), id);
}

struct mim_state* get_hot_mim_state_by_personal_id(int id)
{
	return check_my_mim_state(find_mim_state_by_id(&g.hot_mim_states, id), id);
}

struct mim_state* get_mim_state_by_index(int index)
{
	return daPtr(g.hot_mim_states, index);
}

#if 0
int mim_state_cool_compar(struct mim_state_cool* a, struct mim_state_cool* b)
{
	const int d0 = a->document_id - b->document_id;
	if (d0!=0) return d0;
	const int d1 = (int)a->mode - (int)b->mode;
	if (d1!=0) return d1;
	const int n = arrlen(a->query_arr);
	const int d2 = n - arrlen(b->query_arr);
	if (d2!=0) return d2;
	if (n>0) {
		const int d3 = memcmp(a->query_arr, b->query_arr, n);
		if (d3!=0) return d3;
	}
	if (arrlen(a->caret_id_arr)>0 || arrlen(b->caret_id_arr)>0) {
		assert(!"TODO");
	}
	return 0;
}

static void mim_chew(struct mim_state_cool* cool, struct mim_state_hot* hot, uint8_t input, int* hot_update)
{
	const int is_escape = ('\033'==input);
	const int is_return = ('\n'==input);

	if (cool != NULL) {
		switch (cool->mode) {
		case MIM_COMMAND: {
			switch (input) {
			case ':': cool->mode = MIM_EX_COMMAND; break;
			case '/': cool->mode = MIM_SEARCH_FORWARD; break;
			case '?': cool->mode = MIM_SEARCH_BACKWARD; break;

			case 'h':
			case 'j':
			case 'k':
			case 'l':
				printf("mim_chew(): TODO caret movement (%c)\n", input);
				break;

			case 'i':
			case 'I':
			case 'a':
			case 'A':
			case 'o':
			case 'O':
				// XXX how to handle caret movements and line insertions here?
				if (hot_update) *hot_update=1;
				cool->mode = MIM_INSERT;
				break;

			//default: assert(!"unhandled command"); // XXX harsh?
			default:
				printf("mim_chew(): unhandled input [%c/%d]\n", input, input);
				break;
			}
		}	break;
		case MIM_VISUAL:
		case MIM_VISUAL_LINE:
		//MIM_VISUAL_BLOCK?
			assert(!"TODO visual command");
		case MIM_EX_COMMAND:
		case MIM_SEARCH_FORWARD:
		case MIM_SEARCH_BACKWARD: {
			if (is_escape) {
				arrsetlen(cool->query_arr, 0);
				cool->mode = MIM_COMMAND;
			} else if (is_return) {
				arrput(cool->query_arr, 0);
				char* q = cool->query_arr;
				if (cool->mode == MIM_EX_COMMAND) {
					char* q1;
					for (q1=q; *q1 && *q1!=' '; ++q1) {}
					*q1=0;
					if (strcmp("document",q)==0) {
						const long document_id = strtol(q1+1, NULL, 10);
						cool->document_id = document_id;
					} else {
						printf("mim_chew(): unhandled ex command [%s]\n", q);
					}
				} else {
					assert(!"TODO search");
				}
				cool->mode = MIM_COMMAND;
			} else {
				arrput(cool->query_arr, input);
			}
		}	break;
		case MIM_INSERT:
			if (is_escape) {
				cool->mode = MIM_COMMAND;
			} else {
				if (hot_update) *hot_update=1;
			}
			break;
		default: assert(!"unhandled mim mode");
		}
	}

	assert((hot == NULL) && "unhandled");
}
#endif

#if 0
static char* wrote_command_cb(const char* buf, void* user, int len)
{
	struct ringbuf* rb = &g.command_ringbuf;
	ringbuf_wrote_linear(rb, len);
	return ringbuf_get_write_pointer(rb);
}

static void mimf_raw_va(/*int mim_state_id,*/ const char* fmt, va_list va0)
{
	struct codec c = {0};
	struct ringbuf* rb = &g.command_ringbuf;
	codec_begin_encode(&c, rb);
	const size_t w0 = rb->write_cursor;
	codec_write_varint(&c, -1); // size placeholder
	const size_t w1 = rb->write_cursor;
	assert(w1 == (w0+1));
	va_list va;
	va_copy(va, va0);
	stbsp_vsprintfcb(wrote_command_cb, NULL, ringbuf_get_write_pointer(rb), fmt, va);
	const size_t w2 = rb->write_cursor;
	const size_t print_size = w2-w1;
	rb->write_cursor = w0; // reset and write size
	codec_write_varint64(&c, print_size);
	const size_t w1b = rb->write_cursor;
	if ((w1b-w0) > 1) {
		// oops, 1 byte wasn't enough for the varint length. print again from
		// the correct position
		va_copy(va, va0);
		stbsp_vsprintfcb(wrote_command_cb, NULL, ringbuf_get_write_pointer(rb), fmt, va);
		assert(((rb->write_cursor - w1b) == print_size) && "expected to print same length again");
	} else {
		assert((w1b-w0) == 1);
		rb->write_cursor = w2; // restore write cursor
	}
	codec_end_encode(&c);
}

static void mimf_raw(/*int mim_state_id, */const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	mimf_raw_va(/*mim_state_id, */fmt, va);
	va_end(va);
}
#endif

void begin_mim(int personal_mim_state_id)
{
	assert(!g.in_mim);
	g.in_mim = 1;
	g.using_personal_mim_state_id = personal_mim_state_id;
	daReset(g.mim_buffer);
}

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

void end_mim(void)
{
	assert(g.in_mim);
	const int n = daLen(g.mim_buffer);
	if (n>0) {
		// TODO spool applicable changes into cool mim state? (also validate?)
		uint8_t header[1<<8];
		uint8_t* hp = header;
		uint8_t* end = header + sizeof(header);
		write_varint64(&hp, end, g.using_personal_mim_state_id);
		write_varint64(&hp, end, n);
		const size_t nh = hp - header;
		ringbuf_writen(&g.command_ringbuf, header, nh);
		ringbuf_writen(&g.command_ringbuf, g.mim_buffer.items, n);
		ringbuf_commit(&g.command_ringbuf);
	}
	g.in_mim = 0;
}

static char* get_mim_buffer_top(void)
{
	daSetMinCap(g.mim_buffer, daLen(g.mim_buffer) + STB_SPRINTF_MIN);
	return (char*)g.mim_buffer.items + daLen(g.mim_buffer);
}

static char* wrote_mim_cb(const char* buf, void* user, int len)
{
	daSetLen(g.mim_buffer, daLen(g.mim_buffer)+len);
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

void gig_selftest(void)
{
	{ // test ringbuf_writen()
		struct ringbuf rb;
		uint8_t buf[1<<4] = {0};
		ringbuf_init_with_pointer(&rb, buf, 4);
		const char* p0 = "0123456";
		const size_t s0 = strlen(p0);
		const char* p1 = "xy";
		const size_t s1 = strlen(p1);

		// write p0 twice
		ringbuf_writen(&rb, p0, s0);
		assert(rb.write_cursor == (s0));
		ringbuf_writen(&rb, p0, s0);
		assert(rb.write_cursor == (s0+s0));

		// write exactly to the end of the ring buffer wrap around point: there
		// should be an assert that we don't do a zero-byte memcpy (i.e. that
		// we mistakenly split the write into two memcpy's)
		ringbuf_writen(&rb, p1, s1);
		assert(rb.write_cursor == (s0+s0+s1));
		assert((rb.write_cursor & ((1L<<rb.size_log2)-1L)) == 0L);

		// verify data, expect 2 × p0, 1 × p1
		char* p = (char*)rb.data;
		const char* expected = "01234560123456xy";
		assert(strlen(expected) == 16);
		for (int i=0; i<strlen(expected); ++i) assert(*(p++) == expected[i]);

		// write p0 again to advance write cursor
		ringbuf_writen(&rb, p0, s0);
		assert(rb.write_cursor == (s0+s0+s1+s0));

		// write across wrap-around point (should be 2 memcpy's)
		const char* p2 = "abcdefghijk";
		const size_t s2 = strlen(p2);
		ringbuf_writen(&rb, p2, s2);
		assert(rb.write_cursor == (s0+s0+s1+s0+s2));

		// verify final data
		expected = "jk23456abcdefghi";
		assert(strlen(expected) == 16);
		p = (char*)rb.data;
		for (int i=0; i<strlen(expected); ++i) assert(*(p++) == expected[i]);

		// test commit
		assert(rb.commit_cursor == 0);
		ringbuf_commit(&rb);
		assert(rb.commit_cursor == rb.write_cursor);
	}

	#if 0
	{ // test mimf_raw
		uint8_t buf[(1<<9) + STB_SPRINTF_MIN];
		assert(sizeof(buf) == 1024);
		uint8_t* p;

		{
			ringbuf_init_with_pointer(&g.command_ringbuf, buf, 9, STB_SPRINTF_MIN);
			mimf_raw("x=%d", 69);
			p = buf;
			assert(*(p++) == 4); // string length
			assert(*(p++) == 'x'); assert(*(p++) == '=');
			assert(*(p++) == '6'); assert(*(p++) == '9');
		}

		{ // test special case when string is 64 bytes or longer (length is varint)
			ringbuf_init_with_pointer(&g.command_ringbuf, buf, 9, STB_SPRINTF_MIN);
			mimf_raw(
			"0123456789ABCDEF"
			"0123456789ABCDEF"
			"0123456789ABCDEF"
			"0123456789ABCDEF"
			"!"
			);
			p = buf;
			assert(*(p++) == 0xc1); assert(*(p++) == 0x00); // 65 (string length) as leb128
			for (int i=0;i<4;++i) {
				for (int ii=0;ii<16;++ii) {
					assert(*(p++) == "0123456789ABCDEF"[ii]);
				}
			}
			assert(*p == '!');
		}
	}
	#endif
}
