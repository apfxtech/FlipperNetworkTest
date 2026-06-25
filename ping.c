#include <furi.h>
#include <gui/gui.h>
#include <network/network.h>
#include <string.h>

#define PING_CONNECTION_ID 11
#define PING_HOST          "1.1.1.1"
#define PING_PORT          443
#define PING_TIMEOUT_MS    3000
#define PING_INTERVAL_MS   1000
#define PING_PAYLOAD_BYTES 56
#define PING_COUNT         4

typedef enum {
    PingStateInit,
    PingStateNoBridge,
    PingStateRunning,
    PingStateDone,
    PingStateError,
} PingState;

typedef struct {
    bool received;
    bool timeout;
    uint32_t seq;
    uint32_t time_ms;
    NetworkError error;
} PingReply;

typedef struct {
    FuriMutex* mutex;
    PingState state;
    NetworkError error;
    char resolved_ip[48];
    PingReply replies[PING_COUNT];
    uint32_t sent;
    uint32_t received;
    uint32_t min_ms;
    uint32_t max_ms;
    uint32_t total_ms;
    uint32_t active_seq;
    uint32_t started_tick;
    bool probe_active;
    bool start_probe;
} PingApp;

static void ping_record_success(PingApp* app, uint32_t elapsed_ms) {
    if(app->active_seq >= PING_COUNT) return;
    PingReply* reply = &app->replies[app->active_seq];
    reply->received = true;
    reply->timeout = false;
    reply->seq = app->active_seq;
    reply->time_ms = elapsed_ms ? elapsed_ms : 1;
    reply->error = NetworkErrorNone;

    app->received++;
    app->total_ms += reply->time_ms;
    if(app->received == 1 || reply->time_ms < app->min_ms) app->min_ms = reply->time_ms;
    if(reply->time_ms > app->max_ms) app->max_ms = reply->time_ms;
}

static void ping_record_failure(PingApp* app, NetworkError error) {
    if(app->active_seq >= PING_COUNT) return;
    PingReply* reply = &app->replies[app->active_seq];
    reply->received = false;
    reply->timeout = true;
    reply->seq = app->active_seq;
    reply->time_ms = 0;
    reply->error = error;
}

static void ping_finish_probe(PingApp* app) {
    app->probe_active = false;
    app->active_seq++;
    app->start_probe = app->active_seq < PING_COUNT;
    if(app->active_seq >= PING_COUNT) {
        app->state = app->received ? PingStateDone : PingStateError;
        if(!app->received && app->error == NetworkErrorNone) app->error = NetworkErrorTimeout;
    }
}

static void ping_event_callback(const NetworkEvent* event, void* context) {
    if(event->connection_id != PING_CONNECTION_ID) return;
    PingApp* app = context;

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(event->type == NetworkEventConnected && app->probe_active) {
        if(event->error == NetworkErrorNone && event->state == NetworkStateConnected) {
            uint32_t elapsed_ms = furi_get_tick() - app->started_tick;
            ping_record_success(app, elapsed_ms);
            if(event->resolved_ip) strlcpy(app->resolved_ip, event->resolved_ip, sizeof(app->resolved_ip));
            ping_finish_probe(app);
        } else {
            app->error = event->error;
            ping_record_failure(app, event->error);
            ping_finish_probe(app);
        }
    } else if(event->type == NetworkEventStateChanged && event->state == NetworkStateError && app->probe_active) {
        app->error = event->error;
        ping_record_failure(app, event->error);
        ping_finish_probe(app);
    }
    furi_mutex_release(app->mutex);
}

static void ping_render_callback(Canvas* canvas, void* context) {
    PingApp* app = context;
    furi_mutex_acquire(app->mutex, FuriWaitForever);

    char line[64];
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 0, 8, "PING 1.1.1.1 (56 bytes)");

    uint32_t cursor = app->active_seq;
    if(app->probe_active && cursor < PING_COUNT) {
        cursor++;
    }
    uint32_t first = cursor > 2 ? cursor - 2 : 0;
    uint32_t visible = cursor - first;
    if(visible > 2) visible = 2;

    for(uint32_t slot = 0; slot < visible; slot++) {
        uint32_t i = first + slot;
        const PingReply* reply = &app->replies[i];
        uint8_t y = 21 + slot * 20;
        if(reply->received) {
            snprintf(
                line,
                sizeof(line),
                "seq=%lu ttl=57 size=%u",
                (unsigned long)reply->seq,
                PING_PAYLOAD_BYTES);
            canvas_draw_str(canvas, 0, y, line);
            snprintf(
                line,
                sizeof(line),
                "time=%lu ms host=%s",
                (unsigned long)reply->time_ms,
                PING_HOST);
            canvas_draw_str(canvas, 0, y + 9, line);
        } else if(reply->timeout) {
            snprintf(line, sizeof(line), "seq=%lu ttl=? size=%u", (unsigned long)i, PING_PAYLOAD_BYTES);
            canvas_draw_str(canvas, 0, y, line);
            snprintf(line, sizeof(line), "%s", network_error_to_string(reply->error));
            canvas_draw_str(canvas, 0, y + 9, line);
        } else if(app->probe_active && app->active_seq == i) {
            snprintf(line, sizeof(line), "seq=%lu ttl=? size=%u", (unsigned long)i, PING_PAYLOAD_BYTES);
            canvas_draw_str(canvas, 0, y, line);
            canvas_draw_str(canvas, 0, y + 9, "waiting...");
        } else {
            line[0] = '\0';
            canvas_draw_str(canvas, 0, y, line);
        }
    }

    if(app->state == PingStateNoBridge) {
        canvas_draw_str(canvas, 0, 63, "No USB/BLE connection");
    } else if(app->state == PingStateDone || app->state == PingStateError) {
        uint32_t loss = PING_COUNT ? ((PING_COUNT - app->received) * 100) / PING_COUNT : 0;
        if(app->received) {
            snprintf(
                line,
                sizeof(line),
                "%lu/%u rx %lu%% loss %lu/%lu/%lums",
                (unsigned long)app->received,
                PING_COUNT,
                (unsigned long)loss,
                (unsigned long)app->min_ms,
                (unsigned long)(app->total_ms / app->received),
                (unsigned long)app->max_ms);
        } else {
            snprintf(line, sizeof(line), "0/%u rx, 100%% loss: %s", PING_COUNT, network_error_to_string(app->error));
        }
        canvas_draw_str(canvas, 0, 63, line);
    } else if(app->resolved_ip[0]) {
        snprintf(line, sizeof(line), "resolved %s", app->resolved_ip);
        canvas_draw_str(canvas, 0, 63, line);
    } else {
        canvas_draw_str(canvas, 0, 63, "TCP RTT probe via RPC Network");
    }

    furi_mutex_release(app->mutex);
}

static void ping_input_callback(InputEvent* input_event, void* context) {
    FuriMessageQueue* event_queue = context;
    furi_message_queue_put(event_queue, input_event, FuriWaitForever);
}

int32_t network_ping_app(void* p) {
    UNUSED(p);

    PingApp* app = malloc(sizeof(PingApp));
    memset(app, 0, sizeof(PingApp));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->state = PingStateInit;
    app->error = NetworkErrorNone;
    app->start_probe = true;

    FuriMessageQueue* event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    Network* network = furi_record_open(RECORD_NETWORK);
    network_set_event_callback(network, ping_event_callback, app);

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, ping_render_callback, app);
    view_port_input_callback_set(view_port, ping_input_callback, event_queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);

    InputEvent event;
    uint32_t next_probe_tick = 0;
    for(bool processing = true; processing;) {
        if(furi_message_queue_get(event_queue, &event, 100) == FuriStatusOk) {
            if(event.type == InputTypeShort && event.key == InputKeyBack) processing = false;
        }

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        bool start_probe = app->start_probe && !app->probe_active && app->active_seq < PING_COUNT &&
                           furi_get_tick() >= next_probe_tick;
        if(start_probe) {
            app->start_probe = false;
            app->probe_active = true;
            app->sent++;
            app->started_tick = furi_get_tick();
            app->state = PingStateRunning;
        }
        bool timed_out = app->probe_active &&
                         furi_get_tick() - app->started_tick > furi_ms_to_ticks(PING_TIMEOUT_MS);
        if(timed_out) {
            app->error = NetworkErrorTimeout;
            ping_record_failure(app, NetworkErrorTimeout);
            ping_finish_probe(app);
        }
        furi_mutex_release(app->mutex);

        if(start_probe) {
            network_close(network, PING_CONNECTION_ID);
            bool connected = network_connect(
                network,
                PING_CONNECTION_ID,
                PING_HOST,
                PING_PORT,
                NetworkProtocolTcp,
                PING_TIMEOUT_MS);
            if(!connected) {
                furi_mutex_acquire(app->mutex, FuriWaitForever);
                app->state = PingStateNoBridge;
                app->probe_active = false;
                app->start_probe = false;
                furi_mutex_release(app->mutex);
            }
            next_probe_tick = furi_get_tick() + furi_ms_to_ticks(PING_INTERVAL_MS);
        }

        view_port_update(view_port);
    }

    network_close(network, PING_CONNECTION_ID);
    network_set_event_callback(network, NULL, NULL);
    furi_record_close(RECORD_NETWORK);

    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_message_queue_free(event_queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
