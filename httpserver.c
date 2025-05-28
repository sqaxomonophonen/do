#include <stdio.h>
#include <assert.h>

#include "httpserver.h"
#include "io.h"
#include "util.h"

#define LISTEN_ECHO (-1)

static struct {
	int port_id;
	int listen_file_id;
	char buf[1<<10]; // XXX
} g;

void httpserver_init(void)
{
	g.port_id = io_port_create();
	io_echo echo0 = { .i64 = LISTEN_ECHO };
	g.listen_file_id = io_listen_tcp(6510, g.port_id, echo0);
}

int httpserver_tick(void)
{
	int did_work = 0;
	struct io_event ev;
	while (io_port_poll(g.port_id, &ev)) {
		if (ev.echo.i64 == LISTEN_ECHO) {
			printf("http ev status=%d!\n", ev.status);
			if (ev.status >= 0) {
				io_addr(ev.status);
				did_work = 1;
			}
			io_echo echo = { .i64 = 42 };
			io_port_read(g.port_id, echo, ev.status, g.buf, sizeof g.buf);
		} else {
			printf("%ld\n", ev.echo.i64);
		}
	}
	return did_work;
}
