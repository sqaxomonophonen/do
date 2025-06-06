#include <stdio.h>
#include <assert.h>

#include <emscripten.h>
#include <emscripten/wget.h>
#include <emscripten/websocket.h>
//#include <emscripten/webaudio.h>
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
#include "gig.h"
#include "bb.h"
#include "protocol.h"
#include "bufstream.h"

static struct {
	//int num_cores;
	double start_time;
	char* text_buffer_arr;
	int*  key_buffer_arr;
	int running;
	int64_t journal_cursor;
	uint8_t* bb;
	EMSCRIPTEN_WEBSOCKET_T socket;
} g;

EM_JS(char*, get_websocket_url, (void), {
	let loc = window.location;
	let url = ((loc.protocol==="https:") ? "wss:" : "ws:");
	url += "//";
	url += loc.hostname;
	if (loc.port) url += (":"+loc.port);
	url += (loc.pathname + "/websocket");
	return stringToNewUTF8(url);
})

EM_JS(char*, get_info_url, (void), {
	let loc = window.location;
	let url = loc.protocol + "//" + loc.hostname;
	if (loc.port) url += (":"+loc.port);
	url += (loc.pathname + "/info");
	return stringToNewUTF8(url);
})

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
	if (!g.running) return;

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

static char *WS_URL, *INFO_URL;

static void request_journal(void)
{
	uint8_t** bb = &g.bb;
	arrreset(*bb);
	bb_append_u8(bb, WS0_HELLO);
	bb_append_leb128(bb, g.journal_cursor);
	emscripten_websocket_send_binary(g.socket, *bb, arrlen(*bb));
}

bool ws_on_open(int type, const EmscriptenWebSocketOpenEvent* e, void* usr)
{
	printf("WS OPEN!\n");
	request_journal();
	g.running = 1;
	return 0;
}

bool ws_on_close(int type, const EmscriptenWebSocketCloseEvent* e, void *usr)
{
	printf("WS CLOSE!\n");
	return 0;
}

bool ws_on_error(int type, const EmscriptenWebSocketErrorEvent* e, void *usr)
{
	printf("WS ERROR!\n");
	return 0;
}

enum {
	CLOSE_NORMAL         = 1000,
	CLOSE_PROTOCOL_ERROR = 1002,
};

bool ws_on_message(int type, const EmscriptenWebSocketMessageEvent* e, void *usr)
{
	printf("WS MESSAGE (n=%d)!\n", e->numBytes);
	struct bufstream bs;
	bufstream_init_from_memory(&bs, e->data, e->numBytes);
	while (bs.offset < e->numBytes) {
		const uint8_t op = bs_read_u8(&bs);
		switch (op) {

		case WS1_JOURNAL_UPDATE: {
			const int64_t count = bs_read_leb128(&bs);
			if (peer_spool_raw_journal_into_upstream_snapshot(e->data + bs.offset, count) < 0) {
				printf("spool error: received bad message from host?\n");
				emscripten_websocket_close(e->socket, CLOSE_PROTOCOL_ERROR, "bad journal");
				return 0;
			}
			bs_skip(&bs, count);
			g.journal_cursor += count;
		}	break;

		case WS1_HELLO: {
			const int64_t my_artist_id = bs_read_leb128(&bs);
			set_my_artist_id(my_artist_id);
		}	break;

		default: {
			printf("bad opcode (%d) received\n", op);
			emscripten_websocket_close(e->socket, CLOSE_PROTOCOL_ERROR, "bad opcode");
			return 0;
		}

		}
	}

	if (bs.offset != e->numBytes) {
		printf("bad alignment? offset=%lld numBytes=%d\n", bs.offset, e->numBytes);
		emscripten_websocket_close(e->socket, CLOSE_PROTOCOL_ERROR, "bad alignment");
		return 0;
	}

	return 0;
}

void info_on_load(void* usr, void* data, int size)
{
	g.journal_cursor = restore_upstream_snapshot_from_data(data, size);

	EmscriptenWebSocketCreateAttributes attr = {0};
	emscripten_websocket_init_create_attributes(&attr);
	attr.url = WS_URL;
	//attr.protocols = "binary,base64";
	g.socket = emscripten_websocket_new(&attr);

	emscripten_websocket_set_onopen_callback(g.socket, NULL,    ws_on_open);
	emscripten_websocket_set_onclose_callback(g.socket, NULL,   ws_on_close);
	emscripten_websocket_set_onerror_callback(g.socket, NULL,   ws_on_error);
	emscripten_websocket_set_onmessage_callback(g.socket, NULL, ws_on_message);
}

void info_on_error(void* usr)
{
	assert(!"failed to load info");
}

void transmit_mim(int mim_session_id, int64_t tracer, uint8_t* data, int count)
{
	uint8_t** bb = &g.bb;
	arrreset(*bb);
	bb_append_u8(bb, WS0_MIM);
	bb_append_leb128(bb, mim_session_id);
	bb_append_leb128(bb, tracer);
	bb_append(bb, data, count);
	emscripten_websocket_send_binary(g.socket, *bb, arrlen(*bb));
}

int main(int argc, char** argv)
{
	WS_URL = get_websocket_url();
	INFO_URL = get_info_url();
	printf("WS_URL=[%s] INFO_URL=[%s]\n", WS_URL, INFO_URL);
	//g.num_cores = emscripten_navigator_hardware_concurrency();
	g.start_time = emscripten_get_now();
	assert(emscripten_websocket_is_supported());

	gl_init();
	run_selftest();
	mie_thread_init();
	gig_init();
	gig_configure_as_peer_only("/data");
	gui_init();

	emscripten_async_wget_data(INFO_URL, NULL, info_on_load, info_on_error);

	open_window();
	get_window(0)->state = WINDOW_IS_OPEN;

	const char* window = EMSCRIPTEN_EVENT_TARGET_WINDOW;
	emscripten_set_keydown_callback(window, NULL, false, handle_key_event);
	emscripten_set_keyup_callback  (window, NULL, false, handle_key_event);

	//#if 1
	//printf("g.num_cores=%d\n", g.num_cores);
	//#endif

	//emscripten_wasm_worker_post_function_v(emscripten_malloc_wasm_worker(1L<<20), gig_thread_run);
	emscripten_set_main_loop(main_loop, 0, false);

	return EXIT_SUCCESS;
}
