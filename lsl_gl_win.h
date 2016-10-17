#include <windows.h>
#include <GL/gl.h>


/*
** glext stuff I need, lifted from gl3w, thanks! (https://github.com/skaslev/gl3w)
*/

typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef char GLchar;
typedef short GLshort;
typedef signed char GLbyte;
typedef unsigned short GLushort;

#define GL_COMPILE_STATUS                 0x8B81
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_LINK_STATUS                    0x8B82
#define GL_ARRAY_BUFFER                   0x8892
#define GL_STREAM_DRAW                    0x88E0
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_CLAMP_TO_BORDER                0x812D

typedef GLuint LGLCreateShader (GLenum type);
typedef void LGLShaderSource (GLuint shader, GLsizei count, const GLchar *const*string, const GLint *length);
typedef void LGLCompileShader (GLuint shader);
typedef void LGLGetShaderiv (GLuint shader, GLenum pname, GLint *params);
typedef void LGLGetShaderInfoLog (GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef GLuint LGLCreateProgram (void);
typedef void LGLAttachShader (GLuint program, GLuint shader);
typedef void LGLLinkProgram (GLuint program);
typedef void LGLGetProgramiv (GLuint program, GLenum pname, GLint *params);
typedef void LGLGetProgramInfoLog (GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
typedef void LGLDeleteShader (GLuint shader);
typedef GLint LGLGetUniformLocation (GLuint program, const GLchar *name);
typedef GLint LGLGetAttribLocation (GLuint program, const GLchar *name);
typedef void LGLGenBuffers (GLsizei n, GLuint *buffers);
typedef void LGLGenVertexArrays (GLsizei n, GLuint *arrays);
typedef void LGLBindVertexArray (GLuint array);
typedef void LGLBindBuffer (GLenum target, GLuint buffer);
typedef void LGLBufferData (GLenum target, GLsizeiptr size, const void *data, GLenum usage);
typedef void LGLEnableVertexAttribArray (GLuint index);
typedef void LGLVertexAttribPointer (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void *pointer);
typedef void LGLUseProgram (GLuint program);
typedef void LGLUniform1i (GLint location, GLint v0);
typedef void LGLUniform2f (GLint location, GLfloat v0, GLfloat v1);
typedef void LGLBufferSubData (GLenum target, GLintptr offset, GLsizeiptr size, const void *data);

static LGLCreateShader* glCreateShader;
static LGLShaderSource* glShaderSource;
static LGLCompileShader* glCompileShader;
static LGLGetShaderiv* glGetShaderiv;
static LGLGetShaderInfoLog* glGetShaderInfoLog;
static LGLCreateProgram* glCreateProgram;
static LGLAttachShader* glAttachShader;
static LGLLinkProgram* glLinkProgram;
static LGLGetProgramiv* glGetProgramiv;
static LGLGetProgramInfoLog* glGetProgramInfoLog;
static LGLDeleteShader* glDeleteShader;
static LGLGetUniformLocation* glGetUniformLocation;
static LGLGetAttribLocation* glGetAttribLocation;
static LGLGenBuffers* glGenBuffers;
static LGLGenVertexArrays* glGenVertexArrays;
static LGLBindVertexArray* glBindVertexArray;
static LGLBindBuffer* glBindBuffer;
static LGLBufferData* glBufferData;
static LGLEnableVertexAttribArray* glEnableVertexAttribArray;
static LGLVertexAttribPointer* glVertexAttribPointer;
static LGLUseProgram* glUseProgram;
static LGLUniform1i* glUniform1i;
static LGLUniform2f* glUniform2f;
static LGLBufferSubData* glBufferSubData;


#if 0
static void die(const char* msg)
{
	MessageBoxA(0, "hello", "world", MB_OK|MB_ICONINFORMATION);
	fprintf(stderr, msg);
	abort();
}
#endif

HCURSOR current_cursor;
HCURSOR cursor_default;
HCURSOR cursor_sizeall;
HCURSOR cursor_hand;
HCURSOR cursor_sizens;
HCURSOR cursor_sizewe;

WNDCLASSA win_class;

#define MAX_WIN (32)
struct win {
	int open;
	int(*proc)(void*);
	void* usr;
	HWND hwnd;
	HDC hdc;
	HGLRC glrc;
	struct wglobal wglobal;
	struct wframe wframe_init;
} wins[MAX_WIN];
struct win* current_win;

static struct wglobal* wglobal_get()
{
	assert(current_win != NULL);
	return &current_win->wglobal;
}

static struct win* get_win_by_hwnd(HWND hwnd)
{
	for (int i = 0; i < MAX_WIN; i++) {
		struct win* w = &wins[i];
		if (!w->open || w->hwnd != hwnd) continue;
		return w;
	}
	return NULL;
}


static void* gl_get_proc_address(const char* name)
{
	void* proc = wglGetProcAddress(name);
	if (proc == NULL) {
		int err = GetLastError();
		fprintf(stderr, "wglGetProcAddress: %d\n", err);
		abort();
	}
	return proc;
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

	char sep = '\\';
	memcpy(buffer, exe_path, exe_path_sz);
	buffer[exe_path_sz] = sep;
	memcpy(buffer + exe_path_sz + 1, relpath, relpath_sz);
	buffer[exe_path_sz + 1 + relpath_sz] = 0;

	for (int i = exe_path_sz; i < (exe_path_sz + 1 + relpath_sz); i++) {
		if (buffer[i] == '/') buffer[i] = sep;
	}

	return 0;
}

static void handle_mouse_button(struct wglobal* wg, int button, int pressed)
{
	wg->button[button] = pressed;
	wg->button_cycles[button]++;
}

static int handle_mouse_msg(MSG* msg)
{
	struct win* w = get_win_by_hwnd(msg->hwnd);
	if (w == NULL) return 0;

	struct wglobal* wg = &w->wglobal;
	struct wframe* wf = &w->wframe_init;

	switch (msg->message) {
	case WM_LBUTTONDOWN:
		handle_mouse_button(wg, 0, 1);
		break;
	case WM_LBUTTONUP:
		handle_mouse_button(wg, 0, 0);
		break;
	case WM_RBUTTONDOWN:
		handle_mouse_button(wg, 2, 1);
		break;
	case WM_RBUTTONUP:
		handle_mouse_button(wg, 2, 0);
		break;
	case WM_MBUTTONDOWN:
		handle_mouse_button(wg, 1, 1);
		break;
	case WM_MBUTTONUP:
		handle_mouse_button(wg, 1, 0);
		break;

	case WM_MOUSEMOVE:
		wf->minside = 1;
		wf->mpos.x = (signed short)(msg->lParam);
		wf->mpos.y = (signed short)(msg->lParam >> 16);
		break;

	default:
		return 0;
	}

	return 1;
}

static void handle_keyboard_character_code(struct wglobal* wg, int cc)
{
	if (wg->text_length >= LSL_MAX_TEXT_LENGTH) return;
	wg->text[wg->text_length++] = cc; // XXX FIXME handle non-ascii
}

static int handle_key(struct wglobal* wg, MSG* msg)
{
	int down = msg->message == WM_KEYDOWN;

	switch (msg->wParam) {
	case VK_DELETE:
		if (down) handle_keyboard_character_code(wg, 127);
		return 1;

	}

	return 0;
}

static int handle_keyboard_input(MSG* msg)
{
	struct win* w = get_win_by_hwnd(msg->hwnd);
	if (w == NULL) return 0;

	struct wglobal* wg = &w->wglobal;

	switch (msg->message) {
	case WM_KEYDOWN:
	case WM_KEYUP:
		return handle_key(wg, msg);
	case WM_CHAR:
		if (msg->wParam > 0 && msg->wParam < 0x10000) {
			handle_keyboard_character_code(wg, (unsigned short)msg->wParam);
		}
		break;
	default:
		return 0;
	}

	return 1;
}

static void handle_modifier_key(struct wglobal* wg, int key_state, int mod_mask)
{
	if (GetKeyState(key_state) & 0x8000) {
		wg->mod |= mod_mask;
	} else {
		wg->mod &= ~mod_mask;
	}
}

static void handle_modifier_keys(struct wglobal* wg)
{
	handle_modifier_key(wg, VK_LSHIFT, LSL_MOD_LSHIFT);
	handle_modifier_key(wg, VK_RSHIFT, LSL_MOD_RSHIFT);
	handle_modifier_key(wg, VK_LCONTROL, LSL_MOD_LCTRL);
	handle_modifier_key(wg, VK_RCONTROL, LSL_MOD_RCTRL);
	handle_modifier_key(wg, VK_MENU, LSL_MOD_LALT);
}

void lsl_main_loop()
{
	{
		glCreateShader = (LGLCreateShader*) gl_get_proc_address("glCreateShader");
		glShaderSource = (LGLShaderSource*) gl_get_proc_address("glShaderSource");
		glCompileShader = (LGLCompileShader*) gl_get_proc_address("glCompileShader");
		glGetShaderiv = (LGLGetShaderiv*) gl_get_proc_address("glGetShaderiv");
		glGetShaderInfoLog = (LGLGetShaderInfoLog*) gl_get_proc_address("glGetShaderInfoLog");
		glCreateProgram = (LGLCreateProgram*) gl_get_proc_address("glCreateProgram");
		glAttachShader = (LGLAttachShader*) gl_get_proc_address("glAttachShader");
		glLinkProgram = (LGLLinkProgram*) gl_get_proc_address("glLinkProgram");
		glGetProgramiv = (LGLGetProgramiv*) gl_get_proc_address("glGetProgramiv");
		glGetProgramInfoLog = (LGLGetProgramInfoLog*) gl_get_proc_address("glGetProgramInfoLog");
		glDeleteShader = (LGLDeleteShader*) gl_get_proc_address("glDeleteShader");
		glGetUniformLocation = (LGLGetUniformLocation*) gl_get_proc_address("glGetUniformLocation");
		glGetAttribLocation = (LGLGetAttribLocation*) gl_get_proc_address("glGetAttribLocation");
		glGenBuffers = (LGLGenBuffers*) gl_get_proc_address("glGenBuffers");
		glGenVertexArrays = (LGLGenVertexArrays*) gl_get_proc_address("glGenVertexArrays");
		glBindVertexArray = (LGLBindVertexArray*) gl_get_proc_address("glBindVertexArray");
		glBindBuffer = (LGLBindBuffer*) gl_get_proc_address("glBindBuffer");
		glBufferData = (LGLBufferData*) gl_get_proc_address("glBufferData");
		glEnableVertexAttribArray = (LGLEnableVertexAttribArray*) gl_get_proc_address("glEnableVertexAttribArray");
		glVertexAttribPointer = (LGLVertexAttribPointer*) gl_get_proc_address("glVertexAttribPointer");
		glUseProgram = (LGLUseProgram*) gl_get_proc_address("glUseProgram");
		glUniform1i = (LGLUniform1i*) gl_get_proc_address("glUniform1i");
		glUniform2f = (LGLUniform2f*) gl_get_proc_address("glUniform2f");
		glBufferSubData = (LGLBufferSubData*) gl_get_proc_address("glBufferSubData");
	}

	gl_init();

	MSG msg = {0};
	while (msg.message != WM_QUIT) {
		if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			int was_handled = 0;

			switch (msg.message) {
			case WM_LBUTTONDOWN:
			case WM_LBUTTONUP:
			case WM_RBUTTONDOWN:
			case WM_RBUTTONUP:
			case WM_MBUTTONDOWN:
			case WM_MBUTTONUP:
			case WM_MOUSEMOVE:
				was_handled = handle_mouse_msg(&msg);
				break;

			case WM_KEYDOWN:
			case WM_KEYUP:
			case WM_CHAR:
				was_handled = handle_keyboard_input(&msg);
				break;
			}

			if (!was_handled) {
				TranslateMessage(&msg);
				DispatchMessage(&msg);
			}

			continue;
		}

		current_cursor = cursor_default;

		for (int i = 0; i < MAX_WIN; i++) {
			struct win* w = &wins[i];
			if (!w->open) continue;

			if (!wglMakeCurrent(w->hdc, w->glrc)) {
				// XXX?
				abort();
			}

			RECT cr;
			GetClientRect(w->hwnd, &cr);
			int width = cr.right - cr.left;
			int height = cr.bottom - cr.top;

			// fetch window dimensions
			struct wframe* wf = &w->wframe_init;
			wf->rect.p0.x = wf->rect.p0.y = 0;
			wf->rect.dim.w = width;
			wf->rect.dim.h = height;

			handle_modifier_keys(&w->wglobal);

			gl_frame_begin(width, height);

			current_win = w;
			new_frame(wf);

			int ret = w->proc(w->usr);

			// post frame cleanup
			wglobal_post_frame_reset();
			current_win = NULL;

			gl_draw_flush();

			SwapBuffers(w->hdc);

			if (ret != 0) {
				PostQuitMessage(0);
			}
		}
	}
}

void lsl_set_pointer(int id)
{
	HCURSOR c = cursor_default;
	switch (id) {
		case LSL_POINTER_HORIZONTAL:
			c = cursor_sizewe;
			break;
		case LSL_POINTER_VERTICAL:
			c = cursor_sizens;
			break;
		case LSL_POINTER_4WAY:
			c = cursor_sizeall;
			break;
		case LSL_POINTER_TOUCH:
			c = cursor_hand;
			break;
	}
	current_cursor = c;
}

static LRESULT WINAPI win_proc(HWND w, UINT msg, WPARAM wParam, LPARAM lParam)
{
	switch (msg) {
	#if 0
	case WM_SYSCOMMAND:
		if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
		break;
	#endif
	case WM_PAINT: {
		PAINTSTRUCT paint;
		BeginPaint(w, &paint);
		EndPaint(w, &paint);
	} break;
	case WM_SETCURSOR:
		SetCursor(current_cursor);
		return 0;
	case WM_DESTROY:
	case WM_CLOSE: // XXX just close window?
		PostQuitMessage(0);
		break;
	}

	return DefWindowProc(w, msg, wParam, lParam);
}

void lsl_win_open(const char* title, int(*proc)(void*), void* usr)
{
	struct win* w = NULL;
	for (int i = 0; i < MAX_WIN; i++) {
		w = &wins[i];
		if (!w->open) {
			w->open = 1;
			w->proc = proc;
			w->usr = usr;
			break;
		}
		w = NULL;
	}
	assert(w != NULL);

	w->hwnd = CreateWindow(
		win_class.lpszClassName,
		title,
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
		NULL,
		NULL,
		GetModuleHandle(NULL),
		NULL);
	assert(w->hwnd != NULL);

	w->hdc = GetDC(w->hwnd);

	PIXELFORMATDESCRIPTOR desired_pixfmt = {0};
	desired_pixfmt.nSize = sizeof(desired_pixfmt);
	desired_pixfmt.nVersion = 1;
	desired_pixfmt.dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW | PFD_DOUBLEBUFFER;
	//desired_pixfmt.dwFlags = PFD_SUPPORT_OPENGL | PFD_DRAW_TO_WINDOW;
	desired_pixfmt.cColorBits = 32;
	desired_pixfmt.cAlphaBits = 8;
	desired_pixfmt.iPixelType = PFD_TYPE_RGBA;
	desired_pixfmt.iLayerType = PFD_MAIN_PLANE;

	int pixfmt_index = ChoosePixelFormat(w->hdc, &desired_pixfmt);
	assert(pixfmt_index != 0);

	PIXELFORMATDESCRIPTOR pixfmt;
	DescribePixelFormat(w->hdc, pixfmt_index, sizeof(pixfmt), &pixfmt);
	SetPixelFormat(w->hdc, pixfmt_index, &pixfmt);

	w->glrc = wglCreateContext(w->hdc);
	assert(wglMakeCurrent(w->hdc, w->glrc));

	ShowWindow(w->hwnd, SW_SHOWDEFAULT);
}

int main(int argc, char** argv)
{
	{
		// get path of executable
		int n = GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));
		assert(n > 0);
		for (int i = n-1; i >= 0; i--) {
			if (exe_path[i] == '\\') {
				exe_path[i] = 0;
				break;
			}
		}
		exe_path_sz = strlen(exe_path);
	}

	{
		current_cursor = cursor_default = LoadCursor(NULL, IDC_ARROW);
		cursor_sizeall = LoadCursor(NULL, IDC_SIZEALL);
		cursor_hand = LoadCursor(NULL, IDC_HAND);
		cursor_sizens = LoadCursor(NULL, IDC_SIZENS);
		cursor_sizewe = LoadCursor(NULL, IDC_SIZEWE);
	}

	win_class.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
	win_class.lpfnWndProc = win_proc;
	win_class.hInstance = GetModuleHandle(NULL);
	win_class.hCursor = current_cursor;
	win_class.lpszClassName = "LSLWindowClass";
	assert(RegisterClassA(&win_class));

	lsl_main(argc, argv);

	return 0;
}
