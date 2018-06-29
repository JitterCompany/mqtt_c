#ifndef BUFFERED_WRITE_H
#define BUFFERED_WRITE_H

#include "socket_HAL.h"
#include <stdint.h>
#include <stddef.h>

typedef struct {
    SocketWriteFunc writer;
    void *writer_ctx;
    size_t size;
    uint8_t *buffer;

    uint8_t *ptr;
    size_t count;
} BufferedWrite;

void buffered_write_init(BufferedWrite *ctx,
        SocketWriteFunc writer, void *writer_ctx,
    uint8_t *buffer, size_t sizeof_buffer);

/**
 * Wraps a SocketWriteFunc to write all-or-nothing without copying overhead
 *
 * This is a three-stage process:
 *
 * 1. buffered_write_ptr() returns a pointer where you can write the data
 *      (up to buffered_write_max_size() bytes). If NULL is returned,
 *      try again later.
 *
 * 2. when done writing, call buffered_write_commit(num_bytes_written).
 * 3. call buffered_write_flush() untill it returns 1
 */
uint8_t *buffered_write_ptr(BufferedWrite *ctx);
bool buffered_write_commit(BufferedWrite *ctx, size_t num_bytes);
size_t buffered_write_max_size(const BufferedWrite *ctx);

// return values: 1 = done, 0 = try again, <0 = error
int buffered_write_flush(BufferedWrite *ctx);

#endif

