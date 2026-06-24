//========================================================================
// GLFW 3.5 X11 IME module prototype - www.glfw.org
//------------------------------------------------------------------------
// This is an experimental internal ABI for dynamically loaded X11 IME
// modules.  It is intentionally not part of the public GLFW API.
//========================================================================

#ifndef _glfw3_x11_ime_module_h_
#define _glfw3_x11_ime_module_h_

#define GLFW_X11_IME_MODULE_ABI_VERSION 2

typedef struct GLFWx11IMEBackend GLFWx11IMEBackend;

typedef struct GLFWx11IMEKeyEvent
{
    void* window;
    unsigned long x11_window;
    unsigned int keycode;
    unsigned int keysym;
    unsigned int state;
    int action;
    int mods;
    unsigned long time;
    int cursor_rect_valid;
    int cursor_x, cursor_y, cursor_width, cursor_height;
    int cursor_root_x, cursor_root_y;
} GLFWx11IMEKeyEvent;

typedef struct GLFWx11IMEKeyResult
{
    int handled;
    int timed_out;
    double elapsed_ms;
    unsigned long request_id;
} GLFWx11IMEKeyResult;

typedef struct GLFWx11IMEHostAPI
{
    void (*commit_text)(void* window, const char* utf8);
    void (*update_preedit)(void* window,
                           const char* utf8,
                           int caret,
                           const int* block_sizes,
                           int block_count,
                           int focused_block);
    void (*clear_preedit)(void* window);
    void (*status_changed)(void* window);
    double (*get_time)(void);
    void (*post_empty_event)(void);
    void (*log)(const char* message);
} GLFWx11IMEHostAPI;

typedef struct GLFWx11IMEBackendAPI
{
    GLFWx11IMEBackend* (*create)(const GLFWx11IMEHostAPI* host);
    void (*destroy)(GLFWx11IMEBackend* backend);
    void (*focus_in)(GLFWx11IMEBackend* backend, void* window, unsigned long x11_window);
    void (*focus_out)(GLFWx11IMEBackend* backend, void* window, unsigned long x11_window);
    void (*set_cursor_rect)(GLFWx11IMEBackend* backend, void* window, int x, int y, int w, int h);
    void (*reset)(GLFWx11IMEBackend* backend, void* window);
    int (*process_key)(GLFWx11IMEBackend* backend,
                       const GLFWx11IMEKeyEvent* event,
                       GLFWx11IMEKeyResult* result);
    int (*get_status)(GLFWx11IMEBackend* backend, void* window);
    void (*set_status)(GLFWx11IMEBackend* backend, void* window, int enabled);
    void (*drain_events)(GLFWx11IMEBackend* backend);
} GLFWx11IMEBackendAPI;

typedef int (* PFN_glfwGetX11IMEBackend)(int,const GLFWx11IMEHostAPI*,GLFWx11IMEBackendAPI*);

/*
 * The experimental IME module ABI currently references _GLFWwindow in GLFW's
 * internal X11 helpers, so we need the internal declaration here.
 *
 * A future public/stable module ABI should avoid exposing internal GLFW types
 * and pass the required state explicitly instead.
 */
#include "internal.h"

int _glfwLoadIMEModuleX11(void);
void _glfwUnloadIMEModuleX11(void);
int _glfwHasIMEModuleX11(void);
void _glfwDrainIMEModuleX11(void);
int _glfwProcessKeyIMEModuleX11(_GLFWwindow* window,
                                unsigned int keycode,
                                unsigned int keysym,
                                unsigned int state,
                                int action,
                                int mods,
                                unsigned long time);
void _glfwFocusInIMEModuleX11(_GLFWwindow* window);
void _glfwFocusOutIMEModuleX11(_GLFWwindow* window);
void _glfwSetCursorRectIMEModuleX11(_GLFWwindow* window, int x, int y, int w, int h);
void _glfwRefreshCursorRectIMEModuleX11(_GLFWwindow* window, const char* reason);
void _glfwRefreshPendingCursorRectsIMEModuleX11(const char* reason);
void _glfwNotifyNormalKeyIMEModuleX11(_GLFWwindow* window);
void _glfwResetIMEModuleX11(_GLFWwindow* window);
void _glfwSetStatusIMEModuleX11(_GLFWwindow* window, int active);
int _glfwGetStatusIMEModuleX11(_GLFWwindow* window);

#endif // _glfw3_x11_ime_module_h_
