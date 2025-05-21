#ifndef MAIN_H

// main interface

#include <stdint.h>

#include "io.h"
#include "gig.h"
#include "util.h"
#include "mie.h"
#include "allocator.h"
#include "selftest.h"
#include "arg.h"

int64_t get_nanoseconds(void);
// returns number of nanoseconds since program started

int64_t get_nanoseconds_epoch(void);

void sleep_nanoseconds(int64_t);

static inline void common_main_init(void)
{
	run_selftest();
	io_init(16);
	mie_thread_init();
	gig_init();
	gig_testsetup();
	gig_host(arg_dir ? arg_dir : ".");
}

#define MAIN_H
#endif
