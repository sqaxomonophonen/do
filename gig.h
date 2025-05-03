#ifndef GIG_H

#include <stdint.h>

#include "util.h"
#include "da.h"

struct location {
	int line;
	int column;
};

static inline int location_compare(struct location* a, struct location* b)
{
	const int d0 = a->line - b->line;
	if (d0 != 0) return d0;
	const int d1 = a->column - b->column;
	return d1;
}

struct range {
	struct location from, to;
};

struct fat_char {
	unsigned codepoint;
	unsigned timestamp; // last change (or created)
	unsigned artist_id;
	unsigned color[3];
	unsigned is_insert :1;
	unsigned is_delete :1;
	unsigned is_defer  :1;
};

struct caret {
	int tag;
	//unsigned flags; // visible? visibility-groups?
	struct range range;
};

struct mim_state {
	int artist_id;
	int personal_id;
	// [artist_id,personal_id] is the "unique key"
	int document_id;
	DA(struct caret, carets);
	// (update mim_state_copy() when adding da-fields here)
};

enum document_type {
	DOCUMENT_TYPE_ROOT = 1,
	DOCUMENT_TYPE_AUDIO,
	DOCUMENT_TYPE_VIDEO,
	//DOCUMENT_TYPE_SCRATCHPAD // XXX do I want/need this?
	DOCUMENT_TYPE_PRESENTATION_SHADER,
	// XXX re:DOCUMENT_TYPE_PRESENTATION_SHADER: code that translates "fat_chars"
	// into "render_chars". but do I need a type per "presentation backend"?
	// would a "braille backend" need another doc type?
};

enum document_flag {

	DOCUMENT_FLAG_REPLICA = (1<<0),
	// the document is a replica from a remote venue. if this flag is not set
	// it implies we're the venue.

	DOCUMENT_FLAG_SYSTEM = (1<<1),
	// the document is a system document which is always executed before your
	// non-system documents. these typically contain language defining- and
	// library/shared code. if set, document type must be DOCUMENT_TYPE_ROOT,
	// DOCUMENT_TYPE_AUDIO or DOCUMENT_TYPE_VIDEO. the root type is executed
	// before all document types whereas the others are only executed before
	// their corresponding type.

	DOCUMENT_FLAG_HIDDEN = (1<<2),
	// suggests hiding the document in lists (you may override?). used with
	// DOCUMENT_FLAG_SYSTEM for stdlib files?

};

struct document {
	//int flags; // XXX flags? for what?
	int id;
	//int version;
	enum document_type type;
	DA(struct fat_char, fat_chars);
	// (update document_copy() when adding da-fields here)
};

struct doc_iterator {
	struct document* doc;
	int offset;
	struct fat_char* fat_char;
	struct location location;
	unsigned new_line :1;
	unsigned done :1;
	unsigned last :1;
};

static inline struct doc_iterator doc_iterator(struct document* doc)
{
	return ((struct doc_iterator) {
		.doc = doc,
		.new_line = 1,
		.offset = -1,
	});
}

static inline int doc_iterator_next(struct doc_iterator* it)
{
	assert((!it->done) && "you cannot call this function after it has returned 0");
	struct document* d = it->doc;
	const int num_chars = daLen(d->fat_chars);
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
		it->fat_char = daPtr(d->fat_chars, off);
		if (it->fat_char->codepoint == '\n') {
			it->new_line = 1;
		}
	} else {
		assert(off == num_chars);
		it->fat_char = NULL;
		it->last = 1;
	}

	return 1;
}

static inline void doc_iterator_locate(struct doc_iterator* it, struct location loc)
{
	while (doc_iterator_next(it)) {
		struct location dloc = it->location;
		if ((loc.line < dloc.line) || (loc.line == dloc.line && loc.column <= dloc.column)) {
			break;
		}
	}
}

void gig_init(void);
void gig_spool(void);
void gig_thread_tick(void);
void gig_selftest(void);

void mim_set_latency(double mu, double sigma);

void begin_mim(int personal_mim_state_id);
void end_mim(void);

FORMATPRINTF1
void mimf(const char* fmt, ...);
void mim8(uint8_t v);

void get_state_and_doc(int personal_id, struct mim_state** out_mim_state, struct document** out_doc);

int get_my_artist_id(void);

#define GIG_H
#endif
