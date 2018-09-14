#ifndef SOCKET_HAL_H
#define SOCKET_HAL_H

#include <stdbool.h>

// returns false on failure
typedef bool (*SocketOpenFunc)(void *void_ctx,
        const char *hostname, const int port);

// try again when it returns false
typedef bool (*SocketCloseFunc)(void *void_ctx);

// these return -1 for error, 0 for call again, or the number of bytes read
typedef int (*SocketReadFunc)(void *void_ctx,
        unsigned char* buffer, int sizeof_buffer);
typedef int (*SocketWriteFunc)(void *void_ctx,
        const unsigned char* buffer, int sizeof_buffer);

enum socket_result {
    SOCKET_ERROR = -1,
    SOCKET_TRY_AGAIN = 0,
};

typedef struct {
    void *ctx;

    SocketOpenFunc open;
    SocketCloseFunc close;
    
    SocketReadFunc read;
    SocketWriteFunc write;

} SocketHAL;

#endif

