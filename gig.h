#ifndef GIG_H

#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "util.h"

struct location {
	int line;
	int column;
};

struct colorchar {
	int32_t codepoint;
	int16_t splash4; // [0000:9999]
};

static inline int location_compare(const struct location* a, const struct location* b)
{
	const int d0 = a->line - b->line;
	if (d0 != 0) return d0;
	const int d1 = a->column - b->column;
	return d1;
}

static inline int location_line_distance(const struct location* a, const struct location* b)
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
#define DC_IS_INSERT (1LL<<0)
#define DC_IS_DELETE (1LL<<1)
#define DC_IS_DEFER  (1LL<<2)
#define DC_PERSISTENT_MASK ((1LL<<24)-1) // NOTE must mask out all DC__* flags (see below)
// these flags are transient/ephemeral, not persisted; keep them in high bits
// so persistent flags can be stored in fewer bytes (due to LEB128):
#define DC__FLIPPED_INSERT (1LL<<24)
#define DC__FLIPPED_DELETE (1LL<<25)
#define DC__FLIPPED_DEFER  (1LL<<26)
#define DC__FILL           (1LL<<31)

struct docchar {
	struct colorchar colorchar;
	unsigned timestamp; // last change (or created)
	uint32_t flags; // DC_*
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
	// XXX the book_id/doc_id/splash4 fields are "artist-authoritative"; only
	// the artist themselves can set these (unlike e.g. carets which can move
	// due to document changes)
	int book_id, doc_id;
	int16_t splash4;
	struct caret* caret_arr;
	uint64_t snapshotcache_offset;
	// (update mim_state_copy() when adding arr-fields here)
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
	// (update snapshot_copy() when adding arr-fields here:)
	char* name_arr;
	struct docchar* docchar_arr;
};

struct snapshot {
	struct book*      book_arr;
	struct document*  document_arr;
	struct mim_state* mim_state_arr;
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

static inline int document_locate(struct document* doc, struct location* loc)
{
	struct doc_iterator it = doc_iterator(doc);
	doc_iterator_locate(&it, loc);
	return it.offset;
}

void gig_init(void);
void gig_set_journal_snapshot_growth_threshold(int);
int peer_tick(void);
int host_tick(void);

int gig_configure_as_host_and_peer(const char* rootdir);
// configure gig to be both host and peer (example: you're performing with the
// desktop build, but others can join on your IP address)

int gig_configure_as_host_only(const char* rootdir);
// configure gig to be host-only (example: headless server, no audio/mie)

int gig_configure_as_peer_only(const char* savedir);
// configure gig to be peer-only (example: you join somebody else's host,
// whether on LAN or over internet)

void gig_unconfigure(void);

void peer_set_artificial_mim_latency(double mean, double variance);
// latency to add to mim commands for simulating roundtrip latency. values are
// in seconds: mean is mu in the normal distribution, and variance is
// sigma-squared


//void gig_maybe_setup_stub(void);

// TODO I think I may want to support some kind of "dry run", or "exploratory
// fork", or something; specifically I'm considering having "commit intention
// markers" implemented using mim state carets (having another tag than your
// standard carets?).
//  - they reveal your near-future intents to your fellow artists
//  - allows you to commit multiple disjoint diffs at once
//  - it can check/test/run your mie code and give you feedback for every
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

void peer_begin_mim(int session_mim_state_id);
void peer_end_mim(void);

FORMATPRINTF1
void mimf(const char* fmt, ...);
void mim8(uint8_t v);
void mimex(const char*);
void mimi(int tag, const char*);
void mimc(int tag, const struct colorchar*, int count);

struct snapshot* get_snapshot(void);

int get_my_artist_id(void);
void set_my_artist_id(int);

int alloc_artist_id(void);
void free_artist_id(int);

int copy_journal(void* dst, int64_t count, int64_t offset);

void commit_mim_to_host(int artist_id, int session_id, int64_t tracer, uint8_t* data, int count);

int64_t restore_upstream_snapshot_from_data(void* data, size_t sz);

int peer_spool_raw_journal_into_upstream_snapshot(void* data, int64_t count);

void* get_present_snapshot_data(size_t* out_size);
// returns snapshot data. you're responsible for free()ing it when done

int64_t get_monotonic_jam_time_us(void);

void get_time_travel_range(int64_t* out_ts0, int64_t* out_ts1);
void render_activity(uint8_t* image1d, int image1d_width, int64_t ts0, int64_t ts1);
void suspend_time_at(int64_t ts);
void unsuspend_time(void);

#define GIG_H
#endif
