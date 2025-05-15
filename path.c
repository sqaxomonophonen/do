#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>

#include "path.h"
#include "stb_sprintf.h"

#ifdef _WIN32
#define SEPS "\\" // XXX not tested
#else
#define SEPS "/"
#endif
#define SEPC ((SEPS)[0])

void path_join(char* buf, size_t bufsize, ...)
{
	// XXX maybe stress test this a bit
	va_list va;
	va_start(va, bufsize);
	char* p = buf;
	char* pend = p+bufsize-1;
	int sep=0;
	for (;;) {
		char* e = va_arg(va, char*);
		if (e == NULL) break;
		if (sep) {
			if ((p+1)>pend) break;
			*(p++) = SEPC;
			sep=0;
		}
		const size_t n = strlen(e);
		if (n==0) continue;
		if ((p+n)>pend) break;
		memcpy(p, e, n);
		p += n;
		sep=1;
	}
	va_end(va);
	pend[0]=0;
}
