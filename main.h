#ifndef MAIN_H

// main interface

#include <stdint.h>

#include "io.h"
#include "gig.h"
#include "util.h"
#include "mie.h"
#include "selftest.h"

// returns number of nanoseconds since program started
int64_t get_nanoseconds(void);
void sleep_nanoseconds(int64_t);

static inline void common_main_init(void)
{
	run_selftest();
	io_init(16);
	mie_thread_init();
	gig_init();
}

#define MAIN_H
#endif
