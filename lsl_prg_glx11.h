/*
#if __STDC_VERSION__ >= 199901L
#define _XOPEN_SOURCE 600
#else
#define _XOPEN_SOURCE 500
#endif
*/


// TODO noget ala https://github.com/ApoorvaJ/Papaya/blob/master/src/libs/gl_lite.h?

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <math.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>

#define GL_GLEXT_PROTOTYPES
#include <GL/glx.h>


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

Display* dpy;
XVisualInfo* vis;
GLXContext ctx;
XIM xim;
struct draw_vertex* draw_vertices;
GLuint vertex_buffer;
GLuint vertex_array;
GLushort* draw_elements;
GLuint element_buffer;
int draw_n_vertices;
int draw_n_elements;
GLuint atlas_texture;
int tmp_ctx_error;
int viewport_height;
union vec2 dotuv;
Cursor cursor_default;
Cursor cursor_horiz;
Cursor cursor_vert;
Cursor cursor_cross;

Cursor current_cursor;
Cursor last_cursor;

#define MAX_WIN (32)

struct win {
	int open;
	int(*proc)(void*);
	void* usr;
	Window window;
	XIC xic;
	struct lsl_frame frame;
} wins[MAX_WIN];

void lsl_win_open(const char* title, int(*proc)(void*), void* usr)
{
	struct win* lw = NULL;
	for (int i = 0; i < MAX_WIN; i++) {
		lw = &wins[i];
		if (!lw->open) {
			lw->open = 1;
			lw->proc = proc;
			lw->usr = usr;
			break;
		}
	}

	if (!lw->open) {
		fprintf(stderr, "no free windows\n");
		exit(EXIT_FAILURE);
	}

	XSetWindowAttributes attrs;
	Window root = RootWindow(dpy, vis->screen);
	attrs.colormap = XCreateColormap(
		dpy,
		root,
		vis->visual,
		AllocNone);
	attrs.background_pixmap = None;
	attrs.border_pixel = 0;
	attrs.event_mask =
		StructureNotifyMask
		| EnterWindowMask
		| LeaveWindowMask
		| ButtonPressMask
		| ButtonReleaseMask
		| PointerMotionMask
		| KeyPressMask
		| KeyReleaseMask
		| ExposureMask
		| VisibilityChangeMask;

	lw->window = XCreateWindow(
		dpy,
		root,
		0, 0,
		100, 100,
		0,
		vis->depth,
		InputOutput,
		vis->visual, CWBorderPixel | CWColormap | CWEventMask,
		&attrs);

	if (!lw->window) {
		fprintf(stderr, "XCreateWindow failed\n");
		exit(EXIT_FAILURE);
	}

	lw->xic = XCreateIC(
		xim,
		XNInputStyle,
		XIMPreeditNothing | XIMStatusNothing,
		XNClientWindow,
		lw->window,
		XNFocusWindow,
		lw->window,
		NULL);
	if (lw->xic == NULL) {
		fprintf(stderr, "XCreateIC failed\n");
		exit(EXIT_FAILURE);
	}

	XStoreName(dpy, lw->window, title);
	XMapWindow(dpy, lw->window);
}


static struct win* wlookup(Window w)
{
	for (int i = 0; i < MAX_WIN; i++) {
		struct win* lw = &wins[i];
		if (lw->open && lw->window == w) return lw;
	}
	return NULL;
}

static void handle_text_event(char* text, int length, struct lsl_frame* f)
{
	if (length <= 0) return;
	if ((f->text_length + length) >= LSL_MAX_TEXT_LENGTH) return;
	memcpy(f->text + f->text_length, text, length);
	f->text_length += length;
	f->text[f->text_length] = 0;
}

static void handle_key_event(XKeyEvent* e, struct win* lw)
{
	struct lsl_frame* f = &lw->frame;

	KeySym sym = XkbKeycodeToKeysym(dpy, e->keycode, 0, 0);
	int mask = 0;
	int is_keypress = e->type == KeyPress;
	switch (sym) {
		case XK_Return:
			if (is_keypress) {
				// XLookupString would give "\r" :-/
				handle_text_event("\n", 1, f);
			}
			return;
		case XK_Shift_L:
			mask = LSL_MOD_LSHIFT;
			break;
		case XK_Shift_R:
			mask = LSL_MOD_RSHIFT;
			break;
		case XK_Control_L:
			mask = LSL_MOD_LCTRL;
			break;
		case XK_Control_R:
			mask = LSL_MOD_RCTRL;
			break;
		case XK_Alt_L:
			mask = LSL_MOD_LALT;
			break;
		case XK_Alt_R:
			mask = LSL_MOD_RALT;
			break;
	}

	if (mask) {
		if (is_keypress) {
			f->mod |= mask;
		} else {
			f->mod &= ~mask;
		}
	} else if (is_keypress) {
		char buf[16];
		int len = Xutf8LookupString(lw->xic, e, buf, sizeof(buf), NULL, NULL);
		if (len > 0) {
			handle_text_event(buf, len, f);
		}
	}
}

static union vec2 get_dim_vec2(Window w)
{
	Window _root;
	int _x, _y;
	unsigned int _width, _height, _border_width, _depth;
	XGetGeometry(dpy, w, &_root, &_x, &_y, &_width, &_height, &_border_width, &_depth);
	return (union vec2) { .w = _width, .h = _height };
}

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

static void draw_flush()
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
		draw_flush();
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
	struct rect fr = lsl_frame_top()->rect;

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

void lsl_line(union vec2 p0, union vec2 p1)
{
	struct rect fr = lsl_frame_top()->rect;

	p0 = vec2_add(p0, fr.p0);
	p1 = vec2_add(p1, fr.p0);

	const float thickness = 0.1;
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
	draw_rect(*r, (struct rect) { .p0 = dotuv, .dim = { .w = 0, .h = 0 }});
}

void lsl_clear()
{
	struct rect r;
	r.p0.x = r.p0.y = 0;
	r.dim = lsl_frame_top()->rect.dim;
	lsl_fill_rect(&r);
}

void lsl_set_pointer(int id)
{
	Cursor c = cursor_default;
	switch (id) {
		case LSL_POINTER_HORIZONTAL:
			c = cursor_horiz;
			break;
		case LSL_POINTER_VERTICAL:
			c = cursor_vert;
			break;
		case LSL_POINTER_4WAY:
			c = cursor_cross;
			break;
	}
	current_cursor = c;
}

void lsl_main_loop()
{
	/* opengl initialization stuff will fail without a context. we're
	 * expected to have at least one window at this point, so bind context
	 * to the first that is found */
	for (int i = 0; i < MAX_WIN; i++) {
		struct win* lw = &wins[i];
		if (!lw->open) continue;
		glXMakeCurrent(dpy, lw->window, ctx);
		break;
	}

	GLuint glprg;
	GLuint u_texture;
	GLuint u_scaling;

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

	for (;;) {
		while (XPending(dpy)) {
			XEvent xe;
			XNextEvent(dpy, &xe);
			Window w = xe.xany.window;
			if (XFilterEvent(&xe, w)) continue;

			struct win* lw = wlookup(w);
			if (lw == NULL) continue;

			struct lsl_frame* f = &lw->frame;

			switch (xe.type) {
				case EnterNotify:
					f->minside = 1;
					f->mpos.x = xe.xcrossing.x;
					f->mpos.y = xe.xcrossing.y;
					break;
				case LeaveNotify:
					f->minside = 0;
					f->mpos.x = 0;
					f->mpos.y = 0;
					break;
				case ButtonPress:
				case ButtonRelease:
				{
					int i = xe.xbutton.button - 1;
					if (i >= 0 && i < LSL_MAX_BUTTONS) {
						f->button[i] = xe.type == ButtonPress;
						f->button_cycles[i]++;
					}
				}
				break;
				case MotionNotify:
					f->minside = 1;
					f->mpos.x = xe.xmotion.x;
					f->mpos.y = xe.xmotion.y;
					break;
				case KeyPress:
				case KeyRelease:
					handle_key_event(&xe.xkey, lw);
					break;
			}
		}

		for (int i = 0; i < MAX_WIN; i++) {
			struct win* lw = &wins[i];
			if (!lw->open) continue;

			// fetch window dimensions
			struct lsl_frame* f = &lw->frame;
			f->rect.p0.x = f->rect.p0.y = 0;
			f->rect.dim = get_dim_vec2(lw->window);
			viewport_height = f->rect.dim.h;

			glXMakeCurrent(dpy, lw->window, ctx);
			glViewport(0, 0, f->rect.dim.w, f->rect.dim.h);

			glUseProgram(glprg);
			glUniform1i(u_texture, 0);
			glUniform2f(u_scaling, 1.0f / (float)f->rect.dim.w, -1.0f / (float)f->rect.dim.h);

			glBindVertexArray(vertex_array);

			current_cursor = cursor_default;

			frame_stack_reset(f);

			// run user callback
			int ret = lw->proc(lw->usr);

			if (current_cursor != last_cursor) {
				XDefineCursor(dpy, RootWindow(dpy, vis->screen), current_cursor);
				last_cursor = current_cursor;
			}

			draw_flush();

			glXSwapBuffers(dpy, lw->window);

			// clear per-frame input stuff
			for (int i = 0; i < LSL_MAX_BUTTONS; i++) f->button_cycles[i] = 0;
			f->text_length = 0;

			if (ret != 0) {
				return; // XXX or close window?
			}
		}
	}
}

static int is_extension_supported(const char* extensions, const char* extension)
{
	const char* p0 = extensions;
	const char* p1 = p0;
	for (;;) {
		while (*p1 != ' ' && *p1 != '\0') p1++;
		if (memcmp(extension, p0, p1 - p0) == 0) return 1;
		if (*p1 == '\0') return 0;
		p0 = p1++;
	}
}


static int tmp_ctx_error_handler(Display *dpy, XErrorEvent *ev)
{
	tmp_ctx_error = 1;
	return 0;
}


int main(int argc, char** argv)
{
	dpy = XOpenDisplay(NULL);
	if (!dpy) {
		fprintf(stderr, "XOpenDisplay failed");
		exit(EXIT_FAILURE);
	}

	xim = XOpenIM(dpy, NULL, NULL, NULL);
	if (xim == NULL) {
		fprintf(stderr, "XOpenIM failed");
		exit(EXIT_FAILURE);
	}

	{
		int major = -1;
		int minor = -1;
		Bool success = glXQueryVersion(dpy, &major, &minor);
		if (success == False || major < 1 || (major == 1 && minor < 3)) {
			fprintf(stderr, "invalid glx version, major=%d, minor=%d\n", major, minor);
			exit(EXIT_FAILURE);
		}
	}

	/* find visual */
	vis = NULL;
	GLXFBConfig fb_config = NULL;
	{
		static int attrs[] = {
			GLX_X_RENDERABLE    , True,
			GLX_DRAWABLE_TYPE   , GLX_WINDOW_BIT,
			GLX_RENDER_TYPE     , GLX_RGBA_BIT,
			GLX_X_VISUAL_TYPE   , GLX_TRUE_COLOR,
			GLX_RED_SIZE        , 8,
			GLX_GREEN_SIZE      , 8,
			GLX_BLUE_SIZE       , 8,
			GLX_ALPHA_SIZE      , 8,
			GLX_DEPTH_SIZE      , 0,
			GLX_STENCIL_SIZE    , 8,
			GLX_DOUBLEBUFFER    , True,
			GLX_SAMPLE_BUFFERS  , 0,
			GLX_SAMPLES         , 0,
			None
		};

		int n;
		GLXFBConfig* cs = glXChooseFBConfig(dpy, XDefaultScreen(dpy), attrs, &n);
		if (cs == NULL) {
			fprintf(stderr, "glXChooseFBConfig failed\n");
			exit(1);
		}

		int first_valid = -1;

		for (int i = 0; i < n; i++) {
			XVisualInfo* try_vis = glXGetVisualFromFBConfig(dpy, cs[i]);
			if (try_vis) {
				if (first_valid == -1) first_valid = i;

				#define REJECT(name) \
					{ \
						int value = 0; \
						if (glXGetFBConfigAttrib(dpy, cs[i], name, &value) != Success) { \
							fprintf(stderr, "glXGetFBConfigAttrib failed for " #name " \n"); \
							exit(1); \
						} \
						if (value > 0) { \
							XFree(try_vis); \
							continue; \
						} \
					}
				REJECT(GLX_SAMPLE_BUFFERS);
				REJECT(GLX_SAMPLES);
				REJECT(GLX_ACCUM_RED_SIZE);
				REJECT(GLX_ACCUM_GREEN_SIZE);
				REJECT(GLX_ACCUM_BLUE_SIZE);
				REJECT(GLX_ACCUM_ALPHA_SIZE);
				#undef REJECT

				// not rejected? pick it!
				vis = try_vis;
				fb_config = cs[i];
				break;
			}
		}

		if (vis == NULL) {
			if (first_valid == -1) {
				fprintf(stderr, "found no visual\n");
				exit(1);
			} else {
				vis = glXGetVisualFromFBConfig(dpy, cs[first_valid]);
				fb_config = cs[first_valid];
			}
		}

		XFree(cs);
	}

	/* create gl context */
	ctx = 0;
	{
		PFNGLXCREATECONTEXTATTRIBSARBPROC create_context =
			(PFNGLXCREATECONTEXTATTRIBSARBPROC)
			glXGetProcAddressARB((const GLubyte*)"glXCreateContextAttribsARB");
		if (!create_context) {
			fprintf(stderr, "failed to get proc address for glXCreateContextAttribsARB\n");
			exit(1);
		}

		const char *extensions = glXQueryExtensionsString(
			dpy,
			DefaultScreen(dpy));

		if (!is_extension_supported(extensions, "GLX_ARB_create_context")) {
			fprintf(stderr, "GLX_ARB_create_context not supported\n");
			exit(1);
		}

		int (*old_handler)(Display*, XErrorEvent*) = XSetErrorHandler(&tmp_ctx_error_handler);

		int attrs[] = {
			GLX_CONTEXT_MAJOR_VERSION_ARB, 3,
			GLX_CONTEXT_MINOR_VERSION_ARB, 0,
			None
		};

		ctx = create_context(
			dpy,
			fb_config,
			0,
			True,
			attrs);

		XSync(dpy, False);

		if (!ctx || tmp_ctx_error) {
			fprintf(stderr, "could not create opengl context\n");
			exit(1);
		}

		XSetErrorHandler(old_handler);
	}

	cursor_default = XCreateFontCursor(dpy, XC_left_ptr);
	cursor_horiz = XCreateFontCursor(dpy, XC_sb_h_double_arrow);
	cursor_vert = XCreateFontCursor(dpy, XC_sb_v_double_arrow);
	cursor_cross = XCreateFontCursor(dpy, XC_fleur);

	int exit_status = lsl_main(argc, argv);

	XDefineCursor(dpy, RootWindow(dpy, vis->screen), cursor_default);

	XCloseDisplay(dpy);

	return exit_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

