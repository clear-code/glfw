#define _POSIX_C_SOURCE 200809L

//========================================================================
// GLFW X11 Fcitx5 IME module prototype
//========================================================================

#include "x11_ime_module.h"

#include <dbus/dbus.h>
#include <pthread.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char* FCITX5_SERVICE = "org.fcitx.Fcitx5";
static const char* FCITX5_PATH = "/org/freedesktop/portal/inputmethod";
static const char* FCITX5_INTERFACE = "org.fcitx.Fcitx.InputMethod1";
static const char* FCITX5_INPUT_INTERFACE = "org.fcitx.Fcitx.InputContext1";

#define FCITX5_CAP_PREEDIT               (1ULL << 1)
#define FCITX5_CAP_FORMATTED_PREEDIT     (1ULL << 4)
#define FCITX5_CAP_CLIENT_UNFOCUS_COMMIT (1ULL << 5)
#define FCITX5_CAP_KEY_EVENT_ORDER_FIX   (1ULL << 37)

enum
{
    FCITX5_SHIFT_MASK = 1 << 0,
    FCITX5_LOCK_MASK = 1 << 1,
    FCITX5_CONTROL_MASK = 1 << 2,
    FCITX5_MOD1_MASK = 1 << 3,
    FCITX5_MOD2_MASK = 1 << 4,
    FCITX5_MOD4_MASK = 1 << 6
};

typedef enum CommandType
{
    COMMAND_KEY,
    COMMAND_FOCUS_IN,
    COMMAND_FOCUS_OUT,
    COMMAND_CURSOR_RECT,
    COMMAND_RESET,
    COMMAND_SET_STATUS,
    COMMAND_STOP
} CommandType;

typedef enum EventType
{
    EVENT_COMMIT,
    EVENT_PREEDIT,
    EVENT_CLEAR_PREEDIT,
    EVENT_STATUS
} EventType;

typedef struct Request
{
    unsigned long id;
    GLFWx11IMEKeyEvent event;
    int completed;
    int handled;
    int timed_out;
    int failed;
    double queued_at;
    double completed_at;
    int refs;
    struct Request* next_recent;
} Request;

typedef struct Command
{
    CommandType type;
    void* window;
    unsigned long x11_window;
    int x, y, w, h;
    int status;
    Request* request;
    struct Command* next;
} Command;

typedef struct QueuedEvent
{
    EventType type;
    void* window;
    char* text;
    int caret;
    int* block_sizes;
    int block_count;
    int focused_block;
    unsigned long request_id;
    int request_timed_out;
    double timestamp;
    struct QueuedEvent* next;
} QueuedEvent;

typedef struct PreeditInfo
{
    char* text;
    int caret;
    int* block_sizes;
    int block_count;
    int focused_block;
} PreeditInfo;

struct GLFWx11IMEBackend
{
    GLFWx11IMEHostAPI host;
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int running;
    int ready;
    int status;
    double timeout_ms;
    double reconnect_at;
    double reconnect_interval;
    void* focused_window;
    void* active_window;
    unsigned long next_request_id;
    unsigned long active_request_id;
    unsigned long last_request_id;
    // Commands are written by the GLFW/X11 thread and consumed by the worker.
    Command* command_head;
    Command* command_tail;
    // Events are written by the worker and drained by GLFW on the main thread.
    // The worker never calls GLFW callbacks directly.
    QueuedEvent* event_head;
    QueuedEvent* event_tail;
    // Recent requests are retained only to observe late replies and signals
    // after a ProcessKeyEvent timeout.
    Request* recent;
    DBusConnection* connection;
    char* input_context_path;
    int filter_added;
};

static char* xstrdup(const char* string)
{
    size_t length;
    char* copy;

    if (!string)
        return NULL;

    length = strlen(string) + 1;
    copy = malloc(length);
    if (copy)
        memcpy(copy, string, length);
    return copy;
}

static double now_seconds(GLFWx11IMEBackend* backend)
{
    if (backend->host.get_time)
        return backend->host.get_time();

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1000000000.0;
}

static void log_line(GLFWx11IMEBackend* backend, const char* fmt, ...)
{
    char buffer[1024];
    const char* debug = getenv("GLFW_IME_DEBUG");
    va_list vl;
    va_start(vl, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, vl);
    va_end(vl);

    if (backend->host.log)
        backend->host.log(buffer);
    else if (debug && *debug && strcmp(debug, "0") != 0)
        fprintf(stderr, "glfw-fcitx5: %s\n", buffer);
}

static uint32_t fcitx5_state_from_glfw(unsigned int mods)
{
    uint32_t state = 0;

    if (mods & GLFW_MOD_SHIFT)
        state |= FCITX5_SHIFT_MASK;
    if (mods & GLFW_MOD_CAPS_LOCK)
        state |= FCITX5_LOCK_MASK;
    if (mods & GLFW_MOD_CONTROL)
        state |= FCITX5_CONTROL_MASK;
    if (mods & GLFW_MOD_ALT)
        state |= FCITX5_MOD1_MASK;
    if (mods & GLFW_MOD_NUM_LOCK)
        state |= FCITX5_MOD2_MASK;
    if (mods & GLFW_MOD_SUPER)
        state |= FCITX5_MOD4_MASK;

    return state;
}

static void release_request(GLFWx11IMEBackend* backend, Request* request)
{
    int refs;

    if (!request)
        return;

    refs = --request->refs;
    if (refs == 0)
        free(request);
    (void) backend;
}

static Request* find_recent(GLFWx11IMEBackend* backend, unsigned long id)
{
    for (Request* request = backend->recent;  request;  request = request->next_recent)
    {
        if (request->id == id)
            return request;
    }

    return NULL;
}

static void remember_recent(GLFWx11IMEBackend* backend, Request* request)
{
    int count = 0;
    Request* prev = NULL;
    Request* item;

    request->refs++;
    request->next_recent = backend->recent;
    backend->recent = request;

    item = backend->recent;
    while (item)
    {
        count++;
        if (count > 64)
        {
            Request* old = item;
            if (prev)
                prev->next_recent = NULL;
            while (old)
            {
                Request* next = old->next_recent;
                old->next_recent = NULL;
                release_request(backend, old);
                old = next;
            }
            break;
        }
        prev = item;
        item = item->next_recent;
    }
}

static void enqueue_event(GLFWx11IMEBackend* backend,
                          EventType type,
                          void* window,
                          const char* text,
                          int caret,
                          const int* block_sizes,
                          int block_count,
                          int focused_block)
{
    QueuedEvent* event = calloc(1, sizeof(QueuedEvent));
    unsigned long request_id;
    Request* request;

    if (!event)
        return;

    pthread_mutex_lock(&backend->mutex);

    // Fcitx5 signals do not identify the ProcessKeyEvent that caused them.
    // For instrumentation we attribute them to the active request, falling
    // back to the most recent request, and record whether that request timed out.
    request_id = backend->active_request_id ? backend->active_request_id :
                                             backend->last_request_id;
    request = find_recent(backend, request_id);

    event->type = type;
    event->window = window ? window :
                    request ? request->event.window :
                    backend->active_window ? backend->active_window :
                    backend->focused_window;
    event->text = text ? xstrdup(text) : NULL;
    if (block_sizes && block_count > 0)
    {
        event->block_sizes = calloc((size_t) block_count, sizeof(int));
        if (event->block_sizes)
        {
            memcpy(event->block_sizes, block_sizes,
                   sizeof(int) * (size_t) block_count);
            event->block_count = block_count;
        }
    }
    event->focused_block = focused_block;
    event->caret = caret;
    event->request_id = request_id;
    event->request_timed_out = request ? request->timed_out : 0;
    event->timestamp = now_seconds(backend);

    if (backend->event_tail)
        backend->event_tail->next = event;
    else
        backend->event_head = event;
    backend->event_tail = event;

    pthread_mutex_unlock(&backend->mutex);

    log_line(backend,
             "event queued type=%i request=%lu timed_out=%i timestamp=%.6f caret=%i blocks=%i focused=%i text='%s'",
             type, event->request_id, event->request_timed_out,
             event->timestamp, event->caret, event->block_count,
             event->focused_block, text ? text : "");

    if (backend->host.post_empty_event)
        backend->host.post_empty_event();
}

static Command* pop_command(GLFWx11IMEBackend* backend)
{
    Command* command = backend->command_head;
    if (command)
    {
        backend->command_head = command->next;
        if (!backend->command_head)
            backend->command_tail = NULL;
    }
    return command;
}

static void push_command(GLFWx11IMEBackend* backend, Command* command)
{
    pthread_mutex_lock(&backend->mutex);
    if (backend->command_tail)
        backend->command_tail->next = command;
    else
        backend->command_head = command;
    backend->command_tail = command;
    pthread_cond_signal(&backend->cond);
    pthread_mutex_unlock(&backend->mutex);
}

static int call_no_reply(GLFWx11IMEBackend* backend, const char* method, int first_type, ...)
{
    DBusMessage* message;
    va_list vl;
    dbus_uint32_t serial = 0;

    if (!backend->connection || !backend->input_context_path)
        return GLFW_FALSE;

    message = dbus_message_new_method_call(FCITX5_SERVICE,
                                           backend->input_context_path,
                                           FCITX5_INPUT_INTERFACE,
                                           method);
    if (!message)
        return GLFW_FALSE;

    va_start(vl, first_type);
    if (first_type != DBUS_TYPE_INVALID &&
        !dbus_message_append_args_valist(message, first_type, vl))
    {
        va_end(vl);
        dbus_message_unref(message);
        return GLFW_FALSE;
    }
    va_end(vl);

    if (!dbus_connection_send(backend->connection, message, &serial))
    {
        dbus_message_unref(message);
        return GLFW_FALSE;
    }

    dbus_connection_flush(backend->connection);
    dbus_message_unref(message);
    return GLFW_TRUE;
}

static int utf8_count(const char* text)
{
    int count = 0;
    const unsigned char* p = (const unsigned char*) text;

    while (p && *p)
    {
        if ((*p & 0xc0) != 0x80)
            count++;
        p++;
    }

    return count;
}

static int normalize_fcitx5_index(const char* text, int char_count, int index)
{
    const char* p = text;
    int chars = 0;

    if (index <= 0)
        return 0;

    if (index <= char_count)
        return index;

    for (int byte = 0;  p && *p;  byte++)
    {
        if (byte == index)
            return chars;
        if (((unsigned char) *p & 0xc0) != 0x80)
            chars++;
        p++;
    }

    return char_count;
}

static int append_text(char** buffer, size_t* length, size_t* capacity, const char* text)
{
    size_t text_length;
    char* next;

    if (!text)
        text = "";

    text_length = strlen(text);
    if (*length + text_length + 1 > *capacity)
    {
        size_t next_capacity = *capacity ? *capacity * 2 : 32;
        while (*length + text_length + 1 > next_capacity)
            next_capacity *= 2;

        next = realloc(*buffer, next_capacity);
        if (!next)
            return GLFW_FALSE;

        *buffer = next;
        *capacity = next_capacity;
    }

    memcpy(*buffer + *length, text, text_length);
    *length += text_length;
    (*buffer)[*length] = '\0';
    return GLFW_TRUE;
}

static int append_preedit_block_size(PreeditInfo* info, int size)
{
    int* block_sizes;

    if (size <= 0)
        return GLFW_TRUE;

    block_sizes = realloc(info->block_sizes,
                          sizeof(int) * (size_t) (info->block_count + 1));
    if (!block_sizes)
        return GLFW_FALSE;

    info->block_sizes = block_sizes;
    info->block_sizes[info->block_count++] = size;
    return GLFW_TRUE;
}

static int parse_fcitx5_formatted_preedit(GLFWx11IMEBackend* backend,
                                          DBusMessage* message,
                                          PreeditInfo* info)
{
    DBusMessageIter iter, array;
    size_t length = 0;
    size_t capacity = 0;
    int char_count = 0;
    int segment_index = 0;
    int focused_segment = -1;

    memset(info, 0, sizeof(*info));
    info->caret = -1;

    if (!dbus_message_iter_init(message, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_ARRAY)
    {
        return GLFW_FALSE;
    }

    dbus_message_iter_recurse(&iter, &array);
    while (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_INVALID)
    {
        DBusMessageIter structure;
        const char* segment = NULL;
        dbus_int32_t format = 0;
        int segment_count;

        if (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_STRUCT)
        {
            free(info->text);
            free(info->block_sizes);
            info->text = NULL;
            info->block_sizes = NULL;
            return GLFW_FALSE;
        }

        dbus_message_iter_recurse(&array, &structure);
        if (dbus_message_iter_get_arg_type(&structure) != DBUS_TYPE_STRING)
        {
            free(info->text);
            free(info->block_sizes);
            info->text = NULL;
            info->block_sizes = NULL;
            return GLFW_FALSE;
        }

        dbus_message_iter_get_basic(&structure, &segment);

        if (!dbus_message_iter_next(&structure) ||
            dbus_message_iter_get_arg_type(&structure) != DBUS_TYPE_INT32)
        {
            free(info->text);
            free(info->block_sizes);
            info->text = NULL;
            info->block_sizes = NULL;
            return GLFW_FALSE;
        }

        dbus_message_iter_get_basic(&structure, &format);
        segment_count = utf8_count(segment);

        log_line(backend,
                 "preedit segment index=%i chars=%i format=%i text='%s'",
                 segment_index, segment_count, format, segment ? segment : "");

        if (format != 0 && focused_segment < 0)
            focused_segment = segment_index;

        if (!append_text(&info->text, &length, &capacity, segment) ||
            !append_preedit_block_size(info, segment_count))
        {
            free(info->text);
            free(info->block_sizes);
            info->text = NULL;
            info->block_sizes = NULL;
            return GLFW_FALSE;
        }

        char_count += segment_count;
        segment_index++;
        dbus_message_iter_next(&array);
    }

    if (!info->text)
        info->text = xstrdup("");

    if (dbus_message_iter_next(&iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_INT32)
    {
        dbus_int32_t caret = 0;
        dbus_message_iter_get_basic(&iter, &caret);
        info->caret = normalize_fcitx5_index(info->text, char_count, caret);
    }

    if (info->caret < 0)
        info->caret = char_count;

    if (info->block_count <= 0 && char_count > 0)
        append_preedit_block_size(info, char_count);

    if (focused_segment >= 0 && focused_segment < info->block_count)
        info->focused_block = focused_segment;
    else
    {
        for (int i = 0, offset = 0;  i < info->block_count;  i++)
        {
            const int next = offset + info->block_sizes[i];
            if (info->caret >= offset && (info->caret < next || i == info->block_count - 1))
            {
                info->focused_block = i;
                break;
            }
            offset = next;
        }
    }

    return GLFW_TRUE;
}

static int parse_create_input_context_reply(DBusMessage* reply,
                                            const char** path)
{
    DBusMessageIter iter;

    *path = NULL;

    if (!dbus_message_iter_init(reply, &iter) ||
        dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_OBJECT_PATH)
    {
        return GLFW_FALSE;
    }

    dbus_message_iter_get_basic(&iter, path);
    return *path && **path;
}

static DBusHandlerResult dbus_filter(DBusConnection* connection,
                                     DBusMessage* message,
                                     void* data)
{
    GLFWx11IMEBackend* backend = data;
    PreeditInfo info;
    (void) connection;

    if (dbus_message_is_signal(message, FCITX5_INPUT_INTERFACE, "CommitString"))
    {
        const char* text = NULL;
        DBusError error;

        dbus_error_init(&error);
        if (dbus_message_get_args(message, &error,
                                  DBUS_TYPE_STRING, &text,
                                  DBUS_TYPE_INVALID))
        {
            enqueue_event(backend, EVENT_COMMIT, NULL, text ? text : "", -1,
                          NULL, 0, 0);
        }
        dbus_error_free(&error);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal(message, FCITX5_INPUT_INTERFACE, "UpdateFormattedPreedit"))
    {
        if (parse_fcitx5_formatted_preedit(backend, message, &info))
        {
            if (info.text && *info.text)
            {
                enqueue_event(backend, EVENT_PREEDIT, NULL, info.text ? info.text : "",
                              info.caret, info.block_sizes, info.block_count,
                              info.focused_block);
            }
            else
            {
                enqueue_event(backend, EVENT_CLEAR_PREEDIT, NULL, NULL, -1,
                              NULL, 0, 0);
            }
        }
        free(info.text);
        free(info.block_sizes);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal(message, FCITX5_INPUT_INTERFACE, "NotifyFocusOut"))
    {
        enqueue_event(backend, EVENT_CLEAR_PREEDIT, NULL, NULL, -1, NULL, 0, 0);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void disconnect_fcitx5(GLFWx11IMEBackend* backend, const char* reason)
{
    if (backend->connection || backend->input_context_path || backend->ready)
        log_line(backend, "disconnecting from Fcitx5 reason=%s", reason ? reason : "");

    backend->ready = GLFW_FALSE;
    backend->active_request_id = 0;
    backend->active_window = NULL;

    if (backend->connection)
    {
        if (backend->filter_added)
            dbus_connection_remove_filter(backend->connection, dbus_filter, backend);
        dbus_connection_close(backend->connection);
        dbus_connection_unref(backend->connection);
        backend->connection = NULL;
    }

    free(backend->input_context_path);
    backend->input_context_path = NULL;
    backend->filter_added = GLFW_FALSE;
    backend->reconnect_at = now_seconds(backend) + backend->reconnect_interval;
}

static void complete_failed_request(GLFWx11IMEBackend* backend, Request* request)
{
    if (!request)
        return;

    pthread_mutex_lock(&backend->mutex);
    request->completed = GLFW_TRUE;
    request->failed = GLFW_TRUE;
    request->handled = GLFW_FALSE;
    request->completed_at = now_seconds(backend);
    pthread_cond_broadcast(&backend->cond);
    pthread_mutex_unlock(&backend->mutex);

    release_request(backend, request);
}

static int connect_fcitx5(GLFWx11IMEBackend* backend)
{
    DBusError error;
    DBusMessage* message;
    DBusMessage* reply;
    const char* path = NULL;
    dbus_uint64_t caps = FCITX5_CAP_PREEDIT |
                         FCITX5_CAP_FORMATTED_PREEDIT |
                         FCITX5_CAP_CLIENT_UNFOCUS_COMMIT |
                         FCITX5_CAP_KEY_EVENT_ORDER_FIX;

    log_line(backend, "connecting to Fcitx5 on session bus");

    dbus_error_init(&error);
    backend->connection = dbus_bus_get_private(DBUS_BUS_SESSION, &error);
    if (!backend->connection)
    {
        log_line(backend, "failed to open session bus: %s", error.message ? error.message : "");
        dbus_error_free(&error);
        backend->reconnect_at = now_seconds(backend) + backend->reconnect_interval;
        return GLFW_FALSE;
    }

    dbus_connection_set_exit_on_disconnect(backend->connection, FALSE);

    log_line(backend, "creating Fcitx5 input context");

    message = dbus_message_new_method_call(FCITX5_SERVICE, FCITX5_PATH,
                                           FCITX5_INTERFACE, "CreateInputContext");
    if (!message)
    {
        disconnect_fcitx5(backend, "CreateInputContext message allocation failed");
        return GLFW_FALSE;
    }

    {
        DBusMessageIter iter, array, structure;
        const char* program_key = "program";
        const char* program_value = "GLFW research prototype";
        const char* display_key = "display";
        char display_value[256];
        const char* display_value_string = display_value;

        snprintf(display_value, sizeof(display_value), "x11:%s",
                 getenv("DISPLAY") ? getenv("DISPLAY") : "");

        dbus_message_iter_init_append(message, &iter);
        if (!dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "(ss)", &array))
        {
            dbus_message_unref(message);
            disconnect_fcitx5(backend, "CreateInputContext argument open failed");
            return GLFW_FALSE;
        }

        if (!dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, NULL, &structure) ||
            !dbus_message_iter_append_basic(&structure, DBUS_TYPE_STRING, &program_key) ||
            !dbus_message_iter_append_basic(&structure, DBUS_TYPE_STRING, &program_value) ||
            !dbus_message_iter_close_container(&array, &structure) ||
            !dbus_message_iter_open_container(&array, DBUS_TYPE_STRUCT, NULL, &structure) ||
            !dbus_message_iter_append_basic(&structure, DBUS_TYPE_STRING, &display_key) ||
            !dbus_message_iter_append_basic(&structure, DBUS_TYPE_STRING, &display_value_string) ||
            !dbus_message_iter_close_container(&array, &structure))
        {
            dbus_message_unref(message);
            disconnect_fcitx5(backend, "CreateInputContext argument append failed");
            return GLFW_FALSE;
        }

        dbus_message_iter_close_container(&iter, &array);
    }

    reply = dbus_connection_send_with_reply_and_block(backend->connection,
                                                      message, 3000, &error);
    dbus_message_unref(message);
    if (!reply)
    {
        log_line(backend, "CreateInputContext failed: %s", error.message ? error.message : "");
        dbus_error_free(&error);
        disconnect_fcitx5(backend, "CreateInputContext failed");
        return GLFW_FALSE;
    }

    if (!parse_create_input_context_reply(reply, &path))
    {
        log_line(backend, "CreateInputContext reply parse failed");
        dbus_message_unref(reply);
        disconnect_fcitx5(backend, "CreateInputContext reply parse failed");
        return GLFW_FALSE;
    }

    backend->input_context_path = xstrdup(path);
    dbus_message_unref(reply);
    if (!backend->input_context_path)
    {
        disconnect_fcitx5(backend, "input context path allocation failed");
        return GLFW_FALSE;
    }

    log_line(backend, "created Fcitx5 input context path=%s",
             backend->input_context_path ? backend->input_context_path : "");

    dbus_error_init(&error);
    dbus_bus_add_match(backend->connection,
                       "type='signal',interface='org.fcitx.Fcitx.InputContext1'",
                       &error);
    if (dbus_error_is_set(&error))
    {
        log_line(backend, "failed to add Fcitx5 signal match: %s",
                 error.message ? error.message : "");
        dbus_error_free(&error);
        disconnect_fcitx5(backend, "signal match failed");
        return GLFW_FALSE;
    }

    if (!dbus_connection_add_filter(backend->connection, dbus_filter, backend, NULL))
    {
        disconnect_fcitx5(backend, "signal filter failed");
        return GLFW_FALSE;
    }
    backend->filter_added = GLFW_TRUE;
    log_line(backend, "setting Fcitx5 capabilities=0x%llx",
             (unsigned long long) caps);
    if (!call_no_reply(backend, "SetCapability",
                       DBUS_TYPE_UINT64, &caps,
                       DBUS_TYPE_INVALID))
    {
        disconnect_fcitx5(backend, "SetCapability failed");
        return GLFW_FALSE;
    }
    backend->ready = GLFW_TRUE;
    backend->status = GLFW_TRUE;
    backend->reconnect_at = 0.0;

    log_line(backend, "connected to Fcitx5 path=%s",
             backend->input_context_path ? backend->input_context_path : "");

    if (backend->focused_window &&
        !call_no_reply(backend, "FocusIn", DBUS_TYPE_INVALID))
    {
        disconnect_fcitx5(backend, "restored FocusIn failed");
        return GLFW_FALSE;
    }

    return GLFW_TRUE;
}

static void process_key_command(GLFWx11IMEBackend* backend, Command* command)
{
    Request* request = command->request;
    DBusError error;
    DBusMessage* message;
    DBusMessage* reply;
    dbus_bool_t handled = FALSE;
    dbus_uint32_t keyval = request->event.keysym;
    dbus_uint32_t x11_keycode = request->event.keycode;
    dbus_uint32_t keycode = x11_keycode;
    dbus_uint32_t state = fcitx5_state_from_glfw(request->event.mods);
    dbus_bool_t is_release = request->event.action == GLFW_RELEASE;
    dbus_uint32_t time = request->event.time;
    int disconnect_after_request = GLFW_FALSE;

    pthread_mutex_lock(&backend->mutex);
    backend->active_request_id = request->id;
    backend->active_window = request->event.window;
    backend->last_request_id = request->id;
    pthread_mutex_unlock(&backend->mutex);

    log_line(backend,
             "request start id=%lu key_serial=%lu timestamp=%.6f keyval=0x%x x11_keycode=%u fcitx5_keycode=%u state=0x%x release=%i",
             request->id, request->event.time, request->queued_at,
             keyval, x11_keycode, keycode, state, is_release);

    if (request->event.cursor_rect_valid)
    {
        log_line(backend,
                 "request cursor id=%lu valid=1 original=(%i,%i %ix%i) root=(%i,%i %ix%i) sent_before_process=1",
                 request->id,
                 request->event.cursor_x,
                 request->event.cursor_y,
                 request->event.cursor_width,
                 request->event.cursor_height,
                 request->event.cursor_root_x,
                 request->event.cursor_root_y,
                 request->event.cursor_width,
                 request->event.cursor_height);
        call_no_reply(backend, "SetCursorRect",
                      DBUS_TYPE_INT32, &request->event.cursor_root_x,
                      DBUS_TYPE_INT32, &request->event.cursor_root_y,
                      DBUS_TYPE_INT32, &request->event.cursor_width,
                      DBUS_TYPE_INT32, &request->event.cursor_height,
                      DBUS_TYPE_INVALID);
    }
    else
    {
        log_line(backend,
                 "request cursor id=%lu valid=0 sent_before_process=0",
                 request->id);
    }

    dbus_error_init(&error);
    message = dbus_message_new_method_call(FCITX5_SERVICE,
                                           backend->input_context_path,
                                           FCITX5_INPUT_INTERFACE,
                                           "ProcessKeyEvent");
    if (message &&
        dbus_message_append_args(message,
                                 DBUS_TYPE_UINT32, &keyval,
                                 DBUS_TYPE_UINT32, &keycode,
                                 DBUS_TYPE_UINT32, &state,
                                 DBUS_TYPE_BOOLEAN, &is_release,
                                 DBUS_TYPE_UINT32, &time,
                                 DBUS_TYPE_INVALID))
    {
        reply = dbus_connection_send_with_reply_and_block(backend->connection,
                                                          message, 3000, &error);
        if (reply)
        {
            dbus_message_get_args(reply, &error,
                                  DBUS_TYPE_BOOLEAN, &handled,
                                  DBUS_TYPE_INVALID);
            dbus_message_unref(reply);
        }
        else
        {
            request->failed = GLFW_TRUE;
            disconnect_after_request = GLFW_TRUE;
        }
    }
    else
    {
        request->failed = GLFW_TRUE;
        disconnect_after_request = GLFW_TRUE;
    }

    if (message)
        dbus_message_unref(message);

    if (dbus_error_is_set(&error))
    {
        log_line(backend, "request error id=%lu error=%s", request->id,
                 error.message ? error.message : "");
        dbus_error_free(&error);
        request->failed = GLFW_TRUE;
        disconnect_after_request = GLFW_TRUE;
    }

    pthread_mutex_lock(&backend->mutex);
    backend->active_request_id = 0;
    backend->active_window = NULL;
    request->handled = handled ? GLFW_TRUE : GLFW_FALSE;
    request->completed = GLFW_TRUE;
    request->completed_at = now_seconds(backend);
    remember_recent(backend, request);
    pthread_cond_broadcast(&backend->cond);
    pthread_mutex_unlock(&backend->mutex);

    log_line(backend,
             "request reply id=%lu latency=%.3fms handled=%i timed_out=%i failed=%i",
             request->id, (request->completed_at - request->queued_at) * 1000.0,
             request->handled, request->timed_out, request->failed);

    release_request(backend, request);

    if (disconnect_after_request)
        disconnect_fcitx5(backend, "ProcessKeyEvent failed");
}

static void dispatch_dbus(GLFWx11IMEBackend* backend)
{
    if (!backend->connection)
        return;

    dbus_connection_read_write_dispatch(backend->connection, 0);
    while (dbus_connection_dispatch(backend->connection) == DBUS_DISPATCH_DATA_REMAINS)
    {
    }

    if (!dbus_connection_get_is_connected(backend->connection))
        disconnect_fcitx5(backend, "D-Bus connection disconnected");
}

static void handle_unready_command(GLFWx11IMEBackend* backend, Command* command)
{
    switch (command->type)
    {
        case COMMAND_FOCUS_IN:
            backend->focused_window = command->window;
            break;
        case COMMAND_FOCUS_OUT:
            if (backend->focused_window == command->window)
                backend->focused_window = NULL;
            break;
        case COMMAND_SET_STATUS:
            backend->status = command->status;
            break;
        case COMMAND_KEY:
            complete_failed_request(backend, command->request);
            command->request = NULL;
            break;
        default:
            break;
    }
}

static void* worker_main(void* data)
{
    GLFWx11IMEBackend* backend = data;

    // This thread owns all D-Bus traffic for the module.  GLFW only sees the
    // synchronous process_key result and queued events drained on the main thread.
    for (;;)
    {
        Command* command = NULL;

        if (backend->running && !backend->ready)
        {
            const double now = now_seconds(backend);
            if (backend->reconnect_at <= 0.0 || now >= backend->reconnect_at)
                connect_fcitx5(backend);
        }

        dispatch_dbus(backend);

        pthread_mutex_lock(&backend->mutex);
        if (backend->running && !backend->command_head)
        {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 10 * 1000 * 1000;
            if (ts.tv_nsec >= 1000000000L)
            {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }
            pthread_cond_timedwait(&backend->cond, &backend->mutex, &ts);
        }

        command = pop_command(backend);
        if (!backend->running && !command)
        {
            pthread_mutex_unlock(&backend->mutex);
            break;
        }
        pthread_mutex_unlock(&backend->mutex);

        if (!command)
            continue;

        if (command->type == COMMAND_STOP)
        {
            free(command);
            break;
        }

        if (!backend->ready && command->type != COMMAND_STOP)
        {
            handle_unready_command(backend, command);
            free(command);
            continue;
        }

        switch (command->type)
        {
            case COMMAND_KEY:
                process_key_command(backend, command);
                break;
            case COMMAND_FOCUS_IN:
                backend->focused_window = command->window;
                call_no_reply(backend, "FocusIn", DBUS_TYPE_INVALID);
                break;
            case COMMAND_FOCUS_OUT:
                if (backend->focused_window == command->window)
                    backend->focused_window = NULL;
                call_no_reply(backend, "FocusOut", DBUS_TYPE_INVALID);
                break;
            case COMMAND_CURSOR_RECT:
                log_line(backend,
                         "SetCursorRect final=(%i,%i %ix%i)",
                         command->x, command->y, command->w, command->h);
                call_no_reply(backend, "SetCursorRect",
                              DBUS_TYPE_INT32, &command->x,
                              DBUS_TYPE_INT32, &command->y,
                              DBUS_TYPE_INT32, &command->w,
                              DBUS_TYPE_INT32, &command->h,
                              DBUS_TYPE_INVALID);
                break;
            case COMMAND_RESET:
                call_no_reply(backend, "Reset", DBUS_TYPE_INVALID);
                break;
            case COMMAND_SET_STATUS:
                backend->status = command->status;
                call_no_reply(backend, command->status ? "FocusIn" : "FocusOut",
                              DBUS_TYPE_INVALID);
                break;
            default:
                break;
        }

        free(command);
    }

    disconnect_fcitx5(backend, "worker shutdown");
    return NULL;
}

static GLFWx11IMEBackend* backend_create(const GLFWx11IMEHostAPI* host)
{
    GLFWx11IMEBackend* backend = calloc(1, sizeof(GLFWx11IMEBackend));
    const char* timeout;

    if (!backend)
        return NULL;

    backend->host = *host;
    backend->running = GLFW_TRUE;
    backend->status = GLFW_TRUE;
    backend->timeout_ms = 100.0;
    backend->reconnect_interval = 1.0;
    timeout = getenv("GLFW_FCITX5_TIMEOUT_MS");
    if (timeout && *timeout)
        backend->timeout_ms = atof(timeout);

    dbus_threads_init_default();

    pthread_mutex_init(&backend->mutex, NULL);
    pthread_cond_init(&backend->cond, NULL);

    if (pthread_create(&backend->thread, NULL, worker_main, backend) != 0)
    {
        pthread_cond_destroy(&backend->cond);
        pthread_mutex_destroy(&backend->mutex);
        free(backend);
        return NULL;
    }

    log_line(backend, "module created timeout=%.3fms", backend->timeout_ms);
    return backend;
}

static void backend_destroy(GLFWx11IMEBackend* backend)
{
    Command* command;
    QueuedEvent* event;
    Request* request;

    if (!backend)
        return;

    command = calloc(1, sizeof(Command));
    if (command)
    {
        command->type = COMMAND_STOP;
        push_command(backend, command);
    }

    pthread_mutex_lock(&backend->mutex);
    backend->running = GLFW_FALSE;
    pthread_cond_signal(&backend->cond);
    pthread_mutex_unlock(&backend->mutex);

    pthread_join(backend->thread, NULL);

    while (backend->command_head)
    {
        command = backend->command_head;
        backend->command_head = command->next;
        free(command);
    }

    while (backend->event_head)
    {
        event = backend->event_head;
        backend->event_head = event->next;
        free(event->text);
        free(event->block_sizes);
        free(event);
    }

    request = backend->recent;
    while (request)
    {
        Request* next = request->next_recent;
        request->next_recent = NULL;
        release_request(backend, request);
        request = next;
    }

    pthread_cond_destroy(&backend->cond);
    pthread_mutex_destroy(&backend->mutex);
    free(backend);
}

static void enqueue_simple(GLFWx11IMEBackend* backend,
                           CommandType type,
                           void* window,
                           unsigned long x11_window)
{
    Command* command = calloc(1, sizeof(Command));
    if (!command)
        return;

    command->type = type;
    command->window = window;
    command->x11_window = x11_window;
    push_command(backend, command);
}

static void backend_focus_in(GLFWx11IMEBackend* backend, void* window, unsigned long x11_window)
{
    enqueue_simple(backend, COMMAND_FOCUS_IN, window, x11_window);
}

static void backend_focus_out(GLFWx11IMEBackend* backend, void* window, unsigned long x11_window)
{
    enqueue_simple(backend, COMMAND_FOCUS_OUT, window, x11_window);
}

static void backend_set_cursor_rect(GLFWx11IMEBackend* backend,
                                    void* window,
                                    int x, int y, int w, int h)
{
    Command* command = calloc(1, sizeof(Command));
    if (!command)
        return;

    command->type = COMMAND_CURSOR_RECT;
    command->window = window;
    command->x = x;
    command->y = y;
    command->w = w;
    command->h = h;
    push_command(backend, command);
}

static void backend_reset(GLFWx11IMEBackend* backend, void* window)
{
    enqueue_simple(backend, COMMAND_RESET, window, 0);
}

static int backend_process_key(GLFWx11IMEBackend* backend,
                               const GLFWx11IMEKeyEvent* event,
                               GLFWx11IMEKeyResult* result)
{
    Command* command;
    Request* request;
    struct timespec deadline;
    int rc = 0;

    request = calloc(1, sizeof(Request));
    command = calloc(1, sizeof(Command));
    if (!request || !command)
    {
        free(request);
        free(command);
        return GLFW_FALSE;
    }

    pthread_mutex_lock(&backend->mutex);
    request->id = ++backend->next_request_id;
    request->event = *event;
    request->queued_at = now_seconds(backend);
    request->refs = 2;
    command->type = COMMAND_KEY;
    command->window = event->window;
    command->x11_window = event->x11_window;
    command->request = request;

    if (backend->command_tail)
        backend->command_tail->next = command;
    else
        backend->command_head = command;
    backend->command_tail = command;
    pthread_cond_signal(&backend->cond);

    clock_gettime(CLOCK_REALTIME, &deadline);
    long nsec = (long) (backend->timeout_ms * 1000000.0);
    deadline.tv_sec += nsec / 1000000000L;
    deadline.tv_nsec += nsec % 1000000000L;
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (!request->completed && rc != ETIMEDOUT)
        rc = pthread_cond_timedwait(&backend->cond, &backend->mutex, &deadline);

    if (!request->completed)
    {
        request->timed_out = GLFW_TRUE;
        result->timed_out = GLFW_TRUE;
        result->handled = GLFW_FALSE;
        result->elapsed_ms = (now_seconds(backend) - request->queued_at) * 1000.0;
        result->request_id = request->id;
        remember_recent(backend, request);
        log_line(backend, "request timeout id=%lu latency=%.3fms key_serial=%lu",
                 request->id, result->elapsed_ms, event->time);
    }
    else
    {
        result->timed_out = request->timed_out;
        result->handled = request->handled;
        result->elapsed_ms = (request->completed_at - request->queued_at) * 1000.0;
        result->request_id = request->id;
    }

    release_request(backend, request);
    pthread_mutex_unlock(&backend->mutex);

    return GLFW_TRUE;
}

static int backend_get_status(GLFWx11IMEBackend* backend, void* window)
{
    (void) window;
    return backend->status;
}

static void backend_set_status(GLFWx11IMEBackend* backend, void* window, int enabled)
{
    Command* command = calloc(1, sizeof(Command));
    if (!command)
        return;

    command->type = COMMAND_SET_STATUS;
    command->window = window;
    command->status = enabled ? GLFW_TRUE : GLFW_FALSE;
    push_command(backend, command);
}

static void backend_drain_events(GLFWx11IMEBackend* backend)
{
    for (;;)
    {
        QueuedEvent* event;

        pthread_mutex_lock(&backend->mutex);
        event = backend->event_head;
        if (event)
        {
            backend->event_head = event->next;
            if (!backend->event_head)
                backend->event_tail = NULL;
        }
        pthread_mutex_unlock(&backend->mutex);

        if (!event)
            break;

        log_line(backend,
                 "event drain type=%i request=%lu timed_out=%i timestamp=%.6f caret=%i blocks=%i focused=%i text='%s'",
                 event->type, event->request_id, event->request_timed_out,
                 event->timestamp, event->caret, event->block_count,
                 event->focused_block, event->text ? event->text : "");

        switch (event->type)
        {
            case EVENT_COMMIT:
                if (backend->host.commit_text)
                    backend->host.commit_text(event->window, event->text ? event->text : "");
                break;
            case EVENT_PREEDIT:
                if (backend->host.update_preedit)
                {
                    backend->host.update_preedit(event->window,
                                                 event->text ? event->text : "",
                                                 event->caret,
                                                 event->block_sizes,
                                                 event->block_count,
                                                 event->focused_block);
                }
                break;
            case EVENT_CLEAR_PREEDIT:
                if (backend->host.clear_preedit)
                    backend->host.clear_preedit(event->window);
                break;
            case EVENT_STATUS:
                if (backend->host.status_changed)
                    backend->host.status_changed(event->window);
                break;
        }

        free(event->text);
        free(event->block_sizes);
        free(event);
    }
}

int glfwGetX11IMEBackend(int abiVersion,
                         const GLFWx11IMEHostAPI* host,
                         GLFWx11IMEBackendAPI* backend)
{
    if (abiVersion != GLFW_X11_IME_MODULE_ABI_VERSION || !host || !backend)
        return GLFW_FALSE;

    memset(backend, 0, sizeof(*backend));
    backend->create = backend_create;
    backend->destroy = backend_destroy;
    backend->focus_in = backend_focus_in;
    backend->focus_out = backend_focus_out;
    backend->set_cursor_rect = backend_set_cursor_rect;
    backend->reset = backend_reset;
    backend->process_key = backend_process_key;
    backend->get_status = backend_get_status;
    backend->set_status = backend_set_status;
    backend->drain_events = backend_drain_events;
    return GLFW_TRUE;
}
