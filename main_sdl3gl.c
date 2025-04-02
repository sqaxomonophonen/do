#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include "impl_sdl3.h"
#define GL_GLEXT_PROTOTYPES
// XXX defining GL_GLEXT_PROTOTYPES works on unixes, but not on windows where
// SDL_GL_GetProcAddress() is needed.
#include <SDL3/SDL_opengl.h>

#include "main.h"

struct texture {
	int type;
	int width;
	int height;
	GLuint gl_texture;
	GLenum gl_format;
};

static struct {
	SDL_GLContext gl_context;
	struct texture* texture_arr;
	int* texture_freelist_arr;
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

	switch (tex->type & TTMASK(2)) {
	case TT_STATIC:
	case TT_STREAM:
		break;
	default: assert(!"invalid TT(2)");
	}

	glGenTextures(1, &tex->gl_texture);
	glBindTexture(GL_TEXTURE_2D, tex->gl_texture);

	switch (tex->type & TTMASK(1)) {
	case TT_SMOOTH:
	case TT_PIXELATED:
		break;
	default: assert(!"invalid TT(1)");
	}

	glTexParameteri(tex->gl_texture, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(tex->gl_texture, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

	int num_comp;
	switch (tex->type & TTMASK(0)) {
	case TT_R8:
		num_comp=1;
		tex->gl_format=GL_RED;
		break;
	case TT_RGBA8888:
		num_comp=4;
		tex->gl_format=GL_RGBA;
		break;
	default: assert(!"invalid TT(0)");
	}

	glTexImage2D(
		GL_TEXTURE_2D,
		0/*=level*/,
		num_comp,
		width,
		height,
		0,
		tex->gl_format,
		GL_UNSIGNED_BYTE,
		NULL);

	return id;
}

static struct texture* get_texture(int texture)
{
	assert((0 <= texture) && (texture < arrlen(g.texture_arr)));
	return &g.texture_arr[texture];
}

void destroy_texture(int texture)
{
	struct texture* t = get_texture(texture);
	glDeleteTextures(1, &t->gl_texture);
	arrput(g.texture_freelist_arr, texture);
}

void update_texture(int texture, int y0, int width, int height, void* data)
{
	struct texture* t = get_texture(texture);
	glBindTexture(GL_TEXTURE_2D, t->gl_texture);
	glTexSubImage2D(
		GL_TEXTURE_2D,
		0/*=level*/,
		0,
		y0,
		width,
		height,
		t->gl_format,
		GL_UNSIGNED_BYTE,
		data);
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

	glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);

	gui_init();

	while (!g0.exiting) {
		handle_events();

		glViewport(0, 0, g0.true_screen_width, g0.true_screen_height);
		glClearColor(.3,.1,0,1);
		glClear(GL_COLOR_BUFFER_BIT);

		gui_draw();

		SDL_GL_SwapWindow(g0.window);
	}

	SDL_GL_DestroyContext(g.gl_context);
	SDL_DestroyWindow(g0.window);
	SDL_Quit();

	return EXIT_SUCCESS;
}
