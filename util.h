#ifndef UTIL_H

#include <stdlib.h>

#define ARRAY_LENGTH(xs) (sizeof(xs)/sizeof((xs)[0]))

// TODO non gcc/clang defines?
#define NO_RETURN      __attribute__((noreturn))
#define NO_DISCARD     __attribute__((warn_unused_result))
#define ALWAYS_INLINE  __attribute__((always_inline))
#define FORMATPRINTF1  __attribute__((format(printf,1,2)))
#define FORMATPRINTF2  __attribute__((format(printf,2,3)))


//#define ALIGNAS(x) _Alignas(x)
//#define ATOMIC(T) _Atomic(T)

// XXX there's std::hardware_destructive_interference_size in C++, doesn't seem
// to exist in C
#define CACHE_LINE_SIZE_LOG2 (6)
#define CACHE_LINE_SIZE      (1LL<<(CACHE_LINE_SIZE_LOG2))
#define CACHE_ALIGN(s)       (((s)+CACHE_LINE_SIZE-1) & ~(CACHE_LINE_SIZE-1))

#define IS_POWER_OF_TWO(v)   (((v)&((v)-1))==0)

static inline int is_digit(int c) { return ('0'<=c) && (c<='9'); }

#define UTIL_H
#endif
