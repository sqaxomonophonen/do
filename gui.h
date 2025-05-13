#ifndef GUI_H

#include <stdint.h>
#include <stddef.h>

enum window_state {
	WINDOW_IS_NEW=1,
	WINDOW_IS_OPEN,
	WINDOW_IS_CLOSING,
};

struct window {
	int id;
	int true_width;
	int true_height;
	int width;
	int height;
	float pixel_ratio;
	int is_fullscreen;
	enum window_state state;
	void* backend_extra;
};

int get_num_windows(void);
struct window* get_window(int index);
void open_window(void);
void remove_closed_windows(void);

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

#define MOD_MASK (MOD_SHIFT | MOD_CONTROL | MOD_ALT | MOD_GUI)
#define KEY_MASK ((1<<22)-1)

static inline int get_key_mod(int key) { return key & MOD_MASK; }
static inline int get_key_code(int key) { return key & KEY_MASK; }
static inline int get_key_down(int key) { return !!(key & KEY_IS_DOWN); }

void gui_init(void);
void gui_setup_gpu_resources(void);
void gui_on_key(int);
void gui_on_text(const char*);

void gui_begin_frame(void);
void gui_draw(struct window*);

void gui_set_dragging(struct window*, int is_dragging);
void gui_drop_file(const char* name, size_t num_bytes, uint8_t* bytes);

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

struct render_mode {
	enum blend_mode blend_mode;
	enum draw_list_type type;
	int do_scissor;
	int scissor_x;
	int scissor_y;
	int scissor_w;
	int scissor_h;
	int texture_id;
	// NOTE update render_mode_same() if fields are added
};

// TODO somewhere: if blend_mode==ADDITIVE, the ordering doesn't matter. so,
// for a given draw_list interval where blend_mode==ADDITIVE there ought to be
// only one of each `draw_list_type` to minimize the amount of mode switching
struct draw_list {
	struct render_mode render_mode;
	union {
		struct {
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
int gui_get_num_draw_lists(void);
struct draw_list* gui_get_draw_list(int index);


#define TT(s)       ((s)<<2)
#define TTMASK(s)   (TT((s)+1)-1)
#define TTGET(v,s)  (((v)>>TT(s))&TTMASK(s))

#define TT_LUMEN8    (0<<TT(0))
#define TT_RGBA8888  (1<<TT(0))

#define TT_SMOOTH    (0<<TT(1))
#define TT_PIXELATED (1<<TT(1))

#define TT_STATIC    (0<<TT(2))
#define TT_STREAM    (1<<TT(2))


// these APIs must be implemented by the host

int create_texture(int type, int width, int height);
void get_texture_dim(int texture, int* out_width, int* out_height);
void destroy_texture(int texture);
void update_texture(int texture, int y0, int width, int height, void* data);


#define GUI_H
#endif
