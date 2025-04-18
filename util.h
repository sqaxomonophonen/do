#ifndef UTIL_H

#define ARRAY_LENGTH(xs) (sizeof(xs)/sizeof((xs)[0]))

// TODO non gcc/clang defines?
#define NO_DISCARD    __attribute__((warn_unused_result))
#define ALWAYS_INLINE __attribute__((always_inline))

static inline int is_numeric(char ch) { return '0' <= ch && ch <= '9'; }

#define UTIL_H
#endif
