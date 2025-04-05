#ifndef GUI_H

typedef uint16_t vertex_index;

#define EMIT_SPECIAL_KEYS \
	X( ESCAPE       ) \
	X( BACKSPACE    ) \
	X( TAB          ) \
	X( ENTER        ) \
	X( HOME         ) \
	X( END          ) \
	X( INSERT       ) \
	X( DELETE       ) \
	X( PAGE_UP      ) \
	X( PAGE_DOWN    ) \
	X( ARROW_UP     ) \
	X( ARROW_DOWN   ) \
	X( ARROW_LEFT   ) \
	X( ARROW_RIGHT  ) \
	X( PRINT_SCREEN ) \
	X( F1           ) \
	X( F2           ) \
	X( F3           ) \
	X( F4           ) \
	X( F5           ) \
	X( F6           ) \
	X( F7           ) \
	X( F8           ) \
	X( F9           ) \
	X( F10          ) \
	X( F11          ) \
	X( F12          ) \
	X( F13          ) \
	X( F14          ) \
	X( F15          ) \
	X( F16          ) \
	X( F17          ) \
	X( F18          ) \
	X( F19          ) \
	X( F20          ) \
	X( F21          ) \
	X( F22          ) \
	X( F23          ) \
	X( F24          ) \
	X( CONTROL      ) \
	X( ALT          ) \
	X( SHIFT        ) \
	X( GUI          )

enum special_key {
	// keys representing printable characters are encoded with their unicode
	// codepoint. codepoints require at most 21 bits.
	SPECIAL_KEY_BEGIN = 1<<21,
	#define X(NAME) KEY_##NAME,
	EMIT_SPECIAL_KEYS
	#undef X
};

enum modifier_key_flag {
	MOD_SHIFT   = (1<<22),
	MOD_CONTROL = (1<<23),
	MOD_ALT     = (1<<24),
	MOD_GUI     = (1<<25),
};

enum key_state_flag {
	KEY_IS_DOWN = (1<<30),
};

#define KEY_MASK ((1<<22)-1)

void gui_init(void);
void gui_emit_keypress_event(int);
void gui_draw(void);

enum draw_list_type {
	MESH_TRIANGLES,
};

enum blend_mode {
	ADDITIVE,
	// ADDITIVE_HDR,
	// SUBTRACTIVE_HDR,
	// PREMULTIPLIED_ALPHA,
};

struct vertex {
	float x,y,u,v;
	uint32_t rgba;
};

// TODO somewhere: if blend_mode==ADDITIVE, the ordering doesn't matter. so,
// for a given draw_list interval where blend_mode==ADDITIVE there ought to be
// only one of each `draw_list_type` to minimize the amount of mode switching
struct draw_list {
	enum blend_mode      blend_mode;
	enum draw_list_type  type;
	union {
		struct {
			int texture_id;
			int num_vertices;
			union {
				struct vertex*  vertices;
				intptr_t       _vertices_offset; // internal use
			};
			int num_indices;
			union {
				vertex_index*   indices;
				intptr_t       _indices_offset; // internal use
			};
		} mesh;
		// TODO optimized quads or something?
	};
};
struct draw_list* gui_get_draw_list(int index);

#define GUI_H
#endif
