//========================================================================
// GLFW 3.5 X11 IME module prototype - www.glfw.org
//========================================================================

#include "internal.h"

#if defined(_GLFW_X11)

#include "x11_ime_module.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_GLFW_EMBED_IBUS_MODULE)
extern int glfwGetX11IMEBackend(int,const GLFWx11IMEHostAPI*,GLFWx11IMEBackendAPI*);
#endif

#if !defined(_GLFW_X11_IME_MODULE_DIR)
#define _GLFW_X11_IME_MODULE_DIR ""
#endif

#if !defined(_GLFW_X11_IME_MODULE_SUFFIX)
#define _GLFW_X11_IME_MODULE_SUFFIX ".so"
#endif

static void hostLog(const char* message)
{
    if (message && _glfw.x11.imeModule.debug)
        fprintf(stderr, "GLFW IME: %s\n", message);
}

static GLFWbool hasPathSeparator(const char* path)
{
    return strchr(path, '/') != NULL;
}

static GLFWbool hasSuffix(const char* string, const char* suffix)
{
    const size_t stringLength = strlen(string);
    const size_t suffixLength = strlen(suffix);

    if (suffixLength > stringLength)
        return GLFW_FALSE;

    return strcmp(string + stringLength - suffixLength, suffix) == 0;
}

static char* makeIMEModulePath(const char* directory,
                               const char* prefix,
                               const char* name,
                               const char* suffix)
{
    const size_t directoryLength = strlen(directory);
    const size_t prefixLength = strlen(prefix);
    const size_t nameLength = strlen(name);
    const size_t suffixLength = strlen(suffix);
    const size_t length = directoryLength + 1 + prefixLength + nameLength +
                          suffixLength;
    char* path = _glfw_calloc(length + 1, 1);

    if (!path)
        return NULL;

    snprintf(path, length + 1, "%s/%s%s%s", directory, prefix, name, suffix);
    return path;
}

static void* loadIMEModulePath(const char* path, char** loadedPath)
{
    void* handle = _glfwPlatformLoadModule(path);

    if (!handle)
        return NULL;

    *loadedPath = _glfw_strdup(path);
    return handle;
}

static void* loadIMEModuleName(const char* name, char** loadedPath)
{
    void* handle;
    const GLFWbool hasModuleSuffix = hasSuffix(name, _GLFW_X11_IME_MODULE_SUFFIX);
    const GLFWbool hasModulePrefix = strncmp(name, "glfw-", 5) == 0;
    const char* directory = _GLFW_X11_IME_MODULE_DIR;
    char* path;

    *loadedPath = NULL;

    if (hasPathSeparator(name))
        return loadIMEModulePath(name, loadedPath);

    handle = loadIMEModulePath(name, loadedPath);
    if (handle)
        return handle;

    if (!directory[0])
        return NULL;

    path = makeIMEModulePath(directory, "", name, "");
    if (path)
    {
        handle = loadIMEModulePath(path, loadedPath);
        _glfw_free(path);
        if (handle)
            return handle;
    }

    if (!hasModuleSuffix)
    {
        path = makeIMEModulePath(directory, "", name,
                                 _GLFW_X11_IME_MODULE_SUFFIX);
        if (path)
        {
            handle = loadIMEModulePath(path, loadedPath);
            _glfw_free(path);
            if (handle)
                return handle;
        }
    }

    if (!hasModulePrefix)
    {
        path = makeIMEModulePath(directory, "glfw-", name, "");
        if (path)
        {
            handle = loadIMEModulePath(path, loadedPath);
            _glfw_free(path);
            if (handle)
                return handle;
        }

        if (!hasModuleSuffix)
        {
            path = makeIMEModulePath(directory, "glfw-", name,
                                     _GLFW_X11_IME_MODULE_SUFFIX);
            if (path)
            {
                handle = loadIMEModulePath(path, loadedPath);
                _glfw_free(path);
                if (handle)
                    return handle;
            }
        }
    }

    return NULL;
}

static void hostCommitText(void* handle, const char* utf8)
{
    _GLFWwindow* window = handle;
    const char* c = utf8;

    if (!window || !utf8)
        return;

    while (*c)
        _glfwInputChar(window, _glfwDecodeUTF8(&c), 0, GLFW_TRUE);
}

static void ensurePreeditBuffers(_GLFWpreedit* preedit, int textCount, int blockCount)
{
    int textBufferCount = preedit->textBufferCount;
    int blockBufferCount = preedit->blockSizesBufferCount;

    while (textBufferCount < textCount + 1)
        textBufferCount = textBufferCount ? textBufferCount * 2 : 8;

    if (textBufferCount != preedit->textBufferCount)
    {
        unsigned int* text = _glfw_realloc(preedit->text,
                                           sizeof(unsigned int) * textBufferCount);
        if (!text)
            return;

        preedit->text = text;
        preedit->textBufferCount = textBufferCount;
    }

    while (blockBufferCount < blockCount)
        blockBufferCount = blockBufferCount ? blockBufferCount * 2 : 8;

    if (blockBufferCount != preedit->blockSizesBufferCount)
    {
        int* blocks = _glfw_realloc(preedit->blockSizes,
                                    sizeof(int) * blockBufferCount);
        if (!blocks)
            return;

        preedit->blockSizes = blocks;
        preedit->blockSizesBufferCount = blockBufferCount;
    }
}

static void hostUpdatePreedit(void* handle,
                              const char* utf8,
                              int caret,
                              const int* blockSizes,
                              int blockCount,
                              int focusedBlock)
{
    _GLFWwindow* window = handle;
    _GLFWpreedit* preedit;
    const char* c;
    int count = 0;

    if (!window || !utf8)
        return;

    c = utf8;
    while (*c)
    {
        _glfwDecodeUTF8(&c);
        count++;
    }

    preedit = &window->preedit;
    if (!blockSizes || blockCount <= 0)
        blockCount = count ? 1 : 0;

    ensurePreeditBuffers(preedit, count, blockCount);
    if (count && (!preedit->text || !preedit->blockSizes))
        return;

    c = utf8;
    for (int i = 0;  i < count;  i++)
        preedit->text[i] = _glfwDecodeUTF8(&c);

    if (preedit->text)
        preedit->text[count] = 0;

    preedit->textCount = count;
    preedit->blockSizesCount = blockCount;
    if (blockSizes && blockCount > 0)
    {
        for (int i = 0;  i < blockCount;  i++)
            preedit->blockSizes[i] = blockSizes[i];
    }
    else if (count)
        preedit->blockSizes[0] = count;

    if (focusedBlock < 0 || focusedBlock >= blockCount)
        focusedBlock = 0;
    preedit->focusedBlockIndex = focusedBlock;
    preedit->caretIndex = (caret >= 0 && caret <= count) ? caret : count;

    _glfwInputPreedit(window);
}

static void hostClearPreedit(void* handle)
{
    _GLFWwindow* window = handle;
    _GLFWpreedit* preedit;

    if (!window)
        return;

    preedit = &window->preedit;
    preedit->textCount = 0;
    preedit->blockSizesCount = 0;
    preedit->focusedBlockIndex = 0;
    preedit->caretIndex = 0;

    _glfwInputPreedit(window);
}

static void hostStatusChanged(void* handle)
{
    _GLFWwindow* window = handle;
    if (window)
        _glfwInputIMEStatus(window);
}

static double hostGetTime(void)
{
    return _glfwPlatformGetTimerValue() / (double) _glfwPlatformGetTimerFrequency();
}

static void hostPostEmptyEvent(void)
{
    // IME modules queue callbacks for the main thread and use this to wake
    // glfwWaitEvents without exposing their worker-thread file descriptors.
    _glfwPostEmptyEventX11();
}

GLFWbool _glfwLoadIMEModuleX11(void)
{
    const char* path = getenv("GLFW_IM_MODULE");
    const char* debug = getenv("GLFW_IME_DEBUG");
    GLFWx11IMEHostAPI host;
    GLFWx11IMEBackendAPI api;
    PFN_glfwGetX11IMEBackend getBackend;
    char* loadedPath = NULL;

#if defined(_GLFW_EMBED_IBUS_MODULE)
    getBackend = glfwGetX11IMEBackend;
#else
    getBackend = NULL;
#endif

    if ((!path || !*path) && !getBackend)
        return GLFW_FALSE;

    _glfw.x11.imeModule.debug = debug && *debug && strcmp(debug, "0") != 0;

    if (path && *path)
    {
        _glfw.x11.imeModule.handle = loadIMEModuleName(path, &loadedPath);
        if (!_glfw.x11.imeModule.handle)
        {
            _glfwInputError(GLFW_PLATFORM_ERROR,
                            "X11: Failed to load IME module %s", path);
            return GLFW_FALSE;
        }

        getBackend = (PFN_glfwGetX11IMEBackend)
            _glfwPlatformGetModuleSymbol(_glfw.x11.imeModule.handle,
                                         "glfwGetX11IMEBackend");
        if (!getBackend)
        {
            _glfwInputError(GLFW_PLATFORM_ERROR,
                            "X11: IME module does not export glfwGetX11IMEBackend");
            _glfwPlatformFreeModule(_glfw.x11.imeModule.handle);
            _glfw_free(loadedPath);
            memset(&_glfw.x11.imeModule, 0, sizeof(_glfw.x11.imeModule));
            return GLFW_FALSE;
        }

        if (_glfw.x11.imeModule.debug)
        {
            fprintf(stderr, "GLFW IME: loaded external module %s\n",
                    loadedPath ? loadedPath : path);
        }

        _glfw_free(loadedPath);
    }
#if defined(_GLFW_EMBED_IBUS_MODULE)
    else if (_glfw.x11.imeModule.debug)
        fprintf(stderr, "GLFW IME: using embedded IBus module\n");
#endif

    memset(&host, 0, sizeof(host));
    host.commit_text = hostCommitText;
    host.update_preedit = hostUpdatePreedit;
    host.clear_preedit = hostClearPreedit;
    host.status_changed = hostStatusChanged;
    host.get_time = hostGetTime;
    host.post_empty_event = hostPostEmptyEvent;
    host.log = hostLog;

    memset(&api, 0, sizeof(api));
    if (!getBackend(GLFW_X11_IME_MODULE_ABI_VERSION, &host, &api) ||
        !api.create || !api.destroy || !api.process_key || !api.drain_events)
    {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "X11: IME module rejected ABI version %i",
                        GLFW_X11_IME_MODULE_ABI_VERSION);
        if (_glfw.x11.imeModule.handle)
            _glfwPlatformFreeModule(_glfw.x11.imeModule.handle);
        memset(&_glfw.x11.imeModule, 0, sizeof(_glfw.x11.imeModule));
        return GLFW_FALSE;
    }

    _glfw.x11.imeModule.api = api;
    _glfw.x11.imeModule.backend = api.create(&host);
    if (!_glfw.x11.imeModule.backend)
    {
        _glfwInputError(GLFW_PLATFORM_ERROR,
                        "X11: IME module failed to create backend");
        if (_glfw.x11.imeModule.handle)
            _glfwPlatformFreeModule(_glfw.x11.imeModule.handle);
        memset(&_glfw.x11.imeModule, 0, sizeof(_glfw.x11.imeModule));
        return GLFW_FALSE;
    }

    return GLFW_TRUE;
}

void _glfwUnloadIMEModuleX11(void)
{
    if (_glfw.x11.imeModule.backend && _glfw.x11.imeModule.api.destroy)
        _glfw.x11.imeModule.api.destroy(_glfw.x11.imeModule.backend);

    if (_glfw.x11.imeModule.handle)
        _glfwPlatformFreeModule(_glfw.x11.imeModule.handle);

    memset(&_glfw.x11.imeModule, 0, sizeof(_glfw.x11.imeModule));
}

GLFWbool _glfwHasIMEModuleX11(void)
{
    return _glfw.x11.imeModule.backend != NULL;
}

void _glfwDrainIMEModuleX11(void)
{
    if (_glfwHasIMEModuleX11())
        _glfw.x11.imeModule.api.drain_events(_glfw.x11.imeModule.backend);
}

static void sendCachedCursorRect(_GLFWwindow* window, const char* reason)
{
    if (!_glfwHasIMEModuleX11() ||
        !_glfw.x11.imeModule.api.set_cursor_rect)
    {
        return;
    }

    if (!window->x11.imeCursorRectValid)
    {
        if (_glfw.x11.imeModule.debug)
        {
            fprintf(stderr,
                    "GLFW IME: skip SetCursorLocation reason=%s valid=0 pending=1 original=(%i,%i %ix%i)\n",
                    reason,
                    window->preedit.cursorPosX,
                    window->preedit.cursorPosY,
                    window->preedit.cursorWidth,
                    window->preedit.cursorHeight);
        }
        window->x11.imeCursorRectPending = GLFW_TRUE;
        return;
    }

    if (!window->x11.imeCursorRectSent && _glfw.x11.imeModule.debug)
    {
        fprintf(stderr,
                "GLFW IME: first SetCursorLocation reason=%s original=(%i,%i %ix%i) window_root=(%i,%i) final=(%i,%i %ix%i)\n",
                reason,
                window->x11.imeCursorX,
                window->x11.imeCursorY,
                window->x11.imeCursorWidth,
                window->x11.imeCursorHeight,
                window->x11.imeWindowRootX,
                window->x11.imeWindowRootY,
                window->x11.imeCursorRootX,
                window->x11.imeCursorRootY,
                window->x11.imeCursorWidth,
                window->x11.imeCursorHeight);
    }

    _glfw.x11.imeModule.api.set_cursor_rect(_glfw.x11.imeModule.backend,
                                            window,
                                            window->x11.imeCursorRootX,
                                            window->x11.imeCursorRootY,
                                            window->x11.imeCursorWidth,
                                            window->x11.imeCursorHeight);

    window->x11.imeCursorRectSent = GLFW_TRUE;
    window->x11.imeCursorRectPending = GLFW_FALSE;
}

void _glfwRefreshCursorRectIMEModuleX11(_GLFWwindow* window, const char* reason)
{
    Window child;
    XWindowAttributes attributes;
    int glfwX = 0;
    int glfwY = 0;
    int windowRootX = 0;
    int windowRootY = 0;
    int cursorRootX = window->preedit.cursorPosX;
    int cursorRootY = window->preedit.cursorPosY;
    const int localX = window->preedit.cursorPosX;
    const int localY = window->preedit.cursorPosY;
    const int localW = window->preedit.cursorWidth;
    const int localH = window->preedit.cursorHeight;
    const Bool windowTranslated =
        XTranslateCoordinates(_glfw.x11.display,
                              window->x11.handle,
                              _glfw.x11.root,
                              0, 0,
                              &windowRootX, &windowRootY,
                              &child);
    const Bool cursorTranslated =
        XTranslateCoordinates(_glfw.x11.display,
                              window->x11.handle,
                              _glfw.x11.root,
                              localX, localY,
                              &cursorRootX, &cursorRootY,
                              &child);
    const Status attributesStatus =
        XGetWindowAttributes(_glfw.x11.display, window->x11.handle, &attributes);
    const int screenWidth = DisplayWidth(_glfw.x11.display, _glfw.x11.screen);
    const int screenHeight = DisplayHeight(_glfw.x11.display, _glfw.x11.screen);
    const GLFWbool onScreen =
        cursorRootX > -screenWidth &&
        cursorRootY > -screenHeight &&
        cursorRootX < screenWidth * 2 &&
        cursorRootY < screenHeight * 2;
    const GLFWbool looksInitialized =
        window->x11.imeNormalKeySeen ||
        windowRootX != 0 ||
        windowRootY != 0 ||
        window->x11.imeCursorRectRetries > 0;
    const GLFWbool plausible =
        windowTranslated &&
        cursorTranslated &&
        attributesStatus &&
        attributes.map_state == IsViewable &&
        onScreen &&
        looksInitialized;

    _glfwGetWindowPosX11(window, &glfwX, &glfwY);

    window->x11.imeCursorX = localX;
    window->x11.imeCursorY = localY;
    window->x11.imeCursorWidth = localW;
    window->x11.imeCursorHeight = localH;
    window->x11.imeWindowRootX = windowRootX;
    window->x11.imeWindowRootY = windowRootY;
    window->x11.imeCursorRootX = cursorRootX;
    window->x11.imeCursorRootY = cursorRootY;
    window->x11.imeCursorRectValid = plausible;
    window->x11.imeCursorRectPending = !plausible;

    if (_glfw.x11.imeModule.debug)
    {
        fprintf(stderr,
                "GLFW IME: cursor translate reason=%s source=0x%lx root=0x%lx window_ret=%i cursor_ret=%i window_root=(%i,%i) cursor_root=(%i,%i) glfw_pos=(%i,%i) local=(%i,%i %ix%i) map_state=%i normal_key_seen=%i retries=%i plausible=%i\n",
                reason,
                (unsigned long) window->x11.handle,
                (unsigned long) _glfw.x11.root,
                windowTranslated ? 1 : 0,
                cursorTranslated ? 1 : 0,
                windowRootX, windowRootY,
                cursorRootX, cursorRootY,
                glfwX, glfwY,
                localX, localY, localW, localH,
                attributesStatus ? attributes.map_state : -1,
                window->x11.imeNormalKeySeen ? 1 : 0,
                window->x11.imeCursorRectRetries,
                plausible ? 1 : 0);
    }

    if (!plausible && window->x11.imeCursorRectRetries < 2)
    {
        window->x11.imeCursorRectRetries++;
        _glfwPostEmptyEventX11();
    }
}

void _glfwRefreshPendingCursorRectsIMEModuleX11(const char* reason)
{
    if (!_glfwHasIMEModuleX11())
        return;

    for (_GLFWwindow* window = _glfw.windowListHead;  window;  window = window->next)
    {
        if (window->x11.imeCursorRectPending)
        {
            _glfwRefreshCursorRectIMEModuleX11(window, reason);
            sendCachedCursorRect(window, reason);
        }
    }
}

void _glfwNotifyNormalKeyIMEModuleX11(_GLFWwindow* window)
{
    if (!_glfwHasIMEModuleX11())
        return;

    window->x11.imeNormalKeySeen = GLFW_TRUE;
    if (window->x11.imeCursorRectPending)
    {
        _glfwRefreshCursorRectIMEModuleX11(window, "after-normal-key");
        sendCachedCursorRect(window, "after-normal-key");
    }
}

GLFWbool _glfwProcessKeyIMEModuleX11(_GLFWwindow* window,
                                     unsigned int keycode,
                                     unsigned int keysym,
                                     unsigned int state,
                                     int action,
                                     int mods,
                                     unsigned long time)
{
    GLFWx11IMEKeyEvent event;
    GLFWx11IMEKeyResult result;

    if (!_glfwHasIMEModuleX11())
        return GLFW_FALSE;

    _glfwRefreshCursorRectIMEModuleX11(window, "before-key");
    sendCachedCursorRect(window, "before-key");

    memset(&event, 0, sizeof(event));
    event.window = window;
    event.x11_window = window->x11.handle;
    event.keycode = keycode;
    event.keysym = keysym;
    event.state = state;
    event.action = action;
    event.mods = mods;
    event.time = time;
    event.cursor_rect_valid = window->x11.imeCursorRectValid;
    event.cursor_x = window->x11.imeCursorX;
    event.cursor_y = window->x11.imeCursorY;
    event.cursor_width = window->x11.imeCursorWidth;
    event.cursor_height = window->x11.imeCursorHeight;
    event.cursor_root_x = window->x11.imeCursorRootX;
    event.cursor_root_y = window->x11.imeCursorRootY;

    memset(&result, 0, sizeof(result));
    if (!_glfw.x11.imeModule.api.process_key(_glfw.x11.imeModule.backend,
                                             &event, &result))
    {
        return GLFW_FALSE;
    }

    _glfwDrainIMEModuleX11();
    return result.handled ? GLFW_TRUE : GLFW_FALSE;
}

void _glfwFocusInIMEModuleX11(_GLFWwindow* window)
{
    if (_glfwHasIMEModuleX11() && _glfw.x11.imeModule.api.focus_in)
    {
        window->x11.imeLogNextKey = GLFW_TRUE;
        _glfw.x11.imeModule.api.focus_in(_glfw.x11.imeModule.backend,
                                         window, window->x11.handle);
        _glfwRefreshCursorRectIMEModuleX11(window, "focus-in");
        sendCachedCursorRect(window, "focus-in");
    }
}

void _glfwFocusOutIMEModuleX11(_GLFWwindow* window)
{
    if (_glfwHasIMEModuleX11() && _glfw.x11.imeModule.api.focus_out)
        _glfw.x11.imeModule.api.focus_out(_glfw.x11.imeModule.backend,
                                          window, window->x11.handle);
}

void _glfwSetCursorRectIMEModuleX11(_GLFWwindow* window, int x, int y, int w, int h)
{
    if (_glfwHasIMEModuleX11() && _glfw.x11.imeModule.api.set_cursor_rect)
    {
        // Applications provide client-area coordinates via the public IME API.
        // The IBus X11 panel needs root-window coordinates for candidate
        // placement, so the conversion stays local to this experimental path.
        window->preedit.cursorPosX = x;
        window->preedit.cursorPosY = y;
        window->preedit.cursorWidth = w;
        window->preedit.cursorHeight = h;
        _glfwRefreshCursorRectIMEModuleX11(window, "cursor-update");
        sendCachedCursorRect(window, "cursor-update");
    }
}

void _glfwResetIMEModuleX11(_GLFWwindow* window)
{
    if (_glfwHasIMEModuleX11() && _glfw.x11.imeModule.api.reset)
        _glfw.x11.imeModule.api.reset(_glfw.x11.imeModule.backend, window);
}

void _glfwSetStatusIMEModuleX11(_GLFWwindow* window, int active)
{
    if (_glfwHasIMEModuleX11() && _glfw.x11.imeModule.api.set_status)
        _glfw.x11.imeModule.api.set_status(_glfw.x11.imeModule.backend,
                                           window, active);
}

int _glfwGetStatusIMEModuleX11(_GLFWwindow* window)
{
    if (_glfwHasIMEModuleX11() && _glfw.x11.imeModule.api.get_status)
        return _glfw.x11.imeModule.api.get_status(_glfw.x11.imeModule.backend, window);

    return GLFW_FALSE;
}

#endif // _GLFW_X11
