#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "impl_sdl3.h"
#include "main.h"

struct texture {
	int type;
	int width;
	int height;
	SDL_Texture* sdl_texture;
};

static struct {
	SDL_Renderer* renderer;
	struct texture* texture_arr;
	int* texture_freelist_arr;
	uint8_t* u8_scratch_arr;
	float* f32_scratch_arr;
} g;

static int alloc_texture(void)
{
	if (arrlen(g.texture_freelist_arr) > 0) return arrpop(g.texture_freelist_arr);
	const int id = arrlen(g.texture_arr);
	arrsetlen(g.texture_arr,id+1);
	return id;
}

static struct texture* get_texture(int id)
{
	assert((0 <= id) && (id < arrlen(g.texture_arr)));
	return &g.texture_arr[id];
}

void get_texture_dim(int texture, int* out_width, int* out_height)
{
	struct texture* t = get_texture(texture);
	if (out_width)  *out_width  = t->width;
	if (out_height) *out_height = t->height;
}

int create_texture(int type, int width, int height)
{
	const int id = alloc_texture();
	struct texture* tex = get_texture(id);
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

	tex->sdl_texture = SDL_CreateTexture(g.renderer, SDL_PIXELFORMAT_RGBA32, access, width, height);
	assert(tex->sdl_texture != NULL);

	switch (tex->type & TTMASK(1)) {
	case TT_SMOOTH:
		SDL_SetTextureScaleMode(tex->sdl_texture, SDL_SCALEMODE_LINEAR);
		break;
	case TT_PIXELATED:
		SDL_SetTextureScaleMode(tex->sdl_texture, SDL_SCALEMODE_NEAREST);
		//SDL_SetTextureScaleMode(tex->texture, SDL_SCALEMODE_PIXELART); // XXX new!
		break;
	default: assert(!"invalid TT(1)");
	}

	switch (tex->type & TTMASK(0)) {
	case TT_LUMEN8:
	case TT_RGBA8888:
		break;
	default: assert(!"invalid TT(0)");
	}

	return id;
}

void destroy_texture(int id)
{
	arrput(g.texture_freelist_arr, id);
}

void update_texture(int id, int y0, int width, int height, void* data)
{
	struct texture* t = get_texture(id);

	void* upload = NULL;
	switch (t->type & TTMASK(0)) {
	case TT_LUMEN8: {
		arrsetlen(g.u8_scratch_arr, 4*width*height);
		uint8_t* rp = data;
		uint8_t* wp = g.u8_scratch_arr;
		for (int y=0; y<height; ++y) {
			for (int x=0; x<width; ++x) {
				const uint8_t v=*(rp++);
				for (int i=0; i<4; ++i) {
					*(wp++)=v;
				}
			}
		}
		upload = g.u8_scratch_arr;
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
	SDL_UpdateTexture(t->sdl_texture, &rect, upload, width*4);
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

	#if 0
	const SDL_BlendMode blend_mode_premultiplied_alpha = SDL_ComposeCustomBlendMode(
		SDL_BLENDFACTOR_ONE,
		SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
		SDL_BLENDOPERATION_ADD,
		SDL_BLENDFACTOR_ONE,
		SDL_BLENDFACTOR_ZERO,
		SDL_BLENDOPERATION_ADD);
	#endif

	const SDL_BlendMode blend_mode_additive = SDL_ComposeCustomBlendMode(
		SDL_BLENDFACTOR_ONE,
		SDL_BLENDFACTOR_ONE,
		SDL_BLENDOPERATION_ADD,
		SDL_BLENDFACTOR_ONE,
		SDL_BLENDFACTOR_ONE,
		SDL_BLENDOPERATION_ADD);

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
		SDL_SetRenderDrawColorFloat(g.renderer,0,0,0,0);
		SDL_RenderClear(g.renderer);

		gui_draw();

		for (int i=0;;++i) {
			struct draw_list* list = gui_get_draw_list(i);
			if (list == NULL) break;

			switch (list->type) {
			case MESH_TRIANGLES: {
				const int nv = list->mesh.num_vertices;
				struct vertex* vs = list->mesh.vertices;
				arrsetlen(g.f32_scratch_arr, 4*nv);
				for (int i=0; i<nv; ++i) {
					for (int ii=0; ii<4; ++ii) {
						g.f32_scratch_arr[i*4+ii] = (float)((vs[i].rgba >> (ii*8)) & 0xff) * (1.f / 255.f);
					}
				}

				SDL_Texture* texture = get_texture(list->mesh.texture_id)->sdl_texture;

				switch (list->blend_mode) {
				case ADDITIVE:
					// XXX not sure I like having to set the mode on the
					// texture itself? (not how it works in graphics apis?)
					SDL_SetTextureBlendMode(texture, blend_mode_additive);
					break;
				default: assert(!"unhandled blend mode");
				}

				SDL_RenderGeometryRaw(
					g.renderer,
					texture,

					// xy,stride
					(float*)vs,
					sizeof(vs[0]),

					// color,stride
					(SDL_FColor*)g.f32_scratch_arr,
					4*sizeof(g.f32_scratch_arr[0]),

					// uv,stride
					(float*)((uint8_t*)vs + offsetof(struct vertex,u)),
					sizeof(vs[0]),

					list->mesh.num_vertices,

					list->mesh.indices,
					list->mesh.num_indices,
					sizeof(list->mesh.indices[0]));

			}	break;
			default: assert(!"unhandled draw list type");
			}
		}

		SDL_RenderPresent(g.renderer);
	}

	SDL_DestroyRenderer(g.renderer);
	SDL_DestroyWindow(g0.window);
	SDL_Quit();

	return EXIT_SUCCESS;
}
