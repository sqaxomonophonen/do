#include <string.h>

#include "dd.h"

u32 next_node_id;

static u32 get_next_node_id()
{
	return ++next_node_id;
}

static void node_free(struct dd_node* n)
{
	/* TODO dispose resources that node might refer to here */
}

static int node_compar(const void* va, const void* vb)
{
	const struct dd_node* a = va;
	const struct dd_node* b = vb;
	return a->id - b->id;
}

static int conn_compar(const void* va, const void* vb)
{
	const struct dd_conn* a = va;
	const struct dd_conn* b = vb;

	int d0 = a->src_node_id - b->src_node_id;
	if (d0 != 0) return d0;

	int d1 = a->src_port - b->src_port;
	if (d1 != 0) return d1;

	int d2 = a->dst_node_id - b->dst_node_id;
	if (d2 != 0) return d2;

	int d3 = a->dst_port - b->dst_port;
	return d3;
}

static void graph_init(struct dd_graph* dg)
{
	memset(dg, 0, sizeof(*dg));
	dya_init(&dg->nodes_dya, (void**)&dg->nodes, sizeof(*dg->nodes), 0);
	dya_init(&dg->conns_dya, (void**)&dg->conns, sizeof(*dg->conns), 0);
}

struct dd_node* dd_graph_new_node(struct dd_graph* dg, enum dd_node_type nt)
{
	struct dd_node nn;
	memset(&nn, 0, sizeof(nn));
	nn.id = get_next_node_id();
	nn.type = nt;
	return dya_bs_insert(&dg->nodes_dya, (void**)&dg->nodes, node_compar, &nn);
}

static int conn_match_node_id(const void* v, void* usr)
{
	const struct dd_conn* c = v;
	int node_id = *((int*)usr);
	return c->src_node_id == node_id || c->dst_node_id == node_id;
}

int dd_graph_delete_node(struct dd_graph* dg, u32 id)
{
	struct dd_node key = {.id = id};
	int index = dya_bs_find(&dg->nodes_dya, (void**)&dg->nodes, node_compar, &key);
	if (index >= 0) {
		struct dd_node* delnode = &dg->nodes[index];
		node_free(delnode);
		int delnode_id = delnode->id;
		dya_delete(&dg->nodes_dya, (void**)&dg->nodes, index);
		dya_delete_scan(&dg->conns_dya, (void**)&dg->conns, conn_match_node_id, &delnode_id);
		return 1;
	} else {
		return 0;
	}
}

struct dd_node* dd_graph_find_node(struct dd_graph* dg, u32 id)
{
	struct dd_node key = {.id = id};
	int index = dya_bs_find(&dg->nodes_dya, (void**)&dg->nodes, node_compar, &key);
	if (index >= 0) {
		return &dg->nodes[index];
	} else {
		return NULL;
	}
}

int dd_graph_connect(struct dd_graph* dg, u32 src_node_id, u32 src_port, u32 dst_node_id, u32 dst_port)
{
	struct dd_conn nc;
	memset(&nc, 0, sizeof(nc));
	nc.src_node_id = src_node_id;
	nc.src_port = src_port;
	nc.dst_node_id = dst_node_id;
	nc.dst_port = dst_port;

	if (dya_bs_find(&dg->conns_dya, (void**)&dg->conns, conn_compar, &nc) < 0) {
		dya_bs_insert(&dg->conns_dya, (void**)&dg->conns, conn_compar, &nc);
		return 1;
	} else {
		return 0;
	}
}

int dd_graph_disconnect(struct dd_graph* dg, u32 src_node_id, u32 src_port, u32 dst_node_id, u32 dst_port)
{
	struct dd_conn nc;
	memset(&nc, 0, sizeof(nc));
	nc.src_node_id = src_node_id;
	nc.src_port = src_port;
	nc.dst_node_id = dst_node_id;
	nc.dst_port = dst_port;

	int index = dya_bs_find(&dg->conns_dya, (void**)&dg->conns, conn_compar, &nc);
	if (index < 0) return 0;
	dya_delete(&dg->conns_dya, (void**)&dg->conns, index);
	return 1;
}

void dd_init(struct dd* ds)
{
	graph_init(&ds->root);
}

struct dd_graph* dd_node_get_graph(struct dd_node* n)
{
	return NULL; // XXX TODO return graph contained within node, if any
}
