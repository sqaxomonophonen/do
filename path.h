#ifndef PATH_H

#include <stddef.h>
#include <stdint.h>

void path_join(char* buf, size_t bufsize, int num_parts, const char** parts);

#define PATH_JOIN(BUF,BUFSZ,...) \
	{ \
		const char* _parts[] = { __VA_ARGS__ }; \
		path_join(BUF, BUFSZ, sizeof(_parts)/sizeof(_parts[0]), _parts); \
	}

#define STATIC_PATH_JOIN(BUF,...) PATH_JOIN(BUF,sizeof(BUF),__VA_ARGS__)

#ifdef PATH_IMPLEMENTATION

#include <string.h>

#ifdef _WIN32
#define SEPS "\\" // XXX not tested
#else
#define SEPS "/"
#endif
#define SEPC ((SEPS)[0])

void path_join(char* buf, size_t bufsize, int num_parts, const char** parts)
{
	char* p = buf;
	char* pend = p+bufsize-1;
	for (int i=0; i<num_parts; ++i) {
		const char* e = parts[i];
		if (i>0) {
			if ((p+1)>pend) break;
			*(p++) = SEPC;
		}
		const size_t n = strlen(e);
		if (n==0) continue;
		if ((p+n)>pend) break;
		memcpy(p, e, n);
		p += n;
	}
	*p = 0;
	pend[0]=0;
}

#endif

#define PATH_H
#endif

#ifdef PATH_UNIT_TEST

#include <assert.h>
#include <string.h>

static void path_unit_test(void)
{
	// XXX these tests will fail on Windows, right?

	char buf[1<<10];
	for (int pass=0; pass<5; ++pass) {
		STATIC_PATH_JOIN(buf,"foo","bar");
		assert(0 == strcmp(buf,"foo/bar"));

		STATIC_PATH_JOIN(buf,"foo","barrr");
		assert(0 == strcmp(buf,"foo/barrr"));

		STATIC_PATH_JOIN(buf, "a", "bb", "ccc");
		assert(0 == strcmp(buf,"a/bb/ccc"));

		STATIC_PATH_JOIN(buf, "aaa", "bb", "c");
		assert(0 == strcmp(buf,"aaa/bb/c"));
	}
}

#endif
