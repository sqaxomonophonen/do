#ifndef __OPT_H__
#define __OPT_H__

#define OPT_FLAG (1)
#define OPT_OPTION (2)

struct opt {
	int type;
	char short_opt;
	const char* long_opt;
};

#define OPT_END (-1)
#define OPT_VALUE (-2)
#define OPT_INVALID (-3)

int opt_get(int argc, char** argv, struct opt* opts, int* index, char** value);

#endif//__OPT_H__
