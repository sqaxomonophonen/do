#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>

#include "main.h"
#include "arg.h"
#include "gig.h"

int64_t get_nanoseconds_monotonic(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_nsec + (int64_t)t.tv_sec * 1000000000LL;
}

int main(int argc, char** argv)
{
	parse_args(argc, argv);
	run_selftest();
	io_init();
	jio_init();
	mie_thread_init();
	gig_init();
	for (;;) {
		gig_thread_tick();
		sleep_microseconds(3000L); // 3ms
	}
	return EXIT_SUCCESS;
}
