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
	SDL_GLContext gl_context;
} g;

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

	g0.window = SDL_CreateWindow("Do", 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	if (g0.window == NULL) {
		fprintf(stderr, "SDL_CreateWindow() failed\n");
		exit(EXIT_FAILURE);
	}

	SDL_StartTextInput(g0.window);
	// see also:
	//   SDL_StartTextInputWithProperties
	//   SDL_SetTextInputArea
	//   SDL_StopTextInput

	g.gl_context = SDL_GL_CreateContext(g0.window);
	if (g.gl_context == NULL) {
		fprintf(stderr, "SDL_GL_CreateContext() failed\n");
		exit(EXIT_FAILURE);
	 }

	SDL_GL_MakeCurrent(g0.window, g.gl_context);

	printf("                 GL_VERSION: %s\n", glGetString(GL_VERSION));
	printf("GL_SHADING_LANGUAGE_VERSION: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
	printf("                  GL_VENDOR: %s\n", glGetString(GL_VENDOR));
	printf("                GL_RENDERER: %s\n", glGetString(GL_RENDERER));

	gl_init();
	gui_init();

	while (!g0.exiting) {
		handle_events();
		gl_frame(g0.true_screen_width, g0.true_screen_height);
		gui_draw();
		gl_render_gui_draw_lists();
		SDL_GL_SwapWindow(g0.window);
	}

	SDL_GL_DestroyContext(g.gl_context);
	SDL_DestroyWindow(g0.window);
	SDL_Quit();

	return EXIT_SUCCESS;
}
