#include <math.h>

#define MAX_VERTICES (1<<16)
#define MAX_ELEMENTS (1<<17)

#define CHKGL \
	do { \
		GLenum CHKGL_error = glGetError(); \
		if (CHKGL_error != GL_NO_ERROR) { \
			fprintf(stderr, "OPENGL ERROR %d in %s:%d\n", CHKGL_error, __FILE__, __LINE__); \
			exit(EXIT_FAILURE); \
		} \
	} while (0)

struct draw_vertex {
	union vec2 position;
	union vec2 uv;
	union vec4 color;
};

GLuint glprg;
GLuint u_texture;
GLuint u_scaling;

struct draw_vertex* draw_vertices;
GLuint vertex_buffer;
GLuint vertex_array;
GLushort* draw_elements;
GLuint element_buffer;
int draw_n_vertices;
int draw_n_elements;
GLuint atlas_texture;
union vec2 dotuv;

static GLuint create_shader(const char* src, GLenum type)
{
	GLuint shader = glCreateShader(type); CHKGL;
	AN(shader);
	glShaderSource(shader, 1, &src, 0); CHKGL;
	glCompileShader(shader); CHKGL;

	GLint status;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status); CHKGL;
	if (status == GL_FALSE) {
		GLint msglen;
		glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &msglen); CHKGL;
		GLchar* msg = malloc(msglen + 1);
		AN(msg);
		glGetShaderInfoLog(shader, msglen, NULL, msg);
		const char* stype = type == GL_VERTEX_SHADER ? "vertex" : type == GL_FRAGMENT_SHADER ? "fragment" : "waaaat";
		fprintf(stderr, "%s shader compile error: %s -- source:\n%s", stype, msg, src);
		exit(EXIT_FAILURE);
	}

	return shader;
}

static GLuint create_program(const char* vert_src, const char* frag_src)
{
	GLuint vert_shader = create_shader(vert_src, GL_VERTEX_SHADER);
	GLuint frag_shader = create_shader(frag_src, GL_FRAGMENT_SHADER);

	GLuint prg = glCreateProgram(); CHKGL;
	AN(prg);

	glAttachShader(prg, vert_shader); CHKGL;
	glAttachShader(prg, frag_shader); CHKGL;

	glLinkProgram(prg);

	GLint status;
	glGetProgramiv(prg, GL_LINK_STATUS, &status); CHKGL;
	if (status == GL_FALSE) {
		GLint msglen;
		glGetProgramiv(prg, GL_INFO_LOG_LENGTH, &msglen); CHKGL;
		GLchar* msg = (GLchar*) malloc(msglen + 1);
		AN(msg);
		glGetProgramInfoLog(prg, msglen, NULL, msg);
		fprintf(stderr, "shader link error: %s", msg);
		exit(EXIT_FAILURE);
	}

	glDeleteShader(vert_shader);
	glDeleteShader(frag_shader);

	return prg;
}

static void gl_init()
{
	{
		const GLchar* vert_src =
			"#version 130\n"

			"uniform vec2 u_scaling;\n"

			"attribute vec2 a_position;\n"
			"attribute vec2 a_uv;\n"
			"attribute vec4 a_color;\n"

			"varying vec2 v_uv;\n"
			"varying vec4 v_color;\n"

			"void main()\n"
			"{\n"
			"	v_uv = a_uv;\n"
			"	v_color = a_color;\n"
			"	gl_Position = vec4(a_position * u_scaling * vec2(2,2) + vec2(-1,1), 0, 1);\n"
			"}\n"
			;

		const GLchar* frag_src =
			"#version 130\n"

			"uniform sampler2D u_texture;\n"

			"varying vec2 v_uv;\n"
			"varying vec4 v_color;\n"

			"void main()\n"
			"{\n"
			"	float v = texture2D(u_texture, v_uv).r;\n"
			"	gl_FragColor = v_color * vec4(v,v,v,v);\n"
			"}\n"
			;
		glprg = create_program(vert_src, frag_src);
		u_texture = glGetUniformLocation(glprg, "u_texture"); CHKGL;
		u_scaling = glGetUniformLocation(glprg, "u_scaling"); CHKGL;

		GLuint a_position = glGetAttribLocation(glprg, "a_position"); CHKGL;
		GLuint a_uv = glGetAttribLocation(glprg, "a_uv"); CHKGL;
		GLuint a_color = glGetAttribLocation(glprg, "a_color"); CHKGL;

		size_t vertices_sz = MAX_VERTICES * sizeof(struct draw_vertex);
		AN(draw_vertices = malloc(vertices_sz));
		glGenBuffers(1, &vertex_buffer); CHKGL;
		glGenVertexArrays(1, &vertex_array); CHKGL;
		glBindVertexArray(vertex_array); CHKGL;
		glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer); CHKGL;
		glBufferData(GL_ARRAY_BUFFER, vertices_sz, NULL, GL_STREAM_DRAW); CHKGL;
		glEnableVertexAttribArray(a_position); CHKGL;
		glEnableVertexAttribArray(a_uv); CHKGL;
		glEnableVertexAttribArray(a_color); CHKGL;

		#define OFZ(e) (GLvoid*)((size_t)&(((struct draw_vertex*)0)->e))
		glVertexAttribPointer(a_position, 2, GL_FLOAT, GL_FALSE, sizeof(struct draw_vertex), OFZ(position)); CHKGL;
		glVertexAttribPointer(a_uv, 2, GL_FLOAT, GL_FALSE, sizeof(struct draw_vertex), OFZ(uv)); CHKGL;
		glVertexAttribPointer(a_color, 4, GL_FLOAT, GL_FALSE, sizeof(struct draw_vertex), OFZ(color)); CHKGL;
		#undef OFZ

		size_t elements_sz = MAX_ELEMENTS * sizeof(GLushort);
		AN(draw_elements = malloc(elements_sz));
		glGenBuffers(1, &element_buffer); CHKGL;
		glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, element_buffer); CHKGL;
		glBufferData(GL_ELEMENT_ARRAY_BUFFER, elements_sz, NULL, GL_STREAM_DRAW); CHKGL;
	}

	// setup atlas texture
	{
		glGenTextures(1, &atlas_texture); CHKGL;
		int level = 0;
		int border = 0;
		glBindTexture(GL_TEXTURE_2D, atlas_texture);
		glTexImage2D(
			GL_TEXTURE_2D,
			level,
			1,
			active_atls->atlas_width, active_atls->atlas_height,
			border,
			GL_RED,
			GL_UNSIGNED_BYTE,
			active_atls->atlas_bitmap); CHKGL;

		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); CHKGL;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST); CHKGL;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER); CHKGL;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER); CHKGL;

		// setup dotuv
		assert(rgly_dot != NULL);
		dotuv = (union vec2) { .u = (float)(rgly_dot->x+1) / (float)active_atls->atlas_width, .v = (float)(rgly_dot->y+1) / (float)active_atls->atlas_height };
	}

	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); CHKGL;

}

static void gl_frame_begin(int width, int height)
{
	glViewport(0, 0, width, height);

	glUseProgram(glprg);
	glUniform1i(u_texture, 0);
	glUniform2f(u_scaling, 1.0f / (float)width, -1.0f / (float)height);

	glBindVertexArray(vertex_array);

	/* XXX I'm getting a weird fatal when nothing is drawn at all; it's
	 * probably safe to leave out, but iono? */
	#if 0
	glClearColor(1,0,1,1);
	glClear(GL_COLOR_BUFFER_BIT);
	#endif
}

static void gl_draw_flush()
{
	if (!draw_n_vertices || !draw_n_elements) {
		// nothing to do
		return;
	}

	glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer);
	glBufferSubData(GL_ARRAY_BUFFER, 0, draw_n_vertices * sizeof(struct draw_vertex), draw_vertices);

	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, element_buffer);
	glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, draw_n_elements * sizeof(GLushort), draw_elements);

	glBindTexture(GL_TEXTURE_2D, atlas_texture);
	glDrawElements(GL_TRIANGLES, draw_n_elements, GL_UNSIGNED_SHORT, 0);

	draw_n_vertices = 0;
	draw_n_elements = 0;
}

static void draw_append(int n_vertices, int n_elements, struct draw_vertex* vertices, GLushort* elements)
{
	// XXX TODO clipping?

	int flush = 0;
	flush |= draw_n_vertices + n_vertices > MAX_VERTICES;
	flush |= draw_n_elements + n_elements > MAX_ELEMENTS;
	if (flush) {
		gl_draw_flush();
		ASSERT((draw_n_vertices + n_vertices) <= MAX_VERTICES);
		ASSERT((draw_n_elements + n_elements) <= MAX_ELEMENTS);
	}

	memcpy(draw_vertices + draw_n_vertices, vertices, n_vertices * sizeof(*vertices));

	GLushort* ebase = draw_elements + draw_n_elements;
	memcpy(ebase, elements, n_elements * sizeof(*ebase));
	for (int i = 0; i < n_elements; i++) ebase[i] += draw_n_vertices;

	draw_n_vertices += n_vertices;
	draw_n_elements += n_elements;
}

static void draw_rect_2col(struct rect posrect, struct rect uvrect, union vec4 c0, union vec4 c1)
{
	struct rect fr = wframe_top()->rect;

	struct rect rx = posrect;
	rx.p0 = vec2_add(rx.p0, fr.p0);

	struct rect apos, auv;
	for (int i = 0; i < 2; i++) {
		// XXX move clipping to draw_append
		apos.p0.s[i] = fmax(rx.p0.s[i], fr.p0.s[i]);
		apos.dim.s[i] = fmin(rx.p0.s[i] + rx.dim.s[i], fr.p0.s[i] + fr.dim.s[i]) - apos.p0.s[i];
		if (apos.dim.s[i] <= 0) return;
		auv.p0.s[i] = uvrect.p0.s[i] + uvrect.dim.s[i] * (apos.p0.s[i] - rx.p0.s[i]) / rx.dim.s[i];
		auv.dim.s[i] = uvrect.dim.s[i] * (apos.dim.s[i] / rx.dim.s[i]);
	}

	float dx0 = apos.p0.x;
	float dy0 = apos.p0.y;
	float dx1 = dx0 + apos.dim.w;
	float dy1 = dy0 + apos.dim.h;
	float u0 = auv.p0.u;
	float v0 = auv.p0.v;
	float u1 = u0 + auv.dim.w;
	float v1 = v0 + auv.dim.h;

	struct draw_vertex vs[4] = {
		{ .position = { .x = dx0, .y = dy0 }, .uv = { .u = u0, .v = v0 }, .color = c0 },
		{ .position = { .x = dx1, .y = dy0 }, .uv = { .u = u1, .v = v0 }, .color = c0 },
		{ .position = { .x = dx1, .y = dy1 }, .uv = { .u = u1, .v = v1 }, .color = c1 },
		{ .position = { .x = dx0, .y = dy1 }, .uv = { .u = u0, .v = v1 }, .color = c1 }
	};
	GLushort es[6] = {0,1,2,0,2,3};

	draw_append(4, 6, vs, es);
}

static struct rect solid_uvrect()
{
	return (struct rect) { .p0 = dotuv, .dim = { .w = 0, .h = 0 }};
}

static void fill_rect(struct rect posrect, union vec4 color)
{
	draw_rect_2col(posrect, solid_uvrect(), color, color);
}

static void draw_rect(struct rect posrect, struct rect uvrect)
{
	draw_rect_2col(posrect, uvrect, draw_color0, draw_color1);
}

static struct rect calc_uv_rect(int x, int y, int w, int h)
{
	return (struct rect) {
		.p0 = { .x = (float)x / (float)active_atls->atlas_width, .y = (float)y / (float)active_atls->atlas_height },
		.dim = { .w = (float)w / (float)active_atls->atlas_width, .h = (float)h / (float)active_atls->atlas_height }
	};
}

static void draw_glyph(struct atls_glyph* gly)
{
	draw_rect(
		(struct rect) {
			.p0 = { .x = cursor_x + gly->xoff, .y = cursor_y + gly->yoff },
			.dim = { .w = gly->w, .h = gly->h }
		},
		calc_uv_rect(gly->x, gly->y, gly->w, gly->h)
	);
}

void lsl_cell_plot(int column, int row, int x, int y, int width, int height)
{
	assert(column >= 0);
	assert(column < active_cell_table->n_columns);
	int cell_width = active_cell_table->widths[column];
	if (width == 0) width = cell_width;

	assert(row >= 0);
	assert(row < active_cell_table->n_rows);
	int cell_height = active_cell_table->heights[row];
	if (height == 0) height = cell_height;

	for (int i = 0; i < active_cell_table->n_layers; i++) {
		struct atls_cell* cell = atls_cell_table_lookup(active_cell_table, column, row, i);
		if (cell == NULL) continue;
		union vec4 col = lsl_eval(active_cell_table->layer_prg_ids[i]);
		draw_rect_2col(
			(struct rect) {
				.p0 = { .x = x, .y = y },
				.dim = { .w = width, .h = height }
			},
			calc_uv_rect(cell->x, cell->y, cell_width, cell_height),
			col, col
		);
	}
}

void lsl_line(float thickness, union vec2 p0, union vec2 p1)
{
	struct rect fr = wframe_top()->rect;

	p0 = vec2_add(p0, fr.p0);
	p1 = vec2_add(p1, fr.p0);

	for (int i = 0; i < 3; i++) {
		union vec2 d = vec2_unit(vec2_normal(vec2_sub(p1, p0)));

		union vec4 transparent = (union vec4) { .r=0, .g=0, .b=0, .a=0 };

		float s0,s1;
		union vec4 c0, c1, c2, c3;
		if (i == 0) {
			s0 = -thickness - 1;
			s1 = -thickness;
			c0 = c1 = transparent;
			c2 = draw_color1;
			c3 = draw_color0;
		} else if (i == 1) {
			s0 = -thickness;
			s1 = thickness;
			c0 = c3 = draw_color0;
			c1 = c2 = draw_color1;
		} else if (i == 2) {
			s0 = thickness;
			s1 = thickness + 1;
			c0 = draw_color0;
			c1 = draw_color1;
			c2 = c3 = transparent;
		}

		union vec2 v0 = vec2_add(p0, vec2_scale(d,s0));
		union vec2 v1 = vec2_add(p1, vec2_scale(d,s0));
		union vec2 v2 = vec2_add(p1, vec2_scale(d,s1));
		union vec2 v3 = vec2_add(p0, vec2_scale(d,s1));

		struct draw_vertex vs[4] = {
			{ .position = v0, .uv = dotuv, .color = c0 },
			{ .position = v1, .uv = dotuv, .color = c1 },
			{ .position = v2, .uv = dotuv, .color = c2 },
			{ .position = v3, .uv = dotuv, .color = c3 }
		};
		GLushort es[6] = {0,1,2,0,2,3};
		draw_append(4, 6, vs, es);
	}
}

void lsl_fill_rect(struct rect* r)
{
	draw_rect(*r, solid_uvrect());
}

void lsl_clear()
{
	struct rect r;
	r.p0.x = r.p0.y = 0;
	r.dim = wframe_top()->rect.dim;
	lsl_fill_rect(&r);
}

