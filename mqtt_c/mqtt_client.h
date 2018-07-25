#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdbool.h>
#include "socket_HAL.h"
#include "timestamp_HAL.h"
#include "buffered_write.h"

enum MQTTState {
    MQTT_CLOSED = 0,
    MQTT_CONNECTING,
    MQTT_IDLE,
    MQTT_PING,
    MQTT_ERROR,
    MQTT_CLOSING,
    MQTT_PUBLISH,
    MQTT_SUBSCRIBE,
    MQTT_RECEIVING,
};

enum MQTTPingState {
    MQTT_PING_IDLE = 0,
    MQTT_PING_REQUIRED,
    MQTT_PING_BUSY,
};

// NOTE: payload may be NULL if a message without payload is received
typedef void (*OnMessageCallback)(void *cb_ctx, const char *topic,
        uint8_t *payload, const size_t sizeof_payload);

typedef void (*LogFunc)(const char format[], ...);

// This is a placeholder struct. See mqtt_client.c
struct MQTTTransportStorage{
    void *a,*b;
    int c,d,e;
    char f;
};

typedef struct {
    enum MQTTState prev_state;
    enum MQTTState state;
    enum MQTTPingState ping;
    uint64_t timestamp_timeout;
    uint64_t timestamp_keepalive;
    uint16_t next_packet_id;
    uint16_t last_subscribe_id;

    unsigned int keepalive_sec;
    unsigned int timeout_sec;
    SocketHAL socket;
    TimestampFunc time_func;
    OnMessageCallback on_message_cb;
    void *on_message_cb_ctx;

    LogFunc log_debug;
    LogFunc log_warning;
    LogFunc log_error;

    struct MQTTTransportStorage transport;
    uint8_t write_buffer[256];
    uint8_t read_buffer[256];
    BufferedWrite writer;

} MQTTClient;

/**
 * Initialize a MQTTClient
 *
 * @param socket    An initialized SocketHAL implementation. Its purpose
 *                  is to abstract (network-)socket functions so that they
 *                  can be switched easily.
 *
 * @param time_func A function that should return the current time in seconds.
 *                  It does not matter what the time offset is (e.g. starting
 *                  at 0 is fine), but the difference between timestamps
 *                  should be correct.
 *
 * @param message_callback      A function that is called whenever a message
 *                              is received (incoming PUBLISH).
 *                              Its arguments are only valid untill the
 *                              callback returns..
 *
 * @param message_callback_ctx  An optional pointer that is passed to the
 *                              message_callback (NULL is also valid).
 *
 * @param keepalive The maximum keepalive timeout in seconds.
 *                  Keepalive (MQTT PINGREQ) packets will be sent while idle,
 *                  at least as frequent as this timeout.
 *
 * @param timeout   This timeout relates to how long the MQTT client will wait
 *                  before giving up and assuming the network is down.
 *                  The timeout (in seconds) should be larger than the largest 
 *                  amount of expected network latency. A larger timeout has no
 *                  effect on performance in normal network conditions.
 *                  Recommended value: > 5 seconds.
 *
 */
void MQTT_client_init(MQTTClient *ctx,
        SocketHAL socket, TimestampFunc time_func,
        OnMessageCallback message_callback, void *message_callback_ctx,
        unsigned int keepalive_sec, unsigned int timeout_sec);

/**
 * Set logging functions
 *
 * This is optional, by default there is no logging.
 *
 * To disable a level, pass NULL
 */
void MQTT_client_set_logging(MQTTClient *ctx,
        LogFunc debug, LogFunc warn, LogFunc err);

/**
 * Connect the client to the broker
 * 
 * @param client_id     A unique identifier for this client.
 *                      For example: use a device serial number,
 *                      or randomly generated string. It is recommended to
 *                      re-use the same client_id on each connection.
 *
 * @param username
 * @param password      Username and password for authenticating to the broker.
 *                      Set to NULL to connect as anonymous user.
 *
 * NOTE: This is non-blocking.
 * Connecting is finished when MQTT_client_is_connected() returns true.
 * If MQTT_client_is_disconnected() returns true, the connection failed
 * or has closed.
 */
bool MQTT_client_connect(MQTTClient *ctx, const char *hostname,
        const int port, const char *client_id,
        const char *username, const char *password);

/**
 * Check if the client is connected
 *
 * NOTE:
 * If the client is NOT connected, this does not mean it is fully
 * disconnected (e.g. it may be busy connecting or disconnecting).
 * 
 * For checking if the client is disconnected, @see MQTT_client_is_disconnected.
 */
bool MQTT_client_is_connected(MQTTClient *ctx);

/**
 * Check if the client is disconnected
 *
 * NOTE:
 * If the client is NOT disconnected, this does not mean it is connected
 * (e.g. it may be busy connecting or disconnecting).
 * 
 * For checking if the client is connected, @see MQTT_client_is_connected.
 */
bool MQTT_client_is_disconnected(MQTTClient *ctx);

/**
 * Disconnect the client
 *
 * Disconnecting is finished when MQTT_client_is_disconnected() returns true
 */
void MQTT_client_disconnect(MQTTClient *ctx);

/**
 * Poll: advance the internal state machine when possible
 *
 * Make sure to call this regularly.
 *
 * @return True means the client is 'idle': it is ready for a publish
 * or subscribe.
 */
bool MQTT_client_poll(MQTTClient *ctx);


/**
 * Publish a message (QOS=0)
 *
 * If succesfull, the message has been copied to an internal buffer and
 * sending is in progress. Because it is sent with QOS=0, there is no feedback.
 *
 * @return  False means try again, True is succes.
 */
bool MQTT_client_publish(MQTTClient *ctx, const char *topic,
        const uint8_t *payload, size_t sizeof_payload);

/**
 * Subscribe on a topic filter (QOS=0)
 *
 * If sucesfull, the client will be subscribed shortly.
 * An internal state machine waits for the SUBACK before allowing
 * new subscribes or messages to be published.
 */
bool MQTT_client_subscribe(MQTTClient *ctx, const char *topic_filter);
#endif

