#include <string.h>
#include <stdlib.h>
#include <assert.h>

#include "dd.h"

static u32 get_next_node_id()
{
	static u32 next_node_id;
	return ++next_node_id;
}

static inline int is_container(const struct dd_node* node)
{
	return node->type > DD__CONTAINER_MIN && node->type < DD__CONTAINER_MAX;
}

static inline int is_builtin(const struct dd_node* node)
{
	return node->type > DD__BUILTIN_MIN && node->type < DD__BUILTIN_MAX;
}

static inline int is_port_node(const struct dd_node* node)
{
	return node->type == DD_IN_PORT || node->type == DD_OUT_PORT;
}

static inline int is_inport_node(const struct dd_node* node)
{
	return node->type == DD_IN_PORT;
}

static inline int get_n_port_nodes(struct dd_graph* dg)
{
	return dg->n_inport_nodes + dg->n_outport_nodes;
}

static void node_free(struct dd_node* n)
{
	/* TODO dispose resources that node might refer to here */
}

static int node_compar(const void* va, const void* vb)
{
	const struct dd_node* a = va;
	const struct dd_node* b = vb;

	int apn = is_port_node(a);
	int bpn = is_port_node(b);
	{
		int d = bpn - apn;
		if (d != 0) return d;
	}
	if (apn && bpn) {
		// port nodes are ordered by (in/out, y)
		int aipn = is_inport_node(a);
		int bipn = is_inport_node(b);
		{
			int d = bipn - aipn;
			if (d != 0) return d;
		} {
			int d = a->y - b->y;
			if (d != 0) return d;
		}
	}
	return a->id - b->id;
}

static int conn_compar(const void* va, const void* vb)
{
	const struct dd_conn* a = va;
	const struct dd_conn* b = vb;

	{
		int d = a->dst_node_id - b->dst_node_id;
		if (d != 0) return d;
	} {
		int d = a->dst_port_id - b->dst_port_id;
		if (d != 0) return d;
	} {
		int d = a->src_node_id - b->src_node_id;
		if (d != 0) return d;
	} {
		int d = a->src_port_id - b->src_port_id;
		return d;
	}
}

static int conn_dst_compar(const void* va, const void* vb)
{
	const struct dd_conn* a = va;
	const struct dd_conn* b = vb;

	{
		int d = a->dst_node_id - b->dst_node_id;
		if (d != 0) return d;
	} {
		int d = a->dst_port_id - b->dst_port_id;
		return d;
	}
}

static void graph_init(struct dd_graph* dg)
{
	memset(dg, 0, sizeof(*dg));
	dya_init(&dg->nodes_dya, (void**)&dg->nodes, sizeof(*dg->nodes), 0);
	dya_init(&dg->conns_dya, (void**)&dg->conns, sizeof(*dg->conns), 0);
}

static int parse_nodedef0(char* def, struct dd_nodedef* nd)
{
	assert(nd != NULL);
	memset(nd, 0, sizeof(*nd));

	size_t deflen = strlen(def);
	if (deflen == 0) return -1;

	char* p = def;
	int state = 0;
	for (;;) {
		char c0 = *p;
		if (c0 == 0) {
			break;
		} else if (state == 0 && c0 == '[') {
			p++;
			char* first = p;
			for (;;) {
				char c1 = *p;
				if (c1 == ']') {
					size_t n = p - first;
					if (0) {}
					#define DDEF(t,s) \
						else if (n == strlen(s) && (n == 0 || memcmp(first, s, n) == 0)) { \
							nd->type = t; \
							break; \
						}
					DD__CONTAINER_LIST
					#undef DDEF
					else {
						return -1;
					}
				} else if (c1 < 'a' || c1 > 'z') {
					return -1;
				}
				p++;
			}
			state = 1;
		} else if (state == 0 && c0 == ':') {
			nd->type = DD_IMPORT;
			nd->ident = def+1;
			nd->ident_len = deflen-1;
			return 0;
		} else if (state == 0) {
			nd->ident = p;
			int n_dash = 0;
			for (;;) {
				char c1 = *p;
				if (state == 0 && c1 == '-') n_dash++;
				if (state == 0 && c1 == '=') {
					state = 2;
				} else if ((state == 0 && c1 == ':') || c1 == 0) {
					nd->ident_len = p - nd->ident;

					if (n_dash > 1) return -1;

					if (state == 2) {
						nd->type = DD_EXPRESSION;
					} else if (nd->ident_len > 1 && nd->ident[0] == '-') {
						nd->type = DD_OUT_PORT;
						nd->ident++;
						nd->ident_len--;
					} else if (nd->ident_len > 1 && nd->ident[nd->ident_len-1] == '-') {
						nd->type = DD_IN_PORT;
						nd->ident_len--;
					} else {
						if (0) {}
						#define DDEF(t,s,p) \
							else if (strlen(s) == nd->ident_len && memcmp(s, nd->ident, nd->ident_len) == 0) { \
								nd->type = t; \
							}
						DD__BUILTIN_LIST
						#undef DDEF
						else {
							return -1;
						}
					}

					if (c1 == 0) {
						return 0;
					} else {
						state = 1;
						p++;
						break;
					}
				}
				p++;
			}
		} else if (state == 1) {
			nd->name = p;
			for (;;) {
				char c1 = *p;
				if (c1 == 0) break;
				/* TODO are there invalid characters in comment
				field? */
				p++;
			}
			nd->name_len = p - nd->name;
			return 0;
		}
		p++;
	}
	return -1;
}

static int parse_nodedef(char* def, struct dd_nodedef* nd)
{
	if (parse_nodedef0(def, nd) == -1) return -1;
	return 0;
}

struct dd_node* dd_graph_new_node(struct dd_graph* dg, char* def)
{
	struct dd_nodedef nd;
	if (parse_nodedef(def, &nd) == -1) {
		return NULL;
	}
	struct dd_node nn;
	memset(&nn, 0, sizeof(nn));
	nn.id = get_next_node_id();
	assert((nn.def = strdup(def)) != NULL);

	nn.type = nd.type;

	if (nn.type > DD__CONTAINER_MIN && nn.type < DD__CONTAINER_MAX) {
		nn.container.graph = malloc(sizeof(*nn.container.graph));
		assert(nn.container.graph != NULL);
		graph_init(nn.container.graph);
	}

	if (nn.type == DD_IN_PORT || nn.type == DD_OUT_PORT) {
		u32 max_id = 0;

		int n_port_nodes = get_n_port_nodes(dg);
		for (int i = 0; i < n_port_nodes; i++) {
			struct dd_node* pn = &dg->nodes[i];
			if (pn->type != nn.type) continue;
			if (pn->port.id > max_id) {
				max_id = pn->port.id;
			}
		}
		assert(max_id < (1<<16)); // XXX can try simply finding a free slot instead
		nn.port.id = max_id + 1;
		nn.port.name_off = nd.ident - def;
		nn.port.name_len = nd.ident_len;
		if (nn.type == DD_IN_PORT) {
			dg->n_inport_nodes++;
		} else {
			dg->n_outport_nodes++;
		}
	}

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
		enum dd_nodetype dnt = delnode->type;
		node_free(delnode);
		int delnode_id = delnode->id;
		dya_delete(&dg->nodes_dya, (void**)&dg->nodes, index);
		dya_delete_scan(&dg->conns_dya, (void**)&dg->conns, conn_match_node_id, &delnode_id);
		if (dnt == DD_IN_PORT) {
			dg->n_inport_nodes--;
		}
		if ( dnt == DD_OUT_PORT) {
			dg->n_outport_nodes--;
		}
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


static int is_valid_port_id(struct dd_node* node, u16 port_id, int in)
{
	for (struct dd_port_it it = (in ? dd_node_inport_it(node) : dd_node_outport_it(node)); it.valid; dd_port_it_next(&it)) {
		assert(it.in == in);
		if (it.multiple) return 1;
		if (it.id == port_id) {
			return 1;
		}
	}
	return 0;
}

static int has_connection(struct dd_graph* dg, struct dd_conn* nc)
{
	return dya_bs_find(&dg->conns_dya, (void**)&dg->conns, conn_compar, nc) >= 0;
}

static int has_connection_dst(struct dd_graph* dg, struct dd_conn* nc)
{
	return dya_bs_find(&dg->conns_dya, (void**)&dg->conns, conn_dst_compar, nc) >= 0;
}

int dd_graph_can_connect(struct dd_graph* dg, u32 src_node_id, u16 src_port_id, u32 dst_node_id, u16 dst_port_id)
{
	struct dd_node* src_node = NULL;
	struct dd_node* dst_node = NULL;

	if (src_node_id > 0) {
		src_node = dd_graph_find_node(dg, src_node_id);
		if (src_node == NULL) return 0;
		if (!is_valid_port_id(src_node, src_port_id, 0)) return 0;
	}

	struct dd_conn nc;
	memset(&nc, 0, sizeof(nc));
	nc.src_node_id = src_node_id;
	nc.src_port_id = src_port_id;
	nc.dst_node_id = dst_node_id;
	nc.dst_port_id = dst_port_id;

	if (dst_node_id > 0) {
		dst_node = dd_graph_find_node(dg, dst_node_id);
		if (dst_node == NULL) return 0;
		if (!is_valid_port_id(dst_node, dst_port_id, 1)) return 0;
		if (has_connection_dst(dg, &nc)) return 0;
	}

	if (src_node != NULL && dst_node != NULL && has_connection(dg, &nc)) {
		return 0;
	}

	/*
	TODO "type check"; assert port types are "compatible"
	TODO assert there are no graph cycles (or that cycles are "legal",
	however that's going to play out)
	*/

	return 1;
}

int dd_graph_connect(struct dd_graph* dg, u32 src_node_id, u16 src_port_id, u32 dst_node_id, u16 dst_port_id)
{
	struct dd_node* src_node = dd_graph_find_node(dg, src_node_id);
	if (src_node == NULL) return -1;
	struct dd_node* dst_node = dd_graph_find_node(dg, dst_node_id);
	if (dst_node == NULL) return -1;

	if (!dd_graph_can_connect(dg, src_node_id, src_port_id, dst_node_id, dst_port_id)) return -1;

	struct dd_conn nc;
	memset(&nc, 0, sizeof(nc));
	nc.src_node_id = src_node_id;
	nc.src_port_id = src_port_id;
	nc.dst_node_id = dst_node_id;
	nc.dst_port_id = dst_port_id;

	dya_bs_insert(&dg->conns_dya, (void**)&dg->conns, conn_compar, &nc);
	return 0;
}

int dd_graph_disconnect(struct dd_graph* dg, u32 src_node_id, u16 src_port_id, u32 dst_node_id, u16 dst_port_id)
{
	struct dd_conn nc;
	memset(&nc, 0, sizeof(nc));
	nc.src_node_id = src_node_id;
	nc.src_port_id = src_port_id;
	nc.dst_node_id = dst_node_id;
	nc.dst_port_id = dst_port_id;

	int index = dya_bs_find(&dg->conns_dya, (void**)&dg->conns, conn_compar, &nc);
	if (index < 0) return -1;
	dya_delete(&dg->conns_dya, (void**)&dg->conns, index);
	return 0;
}

void dd_init(struct dd* ds)
{
	graph_init(&ds->root);
}

struct dd_graph* dd_node_get_graph(struct dd_node* n)
{
	assert(is_container(n));
	return n->container.graph;
}

struct dd__portdef {
	char* name;
	u16 id;
	u8 type;
};

#define IN (1)
#define OUT (0)
#define N_IN (2)
#define N_OUT (3)

struct dd__portdef anon01[] = {
	{"", 1, OUT},
	{0}
};
struct dd__portdef anon10[] = {
	{"", 1, IN},
	{0}
};
struct dd__portdef anon11[] = {
	{"", 1, IN},
	{"", 1, OUT},
	{0}
};
struct dd__portdef anonN1[] = {
	{"", 0, N_IN},
	{"", 1, OUT},
	{0}
};
struct dd__portdef anon21[] = {
	{"", 1, IN},
	{"", 2, IN},
	{"", 1, OUT},
	{0}
};

static struct dd_port_it port_it(struct dd_node* n, int in, int out)
{
	struct dd_port_it it;
	memset(&it, 0, sizeof(it));

	it.valid = 1;

	it._in = in;
	it._out = out;

	// check if a static portdef array can be served
	if (n->type == DD_IN_PORT) {
		it._static_portdef = anon01;
	} else if (n->type == DD_OUT_PORT) {
		it._static_portdef = anon10;
	} else if (n->type > DD__BUILTIN_MIN && n->type < DD__BUILTIN_MAX) {
		switch (n->type) {
		#define DDEF(t,s,ps) \
		case t: \
			it._static_portdef = ps; \
			break;
		DD__BUILTIN_LIST
		#undef DDEF
		default:
			assert(!"unhandled builtin type");
		}
	}

	if (it._static_portdef != NULL) {
		it._use_static_portdef = 1;
		it._index = -1;
	} else if (n->type > DD__CONTAINER_MIN && n->type < DD__CONTAINER_MAX) {
		it._use_graph = 1;
		it._index = -1;
		it._graph = dd_node_get_graph(n);
	} else if (n->type == DD_EXPRESSION) {
		assert(!"TODO expression"); // TODO
	} else {
		assert(!"unhandled type");
	}

	dd_port_it_next(&it);
	return it;
}

void dd_node_nports(struct dd_node* dn, int* n_total, int* n_inports, int* n_outports)
{
	int ni = 0;
	int no = 0;

	if (is_container(dn)) {
		struct dd_graph* dg = dd_node_get_graph(dn);
		ni = dg->n_inport_nodes;
		no = dg->n_outport_nodes;
	} else {
		// iterator fallback
		for (struct dd_port_it it = port_it(dn, 1, 1); it.valid; dd_port_it_next(&it)) {
			if (it.in) {
				ni++;
			} else {
				no++;
			}
		}
	}

	if (n_total) *n_total = ni + no;
	if (n_inports) *n_inports = ni;
	if (n_outports) *n_outports = no;
}


struct dd_port_it dd_node_inport_it(struct dd_node* n)
{
	return port_it(n, 1, 0);
}

struct dd_port_it dd_node_outport_it(struct dd_node* n)
{
	return port_it(n, 0, 1);
}

void dd_port_it_next(struct dd_port_it* it)
{
	assert(it->valid);

	if (it->_use_static_portdef) {
		for (;;) {
			struct dd__portdef* pd = &it->_static_portdef[++it->_index];

			if (!pd->name) {
				it->valid = 0;
				return;
			}

			int in = pd->type == IN || pd->type == N_IN;
			int out = pd->type == OUT || pd->type == N_OUT;

			if (in == it->_in || out == it->_out) {
				it->name = pd->name;
				it->name_len = strlen(it->name);
				it->id = pd->id;
				it->in = in;
				it->multiple = (pd->type == N_IN || pd->type == N_OUT);
				return;
			}
		}
	} else if (it->_use_graph) {
		for (;;) {
			struct dd_graph* g = it->_graph;
			it->_index++;
			int n_port_nodes = get_n_port_nodes(g);
			if (it->_index >= n_port_nodes) {
				it->valid = 0;
				return;
			}
			struct dd_node* pn = &g->nodes[it->_index];
			if ((it->_in && pn->type == DD_IN_PORT) || (it->_out && pn->type == DD_OUT_PORT)) {
				it->name = pn->def + pn->port.name_off;
				it->name_len = pn->port.name_len;

				it->id = pn->port.id;
				it->in = pn->type == DD_IN_PORT;
				it->multiple = 0;
				return;
			}
		}
	} else {
		assert(!"unhandled iterator type");
	}
}
