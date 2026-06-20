#define _POSIX_C_SOURCE 200809L

//========================================================================
// GLFW X11 IBus IME module prototype
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

static const char* IBUS_SERVICE = "org.freedesktop.IBus";
static const char* IBUS_PATH = "/org/freedesktop/IBus";
static const char* IBUS_INTERFACE = "org.freedesktop.IBus";
static const char* IBUS_INPUT_INTERFACE = "org.freedesktop.IBus.InputContext";

enum
{
    IBUS_CAP_PREEDIT_TEXT = 1 << 0,
    IBUS_CAP_FOCUS = 1 << 3
};

enum
{
    IBUS_SHIFT_MASK = 1 << 0,
    IBUS_LOCK_MASK = 1 << 1,
    IBUS_CONTROL_MASK = 1 << 2,
    IBUS_MOD1_MASK = 1 << 3,
    IBUS_MOD2_MASK = 1 << 4,
    IBUS_MOD4_MASK = 1 << 6,
    IBUS_RELEASE_MASK = 1 << 30
};

enum
{
    IBUS_ATTR_TYPE_UNDERLINE = 1,
    IBUS_ATTR_UNDERLINE_SINGLE = 1
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

typedef struct PreeditBlock
{
    int start;
    int end;
    int focused;
} PreeditBlock;

typedef struct PreeditInfo
{
    const char* text;
    int caret;
    int visible;
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
        fprintf(stderr, "glfw-ibus: %s\n", buffer);
}

static uint32_t ibus_state_from_glfw(unsigned int mods, int action)
{
    uint32_t state = action == GLFW_RELEASE ? IBUS_RELEASE_MASK : 0;

    if (mods & GLFW_MOD_SHIFT)
        state |= IBUS_SHIFT_MASK;
    if (mods & GLFW_MOD_CAPS_LOCK)
        state |= IBUS_LOCK_MASK;
    if (mods & GLFW_MOD_CONTROL)
        state |= IBUS_CONTROL_MASK;
    if (mods & GLFW_MOD_ALT)
        state |= IBUS_MOD1_MASK;
    if (mods & GLFW_MOD_NUM_LOCK)
        state |= IBUS_MOD2_MASK;
    if (mods & GLFW_MOD_SUPER)
        state |= IBUS_MOD4_MASK;

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

    // IBus signals do not identify the ProcessKeyEvent that caused them.
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

    message = dbus_message_new_method_call(IBUS_SERVICE,
                                           backend->input_context_path,
                                           IBUS_INPUT_INTERFACE,
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

static int compare_ints(const void* a, const void* b)
{
    const int ia = *(const int*) a;
    const int ib = *(const int*) b;
    return (ia > ib) - (ia < ib);
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

static int normalize_ibus_index(const char* text, int char_count, int index)
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

static void append_preedit_block(PreeditBlock** blocks,
                                 int* count,
                                 int* capacity,
                                 int start,
                                 int end,
                                 int focused)
{
    if (start >= end)
        return;

    if (*count == *capacity)
    {
        const int new_capacity = *capacity ? *capacity * 2 : 8;
        PreeditBlock* new_blocks =
            realloc(*blocks, sizeof(PreeditBlock) * (size_t) new_capacity);
        if (!new_blocks)
            return;

        *blocks = new_blocks;
        *capacity = new_capacity;
    }

    (*blocks)[*count].start = start;
    (*blocks)[*count].end = end;
    (*blocks)[*count].focused = focused;
    (*count)++;
}

static void parse_ibus_attribute(DBusMessageIter* iter,
                                 const char* text,
                                 int char_count,
                                 PreeditBlock** blocks,
                                 int* count,
                                 int* capacity)
{
    DBusMessageIter structure;
    const char* id = NULL;
    dbus_uint32_t type = 0, value = 0, start = 0, end = 0;

    if (dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_VARIANT)
    {
        DBusMessageIter variant;
        dbus_message_iter_recurse(iter, &variant);
        parse_ibus_attribute(&variant, text, char_count, blocks, count, capacity);
        return;
    }

    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_STRUCT)
        return;

    dbus_message_iter_recurse(iter, &structure);
    if (dbus_message_iter_get_arg_type(&structure) != DBUS_TYPE_STRING)
        return;

    dbus_message_iter_get_basic(&structure, &id);
    if (!id || strcmp(id, "IBusAttribute") != 0)
        return;

    if (!dbus_message_iter_next(&structure) ||
        !dbus_message_iter_next(&structure) ||
        dbus_message_iter_get_arg_type(&structure) != DBUS_TYPE_UINT32)
    {
        return;
    }
    dbus_message_iter_get_basic(&structure, &type);

    if (!dbus_message_iter_next(&structure) ||
        dbus_message_iter_get_arg_type(&structure) != DBUS_TYPE_UINT32)
    {
        return;
    }
    dbus_message_iter_get_basic(&structure, &value);

    if (!dbus_message_iter_next(&structure) ||
        dbus_message_iter_get_arg_type(&structure) != DBUS_TYPE_UINT32)
    {
        return;
    }
    dbus_message_iter_get_basic(&structure, &start);

    if (!dbus_message_iter_next(&structure) ||
        dbus_message_iter_get_arg_type(&structure) != DBUS_TYPE_UINT32)
    {
        return;
    }
    dbus_message_iter_get_basic(&structure, &end);

    append_preedit_block(blocks, count, capacity,
                         normalize_ibus_index(text, char_count, (int) start),
                         normalize_ibus_index(text, char_count, (int) end),
                         type != IBUS_ATTR_TYPE_UNDERLINE ||
                         value != IBUS_ATTR_UNDERLINE_SINGLE);
}

static void parse_ibus_attr_list(DBusMessageIter* iter,
                                 const char* text,
                                 int char_count,
                                 PreeditBlock** blocks,
                                 int* count,
                                 int* capacity)
{
    DBusMessageIter structure;
    const char* id = NULL;

    if (dbus_message_iter_get_arg_type(iter) == DBUS_TYPE_VARIANT)
    {
        DBusMessageIter variant;
        dbus_message_iter_recurse(iter, &variant);
        parse_ibus_attr_list(&variant, text, char_count, blocks, count, capacity);
        return;
    }

    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_STRUCT)
        return;

    dbus_message_iter_recurse(iter, &structure);
    if (dbus_message_iter_get_arg_type(&structure) != DBUS_TYPE_STRING)
        return;

    dbus_message_iter_get_basic(&structure, &id);
    if (!id || strcmp(id, "IBusAttrList") != 0)
        return;

    while (dbus_message_iter_next(&structure))
    {
        if (dbus_message_iter_get_arg_type(&structure) == DBUS_TYPE_ARRAY)
        {
            DBusMessageIter array;
            dbus_message_iter_recurse(&structure, &array);
            while (dbus_message_iter_get_arg_type(&array) != DBUS_TYPE_INVALID)
            {
                parse_ibus_attribute(&array, text, char_count,
                                     blocks, count, capacity);
                dbus_message_iter_next(&array);
            }
        }
    }
}

static void build_preedit_blocks(PreeditInfo* info,
                                 PreeditBlock* attrs,
                                 int attr_count)
{
    int boundary_count = 0;
    int focused_start = -1;
    const int text_count = utf8_count(info->text);
    int* boundaries;

    info->focused_block = 0;

    if (!text_count)
        return;

    boundaries = calloc((size_t) attr_count * 2 + 2, sizeof(int));
    if (!boundaries)
        return;

    boundaries[boundary_count++] = 0;
    boundaries[boundary_count++] = text_count;

    for (int i = 0;  i < attr_count;  i++)
    {
        int start = attrs[i].start;
        int end = attrs[i].end;

        if (start < 0)
            start = 0;
        if (end > text_count)
            end = text_count;
        if (start >= end)
            continue;

        boundaries[boundary_count++] = start;
        boundaries[boundary_count++] = end;

        if (attrs[i].focused && focused_start < 0)
            focused_start = start;
    }

    qsort(boundaries, (size_t) boundary_count, sizeof(int), compare_ints);

    info->block_sizes = calloc((size_t) boundary_count, sizeof(int));
    if (!info->block_sizes)
    {
        free(boundaries);
        return;
    }

    int previous = boundaries[0];
    for (int i = 1;  i < boundary_count;  i++)
    {
        const int current = boundaries[i];
        if (current == previous)
            continue;

        info->block_sizes[info->block_count] = current - previous;

        if (focused_start >= previous && focused_start < current)
            info->focused_block = info->block_count;
        else if (focused_start < 0 &&
                 info->caret >= previous &&
                 (info->caret < current || current == text_count))
        {
            info->focused_block = info->block_count;
        }

        info->block_count++;
        previous = current;
    }

    free(boundaries);
}

static int parse_ibus_text_variant(DBusMessageIter* iter,
                                   PreeditInfo* info,
                                   PreeditBlock** attrs,
                                   int* attr_count,
                                   int* attr_capacity)
{
    DBusMessageIter variant, structure;
    const char* id = NULL;

    if (dbus_message_iter_get_arg_type(iter) != DBUS_TYPE_VARIANT)
        return GLFW_FALSE;

    dbus_message_iter_recurse(iter, &variant);
    if (dbus_message_iter_get_arg_type(&variant) != DBUS_TYPE_STRUCT)
        return GLFW_FALSE;

    dbus_message_iter_recurse(&variant, &structure);
    if (dbus_message_iter_get_arg_type(&structure) != DBUS_TYPE_STRING)
        return GLFW_FALSE;

    dbus_message_iter_get_basic(&structure, &id);
    if (!id || strcmp(id, "IBusText") != 0)
        return GLFW_FALSE;

    dbus_message_iter_next(&structure);
    dbus_message_iter_next(&structure);
    if (dbus_message_iter_get_arg_type(&structure) != DBUS_TYPE_STRING)
        return GLFW_FALSE;

    dbus_message_iter_get_basic(&structure, &info->text);

    while (dbus_message_iter_next(&structure))
        parse_ibus_attr_list(&structure, info->text, utf8_count(info->text),
                             attrs, attr_count, attr_capacity);

    return GLFW_TRUE;
}

static int parse_ibus_text(DBusMessage* message, PreeditInfo* info)
{
    DBusMessageIter iter;
    PreeditBlock* attrs = NULL;
    int attr_count = 0;
    int attr_capacity = 0;

    memset(info, 0, sizeof(*info));
    info->caret = -1;
    info->visible = GLFW_TRUE;

    dbus_message_iter_init(message, &iter);
    if (!parse_ibus_text_variant(&iter, info, &attrs, &attr_count, &attr_capacity))
    {
        free(attrs);
        return GLFW_FALSE;
    }

    if (dbus_message_iter_next(&iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_UINT32)
    {
        dbus_uint32_t caret = 0;
        dbus_message_iter_get_basic(&iter, &caret);
        info->caret = normalize_ibus_index(info->text,
                                           utf8_count(info->text),
                                           (int) caret);
    }

    if (dbus_message_iter_next(&iter) &&
        dbus_message_iter_get_arg_type(&iter) == DBUS_TYPE_BOOLEAN)
    {
        dbus_bool_t visible = 1;
        dbus_message_iter_get_basic(&iter, &visible);
        info->visible = visible ? GLFW_TRUE : GLFW_FALSE;
    }

    build_preedit_blocks(info, attrs, attr_count);
    free(attrs);
    return GLFW_TRUE;
}

static DBusHandlerResult dbus_filter(DBusConnection* connection,
                                     DBusMessage* message,
                                     void* data)
{
    GLFWx11IMEBackend* backend = data;
    PreeditInfo info;
    (void) connection;

    if (dbus_message_is_signal(message, IBUS_INPUT_INTERFACE, "CommitText"))
    {
        if (parse_ibus_text(message, &info))
            enqueue_event(backend, EVENT_COMMIT, NULL, info.text ? info.text : "", -1,
                          NULL, 0, 0);
        free(info.block_sizes);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal(message, IBUS_INPUT_INTERFACE, "UpdatePreeditText"))
    {
        if (parse_ibus_text(message, &info))
        {
            if (info.visible)
                enqueue_event(backend, EVENT_PREEDIT, NULL, info.text ? info.text : "",
                              info.caret, info.block_sizes, info.block_count,
                              info.focused_block);
            else
                enqueue_event(backend, EVENT_CLEAR_PREEDIT, NULL, NULL, -1,
                              NULL, 0, 0);
        }
        free(info.block_sizes);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal(message, IBUS_INPUT_INTERFACE, "HidePreeditText"))
    {
        enqueue_event(backend, EVENT_CLEAR_PREEDIT, NULL, NULL, -1, NULL, 0, 0);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal(message, IBUS_INPUT_INTERFACE, "Enabled"))
    {
        backend->status = GLFW_TRUE;
        enqueue_event(backend, EVENT_STATUS, NULL, NULL, -1, NULL, 0, 0);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    if (dbus_message_is_signal(message, IBUS_INPUT_INTERFACE, "Disabled"))
    {
        backend->status = GLFW_FALSE;
        enqueue_event(backend, EVENT_STATUS, NULL, NULL, -1, NULL, 0, 0);
        return DBUS_HANDLER_RESULT_HANDLED;
    }

    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int read_ibus_address(char* buffer, size_t size)
{
    const char* address = getenv("IBUS_ADDRESS");
    char path[4096];
    char display[128];
    const char* config;
    const char* home;
    char* machine_id;
    DBusError error;
    FILE* file;

    if (address && *address)
    {
        snprintf(buffer, size, "%s", address);
        return GLFW_TRUE;
    }

    snprintf(display, sizeof(display), "%s", getenv("DISPLAY") ? getenv("DISPLAY") : ":0.0");
    char* colon = strrchr(display, ':');
    if (!colon)
        return GLFW_FALSE;

    char* screen = strrchr(display, '.');
    if (screen)
        *screen = '\0';

    *colon = '\0';
    const char* host = *display ? display : "unix";
    const char* number = colon + 1;

    config = getenv("XDG_CONFIG_HOME");
    home = getenv("HOME");

    dbus_error_init(&error);
    machine_id = dbus_try_get_local_machine_id(&error);
    if (!machine_id)
    {
        dbus_error_free(&error);
        return GLFW_FALSE;
    }

    if (config && *config)
        snprintf(path, sizeof(path), "%s/ibus/bus/%s-%s-%s", config, machine_id, host, number);
    else if (home && *home)
        snprintf(path, sizeof(path), "%s/.config/ibus/bus/%s-%s-%s", home, machine_id, host, number);
    else
    {
        dbus_free(machine_id);
        return GLFW_FALSE;
    }

    dbus_free(machine_id);

    file = fopen(path, "r");
    if (!file)
        return GLFW_FALSE;

    while (fgets(buffer, size, file))
    {
        if (strncmp(buffer, "IBUS_ADDRESS=", 13) == 0)
        {
            char* value = buffer + 13;
            char* newline = strchr(value, '\n');
            if (newline)
                *newline = '\0';
            memmove(buffer, value, strlen(value) + 1);
            fclose(file);
            return GLFW_TRUE;
        }
    }

    fclose(file);
    return GLFW_FALSE;
}

static int connect_ibus(GLFWx11IMEBackend* backend)
{
    char address[1024];
    DBusError error;
    DBusMessage* message;
    DBusMessage* reply;
    const char* client = "GLFW research prototype";
    const char* path = NULL;
    dbus_uint32_t caps = IBUS_CAP_FOCUS | IBUS_CAP_PREEDIT_TEXT;

    if (!read_ibus_address(address, sizeof(address)))
    {
        log_line(backend, "failed to discover IBus address");
        return GLFW_FALSE;
    }

    dbus_error_init(&error);
    backend->connection = dbus_connection_open_private(address, &error);
    if (!backend->connection)
    {
        log_line(backend, "failed to open IBus connection: %s", error.message ? error.message : "");
        dbus_error_free(&error);
        return GLFW_FALSE;
    }

    dbus_connection_set_exit_on_disconnect(backend->connection, FALSE);
    if (!dbus_bus_register(backend->connection, &error))
    {
        log_line(backend, "failed to register IBus bus connection: %s", error.message ? error.message : "");
        dbus_error_free(&error);
        return GLFW_FALSE;
    }

    message = dbus_message_new_method_call(IBUS_SERVICE, IBUS_PATH,
                                           IBUS_INTERFACE, "CreateInputContext");
    if (!message)
        return GLFW_FALSE;

    dbus_message_append_args(message,
                             DBUS_TYPE_STRING, &client,
                             DBUS_TYPE_INVALID);
    reply = dbus_connection_send_with_reply_and_block(backend->connection,
                                                      message, 3000, &error);
    dbus_message_unref(message);
    if (!reply)
    {
        log_line(backend, "CreateInputContext failed: %s", error.message ? error.message : "");
        dbus_error_free(&error);
        return GLFW_FALSE;
    }

    if (!dbus_message_get_args(reply, &error,
                               DBUS_TYPE_OBJECT_PATH, &path,
                               DBUS_TYPE_INVALID))
    {
        log_line(backend, "CreateInputContext reply parse failed: %s", error.message ? error.message : "");
        dbus_error_free(&error);
        dbus_message_unref(reply);
        return GLFW_FALSE;
    }

    backend->input_context_path = xstrdup(path);
    dbus_message_unref(reply);

    dbus_bus_add_match(backend->connection,
                       "type='signal',interface='org.freedesktop.IBus.InputContext'",
                       NULL);
    dbus_connection_add_filter(backend->connection, dbus_filter, backend, NULL);
    call_no_reply(backend, "SetCapabilities",
                  DBUS_TYPE_UINT32, &caps,
                  DBUS_TYPE_INVALID);
    backend->ready = GLFW_TRUE;
    backend->status = GLFW_TRUE;

    log_line(backend, "connected to IBus at %s path=%s", address,
             backend->input_context_path ? backend->input_context_path : "");
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
    dbus_uint32_t keycode = request->event.keycode;
    dbus_uint32_t state = ibus_state_from_glfw(request->event.mods,
                                               request->event.action);

    pthread_mutex_lock(&backend->mutex);
    backend->active_request_id = request->id;
    backend->active_window = request->event.window;
    backend->last_request_id = request->id;
    pthread_mutex_unlock(&backend->mutex);

    log_line(backend,
             "request start id=%lu key_serial=%lu timestamp=%.6f keyval=0x%x keycode=%u state=0x%x",
             request->id, request->event.time, request->queued_at,
             keyval, keycode, state);

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
        call_no_reply(backend, "SetCursorLocation",
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
    message = dbus_message_new_method_call(IBUS_SERVICE,
                                           backend->input_context_path,
                                           IBUS_INPUT_INTERFACE,
                                           "ProcessKeyEvent");
    if (message &&
        dbus_message_append_args(message,
                                 DBUS_TYPE_UINT32, &keyval,
                                 DBUS_TYPE_UINT32, &keycode,
                                 DBUS_TYPE_UINT32, &state,
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
            request->failed = GLFW_TRUE;
    }
    else
        request->failed = GLFW_TRUE;

    if (message)
        dbus_message_unref(message);

    if (dbus_error_is_set(&error))
    {
        log_line(backend, "request error id=%lu error=%s", request->id,
                 error.message ? error.message : "");
        dbus_error_free(&error);
        request->failed = GLFW_TRUE;
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
}

static void dispatch_dbus(GLFWx11IMEBackend* backend)
{
    if (!backend->connection)
        return;

    dbus_connection_read_write_dispatch(backend->connection, 0);
    while (dbus_connection_dispatch(backend->connection) == DBUS_DISPATCH_DATA_REMAINS)
    {
    }
}

static void* worker_main(void* data)
{
    GLFWx11IMEBackend* backend = data;

    // This thread owns all D-Bus traffic for the module.  GLFW only sees the
    // synchronous process_key result and queued events drained on the main thread.
    connect_ibus(backend);

    for (;;)
    {
        Command* command = NULL;

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
            if (command->request)
            {
                pthread_mutex_lock(&backend->mutex);
                command->request->completed = GLFW_TRUE;
                command->request->failed = GLFW_TRUE;
                command->request->completed_at = now_seconds(backend);
                pthread_cond_broadcast(&backend->cond);
                pthread_mutex_unlock(&backend->mutex);
                release_request(backend, command->request);
            }
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
                         "SetCursorLocation final=(%i,%i %ix%i)",
                         command->x, command->y, command->w, command->h);
                call_no_reply(backend, "SetCursorLocation",
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

    if (backend->connection)
    {
        dbus_connection_close(backend->connection);
        dbus_connection_unref(backend->connection);
        backend->connection = NULL;
    }

    free(backend->input_context_path);
    backend->input_context_path = NULL;
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
    backend->timeout_ms = 100.0;
    timeout = getenv("GLFW_IBUS_TIMEOUT_MS");
    if (timeout && *timeout)
        backend->timeout_ms = atof(timeout);

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
