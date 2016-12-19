#ifndef DD_H

/*
dd.h: do data, data manipulation, serialization and audio engine.
*/

#include "dya.h"

struct dd_node;
struct dd_conn;

struct dd_graph {
	u16 n_inport_nodes;
	u16 n_outport_nodes;
	struct dya nodes_dya;
	struct dd_node* nodes;
	struct dya conns_dya;
	struct dd_conn* conns;
};

#define DD__CONTAINER_LIST \
	DDEF(DD_C_DUMMY, "") \
	DDEF(DD_C_POLY, "poly") \
	DDEF(DD_C_ARRAY, "array") \
	DDEF(DD_C_SERIAL, "serial") \
	DDEF(DD_C_RESAMPLE, "resample")

#define DD__BUILTIN_LIST \
	DDEF(DD_B_SIN, "sin", anon11) \
	DDEF(DD_B_COS, "cos", anon11) \
	DDEF(DD_B_ADD, "+", anonN1) \
	DDEF(DD_B_SUB, "-", anon21)

enum dd_nodetype {
	DD_IMPORT = -1,

	DD_EXPRESSION = 1,

	DD_IN_PORT = 100,
	DD_OUT_PORT,

	DD__CONTAINER_MIN = 200,
	#define DDEF(d,x) d,
	DD__CONTAINER_LIST
	#undef DDEF
	DD__CONTAINER_MAX,

	DD__BUILTIN_MIN = 300,
	#define DDEF(d,x,y) d,
	DD__BUILTIN_LIST
	#undef DDEF
	DD__BUILTIN_MAX
};

struct dd_nodedef {
	enum dd_nodetype type;

	char* name;
	size_t name_len;

	char* ident;
	size_t ident_len;
};

struct dd_node {
	u32 id;
	s32 x,y;
	char* def;

	enum dd_nodetype type;

	union {
		struct {
			u16 id;
			u8 name_off, name_len;
		} port;
		struct {
			struct dd_graph* graph;
		} container;
	};
};

struct dd_conn {
	u32 src_node_id, dst_node_id;
	u16 src_port_id, dst_port_id;
};

struct dd__portdef;
struct dd_port_it {
	char* name;
	int name_len;
	u16 id;

	u8 valid:1;
	u8 in:1;
	u8 multiple:1;

	u8 _in:1;
	u8 _out:1;
	u8 _use_static_portdef:1;
	u8 _use_graph:1;

	u8 _index;

	union {
		struct dd__portdef* _static_portdef;
		struct dd_graph* _graph;
	};
};

struct dd {
	struct dd_graph root;
};

// initialization
void dd_init(struct dd*);
void dd_free(struct dd*);

int dd_load_file(char* path, struct dd*);
int dd_save_file(struct dd*, char* path, int flags);

struct dd_node* dd_graph_new_node(struct dd_graph*, char* def);
int dd_graph_delete_node(struct dd_graph*, u32 id);
struct dd_node* dd_graph_find_node(struct dd_graph*, u32 id);

int dd_graph_can_connect(struct dd_graph*, u32 src_node_id, u16 src_port_id, u32 dst_node_id, u16 dst_port_id);
int dd_graph_connect(struct dd_graph*, u32 src_node_id, u16 src_port_id, u32 dst_node_id, u16 dst_port_id);
int dd_graph_disconnect(struct dd_graph*, u32 src_node_id, u16 src_port_id, u32 dst_node_id, u16 dst_port_id);

struct dd_graph* dd_node_get_graph(struct dd_node*);

void dd_node_nports(struct dd_node* n, int* n_total, int* n_inports, int* n_outports);
struct dd_port_it dd_node_inport_it(struct dd_node*);
struct dd_port_it dd_node_outport_it(struct dd_node*);
void dd_port_it_next(struct dd_port_it*);

void dd_commit(struct dd*);
void dd_configure_advance(struct dd*, int n_inputs, int n_outputs, int sample_rate, int buffer_size);
void dd_advance(struct dd* /* TODO input/output buffers ... */);

// backend-specific (XXX does this belong in dd.h?)
int dd_jack_init();

#define DD_H
#endif
