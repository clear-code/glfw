# X11 IBus IME Prototype

## Purpose

This document describes an experimental X11 IME backend for GLFW based on IBus.
The goal of the prototype is to determine whether IBus, and Fcitx5 through its
IBus compatibility layer, can be used from GLFW without changing the GLFW event
loop architecture.

This is a research prototype.  It is not intended to be production quality or
ready for upstream submission.

The prototype is meant to answer these questions:

- Can an IME backend be loaded dynamically instead of linked into GLFW?
- Can all D-Bus communication happen outside the GLFW event loop?
- Can X11 key handling synchronously ask the IME whether a key was handled?
- Do IBus replies or text signals arrive late enough to cause duplicated text?
- Is candidate window positioning practical with the existing cursor rectangle
  API?

## Architecture

The prototype adds a private X11-only IME module ABI.  It is not part of the
public GLFW API.

At runtime, GLFW checks `GLFW_IM_MODULE`.  If it is set, GLFW loads the named
shared object with `dlopen()` and looks up the `glfwGetX11IMEBackend` symbol with
`dlsym()`.

For example:

```sh
GLFW_IM_MODULE=/path/to/glfw-ibus.so ./application
```

The standalone IBus IME module is not installed by default.  Configure with
`-DGLFW_INSTALL_IME_MODULES=ON` to install `glfw-ibus.so`.  The default install
location is `${CMAKE_INSTALL_LIBDIR}/glfw` relative to the install prefix,
typically `lib/glfw` under that prefix.  For example, with the default
`/usr/local` prefix this is usually `/usr/local/lib/glfw`.

When `GLFW_IM_MODULE` does not contain a `/`, GLFW also searches this default
module directory.  The `glfw-` prefix and platform module suffix may be omitted,
so `GLFW_IM_MODULE=ibus` loads the installed `glfw-ibus.so` module.  Full paths
continue to be loaded as specified.

### GLFW Core

GLFW core remains unaware of IBus, Fcitx5 and D-Bus.  The X11 backend only knows
about the private plugin ABI in `src/x11_ime_module.h`.

When a module is active, the X11 backend:

- skips XIM setup for that run
- forwards X11 key events to the module
- forwards focus changes to the module
- translates preedit cursor rectangles from client-area coordinates to X11 root
  coordinates
- drains queued module events on the main thread

The public IME API semantics are unchanged.  Applications still use
`glfwSetPreeditCursorRectangle` with GLFW window/client-area coordinates.

### Plugin ABI

The plugin ABI consists of:

- a host callback table provided by GLFW
- a backend function table provided by the module
- opaque window tokens passed back to GLFW by the module

The module must not dereference GLFW window pointers.  They are only handles for
callbacks into GLFW.

The module can request that `glfwWaitEvents` wake up by calling the host
`post_empty_event` callback.  This uses GLFW's existing X11 empty-event pipe; it
does not add D-Bus file descriptors to the GLFW event loop.

### Dynamically Loaded Module

The prototype module is built as `glfw-ibus.so` when the `dbus-1` development
package is available.

The module owns:

- IBus address discovery
- libdbus connection setup
- IBus input context creation
- D-Bus method calls
- D-Bus signal handling
- worker thread lifetime
- request and event queues
- timing instrumentation

### Worker Thread

The module creates a worker thread.  The worker thread owns all D-Bus traffic.

GLFW's X11 thread sends commands to the worker through an explicit queue.  The
worker sends IME events back through a second queue.  The worker never calls GLFW
IME callbacks directly.

Queued events are drained from GLFW's normal X11 event functions on the main
thread.  This preserves the existing `glfwPollEvents`, `glfwWaitEvents` and
`glfwWaitEventsTimeout` architecture.

### D-Bus Communication

The worker uses libdbus directly.  The prototype intentionally does not integrate
libdbus watches or timeouts with GLFW.

`ProcessKeyEvent` is currently sent with a blocking D-Bus call on the worker
thread.  The GLFW/X11 thread waits for the worker to report the result, with a
prototype timeout controlled by `GLFW_IBUS_TIMEOUT_MS`.

The default timeout is 100 ms.

### IBus Integration

The module uses these IBus input context methods and signals:

- `CreateInputContext`
- `SetCapabilities`
- `ProcessKeyEvent`
- `CommitText`
- `UpdatePreeditText`
- `HidePreeditText`
- `FocusIn`
- `FocusOut`
- `SetCursorLocation`
- `Reset`

The module currently uses `SetCursorLocation`, not
`SetCursorLocationRelative`.

For X11 candidate positioning, GLFW translates the application-provided cursor
rectangle from client-area coordinates to root-window coordinates with
`XTranslateCoordinates()` before sending it to the module.

## Relationship to PR #2130

This prototype builds on the IME architecture introduced by PR #2130.

It reuses:

- the public `GLFW_IME` input mode
- preedit callbacks
- IME status callbacks
- preedit cursor rectangle APIs
- shared preedit state in `_GLFWpreedit`
- X11 platform IME hooks for focus, key handling, cursor rectangle updates and
  reset

It does not redesign the application-facing IME API.

The prototype adds an alternative X11 IME backend path behind a dynamically
loaded module.  When no module is loaded, the existing XIM behavior remains the
fallback.

## Why A Plugin Architecture Was Chosen

IBus support brings Linux/X11-specific complexity into an otherwise portable
library.  A plugin boundary keeps that complexity separate from GLFW core.

The plugin design was chosen to:

- avoid a hard D-Bus dependency in GLFW core
- avoid libdbus watch and timeout integration in the GLFW event loop
- avoid changes to `glfwPollEvents`, `glfwWaitEvents` and
  `glfwWaitEventsTimeout`
- isolate IBus/Fcitx5 behavior and failure modes
- make the experiment opt-in with `GLFW_IM_MODULE`
- allow the prototype to be removed or replaced without affecting the public API

This matches the goal of evaluating feasibility without committing GLFW to a
production IBus backend design.

## Current Status

### Preedit Support

Preedit updates from IBus are received through `UpdatePreeditText`, queued by the
worker and drained on the GLFW main thread.

The prototype maps IBus preedit text, caret position and attribute ranges to
GLFW preedit text, block sizes, focused block and caret index.  The block
mapping is intentionally conservative because IBus engines differ in which
attributes they use to mark the active segment.

### Commit Support

Committed text from `CommitText` is queued by the worker and emitted through
GLFW's normal character input path on the main thread.

### Candidate Window Positioning

Candidate window positioning is supported through `SetCursorLocation`.

Applications still provide cursor rectangles in GLFW window/client-area
coordinates.  The X11 plugin bridge translates those coordinates to root-window
coordinates before sending them to IBus.

The bridge tracks whether a valid cursor rectangle has been translated.  It does
not send `SetCursorLocation` until a valid rectangle exists.  It resends the last
valid rectangle on focus and before key processing.

### IME Enable And Disable Behavior

The prototype has minimal IME status support.

IBus `Enabled` and `Disabled` signals update module state and trigger GLFW IME
status callbacks.  `glfwSetInputMode(window, GLFW_IME, value)` maps to
`FocusIn` or `FocusOut` in the prototype.

This does not have the same semantics as Win32 `ImmGetOpenStatus` and
`ImmSetOpenStatus`.  It is sufficient for experimentation, but not a final API
mapping.

## Instrumentation

The prototype intentionally keeps timing and late-event instrumentation.  It is
emitted only when `GLFW_IME_DEBUG` is set to a non-zero value.

Each `ProcessKeyEvent` request logs:

- request id
- X11 key serial
- timestamp
- keyval
- keycode
- IBus state

Each reply logs:

- request id
- latency
- handled status
- timeout status
- failure status

Each timeout logs:

- request id
- elapsed time
- X11 key serial

Each queued and drained IME event logs:

- event type
- attributed request id
- whether the attributed request had timed out
- timestamp
- text, when present

IBus signals do not include the originating `ProcessKeyEvent` request.  The
module attributes signals to the active request when possible, otherwise to the
most recent request.  This attribution is for observation only.

## Known Risks

### Timeout Semantics

The GLFW/X11 thread may stop waiting before the worker receives a
`ProcessKeyEvent` reply.  The key is then treated as not handled and GLFW falls
back to the normal X11 key path.

IBus may still later process the key.

### Possible Duplicated Text After Timeout

If GLFW falls back to normal text input after a timeout and IBus later emits
`CommitText` for the same key, the application may receive duplicated text.

The prototype is instrumented to observe this.  It does not fully solve it.

### Late ProcessKeyEvent Replies

Late replies are logged with their original request id and timeout state.

The prototype keeps a small list of recent requests so late replies and related
signals can be identified.

### Late CommitText Events

Late `CommitText` events are queued and logged with the best available request
attribution.  Because IBus does not identify the originating request, this
attribution is not guaranteed to be exact.

### Candidate And Surrounding Text Completeness

The prototype does not implement lookup-table/candidate-list parsing,
surrounding text, or full IBus preedit attributes.  These would be needed for a
more complete backend.

### Restart And Recovery

The prototype does not attempt production-grade daemon restart handling,
reconnection or failure recovery.

## Current Recommendation

The prototype demonstrates that the architecture is technically feasible:

- an IME backend can be dynamically loaded
- D-Bus communication can be isolated from the GLFW event loop
- worker-thread communication can be kept behind explicit queues
- IBus/Fcitx5 preedit and commit paths can be integrated with the existing IME
  architecture
- candidate window positioning can be handled without changing the public IME API

However, this is still a research prototype.  It is not currently recommended
for upstream submission.

The remaining decision point is semantic reliability: late replies and late text
signals after key-processing timeouts need more real-world measurement before an
upstream-quality design can be justified.
