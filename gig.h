#ifndef GIG_H

#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "util.h"

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

static inline int location_line_distance(struct location* a, struct location* b)
{
	return abs(a->line - b->line);
}

static inline void location_sort2(struct location** a, struct location** b)
{
	if (location_compare(*a,*b) > 0) {
		struct location* tmp = *a;
		*a = *b;
		*b = tmp;
	}
}

struct thicchar {
	int32_t codepoint;
	uint8_t color[4];
};

struct fat_char {
	struct thicchar thicchar;
	unsigned timestamp; // last change (or created)
	//unsigned artist_id; 
	// why do we want `artist_id`? I don't think "commit only my own stuff" is
	// a thing, and the journal tells the story if you really want to know?
	unsigned is_insert :1;
	unsigned is_delete :1;
	unsigned is_defer  :1;
	// TODO these should be cleared after being "acknowledged"?
	unsigned flipped_insert :1;
	unsigned flipped_delete :1;
	unsigned flipped_defer  :1;
	// internal use:
	unsigned _fill  :1;
};

struct caret {
	int tag;
	//unsigned flags; // visible? visibility-groups?
	//struct location loc0, loc1;
	// loc0 & loc1 are "unordered"; loc1 can be before loc0 (this makes
	// shift+arrows easier because loc0 can be fixed and loc1 can be moving
	// left/right of loc0). XXX this is actually convention now, because I also
	// always use loc1 to render caret; can I find better names? caret/anchor?
	struct location caret_loc, anchor_loc;
};

struct mim_state {
	int artist_id;
	int personal_id;
	// [artist_id,personal_id] is the "unique key"
	int document_id;
	struct caret* caret_arr;
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
	struct fat_char* fat_char_arr;
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

int doc_iterator_next(struct doc_iterator* it);
static inline void doc_iterator_locate(struct doc_iterator* it, struct location* loc)
{
	while (doc_iterator_next(it)) {
		struct location dloc = it->location;
		if ((loc->line < dloc.line) || (loc->line == dloc.line && loc->column <= dloc.column)) {
			break;
		}
	}
}

void gig_init(void);
void gig_spool(void);
void gig_thread_tick(void);
void gig_selftest(void);

void mim_set_latency(double mu, double sigma);

// TODO I think I may want to support some kind of "dry run", or "exploratory
// fork", or something; specifically I'm considering having "commit intention
// markers" implemented using mim state carets (having another tag than your
// standard carets?).
//  - they reveal your near-future intents to your fellow artists
//  - allows you to commit multiple disjoint diffs at once
//  - it can check/test/run your mi code and give you feedback for every
//    tiny change instead of waiting for ctrl+enter
// you may say that you can "just" drag a selection around what you want to
// commit and ctrl+enter, and it may usually be fine, but:
//  - your intent is more ambiguous and only revealed few moments before a
//    commit occurs
//  - if you have "foo..bar..baz" then you can't drag a selection that touches
//    only "foo" and "baz" but not "bar". with intent markers you just place
//    markers inside "foo" and "baz"
//  - the program can't tell /why/ you're selecting text (it's a multi-purpose
//    tool!) so it can't really test your code in the same way that intent
//    markers can. so if your code has detectable errors, you may end up making
//    the same selection multiple times until you get it right (also you can't
//    type text, and you lose your caret positions, when you make selections;
//    intent markers have their own "tag space" so they don't cause these
//    problems)

void begin_mim(int personal_mim_state_id);
void end_mim(void);

FORMATPRINTF1
void mimf(const char* fmt, ...);
void mim8(uint8_t v);

void get_state_and_doc(int personal_id, struct mim_state** out_mim_state, struct document** out_doc);

int get_my_artist_id(void);

#define GIG_H
#endif
