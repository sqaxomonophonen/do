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


struct window {
	int pdrag_id;

	struct dya graph_stack_dya;
	u32* graph_stack;

	s32 graph_px, graph_py;

	enum window_type type;
	union {
		// ???
	};
	struct window* next;
}* windows;

static int winproc_graph(struct window* w)
{
	lsl_set_color(lsl_black());
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

	int drag_id = 1;

	const int XXXw = 80;
	const int XXXh = 50;

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

	// draw nodes
	for (int i = 0; i < graph->nodes_dya.n; i++) {
		struct dd_node* n = &graph->nodes[i];

		s32 sx = n->x - w->graph_px;
		s32 sy = n->y - w->graph_py;

		lsl_set_color(lsl_white());
		struct rect r = (struct rect) {
			.p0 = { .x = sx , .y = sy },
			.dim = { .w = XXXw, .h = XXXh }
		};
		lsl_fill_rect(&r);

		lsl_set_color(lsl_black());
		const int border = 2;
		r.p0.x += border; r.p0.y += border; r.dim.w -= border*2; r.dim.h -= border*2;
		lsl_fill_rect(&r);

		lsl_set_cursor(sx, sy);
		lsl_set_color((union vec4) { .r = 0, .g = 1, .b = 0, .a = 0 });
		lsl_printf("hello");

		lsl_drag(&r, drag_id++, &n->x, &n->y, 1, 1);
	}

	// pan drag
	lsl_drag(NULL, drag_id++, &w->graph_px, &w->graph_py, -1, -1);

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


int lsl_main(int argc, char** argv)
{
	dd_init(&state.dd);

	{
		// XXX some debug graph
		struct dd_graph* g = &state.dd.root;
		struct dd_node* n1 = dd_graph_new_node(g, DD_FOO);
		struct dd_node* n2 = dd_graph_new_node(g, DD_FOO);
		struct dd_node* n3 = dd_graph_new_node(g, DD_FOO);
		dd_graph_connect(g, n1->id, 1, n2->id, 1);
		dd_graph_connect(g, n2->id, 1, n3->id, 1);
	}

	struct atls* atls = atls_load_from_file("default.atls");
	assert(atls != NULL);
	lsl_set_atls(atls);

	lsl_set_type_index(atls_get_glyph_table_index(atls, "main"));
	clone_win(NULL);
	lsl_main_loop();

	return 0;
}
