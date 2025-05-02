#ifndef GIG_H

#include <stdint.h>

#include "util.h"
#include "da.h"

struct location {
	int line;
	int column;
};

struct range {
	struct location from, to;
};

struct fat_char {
	unsigned codepoint;
	unsigned timestamp;
	unsigned artist_id;
	unsigned color[3];
	unsigned insert :1;
	unsigned delete :1;
	unsigned defer  :1;
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
