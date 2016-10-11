#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "lsl_prg.h"
#include "dd.h"
#include "dya.h"

struct atls* atls;

int builtin_ctbl;

int colorpid_background;
int colorpid_builtin_footer_text;
int colorpid_builtin_port_text;
int colorpid_connection_shadow;
int colorpid_connection_signal;

int cvi_modifier;

int type_index_main;
int type_index_subs;


struct {
	struct dd dd;
} state;


enum gsel_type {
	GSEL_NONE = 0,
	GSEL_NODE,
	GSEL_CONN
};

struct gsel {
	enum gsel_type type;
	union {
		u32 node_id;
		struct dd_conn conn;
	};
};


static inline struct gsel gsel_none()
{
	struct gsel s;
	s.type = GSEL_NONE;
	return s;
}

static inline struct gsel gsel_node(u32 node_id)
{
	struct gsel s;
	s.type = GSEL_NODE;
	s.node_id = node_id;
	return s;
}
static inline struct gsel gsel_conn(struct dd_conn conn)
{
	struct gsel s;
	s.type = GSEL_CONN;
	s.conn = conn;
	return s;
}
static int gsel_compar(const void* va, const void* vb)
{
	const struct gsel* a = va;
	const struct gsel* b = vb;

	int d0 = a->type - b->type;
	if (d0 != 0) return d0;

	if (a->type == GSEL_NODE) {
		return a->node_id - b->node_id;
	} else if (a->type == GSEL_CONN) {
		int d1 = a->conn.src_node_id - b->conn.src_node_id;
		if (d1 != 0) return d1;

		int d2 = a->conn.dst_node_id - b->conn.dst_node_id;
		if (d2 != 0) return d2;

		int d3 = a->conn.src_port_id - b->conn.src_port_id;
		if (d3 != 0) return d3;

		int d4 = a->conn.dst_port_id - b->conn.dst_port_id;
		return d4;

	} else {
		assert(!"invalid gsel_type");
	}
}

#define MODAL_NONE (0)
#define MODAL_NODEINSERT (1)


struct graph_view {
	u32 node_id;
	s32 pan_x, pan_y;
};

enum window_type {
	WINDOW_TIMELINE = 1,
	WINDOW_GRAPH
};
struct window {
	struct dya graph_view_stack_dya;
	struct graph_view* graph_view_stack;
	struct graph_view graph_view_root;

	struct {
		struct dd_node* node;
		int pos;
		u16 port_id;
		int drag_state;
	} graph_tmp_conn;

	int modal;

	struct dya gsel_dya;
	struct gsel* gsel;

	enum window_type type;
	struct window* next;
}* windows;

static int window_graph_is_selected(struct window* w, struct gsel s)
{
	return dya_bs_find(&w->gsel_dya, (void**)&w->gsel, gsel_compar, &s) >= 0;
}

static int window_graph_select(struct window* w, struct gsel s)
{
	if (!window_graph_is_selected(w, s)) {
		dya_bs_insert(&w->gsel_dya, (void**)&w->gsel, gsel_compar, &s);
		return 1;
	} else {
		return 0;
	}
}

static int window_graph_deselect(struct window* w, struct gsel s)
{
	if (window_graph_is_selected(w, s)) {
		dya_bs_delete(&w->gsel_dya, (void**)&w->gsel, gsel_compar, &s);
		return 1;
	} else {
		return 0;
	}
}

#if 0
static void window_graph_toggle(struct window* w, struct gsel s)
{
	if (!window_graph_select(w, s)) window_graph_deselect(w, s);
}
#endif


static void window_graph_clear_selection(struct window* w)
{
	dya_clear(&w->gsel_dya, (void**)&w->gsel);
}

static void node_box_calc(struct atls_cell_table* clt, struct dd_node* n, int* out_n_in, int* out_n_out, int* w, int* h, int* py0, int* py1)
{
	int n_in, n_out;
	dd_node_nports(n, NULL, &n_in, &n_out);
	if (out_n_in) *out_n_in = n_in;
	if (out_n_out) *out_n_out = n_out;
	int max_ports = n_in > n_out ? n_in : n_out;

	int port_height = clt->heights[2];
	int text_height = lsl_get_text_height();
	int width = clt->widths[0] + lsl_get_text_width(n->def, strlen(n->def)) + clt->widths[2];
	int height = clt->heights[0] + max_ports * port_height + clt->heights[3] + clt->heights[5] + text_height;

	if (w) *w = width;
	if (h) *h = height;

	int h2 = clt->heights[0] + max_ports * port_height + clt->heights[3] + clt->heights[5] + text_height;
	int h3 = h2 - (clt->heights[0] + clt->heights[3] + text_height + clt->heights[6]);
	int y = clt->heights[0];
	int port_y0 = y + (h3 - n_in*port_height)/2;
	int port_y1 = y + (h3 - n_out*port_height)/2;

	if (py0) *py0 = port_y0;
	if (py1) *py1 = port_y1;
}

static inline union vec2 connection_tangent()
{
	return (union vec2) { .x = 40.0f, .y = 0.0f }; // XXX meta?
}

static float connection_distance(union vec2 p0, union vec2 p1, union vec2 p)
{
	union vec2 tangent = connection_tangent();
	union vec2 c0 = vec2_add(p0, tangent);
	union vec2 c1 = vec2_sub(p1, tangent);
	return bezier2_distance(p, p0, c0, c1, p1);
}

static void draw_connection(union vec2 p0, union vec2 p1, int modifier)
{
	atls_ctx_set(atls, cvi_modifier, modifier);

	union vec2 tangent = connection_tangent();
	union vec2 c0 = vec2_add(p0, tangent);
	union vec2 c1 = vec2_sub(p1, tangent);

	lsl_set_color(lsl_eval(colorpid_connection_shadow));
	lsl_bezier(1.2f, p0, c0, c1, p1); // TODO thicknesses -> eval'd?

	lsl_set_color(lsl_eval(colorpid_connection_signal));
	lsl_bezier(0.1f, p0, c0, c1, p1);
}

static inline int encode_pos(int side, int index)
{
	return side == 0 ? index+1 : -index-1;
}

static inline void decode_pos(int pos, int* side, int* index)
{
	if (side) *side = pos < 0;
	if (index) *index = pos > 0 ? pos-1 : -pos-1;
}

static u16 port_id_at(struct dd_node* node, int pos)
{
	int side, index;
	decode_pos(pos, &side, &index);
	struct dd_port_it it = side == 0 ? dd_node_inport_it(node) : dd_node_outport_it(node);
	for (; it.valid; dd_port_it_next(&it)) {
		if (index == 0) return it.id;
		index--;
	}
	assert(!"not found");
}

static int winproc_graph(struct window* w)
{
	struct lsl_frame* top = lsl_frame_top();

	lsl_set_color(lsl_eval(colorpid_background));
	lsl_clear();

	// locate graph
	struct dd_graph* graph = &state.dd.root;
	struct graph_view* view = &w->graph_view_root;
	for (int i = 0; i < w->graph_view_stack_dya.n; i++) {
		view = &w->graph_view_stack[i];
		struct dd_node* node = dd_graph_find_node(graph, view->node_id);
		if (node == NULL) break;
		struct dd_graph* next_graph = dd_node_get_graph(node);
		if (next_graph == NULL) break;
		graph = next_graph;
	}

	struct atls_cell_table* clt = lsl_set_cell_table(builtin_ctbl);
	int port_height = clt->heights[2];
	lsl_set_type_index(type_index_subs);

	struct {
		int r2;
		int pos;
		struct dd_node* node;
		int is_near;
	} nearport = { .r2 = -1 };

	// XXX meta?
	const int port_near_r2 = 50;
	const int port_far_r2 = 250;

	for (int i = graph->nodes_dya.n-1; i >= 0; i--) {
		struct dd_node* node = &graph->nodes[i];

		int n_in, n_out, width, height, port_y0, port_y1;
		node_box_calc(clt, node, &n_in, &n_out, &width, &height, &port_y0, &port_y1);

		s32 rx = top->mpos.x - (node->x - view->pan_x);
		s32 ry = top->mpos.y - (node->y - view->pan_y);

		int max_n = n_in > n_out ? n_in : n_out;
		for (int i = 0; i < max_n; i++) {
			for (int side = 0; side < 2; side++) {
				if ((side == 0 && i >= n_in) || (side == 1 && i >= n_out)) continue;
				int cx = side == 0 ? (clt->widths[0]/2) : (width - clt->widths[2]/2);
				int cy = (side == 0 ? port_y0 : port_y1) + i*port_height + port_height/2;

				int dx = cx - rx;
				int dy = cy - ry;

				int r2 = dx*dx + dy*dy;
				if (r2 < port_far_r2) {
					if (nearport.r2 < 0 || r2 < nearport.r2) {
						nearport.r2 = r2;
						nearport.node = node;
						nearport.pos = encode_pos(side, i);
					}
				}
			}
		}
		nearport.is_near = nearport.node != NULL && nearport.r2 < port_near_r2;

		{
			struct rect nr = (struct rect) {
				.p0 = { .x = node->x - view->pan_x , .y = node->y - view->pan_y },
				.dim = { .w = width, .h = height }
			};
			int mpos_in_area = rect_contains_point(&nr, top->mpos) && !nearport.is_near && w->graph_tmp_conn.node == NULL;
			lsl_scope_push_data(&node->id, sizeof(node->id));
			lsl_drag_pos("node", mpos_in_area, LSL_POINTER_4WAY, &node->x, &node->y, 1, 1);
			lsl_scope_pop();
		}
	}

	{
		int can_init_tmp_conn = nearport.node != NULL;
		if (can_init_tmp_conn && !w->graph_tmp_conn.drag_state) {
			u16 port_id = port_id_at(nearport.node, nearport.pos);
			int side;
			decode_pos(nearport.pos, &side, NULL);
			int valid = 0;
			valid |= side == 0 && dd_graph_can_connect(graph, 0, 0, nearport.node->id, port_id);
			valid |= side == 1 && dd_graph_can_connect(graph, nearport.node->id, port_id, 0, 0);
			if (!valid) can_init_tmp_conn = 0;
		}
		if ((w->graph_tmp_conn.drag_state = lsl_drag("tmpcon", can_init_tmp_conn, LSL_POINTER_TOUCH)) != 0) {
			if (w->graph_tmp_conn.drag_state == LSL_DRAG_START) {
				w->graph_tmp_conn.node = nearport.node;
				w->graph_tmp_conn.pos = nearport.pos;
				w->graph_tmp_conn.port_id = port_id_at(nearport.node, nearport.pos);
			}

			assert(w->graph_tmp_conn.node != NULL);
			union vec2 ps[2];
			for (int i = 0; i < 2; i++) {
				struct dd_node* node = NULL;
				int pos;
				if (i == 0) {
					node = w->graph_tmp_conn.node;
					pos = w->graph_tmp_conn.pos;
				} else {
					node = nearport.node;
					pos = nearport.pos;
				}
				if (node == NULL) continue;

				int side, index;
				decode_pos(pos, &side, &index);

				int n_in, n_out, width, height, port_y0, port_y1;
				node_box_calc(clt, node, &n_in, &n_out, &width, &height, &port_y0, &port_y1);
				if (side == 0) {
					ps[i].x = node->x + clt->widths[0]/2;
					ps[i].y = node->y + port_y0 + index*port_height + port_height/2;
				} else {
					ps[i].x = node->x + width - clt->widths[2]/2;
					ps[i].y = node->y + port_y1 + index*port_height + port_height/2;
				}
			}

			int can_connect = 0;
			u32 src_node_id;
			u16 src_port_id;
			u32 dst_node_id;
			u16 dst_port_id;
			if (nearport.node != NULL) {
				int side0, index0, side1, index1;
				decode_pos(w->graph_tmp_conn.pos, &side0, &index0);
				decode_pos(nearport.pos, &side1, &index1);
				if (side0 != side1) {
					u32 n0 = w->graph_tmp_conn.node->id;
					u16 p0 = w->graph_tmp_conn.port_id;
					u32 n1 = nearport.node->id;
					u16 p1 = port_id_at(nearport.node, nearport.pos);
					if (side0 == 0) {
						src_node_id = n1;
						src_port_id = p1;
						dst_node_id = n0;
						dst_port_id = p0;
					} else {
						src_node_id = n0;
						src_port_id = p0;
						dst_node_id = n1;
						dst_port_id = p1;
					}
					can_connect = dd_graph_can_connect(graph, src_node_id, src_port_id, dst_node_id, dst_port_id);
				}
			}

			union vec2 pan = {
				.x = -view->pan_x,
				.y = -view->pan_y
			};

			union vec2 cp = vec2_add(pan, ps[0]);
			union vec2 ep = can_connect ? vec2_add(pan, ps[1]) : top->mpos;

			int modifier = can_connect ? 3 : 2;
			if (w->graph_tmp_conn.pos > 0) {
				draw_connection(ep, cp, modifier);
			} else {
				draw_connection(cp, ep, modifier);
			}

			if (w->graph_tmp_conn.drag_state == LSL_DRAG_STOP) {
				w->graph_tmp_conn.node = NULL;
				if (can_connect) {
					dd_graph_connect(graph, src_node_id, src_port_id, dst_node_id, dst_port_id);
				}
			}
		}
	}

	// handle connections
	struct gsel nearest = gsel_none();
	float nearest_distance = 0;
	struct dd_conn* next_selection = NULL;
	for (int pass = 0; pass < 2; pass++) {
		int n_shifty_sel = 0;
		int n_shifty_total = 0;
		int grab_next = 0;
		struct dd_conn* first = NULL;
		int n_subjects = 0;

		for (int i = 0; i < graph->conns_dya.n; i++) {
			struct dd_conn* c = &graph->conns[i];
			struct dd_node* src_node = dd_graph_find_node(graph, c->src_node_id);
			struct dd_node* dst_node = dd_graph_find_node(graph, c->dst_node_id);
			assert(src_node != NULL);
			assert(dst_node != NULL);

			int dy0 = 0;
			for (struct dd_port_it it = dd_node_inport_it(dst_node); it.valid; dd_port_it_next(&it)) {
				if (it.id == c->dst_port_id) break;
				dy0++;
			}
			int dy1 = 0;
			for (struct dd_port_it it = dd_node_outport_it(src_node); it.valid; dd_port_it_next(&it)) {
				if (it.id == c->src_port_id) break;
				dy1++;
			}

			int src_node_width, py0, py1;
			node_box_calc(clt, src_node, NULL, NULL, &src_node_width, NULL, NULL, &py0);
			node_box_calc(clt, dst_node, NULL, NULL, NULL,            NULL, &py1, NULL);

			const int pcx = 3; // XXX metadata?
			const int pcy = 5; // XXX metadata?

			s32 sx0 = src_node->x - view->pan_x + src_node_width - pcx;
			s32 sy0 = src_node->y - view->pan_y + pcy + py0 + dy1 * port_height;
			s32 sx1 = dst_node->x - view->pan_x + pcx;
			s32 sy1 = dst_node->y - view->pan_y + pcy + py1 + dy0 * port_height;

			union vec2 p0 = (union vec2) { .x = sx0, .y = sy0 };
			union vec2 p1 = (union vec2) { .x = sx1, .y = sy1 };

			float distance = connection_distance(p0, p1, top->mpos);
			const float min_distance = 16.0; // XXX config?

			struct gsel subject = gsel_conn(*c);

			if (pass == 0) {
				if (distance < min_distance) {
					if (lsl_shift_click()) {
						n_shifty_sel += window_graph_select(w, subject);
						n_shifty_total++;
					} else if (lsl_click()) {
						if (first == NULL) first = c;
						n_subjects++;
						if (nearest.type == GSEL_NONE || distance < nearest_distance) {
							nearest = subject;
							nearest_distance = distance;
						}
						if (grab_next) {
							next_selection = c;
							grab_next = 0;
						}
						if (window_graph_is_selected(w, subject)) {
							grab_next = 1;
						}
					}
				}

				draw_connection(p0, p1, window_graph_is_selected(w, subject));
			} else if (pass == 1) {
				if (distance < min_distance) {
					if (next_selection != NULL) {
						if (next_selection == c) {
							window_graph_select(w, subject);
						} else {
							window_graph_deselect(w, subject);
						}
					} else {
						window_graph_deselect(w, subject);
					}
				}
			}
		}

		if (grab_next) next_selection = first;

		if (lsl_click()) {
			if (next_selection) {
				if (n_subjects == 1) {
					window_graph_deselect(w, gsel_conn(*next_selection));
				} else {
					continue;
				}
			} else {
				window_graph_clear_selection(w);
				window_graph_select(w, nearest);
			}
		} else if (lsl_shift_click() && n_shifty_sel == 0 && n_shifty_total > 0) {
			continue;
		}
		break;
	}

	// draw nodes
	for (int i = 0; i < graph->nodes_dya.n; i++) {
		struct dd_node* n = &graph->nodes[i];

		int n_in, n_out, width, height, port_y0, port_y1;
		node_box_calc(clt, n, &n_in, &n_out, &width, &height, &port_y0, &port_y1);

		s32 sx = n->x - view->pan_x;
		s32 sy = n->y - view->pan_y;

		struct rect r = (struct rect) {
			.p0 = { .x = sx , .y = sy },
			.dim = { .w = width, .h = height }
		};

		int text_height = lsl_get_text_height();

		for (int row = 0; row < 6; row++) {
			for (int column = 0; column < 3; column++) {
				int x;
				int y;
				int w = 0;
				int h = 0;
				if (column == 0) {
					x = sx;
				} else if (column == 1) {
					x = sx + clt->widths[0];
					w = r.dim.w - (clt->widths[0] + clt->widths[2]);
				} else if (column == 2) {
					x = sx + r.dim.w - clt->widths[2];
				}

				int crow;
				if (row == 0) {
					y = sy;
					crow = 0;
				} else if (row == 1) {
					y = sy + clt->heights[0];
					h = r.dim.h - (clt->heights[0] + clt->heights[3] + text_height + clt->heights[6]);
					crow = 1;
				} else if (row == 2) {
					y = sy + r.dim.h - (clt->heights[3] + text_height + clt->heights[6]);
					crow = 3;
				} else if (row == 3) {
					y = sy + r.dim.h - (text_height + clt->heights[6]);
					h = text_height - clt->heights[5];
					crow = 4;
				} else if (row == 4) {
					y = sy + r.dim.h - (clt->heights[5] + clt->heights[6]);
					crow = 5;
				} else if (row == 5) {
					y = sy + r.dim.h - clt->heights[6];
					crow = 6;
				}

				if (row == 1 && (column == 0 || column == 2)) {
					int n = column == 0 ? n_in : n_out;
					int y1 = column == 0 ? port_y0 : port_y1;
					int margin = y1 - clt->heights[0];
					lsl_cell_plot(column, crow, x, y, w, margin);
					for (int i = 0; i < n; i++) {
						lsl_cell_plot(column, 2, x, sy + y1, w, port_height);
						y1 += port_height;
					}
					lsl_cell_plot(column, crow, x, sy + y1, w, h - (margin + port_height*n));
				} else {
					lsl_cell_plot(column, crow, x, y, w, h);
				}
			}
		}


		lsl_set_color(lsl_eval(colorpid_builtin_footer_text));
		lsl_set_cursor(sx + clt->widths[0], sy + r.dim.h - (clt->heights[6]));
		lsl_printf("%s", n->def);

		lsl_set_color(lsl_eval(colorpid_builtin_port_text));
		int i = 0;
		const int port_text_spacing = 3; // XXX use metadata?
		const int port_text_dy = -2; // XXX use metadata?
		for (struct dd_port_it it = dd_node_inport_it(n); it.valid; dd_port_it_next(&it)) {
			lsl_set_cursor(sx + clt->widths[0] + port_text_spacing, sy + port_y0 + i*port_height + text_height + port_text_dy);
			lsl_write(it.name, it.name_len);
			i++;
		}
		i = 0;
		for (struct dd_port_it it = dd_node_outport_it(n); it.valid; dd_port_it_next(&it)) {
			int txtw = lsl_get_text_width(it.name, it.name_len);
			lsl_set_cursor(sx + width - (clt->widths[0] + txtw + port_text_spacing), sy + port_y1 + i*port_height + text_height + port_text_dy);
			lsl_write(it.name, it.name_len);
			i++;
		}
	}

	// pan drag
	lsl_drag_pos("pan", 1, 0, &view->pan_x, &view->pan_y, -1, -1);

	if (lsl_accept('\x7f')) { // 127=DEL
		for (int i = 0; i < w->gsel_dya.n; i++) {
			struct gsel s = w->gsel[i];
			switch (s.type) {
			case GSEL_NONE:
				break;
			case GSEL_NODE:
				dd_graph_delete_node(graph, s.node_id);
				break;
			case GSEL_CONN:
				dd_graph_disconnect(graph,
					s.conn.src_node_id, s.conn.src_port_id,
					s.conn.dst_node_id, s.conn.dst_port_id);
				break;
			}
		}
		window_graph_clear_selection(w);
	}

	if (w->modal == MODAL_NONE && lsl_accept(' ')) w->modal = MODAL_NODEINSERT;

	if (w->modal == MODAL_NODEINSERT) {
	}

	return top->button[2];
}

static int winproc(void* usr)
{
	atls_enter_ctx(atls);

	struct window* win = (struct window*) usr;
	//struct lsl_frame* f = lsl_frame_top();

	int retval = 0;
	switch (win->type) {
	case WINDOW_TIMELINE:
		break;
	case WINDOW_GRAPH:
		retval = winproc_graph(win);
		break;
	}

	atls_leave_ctx(atls);

	return retval;
}

static struct window* clone_win(struct window* ow)
{
	struct window* w = malloc(sizeof(*w));
	assert(w != NULL);
	if (ow == NULL) {
		// new window
		memset(w, 0, sizeof(*w));
		dya_init(&w->graph_view_stack_dya, (void**)&w->graph_view_stack, sizeof(*w->graph_view_stack), 0);
		w->type = WINDOW_GRAPH;
	} else {
		// clone window
		memcpy(w, ow, sizeof(*w));
		dya_clone(
			&w->graph_view_stack_dya, (void**)&w->graph_view_stack,
			&ow->graph_view_stack_dya, (void**)&ow->graph_view_stack);
	}

	// not ever copied
	dya_init(&w->gsel_dya, (void**)&w->gsel, sizeof(*w->gsel), 0);

	w->next = windows;
	windows = w;
	lsl_win_open("do", winproc, w);
	return w;
}

static struct atls* load_atlas(char* path)
{
	struct atls* atls = atls_load_from_file("default.atls");
	if (atls == NULL) {
		fprintf(stderr, "%s\n", atls_get_error());
		assert(!"atls_load_from_file");
	}
	lsl_set_atls(atls);

	atls_use_colorscheme(atls, "default");
	assert(atls->active_colorscheme != NULL);

	assert((type_index_main = atls_get_glyph_table_index(atls, "main")) >= 0);
	assert((type_index_subs = atls_get_glyph_table_index(atls, "subs")) >= 0);

	builtin_ctbl = atls_get_cell_table_index(atls, "builtin");

	colorpid_background = atls_get_prg_id(atls, "background");
	colorpid_builtin_footer_text = atls_get_prg_id(atls, "builtin.footer_text");
	colorpid_builtin_port_text = atls_get_prg_id(atls, "builtin.port_text");
	colorpid_connection_shadow = atls_get_prg_id(atls, "connection.shadow");
	colorpid_connection_signal = atls_get_prg_id(atls, "connection.signal"); // XXX is type a ctxvar?

	cvi_modifier = atls_get_ctxkey_id(atls, "modifier");

	return atls;
}

int lsl_main(int argc, char** argv)
{
	dd_init(&state.dd);

	{
		// XXX some debug graph
		struct dd_graph* g = &state.dd.root;
		struct dd_node* n1 = dd_graph_new_node(g, "sin");
		struct dd_node* n2 = dd_graph_new_node(g, "cos");
		struct dd_node* n3 = dd_graph_new_node(g, "-");
		assert(n1 != NULL);
		assert(n2 != NULL);
		assert(n3 != NULL);
		dd_graph_connect(g, n1->id, 1, n2->id, 1);
		dd_graph_connect(g, n1->id, 1, n3->id, 1);
		dd_graph_connect(g, n2->id, 1, n3->id, 2);

		struct dd_node* n4 = dd_graph_new_node(g, "[poly]32saw");
		assert(n4 != NULL);
		struct dd_graph* g2 = dd_node_get_graph(n4);
		dd_graph_new_node(g2, "in-");
		dd_graph_new_node(g2, "-out");

		dd_graph_connect(g, n3->id, 1, n4->id, 1);
	}

	atls = load_atlas("default.atls");

	clone_win(NULL);
	lsl_main_loop();

	return 0;
}
