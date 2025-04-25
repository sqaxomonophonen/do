#ifndef GIG_H

#include <stdint.h>

#include "util.h"

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

enum vam_mode {
	VAM_COMMAND = 1,
	VAM_VISUAL,
	VAM_VISUAL_LINE,
	//VAM_VISUAL_BLOCK?
	VAM_INSERT,
	VAM_EX_COMMAND, // ":<query>"
	VAM_SEARCH_FORWARD,
	VAM_SEARCH_BACKWARD,
};

// "cool state" contains state that the artist has 100% control over (so no
// need to wait for server response to "predict" these; useful for knowing
// "we're already in insert mode" and so on)
struct vam_state_cool {
	int document_id;
	// if `document_id` is set to an id that does not exist
	enum vam_mode mode;
	char* query_arr;
	// `query_arr` contains the current "unfinished" query as a string, depending
	// on mode:
	//  - VAM_COMMAND: if you type "dw" or "10dw" then just before the last "w"
	//    query contains "d" and "10d" respectively
	//  - VAM_EX / VAM_SEARCH_FORWARD / VAM_SEARCH_BACKWARD: the query as
	//    you type it
	// Q: for VAM_INSERT: should the string be empty? or maybe contain the
	// command that initiated it? e.g. "10i" will insert a thing 10 times after
	// you escape? that state isn't seen elsewhere... or maybe it should just
	// contain "10"?
	int* caret_id_arr;
};

struct caret {
	int id;
	struct range range;
};

// "hot state" is whatever the server says it is; potentially
// chaotic/unpredictable in multi-artist venues
struct vam_state_hot {
	int document_id;
	struct caret* caret_arr;
	// TODO yank buffer contents?
};

struct vam_state {
	int vam_state_id;
	int artist_id;
	struct vam_state_cool cool;
	struct vam_state_hot hot;
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
	struct fat_char* fat_char_arr;
	struct vam_state* vam_state_arr;
	// beware document_copy() when adding new arrays
};

void gig_init(void);
void gig_spool(void);
void gig_thread_tick(void);
void gig_selftest(void);

void vam_set_latency(double mu, double sigma);

FORMATPRINTF2
void vamf(int vam_state_id, const char* fmt, ...);

int get_num_vam_states(void);
struct vam_state* get_vam_state_by_index(int index);
struct vam_state* get_vam_state_by_id(int id);
//struct vam_state_cool* get_own_cool_vam_state_by_index(int index);
struct vam_state_cool* get_own_cool_vam_state_by_id(int id);


int get_num_documents(void);
struct document* get_document_by_index(int);
struct document* get_document_by_id(int);
struct document* find_document_by_id(int);

int get_my_artist_id(void);

#define GIG_H
#endif
