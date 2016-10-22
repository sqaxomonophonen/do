#ifndef LSL_PRG_H

/*
lsl_prg.h: application framework (windowing, drawing, input)
*/

#include "m.h"
#include "atls.h"

#define LSL_MAX_BUTTONS (5)
#define LSL_MAX_TEXT_LENGTH (31)

#define LSL_MOD_LSHIFT (1<<0)
#define LSL_MOD_RSHIFT (1<<1)
#define LSL_MOD_SHIFT  (LSL_MOD_LSHIFT | LSL_MOD_RSHIFT)
#define LSL_MOD_LCTRL  (1<<2)
#define LSL_MOD_RCTRL  (1<<3)
#define LSL_MOD_CTRL   (LSL_MOD_LCTRL | LSL_MOD_RCTRL)
#define LSL_MOD_LALT   (1<<4)
#define LSL_MOD_RALT   (1<<5)
#define LSL_MOD_ALT    (LSL_MOD_LALT | LSL_MOD_RALT)

#define LSL_LMB        (1<<6)
#define LSL_MMB        (1<<7)
#define LSL_RMB        (1<<8)

#define LSL_PRESS      (1<<9)
#define LSL_ACTIVE     (1<<10)
#define LSL_CLICK      (1<<11)
#define LSL_CANCEL     (1<<12)
#define LSL_RELEASE    (1<<13)
#define LSL_DRAGGED    (1<<14)

// some ASCII-fyable keys
#define LSLK_DEL (127)

/* (ab-)using the reserved unicode/latin1 0x80-09f range for special keys I
 * want to capture with lsl_getch() */
#define LSLK_S0 (0x80)
#define LSLK_ARROW_LEFT  (LSLK_S0 + 0)
#define LSLK_ARROW_RIGHT (LSLK_S0 + 1)
#define LSLK_ARROW_UP    (LSLK_S0 + 2)
#define LSLK_ARROW_DOWN  (LSLK_S0 + 3)
#define LSLK_ARROW_DOWN  (LSLK_S0 + 3)
#define LSLK_INSERT      (LSLK_S0 + 4)
#define LSLK_HOME        (LSLK_S0 + 5) // XXX or use STX/ETX? ascii codes 3/4? they mean "start/end of text"
#define LSLK_END         (LSLK_S0 + 6)
#define LSLK_PGUP        (LSLK_S0 + 7)
#define LSLK_PGDN        (LSLK_S0 + 8)

#define LSLK_F1          (LSLK_S0 + 9)
#define LSLK_F2          (LSLK_F1 + 1)
#define LSLK_F3          (LSLK_F1 + 2)
#define LSLK_F4          (LSLK_F1 + 3)
#define LSLK_F5          (LSLK_F1 + 4)
#define LSLK_F6          (LSLK_F1 + 5)
#define LSLK_F7          (LSLK_F1 + 6)
#define LSLK_F8          (LSLK_F1 + 7)
#define LSLK_F9          (LSLK_F1 + 8)
#define LSLK_F10         (LSLK_F1 + 9)
#define LSLK_F11         (LSLK_F1 + 10)
#define LSLK_F12         (LSLK_F1 + 11)


int lsl_getch();

int lsl_main(int argc, char** argv);

int lsl_relpath(char* buffer, size_t buffer_sz, char* relpath);

void lsl_set_atls(struct atls*);
void lsl_set_type_index(unsigned int index);
int lsl_get_text_width(char* str, int n);
int lsl_get_text_height();
struct atls_cell_table* lsl_set_cell_table(unsigned int index);
void lsl_cell_plot(int column, int row, int x, int y, int width, int height);
void lsl_set_cursor(int x, int y);
union vec4 lsl_eval(int atls_prg_id);
void lsl_set_gradient(union vec4 color0, union vec4 color1);
void lsl_set_color(union vec4 color);
void lsl_putch(int codepoint);
int lsl_write(char* buf, int n);
int lsl_printf(const char* fmt, ...);
void lsl_line(float thickness, union vec2 p0, union vec2 p1);
void lsl_bezier(float thickness, union vec2 p0, union vec2 p1, union vec2 p2, union vec2 p3);
void lsl_fill_rect(struct rect*);
void lsl_clear();
void lsl_win_open(const char* title, int(*proc)(void*), void* usr);
void lsl_main_loop();

void lsl_clip_push(struct rect* r);
void lsl_clip_pop();

void lsl_scope_push_data(const void* data, size_t sz);
void lsl_scope_push_static(const void* ptr);
void lsl_scope_pop();

#define LSL_POINTER_HORIZONTAL (1)
#define LSL_POINTER_VERTICAL (2)
#define LSL_POINTER_4WAY (3)
#define LSL_POINTER_TOUCH (4)
void lsl_set_pointer(int);

int lsl_mpos_vec2(union vec2* mpos);
int lsl_mpos(int* mx, int* my);
int lsl_mdelta_vec2(union vec2* mdelta);
int lsl_mdelta(int* dx, int* dy);
int lsl_press(const char* id, int activatable, int pointer);

#define LSL_PRG_H
#endif
