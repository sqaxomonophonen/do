#ifndef JIO_H

// journal I/O:
//  - can only write at end (jio_append())
//  - can read from anywhere (jio_pread())
//  - writes are typically non-blocking, but may block until there's enough
//    space in the ring buffer
//  - reads are blocking unless the requested data is already in memory.
//    special care is taken to read from the ring buffer, so that recently
//    appended data is immediately visible to jio_pread()

// TODO blocking pwrite()-like writes are useful for streaming .wav-renders
// (because the size must be written into the .wav-header). but it's rare
// enough that we shouldn't optimize for it (figure out something that doesn't
// complicate jio.c much)

// TODO tunable/settable integrity/fdatasync()? DO_JAM_JOURNAL wants frequent
// syncs, but snapshotcache.data is fine with none, even though both are
// superficially the same usecase.

#include <stdint.h>
#include <stddef.h>

#include "io.h"

struct jio;

struct jio* jio_open(const char* path, enum io_open_mode, int port_id, int ringbuf_size_log2, int* out_error);
int jio_close(struct jio*);
int64_t jio_get_size(struct jio*);
int jio_append(struct jio*, const void* ptr, int64_t size);
int jio_pread(struct jio*, void* ptr, int64_t size, int64_t offset);
int jio_pwrite(struct jio*, const void* ptr, int64_t size, int64_t offset);
//int jio_pread_memonly(struct jio*, void* ptr, int64_t size, int64_t offset);
int jio_get_error(struct jio*);
void jio_clear_error(struct jio*);

#ifndef __EMSCRIPTEN__
int jio_ack(struct jio*, io_echo);
#endif

#define JIO_H
#endif
