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
struct atls_colorscheme box_palette;
int box2_cltidx;
union vec4 background_color;
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

	const int XXXw = 120;
	const int XXXh = 80;

	// draw connections
	for (int i = 0; i < graph->conns_dya.n; i++) {
		struct dd_conn* c = &graph->conns[i];
		struct dd_node* n0 = dd_graph_find_node(graph, c->src_node_id);
		struct dd_node* n1 = dd_graph_find_node(graph, c->dst_node_id);
		assert(n0 != NULL);
		assert(n1 != NULL);

		s32 sx0 = n0->x - w->graph_px + XXXw/2;
		s32 sy0 = n0->y - w->graph_py + XXXh/2;
		s32 sx1 = n1->x - w->graph_px + XXXw/2;
		s32 sy1 = n1->y - w->graph_py + XXXh/2;

		lsl_set_color(lsl_white());
		lsl_line(
			(union vec2) { .x = sx0, .y = sy0 },
			(union vec2) { .x = sx1, .y = sy1 });
		/* TODO look at blender node editor for delicous curved
		 * connections */
	}

	struct atls_cell_table* clt = lsl_set_cell_table(box2_cltidx, &box_palette);

	// draw nodes
	for (int i = 0; i < graph->nodes_dya.n; i++) {
		struct dd_node* n = &graph->nodes[i];

		s32 sx = n->x - w->graph_px;
		s32 sy = n->y - w->graph_py;

		struct rect r = (struct rect) {
			.p0 = { .x = sx , .y = sy },
			.dim = { .w = XXXw, .h = XXXh }
		};

		for (int row = 0; row < 3; row++) {
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
				if (row == 0) {
					y = sy;
				} else if (row == 1) {
					y = sy + clt->heights[0];
					h = r.dim.h - (clt->heights[0] + clt->heights[2]);
				} else if (row == 2) {
					y = sy + r.dim.h - clt->heights[2];
				}
				lsl_cell_plot(column, row, x, y, w, h);
			}
		}

		lsl_set_type_index(type_index_subs);

		lsl_set_color(lsl_white());
		lsl_set_cursor(sx + 20, sy + 50);
		lsl_printf("%s\n", n->def);

		lsl_set_color((union vec4) { .r = 1, .g = 1, .b = 0, .a = 1 });
		for (struct dd_port_it it = dd_node_inport_it(n); it.valid; dd_port_it_next(&it)) {
			lsl_printf(" in: %s (%d)\n", it.name, it.id);
		}
		lsl_set_color((union vec4) { .r = 0, .g = 1, .b = 1, .a = 1 });
		for (struct dd_port_it it = dd_node_outport_it(n); it.valid; dd_port_it_next(&it)) {
			lsl_printf(" out: %s (%d)\n", it.name, it.id);
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

static union vec4 get_color(char* name)
{
	struct atls_colorscheme cs;
	assert(atls_colorscheme_tag_lookup(&cs, colorscheme, name));
	return color_rgba(&cs.colors[0]);
}

static void load_atlas(char* path)
{
	struct atls* atls = atls_load_from_file("default.atls");
	assert(atls != NULL);
	lsl_set_atls(atls);

	assert((type_index_main = atls_get_glyph_table_index(atls, "main")) >= 0);
	assert((type_index_subs = atls_get_glyph_table_index(atls, "subs")) >= 0);

	box2_cltidx = atls_get_cell_table_index(atls, "box2");

	int cs_id = atls_get_colorscheme(atls, "default");
	assert(cs_id >= 0);
	colorscheme = &atls->colorschemes[cs_id];

	assert(atls_colorscheme_tag_lookup(&box_palette, colorscheme, "box0"));

	background_color = get_color("background");
}

int lsl_main(int argc, char** argv)
{
	dd_init(&state.dd);

	{
		// XXX some debug graph
		struct dd_graph* g = &state.dd.root;
		struct dd_node* n1 = dd_graph_new_node(g, "sin");
		struct dd_node* n2 = dd_graph_new_node(g, "cos");
		struct dd_node* n3 = dd_graph_new_node(g, "+");
		assert(n1 != NULL);
		assert(n2 != NULL);
		assert(n3 != NULL);
		//dd_graph_connect(g, n1->id, 1, n2->id, 1);
		//dd_graph_connect(g, n2->id, 1, n3->id, 1);
		dd_graph_connect(g, n1->id, 0, n2->id, 0);
		dd_graph_connect(g, n2->id, 0, n3->id, 0);

		struct dd_node* n4 = dd_graph_new_node(g, "[poly]32saw");
		assert(n4 != NULL);
		struct dd_graph* g2 = dd_node_get_graph(n4);
		dd_graph_new_node(g2, "in-");
		dd_graph_new_node(g2, "-out");
	}

	load_atlas("default.atls");

	clone_win(NULL);
	lsl_main_loop();

	return 0;
}
