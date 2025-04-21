#ifndef GIG_H

#include <stdint.h>

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

struct caret {
	int id;
	int artist_id;
	struct range range;
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
	int version;
	enum document_type type;
	struct fat_char* fat_char_arr;
	struct caret* caret_arr;
	// beware document_copy() when adding new arrays
};

void gig_init(void);
void gig_spool(void);
void gig_thread_tick(void);
void gig_selftest(void);

enum command_type {
	COMMAND_SET_CARET=1,
	COMMAND_DELETE_CARET,

	COMMAND_INSERT,
	COMMAND_DELETE,
	COMMAND_COMMIT,
	COMMAND_CANCEL,
	COMMAND_DEFER,
	COMMAND_UNDEFER,

	COMMAND_SET_COLOR,

	COMMAND_CREATE_DOCUMENT,
	COMMAND_DELETE_DOCUMENT,

	// TODO?
	//  admin ops? ban?
};

struct command {
	enum command_type type;
	//int document_id;
	//int document_serial;
	union {
		struct {
			int id;
			struct range range;
		} set_caret; // for COMMAND_SET_CARET
		struct {
			int id;
		} delete_caret; // for COMMAND_DELETE_CARET
		struct {
			struct location at;
			const char* text;
		} insert; // for COMMAND_INSERT
		struct {
			struct range range;
		} delete; // for COMMAND_DELETE
		struct {
			int artist_id;
			struct range range;
		} commit; // for COMMAND_COMMIT
		struct {
			int artist_id;
			struct range range;
		} cancel; // for COMMAND_CANCEL
		struct {
			int artist_id;
			struct range range;
		} defer; // for COMMAND_DEFER
		struct {
			int artist_id;
			struct range range;
		} undefer; // for COMMAND_UNDEFER
		struct {
			int red, green, blue;
		} set_color; // for COMMAND_SET_COLOR
		struct {
			int id;
			int flags;
			enum document_type type;
		} create_document; // for COMMAND_CREATE_DOCUMENT
		struct {
			int document_id;
		} delete_document; // for COMMAND_DELETE_DOCUMENT
	};
};

void ed_begin(int document_id, int64_t add_latency_ns);
void ed_do(struct command*);
void ed_end(void);

int get_num_documents(void);
struct document* get_document_by_index(int);
struct document* get_document_by_id(int);
struct document* find_document_by_id(int);

int get_my_artist_id(void);

#define GIG_H
#endif
