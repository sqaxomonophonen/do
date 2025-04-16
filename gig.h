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
	// TODO cursors? note: a single person can probably have multiple cursors,
	// and they can be ranges (selections)
};

void gig_init(void);
void gig_spool(void);

enum command_type {
	COMMAND_MOVE_CARET=1,
	COMMAND_DELETE,
	COMMAND_INSERT,
	COMMAND_SELECT,
	COMMAND_COMMIT,
	COMMAND_FLAG,
};

enum target_type {
	TARGET_NONE=0,
	TARGET_RELATIVE_COLUMN,
	TARGET_RELATIVE_LINE,
	TARGET_ABSOLUTE,
	TARGET_FIND_CHARACTER,
	TARGET_WORD,
	TARGET_FLAG_BOUNDARY,
	TARGET_EVERYTHING,
};

struct target {
	enum target_type type;
	union {
		struct {
			int delta;
		} relative_column;
		struct {
			int delta;
		} relative_line;
		struct {
			unsigned line:20;
			unsigned column:12;
		} absolute;
		struct {
			unsigned codepoint:22;
			unsigned forward:1;
			unsigned num:8;
		} find_character;
		struct {
			unsigned forward:1;
			unsigned num:8;
		} word;
		struct {
			// TODO?
			unsigned stop_at_insert_begin:1;
			unsigned stop_at_insert_end:1;
			unsigned stop_at_delete_begin:1;
			unsigned stop_at_delete_end:1;
			unsigned stop_at_mine:1;
			unsigned stop_at_theirs:1;
			unsigned stop_at_deferred_begin:1;
			unsigned stop_at_deferred_end:1;
			// XXX fun idea: stop at flashing? jump to the action?
			unsigned forward:1;
			unsigned num:8;
		} flag_boundary;
		struct {
		} flood_fill;
		struct {
		} section_fill;
		struct {
		} line;
	};
};

struct command {
	enum command_type type;
	union {
		struct {
			struct target target;
			unsigned add_caret           :1;
			unsigned clear_all_carets    :1;
			unsigned set_selection_begin :1;
			unsigned set_selection_end   :1;
		} move_caret;
		struct {
			const char* text;
		} insert;
		struct {
			unsigned set_deferred          :1;
			unsigned clear_deferred        :1;
			unsigned clear_delete          :1;
			unsigned clear_insert          :1;
			unsigned set_secondary_caret   :1;
			unsigned clear_secondary_caret :1;
		} flag;

		#if 0
		struct {
			struct target target;
		} delete;
		#endif
		#if 0
		struct {
			struct target from, to;
			unsigned add :1; // adds to existing selection
			unsigned cut :1; // removes from existing selection
			// "disjoint islands" are allowed
		} select;
		struct {
			unsigned deferred     :1; // also commit/fill deferred
			unsigned line         :1; // expand commit to entire line or lines
			unsigned flood_fill   :1; // flood fill diffs
			unsigned section_fill :1; // commit everything in section (delimited by empty lines)
			// XXX hvorfor er dette ikke targets?
		} commit;
		#endif
	};
};

void ed_command(struct command*);
void ed_flush(void);

int get_num_documents(void);
struct document* get_document_by_index(int);
struct document* get_document_by_id(int);
struct document* find_document_by_id(int);
int new_document(enum document_type type);

#define GIG_H
#endif
