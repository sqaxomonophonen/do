// cc -O0 -g -Wall allocator.c stb_ds.c jio.c test_jio.c -o _test_jio

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <time.h>
#include <threads.h>

#include "jio.h"

#define PATH_IMPLEMENTATION
#include "path.h"

void sleep_nanoseconds(int64_t ns)
{
	const int64_t one_billion = 1000000000LL;
	const struct timespec ts = {
		.tv_nsec = ns % one_billion,
		.tv_sec  = ns / one_billion,
	};
	nanosleep(&ts, NULL);
}

static int io_thread(void* usr)
{
	jio_thread_run();
	return 0;
}

static char* dir;

static void simple_test(int i)
{
	char pathbuf[1<<10];
	char buf[1<<10];
	snprintf(buf, sizeof buf, "simple%d", i);
	STATIC_PATH_JOIN(pathbuf, dir, buf)
	int err=0;
	struct jio* j0 = jio_open(pathbuf, JIO_CREATE, 10, &err);
	if (j0 == NULL) printf("err code %d\n", err);
	assert(j0 != NULL);
	assert(jio_get_size(j0) == 0);

	const int n0 = jio_get_num_block_sleeps(j0);
	jio_append(j0, "hello0", 6);
	jio_append(j0, "hello1", 6);
	jio_append(j0, "hello2", 6);
	assert(jio_get_size(j0) == 18);
	const int n1 = jio_get_num_block_sleeps(j0);
	assert(n1==n0);

	for (int i=0; i<3; ++i) {
		char x[6];
		assert(0 == jio_pread(j0, x, 6, i*6));
		assert(0 == memcmp(x,"hello",5));
		assert(x[5] == ('0'+i));
	}

	jio_close(j0);
}

static void blocking_append_and_read_back(int i, int N)
{
	assert(N>0);
	char pathbuf[1<<10];
	char buf[1<<10];
	snprintf(buf, sizeof buf, "blkapp%.2d", i);
	STATIC_PATH_JOIN(pathbuf, dir, buf)
	int err=0;
	struct jio* j0 = jio_open(pathbuf, JIO_CREATE, 10, &err);
	if (j0 == NULL) printf("err code %d\n", err);
	assert(j0 != NULL);
	const int n0 = jio_get_num_block_sleeps(j0);
	const char* seq = "0123456789abcde";
	for (int i=0; i<N; ++i) jio_append(j0, seq, 15);
	const int n1 = jio_get_num_block_sleeps(j0);
	assert(n1>n0);

	for (int i=0; i<N; ++i) {
		char x[15];
		memset(x,0,sizeof x);
		assert(0 == jio_pread(j0, x, 15, i*15));
		if (0 != memcmp(x,seq,15)) {
			fprintf(stderr,"at iteration %d, expected [", i);
			for (int ii=0;ii<15;++ii) fprintf(stderr,"%c",seq[ii]);
			fprintf(stderr,"], got [");
			for (int ii=0;ii<15;++ii) fprintf(stderr,"%c",x[ii]);
			fprintf(stderr,"]\n");
			abort();
		}
	}

	jio_close(j0);
}

int main(int argc, char** argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <dir>\n", argv[0]);
		fprintf(stderr, "it creates test files inside that dir\n");
		fprintf(stderr, "maybe try ./test_jio.sh\n");
		exit(EXIT_FAILURE);
	}
	dir=argv[1];

	jio_init();
	thrd_t t = {0};
	assert(0 == thrd_create(&t, io_thread, NULL));

	for (int i=0; i<3; ++i) simple_test(i);
	for (int i=0; i<5; ++i) blocking_append_and_read_back(i,(1+i)*2551);

	return EXIT_SUCCESS;
}
