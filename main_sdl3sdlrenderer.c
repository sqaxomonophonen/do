#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "impl_sdl3.h"
#include "main.h"

static struct {
	SDL_Renderer* renderer;
} g;

int main(int argc, char** argv)
{
	if (!SDL_Init(SDL_INIT_VIDEO)) {
		fprintf(stderr, "SDL_Init() failed\n");
		exit(EXIT_FAILURE);
	}

	g0.window = SDL_CreateWindow("Do", 1280, 720, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
	if (g0.window == NULL) {
		fprintf(stderr, "SDL_CreateWindow() failed: %s\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}

	g.renderer = SDL_CreateRenderer(g0.window, NULL);
	if (g.renderer == NULL) {
		fprintf(stderr, "SDL_CreateRenderer() failed: %s\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}

	SDL_StartTextInput(g0.window);
	// see also:
	//   SDL_StartTextInputWithProperties
	//   SDL_SetTextInputArea
	//   SDL_StopTextInput

	gui_init();

	while (!g0.exiting) {
		handle_events();

		//SDL_SetRenderViewport(g.renderer, ...)
		//SDL_SetRenderClipRect(g.renderer, ...)
		SDL_SetRenderDrawColorFloat(g.renderer,.3,.1,0,1);
		SDL_RenderClear(g.renderer);

		gui_draw();

		SDL_RenderPresent(g.renderer);
	}

	SDL_DestroyWindow(g0.window);
	SDL_Quit();

	return EXIT_SUCCESS;
}
