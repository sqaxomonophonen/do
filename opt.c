#include <string.h>

#include "opt.h"

static int is_end(struct opt* opt) {
	return opt->type == 0 && opt->short_opt == 0 && opt->long_opt == NULL;
}

enum arg_type {
	VALUE,
	SHORTOPT,
	LONGOPT
};

static enum arg_type get_arg_type(char* str) {
	size_t len = strlen(str);
	if(len >= 2 && str[0] == '-') {
		if(len == 2 && str[1] == '-') {
			return VALUE;
		} else if(len >= 3 && str[1] == '-') {
			return LONGOPT;
		} else {
			return SHORTOPT;
		}
	} else {
		return VALUE;
	}
}

int opt_get(int argc, char** argv, struct opt* opts, int* index, char** value) {
	if(*index >= argc) return OPT_END;
	char* arg = argv[*index];
	*value = arg;

	(*index)++;

	enum arg_type arg_type = get_arg_type(arg);
	if(arg_type == SHORTOPT) {
		arg++;
		char ch = *arg;
		struct opt* opt = opts;
		while(!is_end(opt)) {
			if(opt->short_opt != 0 && opt->short_opt == ch) {
				if(arg[1] != 0) {
					*value = arg + 1;
				} else if(opt->type == OPT_OPTION && *index < argc && get_arg_type(argv[*index]) == VALUE) {
					*value = argv[*index];
					(*index)++;
				} else {
					*value = NULL;
				}
				return opt - opts;
			}
			opt++;
		}
		return OPT_INVALID;
	} else if(arg_type == LONGOPT) {
		arg += 2;
		char* argend = arg;
		while(*argend != 0 && *argend != '=') argend++;
		struct opt* opt = opts;
		while(!is_end(opt)) {
			if(opt->long_opt != NULL && memcmp(opt->long_opt, arg, argend-arg) == 0) {
				if(*argend == '=') {
					*value = argend + 1;
				} else if(opt->type == OPT_OPTION && *index < argc && get_arg_type(argv[*index]) == VALUE) {
					*value = argv[*index];
					(*index)++;
				} else {
					*value = NULL;
				}
				return opt - opts;
			}
			opt++;
		}
		return OPT_INVALID;
	} else {
		*value = arg;
		return OPT_VALUE;
	}
}

