#ifndef UTIL_H

#include <stdlib.h>
#include <stdio.h>

#define ARRAY_LENGTH(xs) (sizeof(xs)/sizeof((xs)[0]))
#define STR2(x) #x
#define STR(x) STR2(x)

// TODO non gcc/clang defines?
#define NO_RETURN      __attribute__((noreturn))
#define NO_DISCARD     __attribute__((warn_unused_result))
#define ALWAYS_INLINE  __attribute__((always_inline))
#define THREAD_LOCAL _Thread_local
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

static inline int bounds_check(int index, int length, const char* location)
{
	if (!((0 <= index) && (index < length))) {
		// XXX write to different output?
		fprintf(stderr, "bounds check (0<=%d<%d) failed at %s\n", index, length, location);
		abort();
	}
	return index;
}

// maybe put these in stb_ds.h? (although XXX: I need the bounds check handler
// to be a function)
#define arrchk(xs,index) bounds_check(index, arrlen(xs), __FILE__ ":" STR(__LINE__))
#define arrcpy(dst,src) ((void)((dst)==(src)), /* try to emit warning if not same type */ \
                        arrsetlen(dst,arrlen(src)),\
						memcpy(dst, src, sizeof(*(dst))*arrlen(dst)))
#define arrchkget(xs,index) ((xs)[arrchk(xs,index)])
#define arrchkptr(xs,index) (&(arrchkget(xs,index)))
#define arrreset(xs) arrsetlen(xs,0)
#define arrsetmincap(xs,mincap) ((mincap)>arrcap(xs)?arrsetcap(xs,mincap):0)

#define TODO(S) fprintf(stderr,"TODO(" #S ") at %s:%d\n",__FILE__,__LINE__);
#define TODO_NOW(S) TODO(S);abort();

#define FIXME(S) fprintf(stderr,"FIXME(" #S ") at %s:%d\n",__FILE__,__LINE__);
#define FIXME_NOW(S) FIXME(S);abort();

#define XXX(S) fprintf(stderr,"XXX(" #S ") at %s:%d\n",__FILE__,__LINE__);
#define XXX_NOW(S) XXX(S);abort();

#define UTIL_H
#endif
