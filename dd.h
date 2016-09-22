#ifndef DD_H

#include "dya.h"

struct dd_node;
struct dd_conn;

struct dd_graph {
	struct dya nodes_dya;
	struct dd_node* nodes;
	struct dya conns_dya;
	struct dd_conn* conns;
};

enum dd_node_type {
	DD_FOO = 1,
	DD_BAR,
	DD_BAZ,
	/*
	containers:
	 - dummy
	 - parallel array
	 - serial array
	 - resampling
	 - polyphonic
	 - ???
	arithmetic: plus minus multiply divide etc
	stateful: delays, filters, oscillators, etc
	ports?
	*/
};

struct dd_node {
	u32 id;
	s32 x,y;
	enum dd_node_type type;
	union {
	};
};

struct dd_conn {
	u32 src_node_id, src_port, dst_node_id, dst_port;
};

struct dd {
	struct dd_graph root;
};

// initialization
void dd_init(struct dd*);

void dd_commit(struct dd*);

struct dd_node* dd_graph_new_node(struct dd_graph*, enum dd_node_type nt);
int dd_graph_delete_node(struct dd_graph*, u32 id);
struct dd_node* dd_graph_find_node(struct dd_graph*, u32 id);
int dd_graph_connect(struct dd_graph*, u32 src_node_id, u32 src_port, u32 dst_node_id, u32 dst_port);
int dd_graph_disconnect(struct dd_graph*, u32 src_node_id, u32 src_port, u32 dst_node_id, u32 dst_port);

struct dd_graph* dd_node_get_graph(struct dd_node*);

#define DD_H
#endif
