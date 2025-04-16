#include <assert.h>

#include "stb_ds.h"
#include "gig.h"

static struct {
	struct document* document_arr;
	int next_document_id;
} g;

void gig_init(void)
{
	g.next_document_id = 1;
	new_document(DOC_AUDIO);
	struct document* doc = get_document_by_id(1);
	const char* code =
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	"test xyzzy (foo bar) 0123456789ABCDEF ######\n"
	;
	const size_t n = strlen(code);
	for (int i=0; i<n; ++i) {
		arrput(doc->fat_char_arr, ((struct fat_char){
			.codepoint = code[i],
			.color = {1,1,1,1},
		}));
	}
}

int new_document(enum document_type type)
{
	const int id = g.next_document_id++;
	arrput(g.document_arr, ((struct document){
		.id = id,
		.type = type,

	}));
	return id;
}

int get_num_documents(void)
{
	return arrlen(g.document_arr);
}

struct document* get_document_by_index(int index)
{
	assert((0 <= index) && (index < get_num_documents()));
	return &g.document_arr[index];
}

struct document* find_document_by_id(int id)
{
	const int n = get_num_documents();
	for (int i=0; i<n; ++i) {
		struct document* doc = get_document_by_index(i);
		if (doc->id == id) return doc;
	}
	return NULL;
}

struct document* get_document_by_id(int id)
{
	struct document* doc = find_document_by_id(id);
	assert((doc != NULL) && "document id not found");
	return doc;
}

void gig_spool(void)
{
	// TODO spooling should go through new journal entries, and apply them so
	// that documents are up-to-date. the purpose of having a function to do
	// this is that you can do it when it's convenient, instead of using
	// mutexes in the background to do it automatically. spool can be run once
	// per video frame?
}

void ed_command(struct command* c)
{
	assert(!"TODO ed_command()");
}
