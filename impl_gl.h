#include "stb_ds.h"
#include "util.h"
#include "main.h"

static void _glcheck(const char* file, const int line, const char* body)
{
	GLenum err = glGetError();
	if (err != GL_NO_ERROR) {
		if (body == NULL) {
			fprintf(stderr, "GL ERROR 0x%.4x in %s:%d\n", err, file, line);
		} else {
			fprintf(stderr, "GL ERROR 0x%.4x in %s:%d in code: `%s`\n", err, file, line, body);
		}
		abort();
	}
}
#define GLCHECK      _glcheck(__FILE__, __LINE__, NULL)
#define GLCALL(BODY) { BODY; _glcheck(__FILE__, __LINE__, #BODY); }

struct texture {
	int type;
	int width;
	int height;
	GLuint gl_texture;
	GLenum gl_format;
};

static struct {
	int frame_x0, frame_y0, frame_width, frame_height;

	struct texture* texture_arr;
	int* texture_freelist_arr;
	size_t vertex_buffer_size;
	GLuint vertex_buffer;
	size_t element_buffer_size;
	GLuint element_buffer;

	struct {
		GLuint program;
		GLint  u_projection;
		GLint  u_texture;
		GLint  a_position;
		GLint  a_uv;
		GLint  a_color;
	} mesh_shader;
} gg;

static int alloc_texture(void)
{
	if (arrlen(gg.texture_freelist_arr) > 0) return arrpop(gg.texture_freelist_arr);
	const int id = arrlen(gg.texture_arr);
	arrsetlen(gg.texture_arr,id+1);
	return id;
}

int create_texture(int type, int width, int height)
{
	const int id = alloc_texture();
	struct texture* tex = &gg.texture_arr[id];
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

	GLCALL(glGenTextures(1, &tex->gl_texture));
	GLCALL(glBindTexture(GL_TEXTURE_2D, tex->gl_texture));

	switch (tex->type & TTMASK(1)) {
	case TT_SMOOTH:
	case TT_PIXELATED:
		break;
	default: assert(!"invalid TT(1)");
	}

	GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR));
	GLCALL(glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR));

	int internal_format;
	switch (tex->type & TTMASK(0)) {
	case TT_R8:
		internal_format=GL_R8;
		tex->gl_format=GL_RED;
		break;
	case TT_RGBA8888:
		internal_format=GL_RGBA;
		tex->gl_format=GL_RGBA;
		break;
	default: assert(!"invalid TT(0)");
	}

	GLCALL(glTexImage2D(
		GL_TEXTURE_2D,
		0/*=level*/,
		internal_format,
		width,
		height,
		0,
		tex->gl_format,
		GL_UNSIGNED_BYTE,
		NULL));

	return id;
}

static struct texture* get_texture(int texture)
{
	assert((0 <= texture) && (texture < arrlen(gg.texture_arr)));
	return &gg.texture_arr[texture];
}

void destroy_texture(int texture)
{
	struct texture* t = get_texture(texture);
	GLCALL(glDeleteTextures(1, &t->gl_texture));
	arrput(gg.texture_freelist_arr, texture);
}

void update_texture(int texture, int y0, int width, int height, void* data)
{
	struct texture* t = get_texture(texture);
	GLCALL(glBindTexture(GL_TEXTURE_2D, t->gl_texture));
	GLCALL(glTexSubImage2D(
		GL_TEXTURE_2D,
		0/*=level*/,
		0,
		y0,
		width,
		height,
		t->gl_format,
		GL_UNSIGNED_BYTE,
		data));
}

static void gl_frame(int width, int height)
{
	gg.frame_x0 = 0;
	gg.frame_y0 = 0;
	gg.frame_width = width;
	gg.frame_height = height;
	GLCALL(glViewport(gg.frame_x0, gg.frame_y0, gg.frame_width, gg.frame_height));
	GLCALL(glClearColor(.3,.1,0,1));
	GLCALL(glClear(GL_COLOR_BUFFER_BIT));
}

static GLuint create_shader(GLenum type, const char* src)
{
	GLuint shader = glCreateShader(type); GLCHECK;

	GLCALL(glShaderSource(shader, 1, &src, NULL));
	GLCALL(glCompileShader(shader));

	GLint status;
	GLCALL(glGetShaderiv(shader, GL_COMPILE_STATUS, &status));
	if (status == GL_FALSE) {
		GLint msglen;
		GLCALL(glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &msglen));
		GLchar* msg = (GLchar*) malloc(msglen + 1);
		assert(msg != NULL);
		GLCALL(glGetShaderInfoLog(shader, msglen, NULL, msg));
		const char* stype = type == GL_VERTEX_SHADER ? "VERTEX" : type == GL_FRAGMENT_SHADER ? "FRAGMENT" : "???";

		// attempt to parse "0:<linenumber>" in error message
		int line_number = 0;
		if (strlen(msg) >= 3 && msg[0] == '0' && msg[1] == ':' && is_numeric(msg[2])) {
			const char* p0 = msg+2;
			const char* p1 = p0+1;
			while (is_numeric(*p1)) p1++;
			char buf[32];
			const int n = p1-p0;
			if (n < ARRAY_LENGTH(buf)) {
				memcpy(buf, p0, n);
				buf[n] = 0;
				line_number = atoi(buf);
			}
		}

		fprintf(stderr, "%s GLSL COMPILE ERROR: %s in:\n", stype, msg);
		if (line_number > 0) {
			const char* p = src;
			int remaining_line_breaks_to_find = line_number;
			while (remaining_line_breaks_to_find > 0) {
				for (;;) {
					char ch = *p;
					if (ch == 0) {
						remaining_line_breaks_to_find = 0;
						break;
					} else if (ch == '\n') {
						remaining_line_breaks_to_find--;
						p++;
						break;
					}
					p++;
				}
			}
			fwrite(src, p-src, 1, stderr);
			printf("~^~^~^~ ERROR ~^~^~^~\n");
			printf("%s\n", p);
		} else {
			fprintf(stderr, "%s\n", src);
		}

		abort();
	}

	return shader;
}

static GLuint create_render_program(const char* vert_src, const char* frag_src)
{
	GLuint vs = create_shader(GL_VERTEX_SHADER, vert_src);
	GLuint fs = create_shader(GL_FRAGMENT_SHADER, frag_src);

	GLuint program = glCreateProgram(); GLCHECK;
	GLCALL(glAttachShader(program, vs));
	GLCALL(glAttachShader(program, fs));

	#if 0
	glBindAttribLocation(program, index, name); CHKGL;
	#endif

	GLCALL(glLinkProgram(program));

	GLint status;
	GLCALL(glGetProgramiv(program, GL_LINK_STATUS, &status));
	if (status == GL_FALSE) {
		GLint msglen;
		GLCALL(glGetProgramiv(program, GL_INFO_LOG_LENGTH, &msglen));
		GLchar* msg = (GLchar*) malloc(msglen + 1);
		GLCALL(glGetProgramInfoLog(program, msglen, NULL, msg));
		fprintf(stderr, "shader link error: %s\n", msg);
		abort();
	}

	// safe to detach+delete after program is built
	GLCALL(glDetachShader(program, vs));
	GLCALL(glDetachShader(program, fs));
	GLCALL(glDeleteShader(vs));
	GLCALL(glDeleteShader(fs));

	return program;
}

static void gl_init(void)
{
	GLCALL(glGenBuffers(1, &gg.vertex_buffer));
	GLCALL(glGenBuffers(1, &gg.element_buffer));

	{
		gg.mesh_shader.program = create_render_program(
			"#version 300 es\n"
			"precision mediump float;\n"
			"uniform mat4 u_projection;\n"
			"in vec2 a_position;\n"
			"in vec2 a_uv;\n"
			"in vec4 a_color;\n"
			"out vec2 v_uv;\n"
			"out vec4 v_color;\n"
			"void main()\n"
			"{\n"
			"	v_uv = a_uv;\n"
			"	v_color = a_color;\n"
			"	gl_Position = u_projection * vec4(a_position,0,1);\n"
			"}\n"
		,
			"#version 300 es\n"
			"precision mediump float;\n"
			"uniform sampler2D u_texture;\n"
			"in vec2 v_uv;\n"
			"in vec4 v_color;\n"
			"out vec4 frag_color;\n"
			"void main()\n"
			"{\n"
			"	frag_color = v_color * texture(u_texture, v_uv);\n"
			"}\n"
		);

		gg.mesh_shader.u_projection = glGetUniformLocation(gg.mesh_shader.program, "u_projection"); GLCHECK;
		gg.mesh_shader.u_texture    = glGetUniformLocation(gg.mesh_shader.program, "u_texture"); GLCHECK;

		gg.mesh_shader.a_position   = glGetAttribLocation(gg.mesh_shader.program,  "a_position"); GLCHECK;
		gg.mesh_shader.a_uv         = glGetAttribLocation(gg.mesh_shader.program,  "a_uv"); GLCHECK;
		gg.mesh_shader.a_color      = glGetAttribLocation(gg.mesh_shader.program,  "a_color"); GLCHECK;
	}
}

static void gl_render_gui_draw_lists(void)
{
	glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO); // XXX?

	for (int i=0;;++i) {
		struct draw_list* list = gui_get_draw_list(i);
		if (list == NULL) break;
		switch (list->type) {
		case MESH_TRIANGLES: {

			GLCALL(glUseProgram(gg.mesh_shader.program));

			GLCALL(glBindTexture(GL_TEXTURE_2D, get_texture(list->mesh.texture)->gl_texture));
			GLCALL(glUniform1i(gg.mesh_shader.u_texture, 0));

			const float left   = gg.frame_x0;
			const float right  = gg.frame_x0 + gg.frame_width;
			const float top    = gg.frame_y0;
			const float bottom = gg.frame_y0 + gg.frame_height;
			const GLfloat ortho[] = {
				2.0f/(right-left)         , 0.0f                      ,  0.0f , 0.0f,
				0.0f                      , 2.0f/(top-bottom)         ,  0.0f , 0.0f,
				0.0f                      , 0.0f                      , -1.0f , 0.0f,
				(right+left)/(left-right) , (top+bottom)/(bottom-top) ,  0.0f , 1.0f,
			};
			GLCALL(glUniformMatrix4fv(gg.mesh_shader.u_projection, 1, 0, ortho));

			GLCALL(glBindBuffer(GL_ARRAY_BUFFER, gg.vertex_buffer));
			const size_t req_vertex  = list->mesh.num_vertices * sizeof(struct vertex);
			if (req_vertex > gg.vertex_buffer_size) {
				GLCALL(glBufferData(GL_ARRAY_BUFFER, req_vertex, list->mesh.vertices, GL_STREAM_DRAW));
			} else {
				GLCALL(glBufferSubData(GL_ARRAY_BUFFER, 0, req_vertex, list->mesh.vertices));
			}

			GLCALL(glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, gg.element_buffer));
			const size_t req_element = list->mesh.num_indices * sizeof(vertex_index);
			if (req_element > gg.element_buffer_size) {
				GLCALL(glBufferData(GL_ELEMENT_ARRAY_BUFFER, req_element, list->mesh.indices, GL_STREAM_DRAW));
			} else {
				GLCALL(glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, req_element, list->mesh.indices));
			}

			GLCALL(glEnableVertexAttribArray(gg.mesh_shader.a_position));
			GLCALL(glEnableVertexAttribArray(gg.mesh_shader.a_uv));
			GLCALL(glEnableVertexAttribArray(gg.mesh_shader.a_color));

			const GLsizei stride = sizeof(struct vertex);
			GLCALL(glVertexAttribPointer(
				gg.mesh_shader.a_position,
				4, GL_FLOAT, GL_FALSE,
				stride, (GLvoid*)offsetof(struct vertex, x)));
			GLCALL(glVertexAttribPointer(
				gg.mesh_shader.a_uv,
				2, GL_FLOAT, GL_FALSE,
				stride, (GLvoid*)offsetof(struct vertex, u)));
			GLCALL(glVertexAttribPointer(
				gg.mesh_shader.a_color,
				4, GL_UNSIGNED_BYTE, GL_TRUE,
				stride, (GLvoid*)offsetof(struct vertex, rgba)));

			GLenum index_type;
			switch (sizeof(vertex_index)) {
			case 2: index_type = GL_UNSIGNED_SHORT; break;
			default: assert(!"unhandled sizeof(vertex_index)");
			}

			GLCALL(glDrawElements(
				GL_TRIANGLES,
				list->mesh.num_indices,
				index_type,
				0));

			GLCALL(glDisableVertexAttribArray(gg.mesh_shader.a_position));
			GLCALL(glDisableVertexAttribArray(gg.mesh_shader.a_uv));
			GLCALL(glDisableVertexAttribArray(gg.mesh_shader.a_color));

		}	break;
		default: assert(!"unhandled draw list type");
		}
	}
}
