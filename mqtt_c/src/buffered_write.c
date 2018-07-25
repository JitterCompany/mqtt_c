#include "buffered_write.h"
#include <string.h>


void buffered_write_init(BufferedWrite *ctx,
        SocketWriteFunc writer, void *writer_ctx,
    uint8_t *buffer, size_t sizeof_buffer)
{
    ctx->writer = writer;
    ctx->writer_ctx = writer_ctx;
    ctx->size = sizeof_buffer;
    ctx->buffer = buffer;

    buffered_write_reinit(ctx);
}

void buffered_write_reinit(BufferedWrite *ctx)
{
    ctx->ptr = ctx->buffer;
    ctx->count = 0;
}

int buffered_write_flush(BufferedWrite *ctx)
{
    const size_t to_write = ctx->count;
    if(!to_write) {
        return 1;
    }

    int w = ctx->writer(ctx->writer_ctx, ctx->ptr, to_write);
    if(w <= 0) {
        return w;
    }
    ctx->ptr+= w;
    ctx->count-= w;
    return (ctx->count) ? 0 : 1;
}

// pointer-based api: new

size_t buffered_write_max_size(const BufferedWrite *ctx)
{
    return ctx->size;
}
uint8_t *buffered_write_ptr(BufferedWrite *ctx)
{
    if(buffered_write_flush(ctx) != 1) {
        return NULL;
    }
    ctx->ptr = ctx->buffer;
    return ctx->buffer;
}

bool buffered_write_commit(BufferedWrite *ctx, size_t num_bytes)
{
    // detect if caller is doing something stupid
    if((num_bytes > ctx->size) || (ctx->ptr != ctx->buffer)) {
        return false;
    }

    ctx->count = num_bytes;
    return true;
}

