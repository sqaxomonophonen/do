#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include "args.h"
#include "util.h"
#include "gig.h"

static const char* prg;

#ifdef _WIN32
#define OPTS "/" // XXX not tested
#else
#define OPTS "-"
#endif
#define OPTC (OPTS[0])

NO_RETURN
static void usage(FILE* out, int exit_status)
{
	fprintf(out, "Usage: %s [" OPTS "dir PATH] [" OPTS "connect SERVER]\n", prg);
	exit(exit_status);
}

NO_RETURN
static void help(void)
{
	usage(stdout, EXIT_SUCCESS);
}

NO_RETURN
static void error(void)
{
	usage(stderr, EXIT_FAILURE);
}

static const char* opt_dir;
static const char* opt_connect;

void parse_args(int argc, char** argv)
{
	prg = strdup(argv[0]);
	const char** grab = NULL;
	const char* last_switch = NULL;
	for (int argi=1; argi<argc; ++argi) {
		const char* arg = argv[argi];
		const int is_switch = ((strlen(arg)>=1) && (arg[0]==OPTC));
		if (grab) {
			if (is_switch) break;
			*grab = strdup(arg);
			grab = NULL;
		} else {
			if (grab) break;
			if (!is_switch) {
				fprintf(stderr, "invalid positional arg [%s]\n", arg);
				error();
			}
			last_switch = arg;
			const char* rest = arg+1;
			if (strcmp(rest, "help")==0 || strcmp(rest, "h")==0) {
				help();
			} else if (strcmp(rest, "dir")==0) {
				grab = &opt_dir;
			} else if (strcmp(rest, "connect")==0) {
				grab = &opt_connect;
			} else {
				fprintf(stderr, "invalid switch %s\n", arg);
				error();
			}
		}
	}
	if (grab) {
		fprintf(stderr, "missing argument for switch %s\n", last_switch);
		error();
	}

	if (opt_dir && !opt_connect) {
		gig_serve_dir(opt_dir);
	} else if (opt_dir && opt_connect) {
		gig_record_dir(opt_dir);
		assert(!"TODO");
	} else if (!opt_dir && opt_connect) {
		assert(!"TODO");
	}
}
