#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "impl_sdl3.h"
#define GL_GLEXT_PROTOTYPES
// XXX defining GL_GLEXT_PROTOTYPES works on unixes, but not on windows where
// SDL_GL_GetProcAddress() is needed.
#include <SDL3/SDL_opengl.h>

#include "impl_gl.h"

#include "main.h"

static struct {
	SDL_GLContext shared_gl_context;
} g;

static void housekeep_our_windows(void)
{
	const int num_windows = get_num_windows();
	for (int i=0; i<num_windows; ++i) {
		struct window* window = get_window(i);
		if (window->state == WINDOW_IS_NEW && window->backend_extra == NULL) {
			SDL_Window* sdl_window = SDL_CreateWindow("do", 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
			if (sdl_window == NULL) {
				fprintf(stderr, "SDL_CreateWindow() failed\n");
				exit(EXIT_FAILURE);
			}

			if (g.shared_gl_context == NULL) {
				g.shared_gl_context = SDL_GL_CreateContext(sdl_window);
				if (g.shared_gl_context == NULL) {
					fprintf(stderr, "SDL_GL_CreateContext() failed\n");
					exit(EXIT_FAILURE);
				 }
			}

			struct window_extra* extra = calloc(1, sizeof *extra);
			extra->sdl_window = sdl_window;
			window->backend_extra = extra;
			window->state = WINDOW_IS_OPEN;
		} else if (window->state == WINDOW_IS_CLOSING && window->backend_extra != NULL) {
			SDL_Window* sdl_window = get_sdl_window(window);
			SDL_DestroyWindow(sdl_window);
			window->backend_extra = NULL;
		}
	}
}

int main(int argc, char** argv)
{
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "SDL_Init() failed\n");
		exit(EXIT_FAILURE);
	}

	SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
	SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

	SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
	SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 0);

	open_window();
	housekeep_our_windows(); // <= actually opens the OS window

	SDL_GL_SetSwapInterval(1);
	// XXX doesn't work? how about that 20 year old broken record :-/
	// also notice that main_sdl3sdlrenderer.c has no tearing...


	#if 0
	// XXX do I want this here?
	SDL_StartTextInput(g0.window);
	// see also:
	//   SDL_StartTextInputWithProperties
	//   SDL_SetTextInputArea
	//   SDL_StopTextInput
	#endif

	printf("                 GL_VERSION: %s\n", glGetString(GL_VERSION));
	printf("GL_SHADING_LANGUAGE_VERSION: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
	printf("                  GL_VENDOR: %s\n", glGetString(GL_VENDOR));
	printf("                GL_RENDERER: %s\n", glGetString(GL_RENDERER));

	gl_init();
	gui_init();

	while (!g0.exiting && get_num_windows() > 0) {
		handle_events();
		housekeep_our_windows();
		remove_closed_windows();
		gui_begin_frame();
		// XXX subtlety: windows may be tail-appended by gui, and aren't valid
		// until housekeep_our_windows() has been called, so don't roll
		// get_num_windows() into the for-loop or we might begin
		const int num_windows = get_num_windows();
		for (int i=0; i<num_windows; ++i) {
			struct window* window = get_window(i);
			assert((window->state == WINDOW_IS_OPEN) && "we don't want new/closing windows at this point?");
			SDL_Window* sdl_window = get_sdl_window(window);
			SDL_GL_MakeCurrent(sdl_window, g.shared_gl_context);
			gl_frame(window->true_width, window->true_height);
			gui_draw(window);
			gl_render_gui_draw_lists();
			SDL_GL_SwapWindow(sdl_window);
		}
		remove_closed_windows();
	}

	SDL_GL_DestroyContext(g.shared_gl_context);

	for (int i=0; i<get_num_windows(); ++i) get_window(i)->state = WINDOW_IS_CLOSING;
	housekeep_our_windows();

	SDL_Quit();

	return EXIT_SUCCESS;
}
