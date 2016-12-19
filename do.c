#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "lsl.h"
#include "dd.h"
#include "dya.h"
#include "utf8.h"
#include "opt.h"

struct atls* atls;

int builtin_ctbl;
int boxselect_ctbl;

int colorpid_background;
int colorpid_builtin_footer_text;
int colorpid_builtin_port_text;
int colorpid_connection_shadow;
int colorpid_connection_signal;
int colorpid_text;
int colorpid_text_cursor;
int colorpid_text_bg_select;
int colorpid_text_bg;

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


#if 0
static inline struct gsel gsel_none()
{
	struct gsel s;
	s.type = GSEL_NONE;
	return s;
}
#endif

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
		return 0;
	}
}


#define MODE_NODE_INSERT (1)

struct graph_view {
	u32 node_id;
	s32 pan_x, pan_y;
};


enum window_type {
	WINDOW_TIMELINE = 1,
	WINDOW_GRAPH
};
struct window {
	int mode;

	struct lsl_textedit textedit;

	struct dya graph_view_stack_dya;
	struct graph_view* graph_view_stack;
	struct graph_view graph_view_root;

	union vec2 mpos0;

	struct {
		struct dd_node* node;
		int pos;
		u16 port_id;
		int pstate;
	} graph_tmp_conn;

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
	return 0;
}

static void conn_endpoints(struct dd_graph* dg, struct atls_cell_table* clt, union vec2 sub, struct dd_conn* c, union vec2* out_p0, union vec2* out_p1)
{
	struct dd_node* src_node = dd_graph_find_node(dg, c->src_node_id);
	struct dd_node* dst_node = dd_graph_find_node(dg, c->dst_node_id);
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

	int port_height = clt->heights[2];
	s32 sx0 = src_node->x + src_node_width - pcx;
	s32 sy0 = src_node->y + pcy + py0 + dy1 * port_height;
	s32 sx1 = dst_node->x + pcx;
	s32 sy1 = dst_node->y + pcy + py1 + dy0 * port_height;

	union vec2 p0 = (union vec2) { .x = sx0, .y = sy0 };
	p0 = vec2_sub(p0, sub);
	union vec2 p1 = (union vec2) { .x = sx1, .y = sy1 };
	p1 = vec2_sub(p1, sub);

	if (out_p0) *out_p0 = p0;
	if (out_p1) *out_p1 = p1;
}

static void draw_cell_box(struct atls_cell_table* ct, struct rect r)
{
	int x0 = r.p0.x;
	int y0 = r.p0.y;
	int width = r.dim.w;
	int height = r.dim.h;

	int center_w = width - (ct->widths[0] + ct->widths[2]);
	int center_h = height - (ct->heights[0] + ct->heights[2]);

	int y = y0;
	for (int row = 0; row < 3; row++) {
		int h = row == 1 ? center_h : ct->heights[row];
		if (center_h < 0) h -= (((-center_h)/2)+(row==2));
		int x = x0;
		for (int column = 0; column < 3; column++) {
			int w = column == 1 ? center_w : ct->widths[column];
			if (center_w < 0) w -= (((-center_w)/2)+(column==2));
			if (w > 0 && h > 0) lsl_cell_plot(column, row, x, y, w, h);
			x += w;
		}
		y += h;
	}
}

static int winproc_graph(struct window* w)
{
	int do_commit = 0;

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

	{ // nodes; interaction
		int cursor_node_id = -1;
		int cursor_node_is_selected = 0;
		for (int i = graph->nodes_dya.n-1; i >= 0; i--) {
			struct dd_node* node = &graph->nodes[i];

			int n_in, n_out, width, height, port_y0, port_y1;
			node_box_calc(clt, node, &n_in, &n_out, &width, &height, &port_y0, &port_y1);

			int mx,my;
			if (lsl_mpos(&mx, &my)) {
				s32 rx = mx - (node->x - view->pan_x);
				s32 ry = my - (node->y - view->pan_y);

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
			}

			struct rect nr = (struct rect) {
				.p0 = { .x = node->x - view->pan_x , .y = node->y - view->pan_y },
				.dim = { .w = width, .h = height }
			};

			union vec2 mpos;
			int mpos_in_area = lsl_mpos_vec2(&mpos) && rect_contains_point(&nr, mpos) && !nearport.is_near && w->graph_tmp_conn.node == NULL;
			int is_selected = window_graph_is_selected(w, gsel_node(node->id));
			if (mpos_in_area && cursor_node_id == -1) {
				cursor_node_id = node->id;
				cursor_node_is_selected = is_selected;
			}
		}
		int pmask;
		int node_under_cursor = cursor_node_id > 0;
		if ((pmask = lsl_press("nodes", node_under_cursor, LSL_POINTER_4WAY)) != 0) {
			if (pmask & LSL_PRESS && !cursor_node_is_selected) {
				if (!(pmask & LSL_MOD_SHIFT)) {
					window_graph_clear_selection(w);
				}
				window_graph_select(w, gsel_node(cursor_node_id));
			}
			int dx, dy;
			lsl_mdelta(&dx, &dy);
			for (int i = 0; i < graph->nodes_dya.n; i++) {
				struct dd_node* node = &graph->nodes[i];
				if (!window_graph_is_selected(w, gsel_node(node->id))) continue;
				node->x += dx;
				node->y += dy;
			}
		}
	}

	{ // connections; makes new ones
		int can_init_tmp_conn = nearport.node != NULL;
		if (can_init_tmp_conn && !w->graph_tmp_conn.pstate) {
			u16 port_id = port_id_at(nearport.node, nearport.pos);
			int side;
			decode_pos(nearport.pos, &side, NULL);
			int valid = 0;
			valid |= side == 0 && dd_graph_can_connect(graph, 0, 0, nearport.node->id, port_id);
			valid |= side == 1 && dd_graph_can_connect(graph, nearport.node->id, port_id, 0, 0);
			if (!valid) can_init_tmp_conn = 0;
		}
		if ((w->graph_tmp_conn.pstate = lsl_press("tmpcon", can_init_tmp_conn, LSL_POINTER_TOUCH)) != 0) {
			if (w->graph_tmp_conn.pstate & LSL_PRESS) {
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

			union vec2 mpos;
			lsl_mpos_vec2(&mpos);
			union vec2 ep = can_connect ? vec2_add(pan, ps[1]) : mpos;

			int modifier = can_connect ? 3 : 2;
			if (w->graph_tmp_conn.pos > 0) {
				draw_connection(ep, cp, modifier);
			} else {
				draw_connection(cp, ep, modifier);
			}

			if (w->graph_tmp_conn.pstate & LSL_RELEASE) {
				w->graph_tmp_conn.node = NULL;
				if (can_connect) {
					assert(dd_graph_connect(graph, src_node_id, src_port_id, dst_node_id, dst_port_id) == 0);
					do_commit = 1;
				}
			}
		}
	}

	union vec2 view_pan = { .x = view->pan_x, .y = view->pan_y };

	{ // connections; selection
		struct dd_conn* nearest = NULL;
		float nearest_distance = 0;

		int n_selected = 0;
		int n_total = 0;
		int previous_was_selected = 0;
		struct dd_conn* first = NULL;
		struct dd_conn* next_in_line = NULL;

		const float min_distance = 16.0; // XXX config?

		union vec2 mpos;
		if (lsl_mpos_vec2(&mpos)) {
			for (int i = 0; i < graph->conns_dya.n; i++) {
				struct dd_conn* c = &graph->conns[i];
				union vec2 p0,p1;
				conn_endpoints(graph, clt, view_pan, c, &p0, &p1);
				float distance = connection_distance(p0, p1, mpos);
				if (distance < min_distance) {
					n_total++;
					if (previous_was_selected) {
						next_in_line = c;
						previous_was_selected = 0;
					}
					if (window_graph_is_selected(w, gsel_conn(*c))) {
						n_selected++;
						previous_was_selected = 1;
					}
					if (first == NULL) first = c;
					if (nearest == NULL || distance < nearest_distance) {
						nearest = c;
						nearest_distance = distance;
					}
				}
			}
			if (previous_was_selected) next_in_line = first;
		}

		int pmask;
		if ((pmask = lsl_press("connsel", n_total > 0, 0)) != 0) {
			/* XXX if I try to drag a connection, wouldn't I want
			 * it to "fallthrough" to the next lsl_press() handler?
			 * something like
			    if (pmask & LSL_DRAGGED) lsl_fallthrough()
			 * maybe?
			 * */
			int click = pmask & LSL_CLICK;
			int shift = pmask & LSL_MOD_SHIFT;
			if (click && !shift && (next_in_line || nearest)) {
				window_graph_clear_selection(w);
				for (int i = 0; i < graph->conns_dya.n; i++) {
					struct dd_conn* c = &graph->conns[i];
					struct gsel subject = gsel_conn(*c);
					if ((n_selected > 0 && n_total > 1 && next_in_line == c) || (n_selected == 0 && nearest == c)) {
						window_graph_select(w, subject);
					}
				}
			} else if (shift && click && n_total > 0) {
				for (int i = 0; i < graph->conns_dya.n; i++) {
					struct dd_conn* c = &graph->conns[i];
					union vec2 p0,p1;
					conn_endpoints(graph, clt, view_pan, c, &p0, &p1);
					float distance = connection_distance(p0, p1, mpos);
					if (distance >= min_distance) continue;

					struct gsel subject = gsel_conn(*c);
					if (n_selected == n_total) {
						window_graph_deselect(w, subject);
					} else {
						window_graph_select(w, subject);
					}
				}
			}
		}
	}

	// connections; draw
	for (int i = 0; i < graph->conns_dya.n; i++) {
		struct dd_conn* c = &graph->conns[i];
		union vec2 p0,p1;
		conn_endpoints(graph, clt, view_pan, c, &p0, &p1);
		draw_connection(p0, p1, window_graph_is_selected(w, gsel_conn(*c)));
	}

	// nodes; draw
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
		atls_ctx_set(atls, cvi_modifier, window_graph_is_selected(w, gsel_node(n->id)));

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

	{
		int pmask;
		if ((pmask = lsl_press("bg", 1, 0)) != 0) {
			int is_pan = (pmask & LSL_RMB) || ((pmask & LSL_LMB) && (pmask & LSL_MOD_ALT));
			int is_boxselect = (pmask & LSL_LMB) && !is_pan;
			if ((pmask & LSL_CLICK) && !(pmask & LSL_DRAGGED)) {
				window_graph_clear_selection(w);
			} else if (is_pan) {
				int dx, dy;
				lsl_mdelta(&dx, &dy);
				view->pan_x -= dx;
				view->pan_y -= dy;
			} else if (is_boxselect) {
				union vec2 mpos;
				lsl_mpos_vec2(&mpos);
				if (pmask & LSL_PRESS) {
					w->mpos0 = mpos;
				} else if (pmask & LSL_RELEASE) {
					struct rect r0 = rect_from_points(w->mpos0, mpos);
					if (!(pmask & LSL_MOD_SHIFT)) window_graph_clear_selection(w);
					for (int i = 0; i < graph->nodes_dya.n; i++) {
						struct dd_node* node = &graph->nodes[i];
						int width, height;
						node_box_calc(clt, node, NULL, NULL, &width, &height, NULL, NULL);
						struct rect r1 = (struct rect) {
							.p0 = { .x = node->x, .y = node->y },
							.dim = { .w = width, .h = height }
						};
						r1.p0 = vec2_sub(r1.p0, view_pan);
						if (rect_overlaps(&r0, &r1)) window_graph_select(w, gsel_node(node->id));
					}
				}
				struct atls_cell_table* ct = lsl_set_cell_table(boxselect_ctbl);
				draw_cell_box(ct, rect_from_points(w->mpos0, mpos));
			}
		}
	}

	if (w->mode == MODE_NODE_INSERT) {
		struct lsl_textedit* te = &w->textedit;
		int r = lsl_textedit_io(te);
		if (r == LSL_CANCEL || r == LSL_COMMIT) {
			w->mode = 0;
			if (r == LSL_COMMIT) {
				char utf8repr[LSL_TEXTEDIT_BUFSZ*2];
				utf8_encode_cstr(utf8repr, sizeof(utf8repr), te->buffer, te->buffer_len);
				// XXX ^^^ check return value
				//printf("TODO create node: [%s]\n", utf8repr);
				struct dd_node* node = dd_graph_new_node(graph, utf8repr);
				if (node != NULL) {
					do_commit = 1;
					int mx, my;
					lsl_mpos(&mx, &my);
					node->x = mx + view->pan_x;
					node->y = my + view->pan_y;
				}
			}
		}

		lsl_set_type_index(type_index_main);
		lsl_set_cursor(100, 100);

		int select_min = te->select_start < te->select_end ? te->select_start : te->select_end;
		int select_max = te->select_start > te->select_end ? te->select_start : te->select_end;

		int cursor_x = 0, cursor_y = 0;
		int n = te->buffer_len;
		for (int i = 0; i < n; i++) {
			if (i == te->cursor) lsl_get_cursor(&cursor_x, &cursor_y);
			if (i >= select_min && i < select_max) {
				lsl_set_text_bg_color(lsl_eval(colorpid_text_bg_select));
			} else {
				lsl_set_text_bg_color(lsl_eval(colorpid_text_bg));
			}
			lsl_set_color(lsl_eval(colorpid_text));
			lsl_putch(te->buffer[i]);
		}
		lsl_clear_text_bg_color();
		if (n == te->cursor) lsl_get_cursor(&cursor_x, &cursor_y);

		lsl_set_color(lsl_eval(colorpid_text_cursor));
		int h = lsl_get_text_height();
		struct rect rect = {
			.p0 = { .x = cursor_x, .y = cursor_y-h },
			.dim = { .w = 2, .h = h }
		};
		lsl_fill_rect(&rect);
	}

	int quit = 0;

	int ch;
	while ((ch = lsl_getch()) != -1) {
		switch (ch & LSL_CH_CODEPOINT_MASK) {
		case 127: { // 127=DEL
			for (int i = 0; i < w->gsel_dya.n; i++) {
				struct gsel s = w->gsel[i];
				switch (s.type) {
				case GSEL_NONE:
					break;
				case GSEL_NODE:
					dd_graph_delete_node(graph, s.node_id);
					do_commit = 1;
					break;
				case GSEL_CONN:
					dd_graph_disconnect(graph,
						s.conn.src_node_id, s.conn.src_port_id,
						s.conn.dst_node_id, s.conn.dst_port_id);
					do_commit = 1;
					break;
				}
			}
			window_graph_clear_selection(w);
		} break;
		case ' ': {
			w->mode = MODE_NODE_INSERT;
			lsl_textedit_reset(&w->textedit);
		} break;
		case 'Q':
			quit = 1;
			break;
		}
	}

	if (do_commit) dd_commit(&state.dd);

	return quit;
}

static int winproc(void* usr)
{
	atls_enter_ctx(atls);

	struct window* win = (struct window*) usr;

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
		dya_init(&w->graph_view_stack_dya, (void**)&w->graph_view_stack, sizeof(*w->graph_view_stack));
		w->type = WINDOW_GRAPH;
	} else {
		// clone window
		memcpy(w, ow, sizeof(*w));
		dya_clone(
			&w->graph_view_stack_dya, (void**)&w->graph_view_stack,
			&ow->graph_view_stack_dya, (void**)&ow->graph_view_stack);
	}

	// not ever copied
	dya_init(&w->gsel_dya, (void**)&w->gsel, sizeof(*w->gsel));

	w->next = windows;
	windows = w;
	lsl_win_open("do", winproc, w);
	return w;
}

static struct atls* load_atlas(char* relpath)
{
	char path[4096];
	assert (lsl_relpath(path, sizeof(path), relpath) != -1);

	struct atls* atls = atls_load_from_file(path);
	if (atls == NULL) {
		fprintf(stderr, "%s\n", atls_get_error());
		assert(!"atls_load_from_file");
	}
	lsl_set_atls(atls);

	atls_use_colorscheme(atls, "default");
	assert(atls->active_colorscheme != NULL);

	assert((type_index_main = atls_get_glyph_table_index(atls, "main")) >= 0);
	assert((type_index_subs = atls_get_glyph_table_index(atls, "subs")) >= 0);

	assert((builtin_ctbl = atls_get_cell_table_index(atls, "builtin")) != -1);
	assert((boxselect_ctbl = atls_get_cell_table_index(atls, "boxselect")) != -1);

	colorpid_background = atls_get_prg_id(atls, "background");
	colorpid_builtin_footer_text = atls_get_prg_id(atls, "builtin.footer_text");
	colorpid_builtin_port_text = atls_get_prg_id(atls, "builtin.port_text");
	colorpid_connection_shadow = atls_get_prg_id(atls, "connection.shadow");
	colorpid_connection_signal = atls_get_prg_id(atls, "connection.signal"); // XXX is type a ctxvar?
	colorpid_text = atls_get_prg_id(atls, "text");
	colorpid_text_cursor = atls_get_prg_id(atls, "text.cursor");
	colorpid_text_bg = atls_get_prg_id(atls, "text.bg");
	colorpid_text_bg_select = atls_get_prg_id(atls, "text.bg.select");

	cvi_modifier = atls_get_ctxkey_id(atls, "modifier");

	return atls;
}

int lsl_main(int argc, char** argv)
{
	printf("dd_jack_init() = %d\n", dd_jack_init()); // XXX

	char* load_file = NULL;

	struct opt opts[] = {
		{0, 0, NULL}
	};
	int opt_index = 1;
	char* opt_value;
	int opt_state;
	while ((opt_state = opt_get(argc, argv, opts, &opt_index, &opt_value)) != OPT_END) {
		if (opt_state == OPT_VALUE) {
			if (load_file == NULL) {
				load_file = opt_value;
				continue;
			}
		}
		fprintf(stderr, "invalid arg %s\n", opt_value);
		return 1;
	}

	atls = load_atlas("default.atls");

	if (load_file) {
		if (dd_load_file(load_file, &state.dd) == -1) {
			fprintf(stderr, "dd_load_file(\"%s\") failed\n", load_file);
			return 1;
		}
	} else {
		dd_init(&state.dd);
	}
	if (dd_jack_set_dd(&state.dd) == -1) {
		fprintf(stderr, "dd_jack_set_dd failed (uninitialized?)\n");
		return 1;
	}

	clone_win(NULL);
	lsl_main_loop();

	return dd_save_file(&state.dd, "_save.dd", 0);
}
