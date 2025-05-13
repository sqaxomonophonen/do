#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>

#include "main.h"
#include "args.h"
#include "gig.h"

int64_t get_nanoseconds(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_nsec + (int64_t)t.tv_sec * 1000000000LL;
}

int main(int argc, char** argv)
{
	parse_args(argc, argv);
	common_main_init();
	for (;;) {
		// TODO sleep a few microseconds?
		gig_thread_tick();
	}
	return EXIT_SUCCESS;
}
