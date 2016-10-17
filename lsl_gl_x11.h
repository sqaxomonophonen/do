/*
#if __STDC_VERSION__ >= 199901L
#define _XOPEN_SOURCE 600
#else
#define _XOPEN_SOURCE 500
#endif
*/

#define GL_GLEXT_PROTOTYPES
#include <GL/glx.h>

// TODO noget ala https://github.com/ApoorvaJ/Papaya/blob/master/src/libs/gl_lite.h?

#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/cursorfont.h>

Display* dpy;
XVisualInfo* vis;
GLXContext ctx;
XIM xim;

int tmp_ctx_error;
Cursor cursor_default;
Cursor cursor_horiz;
Cursor cursor_vert;
Cursor cursor_cross;
Cursor cursor_touch;

Cursor current_cursor;
Cursor last_cursor;

#define MAX_WIN (32)

struct win {
	int open;
	int(*proc)(void*);
	void* usr;
	Window window;
	XIC xic;
	struct wglobal wglobal;
	struct wframe wframe_init;
} wins[MAX_WIN];
struct win* current_win;

static struct wglobal* wglobal_get()
{
	assert(current_win != NULL);
	return &current_win->wglobal;
}

char exe_path[8192];
size_t exe_path_sz;
int lsl_relpath(char* buffer, size_t buffer_sz, char* relpath)
{
	size_t relpath_sz = strlen(relpath);
	size_t required_sz = exe_path_sz + 1 + relpath_sz + 1;
	if (required_sz >= buffer_sz) {
		return -1;
	}

	char sep = '/';
	memcpy(buffer, exe_path, exe_path_sz);
	buffer[exe_path_sz] = sep;
	memcpy(buffer + exe_path_sz + 1, relpath, relpath_sz);
	buffer[exe_path_sz + 1 + relpath_sz] = 0;

	return 0;
}


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
		lw = NULL;
	}
	assert(lw != NULL);

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

static void handle_text_event(struct wglobal* wg, char* text, int length)
{
	if (length <= 0) return;
	if ((wg->text_length + length) >= LSL_MAX_TEXT_LENGTH) return;
	memcpy(wg->text + wg->text_length, text, length);
	wg->text_length += length;
	wg->text[wg->text_length] = 0;
}

static void handle_key_event(XKeyEvent* e, struct win* lw)
{
	struct wglobal* wg = &lw->wglobal;

	KeySym sym = XkbKeycodeToKeysym(dpy, e->keycode, 0, 0);
	int mask = 0;
	int is_keypress = e->type == KeyPress;
	switch (sym) {
		case XK_Return:
			if (is_keypress) {
				// XLookupString would give "\r" :-/
				handle_text_event(wg, "\n", 1);
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
			wg->mod |= mask;
		} else {
			wg->mod &= ~mask;
		}
	} else if (is_keypress) {
		char buf[16];
		int len = Xutf8LookupString(lw->xic, e, buf, sizeof(buf), NULL, NULL);
		if (len > 0) {
			handle_text_event(wg, buf, len);
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
		case LSL_POINTER_TOUCH:
			c = cursor_touch;
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

	gl_init();

	for (;;) {
		while (XPending(dpy)) {
			XEvent xe;
			XNextEvent(dpy, &xe);
			Window w = xe.xany.window;
			if (XFilterEvent(&xe, w)) continue;

			struct win* lw = wlookup(w);
			if (lw == NULL) continue;

			struct wframe* wf = &lw->wframe_init;
			struct wglobal* wg = &lw->wglobal;

			switch (xe.type) {
				case EnterNotify:
					wf->minside = 1;
					wf->mpos.x = xe.xcrossing.x;
					wf->mpos.y = xe.xcrossing.y;
					break;
				case LeaveNotify:
					wf->minside = 0;
					wf->mpos.x = 0;
					wf->mpos.y = 0;
					break;
				case ButtonPress:
				case ButtonRelease:
				{
					int i = xe.xbutton.button - 1;
					if (i >= 0 && i < LSL_MAX_BUTTONS) {
						wg->button[i] = xe.type == ButtonPress;
						wg->button_cycles[i]++;
					}
				}
				break;
				case MotionNotify:
					wf->minside = 1;
					wf->mpos.x = xe.xmotion.x;
					wf->mpos.y = xe.xmotion.y;
					break;
				case KeyPress:
				case KeyRelease:
					handle_key_event(&xe.xkey, lw);
					break;
			}
		}

		current_cursor = cursor_default;

		for (int i = 0; i < MAX_WIN; i++) {
			struct win* w = &wins[i];
			if (!w->open) continue;

			// fetch window dimensions
			struct wframe* wf = &w->wframe_init;
			wf->rect.p0.x = wf->rect.p0.y = 0;
			wf->rect.dim = get_dim_vec2(w->window);

			glXMakeCurrent(dpy, w->window, ctx);

			gl_frame_begin(wf->rect.dim.w, wf->rect.dim.h);

			current_win = w;
			new_frame(wf);

			// run user callback
			int ret = w->proc(w->usr);

			// post frame cleanup
			wglobal_post_frame_reset();
			current_win = NULL;

			gl_draw_flush();

			glXSwapBuffers(dpy, w->window);

			if (ret != 0) {
				return; // XXX or close window?
			}
		}

		if (current_cursor != last_cursor) {
			XDefineCursor(dpy, RootWindow(dpy, vis->screen), current_cursor);
			last_cursor = current_cursor;
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
	{
		// get path of executable
		int n = readlink("/proc/self/exe", exe_path, sizeof(exe_path));
		assert(n != -1);
		for (int i = n-1; i >= 0; i--) {
			if (exe_path[i] == '/') {
				exe_path[i] = 0;
				break;
			}
		}
		exe_path_sz = strlen(exe_path);
	}

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

	/* https://tronche.com/gui/x/xlib/appendix/b/ */
	cursor_default = XCreateFontCursor(dpy, XC_left_ptr);
	cursor_horiz = XCreateFontCursor(dpy, XC_sb_h_double_arrow);
	cursor_vert = XCreateFontCursor(dpy, XC_sb_v_double_arrow);
	cursor_cross = XCreateFontCursor(dpy, XC_fleur);
	cursor_touch = XCreateFontCursor(dpy, XC_hand1);

	int exit_status = lsl_main(argc, argv);

	XDefineCursor(dpy, RootWindow(dpy, vis->screen), cursor_default);

	XCloseDisplay(dpy);

	return exit_status == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

