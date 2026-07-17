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
- Can the existing GLFW preedit model represent useful IBus preedit state,
  including caret position and focused text blocks?

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

For experimentation with prebuilt GLFW binaries, the same IBus backend can also
be embedded into GLFW with the `GLFW_EMBED_IBUS_MODULE` CMake option.  In that
mode, GLFW uses the embedded backend when `GLFW_IM_MODULE` is not set, while
still allowing `GLFW_IM_MODULE` to override it.

The standalone IME modules are not installed by default.  Configure with
`-DGLFW_INSTALL_IME_MODULES=ON` to install `glfw-ibus.so` and
`glfw-fcitx5.so`.  The default install location is
`${CMAKE_INSTALL_LIBDIR}/glfw` relative to the install prefix, typically
`lib/glfw` under that prefix.  For example, with the default `/usr/local`
prefix this is usually `/usr/local/lib/glfw`.

When `GLFW_IM_MODULE` does not contain a `/`, GLFW also searches this default
module directory.  The `glfw-` prefix and platform module suffix may be omitted,
so `GLFW_IM_MODULE=ibus` and `GLFW_IM_MODULE=fcitx5` load the installed
`glfw-ibus.so` and `glfw-fcitx5.so` modules.  Full paths continue to be loaded
as specified.

### GLFW Core

GLFW core remains unaware of IBus, Fcitx5 and D-Bus.  The X11 backend only knows
about the private plugin ABI in `src/x11_ime_module.h`.

When a module is active, the X11 backend:

- skips XIM setup for that run
- forwards X11 key events to the module
- passes the X event keysym, including modifier-translated printable keysyms,
  to IBus `ProcessKeyEvent`
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
package is available.  A separate native Fcitx5 experiment is built as
`glfw-fcitx5.so` from the same private X11 IME module ABI.

If `GLFW_EMBED_IBUS_MODULE` is enabled, the same module source is also compiled
into the GLFW library and `dbus-1` becomes a GLFW build dependency.  This is
only an experiment to remove setup friction for local or redistributed test
builds.

The module owns:

- IBus address discovery
- libdbus connection setup
- IBus input context creation
- D-Bus method calls
- D-Bus signal handling
- worker thread lifetime
- request and event queues
- timing instrumentation

### Native Fcitx5 Experiment

The native Fcitx5 module is a sibling of the IBus module.  It is loaded with the
same runtime mechanism:

```sh
GLFW_IM_MODULE=/path/to/glfw-fcitx5.so ./application
```

It keeps Fcitx5 and D-Bus protocol knowledge inside the module.  GLFW core still
only sees the private X11 IME backend ABI.

The module connects to Fcitx5 through the session bus name `org.fcitx.Fcitx5`,
creates an input context with `org.fcitx.Fcitx.InputMethod1.CreateInputContext`,
then talks to the returned `org.fcitx.Fcitx.InputContext1` object.  It currently
uses:

- `SetCapability`
- `FocusIn`
- `FocusOut`
- `Reset`
- `SetCursorRect`
- `ProcessKeyEvent`
- `CommitString`
- `UpdateFormattedPreedit`
- `NotifyFocusOut`

The first version intentionally maps Fcitx5 formatted preedit segments directly
to GLFW preedit blocks.  This preserves useful segmentation without exposing
Fcitx5 formatting values to GLFW core.

The module treats a non-zero formatted-preedit segment format as the focused
GLFW preedit block.  If no formatted segment is marked, it falls back to using
the reported caret position.

Unlike the IBus module, the Fcitx5 module sends the X11 keycode received from
GLFW directly to `ProcessKeyEvent`.  Fcitx5 appears to interpret this field as
an XKB keycode.  Using the IBus-style `keycode - 8` conversion caused obvious
layout mismatches, such as the Enter key being interpreted as `t`.

The module retries connection to Fcitx5 when startup, input context creation or
key processing fails.  While disconnected, focus state is retained, key events
are reported as not handled, and the worker retries connection periodically.

Native Fcitx5 status control is intentionally left as future work.  The current
prototype keeps status behavior minimal and focuses on commit, preedit,
candidate positioning and key processing.

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
- `Enabled`
- `Disabled`

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

This has been tested with normal IBus preedit flow and now behaves well enough
for practical application-side preedit drawing in the prototype.  It is still a
mapping from IBus attributes to GLFW's simpler block model, not a lossless
exposure of every IBus text attribute.

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

### Text Input Focus

`glfwSetTextInputFocus(window, GLFW_TRUE)` maps to IBus `FocusIn` when the X11
window is focused.  If the X11 window is not focused, the request is reflected
by GLFW state and the next X11 `FocusIn` event activates the module.

`glfwSetTextInputFocus(window, GLFW_FALSE)` maps to IBus `Reset` followed by
`FocusOut`.  This disables text input routing for the window while keeping the
window focus state separate from the text input focus abstraction.

The X11 key path also checks GLFW's text input focus state before sending
`ProcessKeyEvent` to the module.  This is required because IBus `FocusOut` is
asynchronous and does not by itself prevent GLFW from continuing to route key
events through IBus.

For application compatibility experiments, GLFW can infer text input focus from
legacy IME and cursor input state.  Set `GLFW_INFER_TEXT_INPUT_FOCUS` to a
non-zero value, or build GLFW with `GLFW_INFER_TEXT_INPUT_FOCUS` enabled.  In
this mode, `glfwSetInputMode(window, GLFW_IME, value)` calls the text input focus
path instead of the native platform IME-status path.

When this compatibility mode is active, GLFW also treats cursor mode changes as
text input focus hints.  The requested text input focus state is kept separately
from the effective platform focus, so cursor-disabled gameplay can suppress IME
routing while cursor-normal text entry can re-enable it.  This helps existing
applications that switch between gameplay and text entry with cursor mode only.

`glfwGetInputMode(window, GLFW_IME)` still returns the native platform IME
status until the remapping path has initialized text input focus state for the
window.  After that, it returns the application-requested text input focus
state, not the cursor-mode-masked effective platform focus.  This preserves the
old query behavior for applications that never use the remapping path.

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
- caret index, block count and focused block for preedit events
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

### Candidate, Attribute And Surrounding Text Completeness

The prototype does not implement lookup-table/candidate-list parsing or
surrounding text.

IBus preedit attributes are mapped to GLFW block sizes and a focused block, but
the mapping is intentionally lossy.  It is enough for useful preedit display,
but it does not expose underline style, foreground/background color or every
IBus text attribute to applications.

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
- IBus preedit text, caret position and focused block information can be mapped
  to GLFW's existing preedit callback model
- candidate window positioning can be handled without changing the public IME API
- explicit text input focus can be made to stop routing X11 keys through IBus

The current prototype is no longer only a proof of concept for drawing basic
preedit text.  It is close to usable for application testing on X11 with IBus or
Fcitx5's IBus compatibility layer.

However, this is still an experimental backend.  It is not currently recommended
for upstream submission as-is.

The remaining decision point is semantic reliability: late replies and late text
signals after key-processing timeouts need more real-world measurement before an
upstream-quality design can be justified.
