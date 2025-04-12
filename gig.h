#ifndef GIG_H

struct fat_char {
	unsigned codepoint;
	unsigned timestamp;
	unsigned author;
	unsigned flags;
	unsigned color[4];
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
};

void gig_init(void);

int get_num_documents(void);
struct document* get_document_by_index(int);
struct document* get_document_by_id(int);
struct document* find_document_by_id(int);
int new_document(enum document_type type);

#define GIG_H
#endif
