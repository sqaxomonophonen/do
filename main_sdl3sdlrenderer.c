#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "impl_sdl3.h"
#include "main.h"

struct texture {
	int type;
	int width;
	int height;
	SDL_Texture* texture;
};

static struct {
	SDL_Renderer* renderer;
	struct texture* texture_arr;
	int* texture_freelist_arr;
	uint8_t* scratch_arr;
} g;

static int alloc_texture(void)
{
	if (arrlen(g.texture_freelist_arr) > 0) return arrpop(g.texture_freelist_arr);
	const int id = arrlen(g.texture_arr);
	arrsetlen(g.texture_arr,id+1);
	return id;
}

int create_texture(int type, int width, int height)
{
	const int id = alloc_texture();
	struct texture* tex = &g.texture_arr[id];
	memset(tex, 0, sizeof *tex);
	tex->type = type;
	tex->width = width;
	tex->height = height;

	enum SDL_TextureAccess access;
	switch (tex->type & TTMASK(2)) {
	case TT_STATIC:
		access = SDL_TEXTUREACCESS_STATIC;
		break;
	case TT_STREAM:
		access = SDL_TEXTUREACCESS_STREAMING;
		break;
	default: assert(!"invalid TT(2)");
	}

	tex->texture = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_RGBA32, access, width, height);
	assert(tex->texture != NULL);

	switch (tex->type & TTMASK(1)) {
	case TT_SMOOTH:
		SDL_SetTextureScaleMode(tex->texture, SDL_SCALEMODE_LINEAR);
		break;
	case TT_PIXELATED:
		SDL_SetTextureScaleMode(tex->texture, SDL_SCALEMODE_NEAREST);
		//SDL_SetTextureScaleMode(tex->texture, SDL_SCALEMODE_PIXELART);
		break;
	default: assert(!"invalid TT(1)");
	}

	switch (tex->type & TTMASK(0)) {
	case TT_R8:
	case TT_RGBA8888:
		break;
	default: assert(!"invalid TT(0)");
	}

	return id;
}

void destroy_texture(int texture)
{
	arrput(g.texture_freelist_arr, texture);
}

void update_texture(int texture, int y0, int width, int height, void* data)
{
	assert((0 <= texture) && (texture < arrlen(g.texture_arr)));
	struct texture* t = &g.texture_arr[texture];

	void* upload = NULL;
	switch (t->type & TTMASK(0)) {
	case TT_R8: {
		arrsetlen(g.scratch_arr, 4*width*height);
		uint8_t* rp = data;
		uint8_t* wp = g.scratch_arr;
		for (int y=0; y<height; ++y) {
			for (int x=0; x<width; ++x) {
				const uint8_t v=*(rp++);
				for (int i=0; i<4; ++i) {
					*(wp++)=v;
				}
			}
		}
		upload = g.scratch_arr;
	}	break;
	case TT_RGBA8888:
		upload = data;
		break;
	default: assert(!"invalid TT(0)");
	}

	SDL_Rect rect = {
		.x=0,
		.y=y0,
		.w=width,
		.h=height,
	};
	SDL_UpdateTexture(t->texture, &rect, upload, width*4);
}

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

	SDL_BlendMode blend_mode_premultiplied_alpha = SDL_ComposeCustomBlendMode(
		SDL_BLENDFACTOR_ONE,
		SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
		SDL_BLENDOPERATION_ADD,
		SDL_BLENDFACTOR_ONE,
		SDL_BLENDFACTOR_ZERO,
		SDL_BLENDOPERATION_ADD);
	SDL_SetRenderDrawBlendMode(g.renderer, blend_mode_premultiplied_alpha);

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

	SDL_DestroyRenderer(g.renderer);
	SDL_DestroyWindow(g0.window);
	SDL_Quit();

	return EXIT_SUCCESS;
}
