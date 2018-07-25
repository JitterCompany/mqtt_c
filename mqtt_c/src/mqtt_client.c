#include "mqtt_client.h"
#include "socket_HAL.h"
#include <paho_mqtt/MQTTPacket.h>
#include <c_utils/static_assert.h>
#include <string.h>

// make sure the MQTTTransport can be stored in MQTTTransportState.
// The reason for the duplicate definition is that we cannot
// expose paho_mqtt in the public header. (if we do that, the parent
// project must depend on paho_mqtt just for that struct definition).
STATIC_ASSERT(sizeof(struct MQTTTransportStorage) >= sizeof(MQTTTransport));

static void dummy_log(const char format[], ...){}

static int next_packet_id(MQTTClient *ctx);
static const char *state_to_str(enum MQTTState state);
static void set_state(MQTTClient *ctx, enum MQTTState new_state);
static void handle_state_machine(MQTTClient *ctx,
        const bool state_changed, const int incoming_packet_type);

static void handle_keepalive(MQTTClient *ctx, const bool first_time);

// state handler functions (see g_state[] below)
static void state_closed(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type);
static void state_connecting(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type);
static void state_idle(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type);
static void state_ping(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type);
static void state_error(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type);
static void state_closing(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type);
static void state_publish(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type);
static void state_subscribe(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type);
static void state_receiving(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type);

typedef void(*StateFunc)(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type);
typedef struct {
    StateFunc run;
    int timeout;
} State;

/**
 * This table maps states to their handler functions.
 *
 * If a state stays 'stuck' for more than its timeout time,
 * the state machine goes to error -> closing -> closed.
 * Some states have timeout=0: they never timeout.
 *
 * Note: the timeout values are multiplied with the timeout value
 * passed to MQTT_client_init(). Example: if MQTT_client_init() is called
 * with a timeout value of 10 seconds, a state with timeout=2 will have a
 * timeout of 2*10=20 seconds.
 */

static const State g_state[] = {
    [MQTT_CLOSED]       = {.run = state_closed,         .timeout = 0},
    [MQTT_CONNECTING]   = {.run = state_connecting,     .timeout = 2},
    [MQTT_IDLE]         = {.run = state_idle,           .timeout = 0},
    [MQTT_PING]         = {.run = state_ping,           .timeout = 1},
    [MQTT_ERROR]        = {.run = state_error,          .timeout = 1},
    [MQTT_CLOSING]      = {.run = state_closing,        .timeout = 1},
    [MQTT_PUBLISH]      = {.run = state_publish,        .timeout = 1},
    [MQTT_SUBSCRIBE]    = {.run = state_subscribe,      .timeout = 1},
    [MQTT_RECEIVING]    = {.run = state_receiving,      .timeout = 1},
};
#define NUM_STATES (sizeof(g_state)/sizeof(g_state[0]))


void MQTT_client_init(MQTTClient *ctx,
        SocketHAL socket, TimestampFunc time_func,
        OnMessageCallback message_callback, void *message_callback_ctx,
        unsigned int keepalive_sec, unsigned int timeout_sec)
{
    ctx->prev_state = MQTT_CLOSED;
    ctx->state = MQTT_CLOSED;
    ctx->ping = MQTT_PING_IDLE;
    ctx->next_packet_id = 1;
    ctx->last_subscribe_id = 0;

    ctx->keepalive_sec = keepalive_sec;
    ctx->timeout_sec = timeout_sec;
    ctx->socket = socket;
    ctx->time_func = time_func;
    ctx->on_message_cb = message_callback;
    ctx->on_message_cb_ctx = message_callback_ctx;

    ctx->log_debug = dummy_log;
    ctx->log_warning = dummy_log;
    ctx->log_error = dummy_log;

    buffered_write_init(&ctx->writer, socket.write, socket.ctx,
            ctx->write_buffer, sizeof(ctx->write_buffer));
    memset(&ctx->transport, 0, sizeof(ctx->transport));
    MQTTTransport *transport = (MQTTTransport*)&ctx->transport;
	transport->sck = socket.ctx;
	transport->getfn = socket.read;
	transport->state = 0;
}

void MQTT_client_set_logging(MQTTClient *ctx,
        LogFunc debug, LogFunc warn, LogFunc err)
{
    if(debug) {
        ctx->log_debug = debug;
    }
    if(warn) {
        ctx->log_warning = warn;
    }
    if(err) {
        ctx->log_error = err;
    }
}

bool MQTT_client_connect(MQTTClient *ctx, const char *hostname,
        const int port, const char *client_id,
        const char *username, const char *password)
{
    if(ctx->state != MQTT_CLOSED) {
        ctx->log_warning("MQTT: failed to connect (not closed)");
        return false;
    }

    if(!ctx->socket.open(ctx->socket.ctx, hostname, port)) {
        return false;
    }
    // the socket is opened, so the state from here on is 'connecting'.
    // If anything after this point fails, remember to close the socket!

    uint8_t *buffer = buffered_write_ptr(&ctx->writer);
    if(!buffer) {
        ctx->log_warning("MQTT: failed to connect (could not get buffer)");
        ctx->socket.close(ctx->socket.ctx);
        return false;
    }
    const size_t sizeof_buffer = buffered_write_max_size(&ctx->writer);

	MQTTPacket_connectData data = MQTTPacket_connectData_initializer;
	data.clientID.cstring = (char*)client_id;
	data.keepAliveInterval = ctx->keepalive_sec;
	data.cleansession = 1;
    data.username.cstring = (char*)username;
    data.password.cstring = (char*)password;

    const int len = MQTTSerialize_connect(buffer, sizeof_buffer, &data);
    if(!buffered_write_commit(&ctx->writer, len)) {
        ctx->log_warning("MQTT: failed to connect (could not commit buffer)");
        ctx->socket.close(ctx->socket.ctx);
        return false;
    }
    set_state(ctx, MQTT_CONNECTING);
    return true;
}

bool MQTT_client_is_connected(MQTTClient *ctx)
{
    switch(ctx->state) {
        case MQTT_CLOSED:
        case MQTT_CONNECTING:
        case MQTT_ERROR:
        case MQTT_CLOSING:
            return false;
        case MQTT_IDLE:
        case MQTT_PING:
        case MQTT_PUBLISH:
        case MQTT_SUBSCRIBE:
        case MQTT_RECEIVING:
            return true;

        // no default: compiler should warn if a new state is added
        // without explicitly handling it here
    }
    return false;
}

bool MQTT_client_is_disconnected(MQTTClient *ctx)
{
    return (ctx->state == MQTT_CLOSED);
}

void MQTT_client_disconnect(MQTTClient *ctx)
{
    if(ctx->state == MQTT_CLOSED) {
        return;
    }
    set_state(ctx, MQTT_CLOSING);
}

bool MQTT_client_poll(MQTTClient *ctx)
{
    int rx_packet_type = 0;
    if(ctx->state != MQTT_CLOSED) {

        if(buffered_write_flush(&ctx->writer) < 0) {
            ctx->log_debug("MQTT: buffered_write_flush() failed");
            // if flush fails during closing, assume the connection is closed
            if(ctx->state == MQTT_CLOSING) {
                set_state(ctx, MQTT_CLOSED);
            } else {
                set_state(ctx, MQTT_ERROR);
            }
        }
        MQTTTransport *transport = (MQTTTransport*)&ctx->transport;
        rx_packet_type = MQTTPacket_readnb(
                ctx->read_buffer, sizeof(ctx->read_buffer), transport);

        if(rx_packet_type) {
            ctx->log_debug("MQTT: incoming packet (type %d)", rx_packet_type);

            if(rx_packet_type == PUBLISH) {
                set_state(ctx, MQTT_RECEIVING);
            }
        }
    }

    handle_state_machine(ctx, false, rx_packet_type);
    return (ctx->state == MQTT_IDLE);
}

bool MQTT_client_publish(MQTTClient *ctx, const char *topic,
        const uint8_t *payload, size_t sizeof_payload)
{
    if(ctx->state != MQTT_IDLE) {
        return false;
    }

    ctx->log_debug("MQTT: publish on topic '%s'", topic);
    uint8_t *buffer = buffered_write_ptr(&ctx->writer);
    if(!buffer) {
        return false;
    }
    const size_t sizeof_buffer = buffered_write_max_size(&ctx->writer);

    const int dup=0;
    const int qos=0;
    const int retained=0;
    const int packet_id=0;
    const MQTTString topic_name = {.cstring = (char*)topic};

    if(payload == NULL) {
        if(sizeof_payload) {
            return false;
        }
        // Make sure payload 'points to something valid' even though it
        // should not be used because sizeof_payload == 0
        payload = (const uint8_t*)"";
    }

    const int len = MQTTSerialize_publish(buffer, sizeof_buffer,
            dup, qos, retained, packet_id,
            topic_name, (uint8_t*)payload, sizeof_payload);
    if(!buffered_write_commit(&ctx->writer, len)) {
        return false;
    }
    set_state(ctx, MQTT_PUBLISH);
    return true;
}

bool MQTT_client_subscribe(MQTTClient *ctx, const char *topic_filter)
{
    if(ctx->state != MQTT_IDLE) {
        return false;
    }

    uint8_t *buffer = buffered_write_ptr(&ctx->writer);
    if(!buffer) {
        return false;
    }
    const size_t sizeof_buffer = buffered_write_max_size(&ctx->writer);

    const MQTTString topic_name = {.cstring = (char*)topic_filter};
    const int dup = 0;
    const int msg_id = next_packet_id(ctx);
    const int qos = 0;

    // subscribe to one topic at a time. topic_name and qos are treated
    // as array of length 1: MQTTSerialize_subscribe supports subsribing
    // to multiple topic filters at once
    const int len = MQTTSerialize_subscribe(buffer, sizeof_buffer,
            dup, msg_id, 1,
            (MQTTString*)&topic_name, (int*)&qos);

    if(!buffered_write_commit(&ctx->writer, len)) {
        return false;
    }
    ctx->last_subscribe_id = msg_id;
    set_state(ctx, MQTT_SUBSCRIBE);
    return true;
}



static int next_packet_id(MQTTClient *ctx)
{
    // increment: implicitly overflows because next_packet_id is uint16_t
    ctx->next_packet_id++;

    // packet_id must be nonzero to be MQTT compliant
    if(!ctx->next_packet_id) {
        ctx->next_packet_id++;
    }
    return ctx->next_packet_id;
}

static void handle_state_machine(MQTTClient *ctx,
        const bool state_changed, const int incoming_packet_type)
{
    State state = g_state[ctx->state];
    if(state.timeout) {
        const int diff = ctx->time_func() - ctx->timestamp_timeout;
        const int max_diff = (state.timeout * ctx->timeout_sec);
        if(diff >= max_diff) {
            ctx->log_warning("MQTT: state %s timeout (%d/%d sec)",
                    state_to_str(ctx->state), diff, max_diff);

            // edge case: break loop in case of timeout during error/closing
            if(ctx->state == MQTT_CLOSING) {
                set_state(ctx, MQTT_CLOSED);
                return;
            }
            if(ctx->state == MQTT_ERROR) {
                set_state(ctx, MQTT_CLOSING);
                return;
            }
            
            // 'normal' timeout: set error state
            set_state(ctx, MQTT_ERROR);
            return;
        }
    }
    state.run(ctx, state_changed, incoming_packet_type);
}
static const char *state_to_str(enum MQTTState state)
{
    switch(state) {
        case MQTT_CLOSED:
            return "MQTT_CLOSED";
        case MQTT_CONNECTING:
            return "MQTT_CONNECTING";
        case MQTT_IDLE:
            return "MQTT_IDLE";
        case MQTT_PING:
            return "MQTT_PING";
        case MQTT_ERROR:
            return "MQTT_ERROR";
        case MQTT_CLOSING:
            return "MQTT_CLOSING";
        case MQTT_PUBLISH:
            return "MQTT_PUBLISH";
        case MQTT_SUBSCRIBE:
            return "MQTT_SUBSCRIBE";
        case MQTT_RECEIVING:
            return "MQTT_RECEIVING";

        // no default: compiler should warn if a new state is added
        // without explicitly handling it here
    }
    return "UNKNOWN";
}

static void set_state(MQTTClient *ctx, enum MQTTState new_state)
{
    if(new_state == ctx->state) {
        return;
    }
    if(new_state > NUM_STATES) {

        ctx->log_error("MQTT: BUG! unknown state %d!", (int)new_state);
        set_state(ctx, MQTT_ERROR);
        return;
    }

    if(new_state == MQTT_ERROR) {
        ctx->log_debug("MQTT: error while in state %s",
                state_to_str(ctx->state));
    } else {
        ctx->log_debug("MQTT: state to %s", state_to_str(new_state));
    }
    ctx->prev_state = ctx->state;
    ctx->state = new_state;
    ctx->timestamp_timeout = ctx->time_func();
    handle_state_machine(ctx, true, 0);
}

static void state_closed(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type)
{
    if(first_time) {
        buffered_write_reinit(&ctx->writer);
        ctx->ping = MQTT_PING_IDLE;
    }
}
static void state_connecting(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type)
{
    if(incoming_packet_type != CONNACK) {
        return;
    }
    ctx->log_debug("MQTT: got CONNACK");

    unsigned char sessionPresent, connack_rc;
    const int ok = MQTTDeserialize_connack(&sessionPresent, &connack_rc,
                ctx->read_buffer, sizeof(ctx->read_buffer));
    if (ok != 1 || connack_rc != 0) {
        ctx->log_warning("MQTT: connecting failed, return code %d", connack_rc);
        set_state(ctx, MQTT_ERROR);
        return;
    }

    set_state(ctx, MQTT_IDLE);
}

static void handle_keepalive(MQTTClient *ctx, const bool first_time)
{
    // (re-)start the keepalive timer upone changing to idle state
    if(first_time) {
        ctx->log_debug("MQTT: start keepalive timer..");
        ctx->timestamp_keepalive = ctx->time_func();
        return;
    }

    // check timing
    const unsigned int diff = ctx->time_func() - ctx->timestamp_keepalive;
    if(diff >= (ctx->keepalive_sec/2 + 1)) {
        ctx->ping = MQTT_PING_REQUIRED;
    }
    if(diff > ctx->keepalive_sec) {
        ctx->log_debug("MQTT: keepalive timeout (%d sec)", diff);
        set_state(ctx, MQTT_ERROR);
    }

    // try to send ping
    if(ctx->ping == MQTT_PING_REQUIRED) {

        uint8_t *buffer = buffered_write_ptr(&ctx->writer);
        if(!buffer) {
            return;
        }
        const size_t sizeof_buffer = buffered_write_max_size(&ctx->writer);

        int len = MQTTSerialize_pingreq(buffer, sizeof_buffer);
        if(buffered_write_commit(&ctx->writer, len)) {

            ctx->ping = MQTT_PING_BUSY;

            ctx->log_debug("MQTT: PING...");
            set_state(ctx, MQTT_PING);
        }
    }
}

static void state_idle(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type)
{
    handle_keepalive(ctx, first_time);
}

static void state_ping(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type)
{
    if(incoming_packet_type != PINGRESP) {
        return;
    }
    ctx->log_debug("MQTT: got PINGRESP");

    ctx->timestamp_keepalive = ctx->time_func();
    set_state(ctx, MQTT_IDLE);
}

static void state_error(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type)
{
    if(first_time) {
        ctx->log_warning("MQTT: Error! Something went wrong...");
    }
    set_state(ctx, MQTT_CLOSING);
}

static void state_closing(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type)
{
    uint8_t *buffer = buffered_write_ptr(&ctx->writer);
    if(!buffer) {
        return;
    }
    const size_t sizeof_buffer = buffered_write_max_size(&ctx->writer);

	int len = MQTTSerialize_disconnect(buffer, sizeof_buffer);
    if(buffered_write_commit(&ctx->writer, len)) {

        ctx->socket.close(ctx->socket.ctx);
        set_state(ctx, MQTT_CLOSED);
    }
}

static void state_publish(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type)
{
    if(buffered_write_flush(&ctx->writer) == 1) {
        set_state(ctx, MQTT_IDLE);
    }
}

static void state_subscribe(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type)
{
    if(incoming_packet_type != SUBACK) {
        return;
    }
    ctx->log_debug("MQTT: got SUBACK");

    // return values: initialized to invalid
    unsigned short packet_id = 0;
    int sub_count = -1;
    int qos = -1;

    const int ok = MQTTDeserialize_suback(&packet_id, 1, &sub_count, &qos,
            ctx->read_buffer, sizeof(ctx->read_buffer));

    if ((ok != 1)
            || (packet_id != ctx->last_subscribe_id)
            || (sub_count != 1) || (qos != 0)) {
        ctx->log_warning("MQTT: subscribe failed!");
        set_state(ctx, MQTT_ERROR);
        return;
    }
    set_state(ctx, MQTT_IDLE);
}

static void state_receiving(MQTTClient *ctx,
        const bool first_time, const int incoming_packet_type)
{
    if(incoming_packet_type != PUBLISH) {
        return;
    }
    ctx->log_debug("MQTT: got PUBLISH (incoming)");

    uint8_t dup;
    int qos;
    uint8_t retained;
    unsigned short msgid;
    MQTTString mqtt_topic;
    uint8_t *payload = NULL;
    int sizeof_payload = 0;
    const int ok = MQTTDeserialize_publish(&dup, &qos, &retained,
            &msgid, &mqtt_topic, &payload, &sizeof_payload,
            ctx->read_buffer, sizeof(ctx->read_buffer));

    if(ok != 1) {
        ctx->log_warning("MQTT: receiving PUBLISH failed!");
        set_state(ctx, MQTT_ERROR);
        return;
    }

    const char *topic = mqtt_topic.cstring;

    // temporarily store topicname in stack buffer to convert to c-style string
    int t_len = mqtt_topic.lenstring.len;
    char topic_name[t_len+1];
    if(!topic) {

        memcpy(topic_name, mqtt_topic.lenstring.data, t_len);
        topic_name[t_len] = '\0';
        topic = topic_name;
    }
    
    // keep track of the 'old' prev_state, to detect if the message_cb
    // changes the state
    const enum MQTTState prev_state = ctx->prev_state;

    if(ctx->on_message_cb) {
        ctx->on_message_cb(ctx->on_message_cb_ctx, topic,
                payload, (size_t)sizeof_payload);
    } else {
        ctx->log_warning("MQTT: got message on topic '%s', "
                "but no callback is registered!");
    }

    // return to the previous state, unless the state is already modified
    // by the on_message_cb().
    if(ctx->prev_state == prev_state) {
        set_state(ctx, ctx->prev_state);
    }
}

