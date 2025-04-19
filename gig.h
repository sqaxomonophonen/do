#ifndef GIG_H

#include <stdint.h>

struct fat_char {
	unsigned codepoint;
	unsigned timestamp;
	unsigned author;
	unsigned color[3];
	unsigned insert :1;
	unsigned delete :1;
	unsigned defer  :1;
};

enum document_type {
	DOC_AUDIO = 1,
	DOC_VIDEO,
	DOC_SCRATCHPAD,
	// XXX re:DOC_SCRATCHPAD: do I want/need this?
	DOC_PRESENTATION_SHADER,
	// XXX re:DOC_PRESENTATION_SHADER: code that translates "fat_chars" into
	// "render_chars". but do I need a type per "presentation backend"? would a
	// "braille backend" need another doc type?
};

struct document {
	int id;
	enum document_type type;
	struct fat_char* fat_char_arr;
	// TODO cursors? note: a single person can probably have multiple cursors,
	// and they can be ranges (selections)
};

void gig_init(void);
void gig_thread_tick(void);
void gig_spool(void);

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
};

struct location {
	int line;
	int column;
};

struct range {
	struct location from, to;
};

struct command {
	enum command_type type;
	//int document_id;
	//int document_serial;
	union {
		struct {
			int id;
			struct range range;
		} set_caret;
		struct {
			int id;
		} delete_caret;
		struct {
			struct location at;
			const char* text;
		} insert;
		struct {
			struct range range;
		} delete;
		struct {
			int author_id;
			struct range range;
		} commit;
		struct {
			int author_id;
			struct range range;
		} cancel;
		struct {
			int author_id;
			struct range range;
		} defer;
		struct {
			int author_id;
			struct range range;
		} undefer;
		struct {
			int red, green, blue;
		} set_color;
	};
};

void ed_begin(void);
void ed_do(struct command*);
int ed_commit(void);

int get_num_documents(void);
struct document* get_document_by_index(int);
struct document* get_document_by_id(int);
struct document* find_document_by_id(int);
int new_document(enum document_type type);

#define GIG_H
#endif
