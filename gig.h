#ifndef GIG_H

#include <stdint.h>

#include "util.h"
#include "da.h"

struct location {
	int line;
	int column;
};

#if 0
struct range {
	struct location from, to;
};
#endif

struct fat_char {
	unsigned codepoint;
	unsigned timestamp;
	unsigned artist_id;
	unsigned color[3];
	unsigned insert :1;
	unsigned delete :1;
	unsigned defer  :1;
};

struct anchor {
	int id;
	struct location loc;
};

struct caret {
	int anchor_id;
};

struct selection {
	int anchor0_id;
	int anchor1_id;
};

struct mim_state {
	int artist_id;
	int personal_id;
	// [artist_id,personal_id] is the "unique key"
	int document_id;
	DA(struct anchor, anchors);
	DA(struct caret, carets);
	DA(struct selection, selections);
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
	int flags;
	int id;
	//int version;
	enum document_type type;
	DA(struct fat_char, fat_chars);
	// beware document_copy() when adding new arrays
};

void gig_init(void);
void gig_spool(void);
void gig_thread_tick(void);
void gig_selftest(void);

void mim_set_latency(double mu, double sigma);

FORMATPRINTF2
void mimf(int personal_mim_state_id, const char* fmt, ...);

int get_num_mim_states(void);
struct mim_state* get_cool_mim_state_by_personal_id(int id);
struct mim_state* get_hot_mim_state_by_personal_id(int id);
struct mim_state* get_mim_state_by_index(int index);

int get_num_documents(void);
struct document* get_document_by_index(int);
struct document* get_document_by_id(int);
struct document* find_document_by_id(int);

int get_my_artist_id(void);

#define GIG_H
#endif
