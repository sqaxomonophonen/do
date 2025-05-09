// #include this (not stb_ds.h) in files where you only want to use the system
// allocator for the stb_ds API, i.e. in files where you don't use
// arrinit()/hminit()/shinit() to explicitly set the allocator. this file sets
// the system allocator as default, so you don't need those calls.

// when you need init calls in order to use other allocators, include stb_ds.h
// instead and init all arrays explicitly. much of the API crashes (calls that
// need the allocator) if an init call is missing, which is better than falling
// back to the system allocator (if a scratch allocator was intended it might
// cause memory leaks)

#include "allocator.h"
#define STBDS_DEFAULT_CONTEXT (&system_allocator)
#include "stb_ds.h"
