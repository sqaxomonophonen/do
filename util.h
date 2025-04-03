#ifndef UTIL_H

#define ARRAY_LENGTH(xs) (sizeof(xs)/sizeof((xs)[0]))

static inline int is_numeric(char ch) { return '0' <= ch && ch <= '9'; }

#define UTIL_H
#endif
