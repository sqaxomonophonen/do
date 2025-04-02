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

static struct {
	SDL_Window* window;
	int true_screen_width;
	int true_screen_height;
	int screen_width;
	int screen_height;
	float pixel_ratio;
	int exiting;
} g0;

static void populate_screen_globals()
{
	const int prev_width = g0.true_screen_width;
	const int prev_height = g0.true_screen_height;
	SDL_GetWindowSizeInPixels(g0.window, &g0.true_screen_width, &g0.true_screen_height);
	int w, h;
	SDL_GetWindowSize(g0.window, &w, &h);
	g0.pixel_ratio = (float)g0.true_screen_width / (float)w;
	g0.screen_width = g0.true_screen_width / g0.pixel_ratio;
	g0.screen_height = g0.true_screen_height / g0.pixel_ratio;
	if (g0.true_screen_width == prev_width && g0.true_screen_height == prev_height) {
		return;
	}
	// TODO?
}

static void handle_events()
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
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

		} else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
			populate_screen_globals();
		}
	}
}
