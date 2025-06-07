#ifndef WEBSERV_H

#include <stdint.h>

void webserv_init(void);
int webserv_tick(void);

void webserv_selftest(void);

int webserv_broadcast_journal(int64_t until_journal_cursor);

#define WEBSERV_H
#endif
