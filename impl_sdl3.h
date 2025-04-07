#include "stb_ds.h"
#include <SDL3/SDL.h>
#include "gui.h"

// this is fairly new?
#ifndef SDLK_EXTENDED_MASK
#define SDLK_EXTENDED_MASK          (1u << 29)
#endif

int64_t get_nanoseconds(void)
{
	return SDL_GetTicksNS();
}

struct window {
	SDL_Window* sdl_window;
	int true_width;
	int true_height;
	int width;
	int height;
	float pixel_ratio;
	int fullscreen;
};

static struct {
	int exiting;
	struct window* window_arr;
} g0;

static void refresh_window_size(struct window* window)
{
	assert(window != NULL);
	const int prev_width = window->true_width;
	const int prev_height = window->true_height;
	SDL_GetWindowSizeInPixels(window->sdl_window, &window->true_width, &window->true_height);
	int w, h;
	SDL_GetWindowSize(window->sdl_window, &w, &h);
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
	for (int i=0; i<arrlen(g0.window_arr); ++i) {
		struct window* w = &g0.window_arr[i];
		if (id == SDL_GetWindowID(w->sdl_window)) {
			return w;
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
	default: return NULL;
	}
	assert(!"unreachable");
}

static void open_window(void);

static void handle_events()
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		struct window* window = get_event_window(&event);

		if (event.type == SDL_EVENT_QUIT) {
			g0.exiting = 1;
		} else if (event.type == SDL_EVENT_TEXT_INPUT) {
			printf("text input [%s]\n", event.text.text);
		} else if ((event.type == SDL_EVENT_KEY_DOWN) || (event.type == SDL_EVENT_KEY_UP)) {
			if (event.key.key == SDLK_ESCAPE) {
				// XXX temp?
				g0.exiting = 1;
			}

			const int is_down = event.key.down;

			if (window != NULL && is_down && event.key.key == 'f') {
				// XXX temp?
				window->fullscreen = !window->fullscreen;
				SDL_SetWindowFullscreen(window->sdl_window, window->fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
			}

			if (is_down && event.key.key == 'w') {
				// XXX temp?
				open_window();
			}

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
			}

			gui_emit_keypress_event((is_down ? KEY_IS_DOWN : 0) | keycode | mod);

		} else if (window != NULL && event.type == SDL_EVENT_WINDOW_RESIZED) {
			refresh_window_size(window);
		}
	}
}

static void add_window(SDL_Window* sdl_window)
{
	arrput(g0.window_arr, ((struct window){
		.sdl_window = sdl_window,
	}));
	refresh_window_size(&g0.window_arr[arrlen(g0.window_arr)-1]);
}

static void close_all_windows(void)
{
	for (int i=0; i<arrlen(g0.window_arr); ++i) {
		struct window* w = &g0.window_arr[i];
		SDL_DestroyWindow(w->sdl_window);
	}
}
