#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <SDL3/SDL.h>
#include <SDL3/SDL_opengl.h>

#include "frontend_windowed.h"

static struct {
	SDL_Window* window;
	SDL_GLContext gl_context;
	int true_screen_width;
	int true_screen_height;
	int screen_width;
	int screen_height;
	float pixel_ratio;
} g;

static void populate_screen_globals()
{
	const int prev_width = g.true_screen_width;
	const int prev_height = g.true_screen_height;
	SDL_GetWindowSizeInPixels(g.window, &g.true_screen_width, &g.true_screen_height);
	int w, h;
	SDL_GetWindowSize(g.window, &w, &h);
	g.pixel_ratio = (float)g.true_screen_width / (float)w;
	g.screen_width = g.true_screen_width / g.pixel_ratio;
	g.screen_height = g.true_screen_height / g.pixel_ratio;
	if (g.true_screen_width == prev_width && g.true_screen_height == prev_height) {
		return;
	}
	// TODO?
}

int main(int argc, char** argv)
{
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "SDL_Init() failed\n");
		exit(EXIT_FAILURE);
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

	SDL_GL_SetSwapInterval(1);

	g.window = SDL_CreateWindow("do", 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	if (g.window == NULL) {
		fprintf(stderr, "SDL_CreateWindow() failed\n");
		exit(EXIT_FAILURE);
	}

	SDL_StartTextInput(g.window);
	// see also:
	//   SDL_StartTextInputWithProperties
	//   SDL_SetTextInputArea
	//   SDL_StopTextInput

	g.gl_context = SDL_GL_CreateContext(g.window);
	if (g.gl_context == NULL) {
		fprintf(stderr, "SDL_GL_CreateContext() failed\n");
		exit(EXIT_FAILURE);
	 }

	SDL_GL_MakeCurrent(g.window, g.gl_context);

	printf("                 GL_VERSION: %s\n", glGetString(GL_VERSION));
	printf("GL_SHADING_LANGUAGE_VERSION: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
	printf("                  GL_VENDOR: %s\n", glGetString(GL_VENDOR));
	printf("                GL_RENDERER: %s\n", glGetString(GL_RENDERER));

	frontend_init();

	int exiting = 0;
	while (!exiting) {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			if (event.type == SDL_EVENT_QUIT) {
				exiting = 1;
			} else if (event.type == SDL_EVENT_TEXT_INPUT) {
				printf("text input [%s]\n", event.text.text);
			} else if ((event.type == SDL_EVENT_KEY_DOWN) || (event.type == SDL_EVENT_KEY_UP)) {
				if (event.key.key == SDLK_ESCAPE) {
					// XXX temp?
					exiting = 1;
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

				frontend_emit_keypress_event((is_down ? KEY_IS_DOWN : 0) | keycode | mod);

			} else if (event.type == SDL_EVENT_WINDOW_RESIZED) {
				populate_screen_globals();
			}
		}

		glViewport(0, 0, g.true_screen_width, g.true_screen_height);
		glClearColor(.3,.1,0,1);
		glClear(GL_COLOR_BUFFER_BIT);
		SDL_GL_SwapWindow(g.window);
	}

	SDL_GL_DestroyContext(g.gl_context);
	SDL_DestroyWindow(g.window);
	SDL_Quit();

	return EXIT_SUCCESS;
}
