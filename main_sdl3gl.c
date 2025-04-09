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

static void open_window(void)
{
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

	add_window(sdl_window);
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

	SDL_GL_SetSwapInterval(1);
	// XXX doesn't work? how about that 20 year old broken record :-/
	// also notice that main_sdl3sdlrenderer.c has no tearing...

	open_window();

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

	while (!g0.exiting) {
		handle_events();
		for (int i=0; i<arrlen(g0.window_arr); ++i) {
			struct window* w = &g0.window_arr[i];
			SDL_GL_MakeCurrent(w->sdl_window, g.shared_gl_context);
			gl_frame(w->true_width, w->true_height);
			gui_draw(w->true_width, w->true_height);
			gl_render_gui_draw_lists();
			SDL_GL_SwapWindow(w->sdl_window);
		}
	}

	SDL_GL_DestroyContext(g.shared_gl_context);
	close_all_windows();
	SDL_Quit();

	return EXIT_SUCCESS;
}
