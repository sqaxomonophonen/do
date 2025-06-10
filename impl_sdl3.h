#include <SDL3/SDL.h>
#include "gui.h"
#include "utf8.h"
#include "util.h"

// this is fairly new?
#ifndef SDLK_EXTENDED_MASK
#define SDLK_EXTENDED_MASK          (1u << 29)
#endif

static struct {
	int exiting;
} g0;

THREAD_LOCAL static struct {
	int64_t last_ticks_ns;
} tlg0;

int64_t get_nanoseconds_monotonic(void)
{
	const int64_t ticks_ns = SDL_GetTicksNS();
	// guarantee "monotonicness"
	if (ticks_ns > tlg0.last_ticks_ns) {
		tlg0.last_ticks_ns = ticks_ns;
	}
	return tlg0.last_ticks_ns;
}

int64_t get_microseconds_epoch(void)
{
	SDL_Time ticks;
	assert(SDL_GetCurrentTime(&ticks));
	return ticks / 1000LL;
}

void sleep_microseconds(int64_t us)
{
	SDL_DelayNS(us*1000LL);
}

struct window_extra {
	SDL_Window* sdl_window;
};

static inline SDL_Window* get_sdl_window(struct window* window)
{
	assert(window->backend_extra != NULL);
	return ((struct window_extra*)window->backend_extra)->sdl_window;
}

static void refresh_window_size(struct window* window)
{
	assert(window != NULL);

	const int prev_width = window->true_width;
	const int prev_height = window->true_height;
	SDL_Window* sdl_window = get_sdl_window(window);
	SDL_GetWindowSizeInPixels(sdl_window, &window->true_width, &window->true_height);
	int w, h;
	SDL_GetWindowSize(sdl_window, &w, &h);
	window->pixel_ratio = (float)window->true_width / (float)w;
	window->width = window->true_width / window->pixel_ratio;
	window->height = window->true_height / window->pixel_ratio;
	if (window->true_width == prev_width && window->true_height == prev_height) {
		return;
	}
	// TODO?
}

static struct window* get_window_by_sdl_id(SDL_WindowID id)
{
	const int n = get_num_windows();
	for (int i=0; i<n; ++i) {
		struct window* window = get_window(i);
		if (id == SDL_GetWindowID(get_sdl_window(window))) {
			return window;
		}
	}
	return NULL;
}

static struct window* get_event_window(SDL_Event* ev)
{
	switch (ev->type) {

	case SDL_EVENT_KEY_DOWN:
	case SDL_EVENT_KEY_UP:
		return get_window_by_sdl_id(ev->key.windowID);

	case SDL_EVENT_WINDOW_RESIZED:
		return get_window_by_sdl_id(ev->window.windowID);

	case SDL_EVENT_DROP_FILE:
	case SDL_EVENT_DROP_TEXT:
	case SDL_EVENT_DROP_BEGIN:
	case SDL_EVENT_DROP_COMPLETE:
	case SDL_EVENT_DROP_POSITION:
		return get_window_by_sdl_id(ev->drop.windowID);

	default: return NULL;
	}
	assert(!"unreachable");
}

static void drop_path(const char* path)
{
	size_t num_bytes;
	void* bytes = SDL_LoadFile(path, &num_bytes);
	if (bytes != NULL) {
		gui_drop_file(path, num_bytes, bytes);
		SDL_free(bytes);
	} else {
		fprintf(stderr, "%s: could not read\n", path);
	}
}

static void handle_events()
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		struct window* window = get_event_window(&event);

		if (event.type == SDL_EVENT_QUIT) {
			g0.exiting = 1;
		} else if (event.type == SDL_EVENT_TEXT_INPUT) {
			gui_on_text(event.text.text);
		} else if ((event.type == SDL_EVENT_KEY_DOWN) || (event.type == SDL_EVENT_KEY_UP)) {
			const int is_down = event.key.down;

			if (window != NULL && is_down && event.key.key == SDLK_ESCAPE) {
				// XXX temp?
				window->state = WINDOW_IS_CLOSING;
			}

			#if 0
			if (window != NULL && is_down && event.key.key == 'f') {
				// XXX temp?
				window->is_fullscreen = !window->is_fullscreen;
				SDL_SetWindowFullscreen(get_sdl_window(window), window->is_fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
			}

			if (is_down && event.key.key == 'w') {
				// XXX temp?
				open_window();
			}
			#endif

			int mod = 0;
			if (event.key.mod & SDL_KMOD_SHIFT) mod |= MOD_SHIFT;
			if (event.key.mod & SDL_KMOD_ALT)   mod |= MOD_ALT;
			if (event.key.mod & SDL_KMOD_CTRL)  mod |= MOD_CONTROL;
			if (event.key.mod & SDL_KMOD_GUI)   mod |= MOD_GUI;

			int keycode = -1;
			switch (event.key.key) {

			#define X(NAME) case SDLK_##NAME: keycode = KEY_##NAME; break;
			X( ESCAPE    )
			X( BACKSPACE )
			X( TAB       )
			X( HOME      )
			X( END       )
			X( INSERT    )
			X( DELETE    )
			X( F1        )
			X( F2        )
			X( F3        )
			X( F4        )
			X( F5        )
			X( F6        )
			X( F7        )
			X( F8        )
			X( F9        )
			X( F10       )
			X( F11       )
			X( F12       )
			X( F13       )
			X( F14       )
			X( F15       )
			X( F16       )
			X( F17       )
			X( F18       )
			X( F19       )
			X( F20       )
			X( F21       )
			X( F22       )
			X( F23       )
			X( F24       )
			#undef X

			#define X(THEIR_NAME,OUR_NAME) case SDLK_##THEIR_NAME: keycode = KEY_##OUR_NAME; break;
			X( RETURN       , ENTER        )
			X( PAGEUP       , PAGE_UP      )
			X( PAGEDOWN     , PAGE_DOWN    )
			X( UP           , ARROW_UP     )
			X( DOWN         , ARROW_DOWN   )
			X( LEFT         , ARROW_LEFT   )
			X( RIGHT        , ARROW_RIGHT  )
			X( PRINTSCREEN  , PRINT_SCREEN )
			X( LSHIFT       , SHIFT        )
			X( RSHIFT       , SHIFT        )
			X( LALT         , ALT          )
			X( RALT         , ALT          )
			X( LCTRL        , CONTROL      )
			X( RCTRL        , CONTROL      )
			X( LGUI         , GUI          )
			X( RGUI         , GUI          )
			#undef X

			default: keycode = -1; break;

			}

			if (keycode == -1 && !(event.key.key & SDLK_EXTENDED_MASK)) {
				keycode = event.key.key;
				assert(keycode < SPECIAL_KEY_BEGIN);
				keycode = utf8_convert_lowercase_codepoint_to_uppercase(keycode);
			}

			gui_on_key((is_down ? KEY_IS_DOWN : 0) | keycode | mod);

		} else if (window != NULL && event.type == SDL_EVENT_WINDOW_RESIZED) {
			refresh_window_size(window);
		#if 0
		} else if (event.type == SDL_EVENT_DROP_POSITION) {
			gui_set_dragging(window, 1);
		#endif
		} else if (event.type == SDL_EVENT_DROP_BEGIN) {
			gui_set_dragging(window, 1);
		} else if (event.type == SDL_EVENT_DROP_COMPLETE) {
			gui_set_dragging(window, 0);
		} else if (event.type == SDL_EVENT_DROP_TEXT) {
			const char* s = event.drop.data;
			printf("text drop [%s]\n", s);
			const size_t n = strlen(s);
			const char* file_scheme = "file://";
			const size_t n2 = strlen(file_scheme);
			if (n>n2 && 0==memcmp(s,file_scheme,n2)) {
				// XXX this doesn't really work :) need proper %20=>" "
				// conversion and so on...
				const char* path = s + n2;
				drop_path(path);
			} else {
				printf("unhandled text drop [%s]\n", s);
			}
			gui_set_dragging(window, 0);
		} else if (event.type == SDL_EVENT_DROP_FILE) {
			// TODO a file was dragged into our window
			//  - files can be be samples... and maybe images? curves? code?
			//  - for "library resources", calculate file hash to see if we
			//    already have it or know about it? the hash is going to be
			//    used regardless (
			//  - (shouldn't it be possible to do this in web too? just a
			//    reminder to use common code where possible)
			printf("TODO file drag'n'drop! path to file: [%s]\n", event.drop.data);
			//if (event.drop.source != NULL) printf("source: [%s]!\n", event.drop.source);
			drop_path(event.drop.data);
			gui_set_dragging(window, 0);
		}
	}
}
