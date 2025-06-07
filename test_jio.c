// run with test_jio.sh

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
	for (;;) {
		io_tick();
		sleep_nanoseconds(500000L); // 500µs
	}
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
	const int port_id = io_port_create();
	struct jio* jio = jio_open(pathbuf, IO_CREATE, port_id, 10, &err);
	if (jio == NULL) printf("err code %d\n", err);
	assert(jio != NULL);
	assert(jio_get_size(jio) == 0);

	assert(0 == jio_append(jio, "hello0", 6));
	assert(0 == jio_append(jio, "hello1", 6));
	assert(0 == jio_append(jio, "hello2", 6));
	assert(jio_get_size(jio) == 18);

	for (int i=0; i<3; ++i) {
		char x[6];
		assert(jio_pread(jio, x, 6, i*6) == 6);
		assert(0 == memcmp(x,"hello",5));
		assert(x[5] == ('0'+i));
	}

	jio_close(jio);
}

static void blocking_append_and_read_back(int i, int N)
{
	assert(N>0);
	char pathbuf[1<<10];
	char buf[1<<10];
	snprintf(buf, sizeof buf, "blkapp%.2d", i);
	STATIC_PATH_JOIN(pathbuf, dir, buf)
	int err=0;
	const int port_id = io_port_create();
	struct jio* jio = jio_open(pathbuf, IO_CREATE, port_id, 10, &err);
	if (jio == NULL) printf("err code %d\n", err);
	assert(jio != NULL);
	const char* seq = "0123456789abcde";
	int ack = 0;
	for (int i=0; i<N; ++i) {
		for (;;) {
			const int e = jio_append(jio, seq, 15);
			assert((e==0) || (e==IO_BUFFER_FULL));
			if (e == IO_BUFFER_FULL) {
				struct io_event ev = {0};
				while (io_port_poll(port_id, &ev)) {
					assert(jio_ack(jio, ev.echo));
					++ack;
				}
				jio_clear_error(jio);
				continue;
			} else if (e == 0) {
				break;
			} else {
				assert(!"what");
			}
		}
	}
	assert(ack>0);

	for (int i=0; i<N; ++i) {
		char x[15];
		memset(x,0,sizeof x);
		assert(jio_pread(jio, x, 15, i*15) == 15);
		if (0 != memcmp(x,seq,15)) {
			fprintf(stderr,"at iteration %d, expected [", i);
			for (int ii=0;ii<15;++ii) fprintf(stderr,"%c",seq[ii]);
			fprintf(stderr,"], got [");
			for (int ii=0;ii<15;++ii) fprintf(stderr,"%c",x[ii]);
			fprintf(stderr,"]\n");
			abort();
		}
	}

	jio_close(jio);
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

	io_init();
	thrd_t t = {0};
	assert(0 == thrd_create(&t, io_thread, NULL));

	for (int i=0; i<3; ++i) simple_test(i);
	for (int i=0; i<5; ++i) blocking_append_and_read_back(i,(1+i)*2551);

	return EXIT_SUCCESS;
}
