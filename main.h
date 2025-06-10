#ifndef MAIN_H

// main interface

#include <stdint.h>

#include "gig.h"
#include "util.h"
#include "mie.h"
#include "allocator.h"
#include "selftest.h"
#include "arg.h"

int64_t get_nanoseconds_monotonic(void);
static inline int64_t get_microseconds_monotonic(void)
{
	return get_nanoseconds_monotonic()/1000LL;
}
int64_t get_microseconds_epoch(void);
void sleep_microseconds(int64_t);

void transmit_mim(int mim_session_id, int64_t tracer, uint8_t* data, int count);

#define MAIN_H
#endif
