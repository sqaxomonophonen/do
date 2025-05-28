#ifndef HTTPSERVER_H

#define LIST_OF_METHODS \
	X(GET) \
	X(POST) \
	X(PUT)

enum http_method {
	UNKNOWN=0,
	#define X(ENUM) ENUM,
	LIST_OF_METHODS
	#undef X
};

void httpserver_init(void);
int httpserver_tick(void);

#define HTTPSERVER_H
#endif
