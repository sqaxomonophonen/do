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

struct colorchar {
	int32_t codepoint;
	uint16_t splash4; // [0000:9999]
};

static inline uint16_t splash4_from_i32(int32_t v)
{
	if (v<0) return 0;
	if (v>9999) return 9999;
	return v;
}

static inline int is_valid_splash4(int v)
{
	return ((0 <= v) && (v <= 9999));
}


// these flags are persistent (written in snapshotcache):
#define FC_IS_INSERT (1LL<<0)
#define FC_IS_DELETE (1LL<<1)
#define FC_IS_DEFER  (1LL<<2)
#define FC_PERSISTENT_MASK ((1LL<<24)-1) // NOTE must mask out all FC__* flags (see below)
// these flags are transient/ephemeral, not persisted; keep them in high bits
// so persistent flags can be stored in fewer bytes (due to leb128):
#define FC__FLIPPED_INSERT (1LL<<24)
#define FC__FLIPPED_DELETE (1LL<<25)
#define FC__FLIPPED_DEFER  (1LL<<26)
#define FC__FILL           (1LL<<31)

struct docchar {
	struct colorchar colorchar;
	unsigned timestamp; // last change (or created)
	uint32_t flags; // FC_*
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
	int artist_id, session_id;
	// mim state is keyed by [artist_id,session_id]
	int book_id, doc_id;
	uint16_t splash4;
	uint64_t snapshotcache_offset;
	struct caret* caret_arr;
	// (update mim_state_copy() when adding arr-fields here)
};

#if 0
enum document_type {
	DOCUMENT_TYPE_ROOT = 1,
	DOCUMENT_TYPE_AUDIO,
	DOCUMENT_TYPE_VIDEO,
	//DOCUMENT_TYPE_SCRATCHPAD // XXX do I want/need this?
	DOCUMENT_TYPE_PRESENTATION_SHADER,
	// XXX re:DOCUMENT_TYPE_PRESENTATION_SHADER: code that translates "docchars"
	// into "render_chars". but do I need a type per "presentation backend"?
	// would a "braille backend" need another doc type?
};
#endif

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

#define LIST_OF_FUNDAMENTS \
	X( MIE_URLYD , "mie-urlyd" )

enum fundament {
	_NO_FUNDAMENT_ = 0,
	#define X(ENUM,_STR) ENUM,
	LIST_OF_FUNDAMENTS
	#undef X
};

struct book {
	int book_id;
	enum fundament fundament;
	uint64_t snapshotcache_offset;
};

struct document {
	int book_id, doc_id;
	uint64_t snapshotcache_offset;
	//enum document_type type;
	char* name_arr;
	struct docchar* docchar_arr;
	// (update document_copy() when adding arr-fields here)
};

struct doc_iterator {
	struct document* doc;
	int offset;
	struct docchar* docchar;
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

void gig_host(const char* dir);
void gig_unhost(void);
void gig_maybe_setup_stub(void);
//void gig_testsetup(void);
void gig_init(void);
void gig_set_journal_snapshot_growth_threshold(int);
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

void begin_mim(int session_mim_state_id);
void end_mim(void);

FORMATPRINTF1
void mimf(const char* fmt, ...);
void mim8(uint8_t v);
void mimex(const char*);
void mimi(int tag, const char*);

void get_state_and_doc(int session_id, struct mim_state** out_mim_state, struct document** out_doc);

int get_my_artist_id(void);

#define GIG_H
#endif
