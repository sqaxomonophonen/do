#ifndef UTIL_H

#define ARRAY_LENGTH(xs) (sizeof(xs)/sizeof((xs)[0]))

#define NO_DISCARD __attribute__((warn_unused_result))

static inline int is_numeric(char ch) { return '0' <= ch && ch <= '9'; }

#define UTIL_H
#endif
