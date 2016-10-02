#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "lsl_prg.h"
#include "dd.h"

struct {
	struct dd dd;
} state;

enum window_type {
	WINDOW_TIMELINE = 1,
	WINDOW_GRAPH
};

struct atls_colorscheme* colorscheme;
struct atls_colorscheme builtin_palette;
int builtin_ctbl;
union vec4 background_color;
union vec4 builtin_footer_text_color;
union vec4 builtin_port_text_color;
union vec4 connection_signal_color;
int type_index_main;
int type_index_subs;


#define MODAL_NONE (0)
#define MODAL_NODEINSERT (1)


struct window {
	struct dya graph_stack_dya;
	u32* graph_stack;

	int modal;

	s32 graph_px, graph_py;

	enum window_type type;
	union {
		// ???
	};
	struct window* next;
}* windows;

static void box_calc(struct atls_cell_table* clt, struct dd_node* n, int* out_n_in, int* out_n_out, int* w, int* h, int* py0, int* py1)
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

static int winproc_graph(struct window* w)
{
	lsl_set_color(background_color);
	lsl_clear();

	// locate graph
	struct dd_graph* graph = &state.dd.root;
	for (int i = 0; i < w->graph_stack_dya.n; i++) {
		u32 node_id = w->graph_stack[i];
		struct dd_node* node = dd_graph_find_node(graph, node_id);
		if (node == NULL) break;
		struct dd_graph* next_graph = dd_node_get_graph(node);
		if (next_graph == NULL) break;
		graph = next_graph;
	}

	struct atls_cell_table* clt = lsl_set_cell_table(builtin_ctbl, &builtin_palette);
	int port_height = clt->heights[2];
	lsl_set_type_index(type_index_subs);

	// draw connections
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
		box_calc(clt, src_node, NULL, NULL, &src_node_width, NULL, NULL, &py0);
		box_calc(clt, dst_node, NULL, NULL, NULL,            NULL, &py1, NULL);

		const int pcx = 3; // XXX metadata?
		const int pcy = 5; // XXX metadata?

		s32 sx0 = src_node->x - w->graph_px + src_node_width - pcx;
		s32 sy0 = src_node->y - w->graph_py + pcy + py0 + dy1 * port_height;
		s32 sx1 = dst_node->x - w->graph_px + pcx;
		s32 sy1 = dst_node->y - w->graph_py + pcy + py1 + dy0 * port_height;

		lsl_set_color(connection_signal_color);
		const float tangent = 50.0f; // XXX metadata?
		lsl_bezier(
			(union vec2) { .x = sx0, .y = sy0 },
			(union vec2) { .x = sx0 + tangent, .y = sy0 },
			(union vec2) { .x = sx1 - tangent, .y = sy1 },
			(union vec2) { .x = sx1, .y = sy1 });
	}

	// draw nodes
	for (int i = 0; i < graph->nodes_dya.n; i++) {
		struct dd_node* n = &graph->nodes[i];

		s32 sx = n->x - w->graph_px;
		s32 sy = n->y - w->graph_py;

		int n_in, n_out, width, height, port_y0, port_y1;
		box_calc(clt, n, &n_in, &n_out, &width, &height, &port_y0, &port_y1);

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


		lsl_set_color(builtin_footer_text_color);
		lsl_set_cursor(sx + clt->widths[0], sy + r.dim.h - (clt->heights[6]));
		lsl_printf("%s", n->def);

		lsl_set_color(builtin_port_text_color);
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

		lsl_scope_push_data(&n->id, sizeof(n->id));
		lsl_drag("node", &r, &n->x, &n->y, 1, 1);
		lsl_scope_pop();
	}

	// pan drag
	lsl_drag("pan", NULL, &w->graph_px, &w->graph_py, -1, -1);

	if (w->modal == MODAL_NONE && lsl_accept(' ')) w->modal = MODAL_NODEINSERT;

	if (w->modal == MODAL_NODEINSERT) {
	}

	return lsl_frame_top()->button[2];
}

static int winproc(void* usr)
{
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

	return retval;
}

static struct window* clone_win(struct window* ow)
{
	struct window* w = malloc(sizeof(*w));
	assert(w != NULL);
	if (ow == NULL) {
		// new window
		memset(w, 0, sizeof(*w));
		dya_init(&w->graph_stack_dya, (void**)&w->graph_stack, sizeof(*w->graph_stack), 0);
		w->type = WINDOW_GRAPH;
	} else {
		// clone window
		memcpy(w, ow, sizeof(*w));
		dya_clone(
			&w->graph_stack_dya, (void**)&w->graph_stack,
			&ow->graph_stack_dya, (void**)&ow->graph_stack);
	}
	w->next = windows;
	windows = w;
	lsl_win_open("do", winproc, w);
	return w;
}

static union vec4 color_rgba(struct atls_color* color)
{
	union vec4 c;
	atls_color_rgba(color, &c.r, &c.g, &c.b, &c.a);
	return c;
}

static union vec4 get_color(char* tag)
{
	struct atls_colorscheme cs;
	assert(atls_colorscheme_tag_lookup(&cs, colorscheme, tag));
	return color_rgba(&cs.colors[0]);
}

static union vec4 get_color2(char* tag, char* layer)
{
	struct atls_colorscheme cs;
	assert(atls_colorscheme_tag_lookup(&cs, colorscheme, tag));
	for (int i = 0; i < cs.n_colors; i++) {
		if (strcmp(cs.colors[i].layer.cstr, layer) == 0) {
			return color_rgba(&cs.colors[i]);
		}
	}
	assert(!"color not found");
}

static void load_atlas(char* path)
{
	struct atls* atls = atls_load_from_file("default.atls");
	assert(atls != NULL);
	lsl_set_atls(atls);

	assert((type_index_main = atls_get_glyph_table_index(atls, "main")) >= 0);
	assert((type_index_subs = atls_get_glyph_table_index(atls, "subs")) >= 0);

	builtin_ctbl = atls_get_cell_table_index(atls, "builtin");

	int cs_id = atls_get_colorscheme(atls, "default");
	assert(cs_id >= 0);
	colorscheme = &atls->colorschemes[cs_id];

	assert(atls_colorscheme_tag_lookup(&builtin_palette, colorscheme, "builtin"));

	background_color = get_color("background");
	builtin_footer_text_color = get_color2("builtin", "footer_text");
	builtin_port_text_color = get_color2("builtin", "port_text");
	connection_signal_color = get_color2("connection", "signal");
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
		//dd_graph_connect(g, n1->id, 1, n2->id, 1);
		//dd_graph_connect(g, n2->id, 1, n3->id, 1);
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

	load_atlas("default.atls");

	clone_win(NULL);
	lsl_main_loop();

	return 0;
}
