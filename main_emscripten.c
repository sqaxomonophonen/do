#include <stdio.h>
#include <assert.h>

#include <emscripten.h>
#include <emscripten/webaudio.h>
#include <emscripten/wasm_worker.h>
#include <emscripten/em_math.h>
//#include <emscripten/atomic.h>
#include <stdatomic.h>

#define GL_GLEXT_PROTOTYPES
#define EGL_EGLEXT_PROTOTYPES
#include <GLES3/gl3.h>
#include <EGL/egl.h>

#include "stb_ds_sysalloc.h"
#include "main.h"
#include "arg.h"
#include "utf8.h"
#include "impl_gl.h"

static struct {
	int num_cores;
	double start_time;
	char* text_buffer_arr;
	int*  key_buffer_arr;
} g;

EM_JS(int, canvas_get_width, (void), {
	const e = document.getElementById("canvas");
	const v = e.width = e.offsetWidth;
	return v;
})

EM_JS(int, canvas_get_height, (void), {
	const e = document.getElementById("canvas");
	const v = e.height = e.offsetHeight;
	return v;
})

EM_JS(void, set_canvas_cursor, (const char* cursor), {
	document.getElementById("canvas").setAttribute("style", "cursor:" + UTF8ToString(cursor) + ";");
})

static bool handle_key_event(int type, const EmscriptenKeyboardEvent* ev, void* usr)
{
	// NOTE: Only key events are handled here; text input is handled elsewhere
	// (see handle_text_input()). Text input is when you enter "hællø wørld",
	// and key events are when you press (or release) [Alt]+[Æ].

	const int is_down = (type == EMSCRIPTEN_EVENT_KEYDOWN);

	int mod = 0;
	if (ev->shiftKey) mod |= MOD_SHIFT;
	if (ev->altKey)   mod |= MOD_ALT;
	if (ev->ctrlKey)  mod |= MOD_CONTROL;
	if (ev->metaKey)  mod |= MOD_GUI;

	//printf("key [%s]\n", ev->key);

	const char* p = ev->key;
	int n = strlen(ev->key);
	int keycode = utf8_decode(&p, &n);
	if (keycode > 0 && n == 0) {
		// XXX ev->key contains exactly one character. This is assumed to mean
		// that it's not one of the special keys (which have longer names like
		// "Escape" and "Tab"), but instead a key that represents the decoded
		// codepoint. However, in what looks like a half-assed attempt at
		// supporting text input through "keydown" events, when ev->key
		// contains a letter, it's uppercased when [Shift] is pressed, and
		// lowercased when it's not. This is not helpful when dealing with key
		// presses and releases: the [A]-key is the [A]-key, not "the [a]-key
		// unless [Shift]'d". So we attempt to uppercase the codepoint here
		// (requires "just" a 11kB look-up table, "no biggie" :-/) to make it
		// uniform. If the input cannot be uppercased it's returned as-is, so
		// we don't have to detect whether it's a bicameral letter or not.
		// True text input is handled elsewhere; see note at top of this
		// function.
		keycode = utf8_convert_lowercase_codepoint_to_uppercase(keycode);
	} else {
		keycode = -1;
		#define X(THEIRS,OURS) if (keycode == -1 && 0 == strcmp(ev->key, #THEIRS)) keycode = KEY_##OURS;
		X( Escape      , ESCAPE       )
		X( Backspace   , BACKSPACE    )
		X( Tab         , TAB          )
		X( Enter       , ENTER        )
		X( Home        , HOME         )
		X( End         , END          )
		X( Insert      , INSERT       )
		X( Delete      , DELETE       )
		X( PageUp      , PAGE_UP      )
		X( PageDown    , PAGE_DOWN    )
		X( ArrowUp     , ARROW_UP     )
		X( ArrowDown   , ARROW_DOWN   )
		X( ArrowLeft   , ARROW_LEFT   )
		X( ArrowRight  , ARROW_RIGHT  )
		X( PrintScreen , PRINT_SCREEN )
		X( F1          , F1           )
		X( F2          , F2           )
		X( F3          , F3           )
		X( F4          , F4           )
		X( F5          , F5           )
		X( F6          , F6           )
		X( F7          , F7           )
		X( F8          , F8           )
		X( F9          , F9           )
		X( F10         , F10          )
		X( F11         , F11          )
		X( F12         , F12          )
		X( F13         , F13          )
		X( F14         , F14          )
		X( F15         , F15          )
		X( F16         , F16          )
		X( F17         , F17          )
		X( F18         , F18          )
		X( F19         , F19          )
		X( F20         , F20          )
		X( F21         , F21          )
		X( F22         , F22          )
		X( F23         , F23          )
		X( F24         , F24          )
		X( Control     , CONTROL      )
		X( Alt         , ALT          )
		X( Shift       , SHIFT        )
		X( Meta        , GUI          )
		#undef X
	}

	if (keycode > 0) {
		const int key = ((is_down ? KEY_IS_DOWN : 0) | keycode | mod);
		arrput(g.key_buffer_arr, key);
	}

	return (mod & MOD_CONTROL) ? true : false;
}

// (NOTE _heap_malloc in Makefile.emscripten)
void* heap_malloc(size_t s)
{
	return malloc(s);
}

// (NOTE _handle_text_input in Makefile.emscripten)
void handle_text_input(const char* s)
{
	const size_t n = strlen(s);
	char* p = arraddnptr(g.text_buffer_arr, n);
	memcpy(p, s, n);
}

// (NOTE _handle_file_drop in Makefile.emscripten)
void handle_file_drop(const char* name, size_t num_bytes, uint8_t* bytes)
{
	gui_drop_file(name, num_bytes, bytes);
	free(bytes);
}

// (NOTE _set_drag_state in Makefile.emscripten)
void set_drag_state(int is_dragging)
{
	gui_set_dragging(get_window(0), is_dragging);
}

static void main_loop(void)
{
	gui_begin_frame();

	if (arrlen(g.text_buffer_arr) > 0) {
		arrput(g.text_buffer_arr, 0);
		gui_on_text(g.text_buffer_arr);
		arrreset(g.text_buffer_arr);
	}

	const int num_keys = arrlen(g.key_buffer_arr);
	for (int i=0; i<num_keys; ++i) gui_on_key(arrchkget(g.key_buffer_arr, i));
	arrreset(g.key_buffer_arr);

	const int canvas_width = canvas_get_width();
	const int canvas_height = canvas_get_height();
	gl_frame(canvas_width, canvas_height);
	struct window* w = get_window(0);
	w->true_width = canvas_width;
	w->true_height = canvas_height;
	w->width = w->true_width;
	w->height = w->true_height;
	w->pixel_ratio = 1;
	gui_draw(w);
	gl_render_gui_draw_lists();
}

int64_t get_nanoseconds_epoch(void)
{
	return emscripten_get_now()*1e6;
}

int64_t get_nanoseconds(void)
{
	return (emscripten_get_now()-g.start_time)*1e6;
}

void sleep_nanoseconds(int64_t ns)
{
	assert(!"don't sleep");
}

#if 0
static void gig_thread_run(void)
{
	for (;;) {
		gig_thread_tick();
		emscripten_wasm_worker_sleep(2000000L);
	}
}
#endif

int main(int argc, char** argv)
{
	//g.num_cores = emscripten_navigator_hardware_concurrency();
	g.start_time = emscripten_get_now();

	open_window();
	get_window(0)->state = WINDOW_IS_OPEN;

	const char* window = EMSCRIPTEN_EVENT_TARGET_WINDOW;
	emscripten_set_keydown_callback(window, NULL, false, handle_key_event);
	emscripten_set_keyup_callback  (window, NULL, false, handle_key_event);

	#if 1
	printf("g.num_cores=%d\n", g.num_cores);
	#endif

	gl_init();
	run_selftest();
	jio_init();
	mie_thread_init();
	gig_init();
	//gig_host(arg_dir ? arg_dir : "."); // XXX?!
	gig_host_no_jio();
	gig_maybe_setup_stub();
	gui_init();
	//emscripten_wasm_worker_post_function_v(emscripten_malloc_wasm_worker(1L<<20), gig_thread_run);
	emscripten_set_main_loop(main_loop, 0, false);

	return EXIT_SUCCESS;
}
