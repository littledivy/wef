# Architecture

## Overview

wef separates browser engines (**backends**) from application logic
(**runtimes**). Backends are native executables; runtimes are shared libraries
(`.dylib`/`.so`/`.dll`) loaded at startup. They communicate through a C ABI
defined in `capi/include/wef.h`.

```
┌──────────────────┐         ┌───────────────────┐
│  Backend (exe)   │ ──C ABI──▶  Runtime (dylib) │
│  CEF / WebView / ◀─────────│  User app logic   │
│  Servo / Winit   │         │  (links wef capi) │
└──────────────────┘         └───────────────────┘
```

### Backends

| Directory  | Engine                                            | Language           | Window ownership     |
| ---------- | ------------------------------------------------- | ------------------ | -------------------- |
| `cef/`     | Chromium Embedded Framework                       | C++                | CEF Views (internal) |
| `webview/` | System webview (WKWebView / WebView2 / WebKitGTK) | C++ (per-platform) | Created directly     |
| `servo/`   | Servo                                             | Rust (winit)       | Created directly     |
| `winit/`   | None (winit only, no web content)                 | Rust (winit)       | Created directly     |

### Runtime (capi)

`capi/` provides the Rust crate that runtimes link against. It wraps the raw C
function pointers from `wef_backend_api_t` into safe Rust types (`Window`,
`Value`, `JsCall`, `KeyboardEvent`, `MouseClickEvent`, etc.).

## Key Patterns

### The C ABI contract (`wef_backend_api_t`)

The central interface is a struct of function pointers (`capi/include/wef.h`).
The backend fills this struct and passes a pointer to `wef_runtime_init()`. The
runtime stores it for the process lifetime. Every capability (navigation, JS
execution, event handlers, window management) is a nullable function pointer in
this struct.

**Adding a new API**: add the field to `wef_backend_api_t` in
`capi/include/wef.h`, then implement it in every backend. The CEF and webview
backends each have a local copy of `wef.h` that must be kept in sync.

### Callback registration (event handlers)

All event handlers follow the same pattern:

1. Define a C callback type (`wef_keyboard_event_fn`, `wef_mouse_click_fn`)
2. Add a `set_*_handler(backend_data, callback, user_data)` function pointer to
   the API struct
3. Backend stores the callback+user_data behind a mutex
4. Backend dispatches from its native event handler, passing the user_data back

On the runtime side (`capi/src/lib.rs`), an `unsafe extern "C"` trampoline
converts C types to Rust types and forwards to a stored `Box<dyn Fn(Event)>`.

**Events are non-consuming** -- handlers always return the event to the
underlying engine. This is an interception model, not a consumption model.

### Winit backend code sharing (`backend-winit-common`)

The `servo/` and `winit/` backends both use winit for windowing. Shared code
lives in `backend-winit-common/src/lib.rs`:

- **`BackendAccess` trait**: each backend implements this to provide access to
  its `CommonState`, event loop proxy, and event type mapping.
- **`define_common_backend_fns!` macro**: generates the `unsafe extern "C"`
  functions for all common operations (title, size, position, visibility, event
  handlers, etc.).
- **`fill_common_api!` macro**: wires those generated functions into a
  `WefBackendApi` struct.
- **`CommonState`**: holds pending window mutations (`Mutex<Option<T>>`) and
  event handler callbacks.
- **`handle_common_event()`**: processes `CommonEvent` variants against a winit
  `Window`.

To add a new winit-based API: add the pending state to `CommonState`, the
function to `define_common_backend_fns!`, the assignment to `fill_common_api!`,
and the dispatch to `handle_common_event()`. Both servo and winit backends pick
it up automatically.

### Pending state pattern (async window ops)

Backend API functions are called from the runtime thread, but window operations
must happen on the UI thread. The pattern is:

1. Store the desired value in a `Mutex<Option<T>>` on `CommonState`
2. Send an event via the winit `EventLoopProxy`
3. On the UI thread, take the pending value and apply it to the window

C++ backends use platform-specific dispatch instead (`dispatch_async` on macOS,
`PostMessage` on Windows, `g_idle_add` on Linux).

### CEF: no native mouse/input handlers

CEF provides `CefKeyboardHandler` for keyboard events but has **no equivalent
`CefMouseHandler`**. This is because CEF Views creates and owns the native
window internally -- the embedder has no direct access to the native event loop.

**Workaround**: platform-specific native event monitors that hook into the OS
event system:

| Platform | Technique                                                                                      | File                           |
| -------- | ---------------------------------------------------------------------------------------------- | ------------------------------ |
| macOS    | `[NSEvent addLocalMonitorForEventsMatchingMask:]`                                              | `cef/src/main_mac.mm`          |
| Windows  | `WM_*BUTTON*` messages in `WindowProc` (via `CefWindow::GetWindowHandle()` + subclassing)      | `cef/src/main_win.cc` (TODO)   |
| Linux    | GTK `button-press-event` / `button-release-event` signals (via `CefWindow::GetWindowHandle()`) | `cef/src/main_linux.cc` (TODO) |

The monitor functions (`InstallNativeMouseMonitor()` /
`RemoveNativeMouseMonitor()`) are declared in `cef/src/runtime_loader.h` and
called from `WefWindowDelegate::OnWindowCreated` / `OnWindowDestroyed` in
`cef/src/app.cc`. This is the same approach Electron uses -- Electron creates
native windows directly (bypassing CEF Views), but since we use CEF Views, we
instead install post-hoc monitors on the window CEF creates.

### Webview backends: direct native window access

Unlike CEF, the webview backends create their own native windows, so event
interception is straightforward:

| Platform                       | Keyboard                                                       | Mouse                                                     |
| ------------------------------ | -------------------------------------------------------------- | --------------------------------------------------------- |
| macOS (`webview_macos.mm`)     | `NSEvent addLocalMonitorForEventsMatchingMask:` for key events | Same mechanism for mouse events                           |
| Windows (`webview_windows.cc`) | `WM_KEYDOWN` / `WM_KEYUP` in `WindowProc`                      | `WM_*BUTTON*` in `WindowProc`                             |
| Linux (`webview_linux.cc`)     | GTK `key-press-event` / `key-release-event` signals            | GTK `button-press-event` / `button-release-event` signals |

### W3C UI Events key mapping

Keyboard events expose `key` (logical, e.g. `"a"`, `"Enter"`) and `code`
(physical, e.g. `"KeyA"`, `"Enter"`) following the W3C UI Events specification.
Each platform has its own mapping:

- **winit backends**: `winit_key_to_string()` / `winit_code_to_string()` in
  `backend-winit-common`
- **CEF**: `CefKeyCodeToString()` / `CefKeyCodeToCode()` in `cef/src/app.cc`
  (maps Windows virtual key codes)
- **webview macOS**: `NSEventKeyToString()` / `NSEventKeyCodeToCode()` (maps
  macOS key codes)
- **webview Windows**: `VirtualKeyToKey()` / `VirtualKeyToCode()` (maps Win32 VK
  codes)
- **webview Linux**: `GdkKeyvalToKey()` / `GdkKeycodeToCode()` (maps GDK keyvals
  and evdev hardware keycodes)

### Mouse button mapping

Mouse buttons are normalized to `WEF_MOUSE_BUTTON_*` constants.
Platform-specific mappings:

- **NSEvent** `buttonNumber`: 0=left, 1=right, 2+=other (detect via event type
  mask)
- **Win32**: separate `WM_*BUTTON*` messages per button; `XBUTTON1`/`XBUTTON2`
  for back/forward
- **GDK**: `event->button`: 1=left, 2=middle, 3=right, 8=back, 9=forward

### Value marshalling

The wef API has a rich value type (`wef_value_t`) for JS interop. Backends own
the value representation:

- **CEF**: wraps `CefValue` / `CefListValue` directly
- **Webview**: uses a custom `Value` class with JSON serialization for JS
  communication
- **Winit backends**: stub implementations (no JS engine)

The runtime crate (`capi/src/lib.rs`) wraps these into a Rust `Value` enum via
the function pointer API, completely opaque to the value's backend
representation.

### Modifier flags

All platforms normalize keyboard modifiers to a shared bitmask:

```
WEF_MOD_SHIFT   = 1 << 0
WEF_MOD_CONTROL = 1 << 1
WEF_MOD_ALT     = 1 << 2
WEF_MOD_META    = 1 << 3
```

Each platform maps from its native representation (`NSEventModifierFlags`,
`GetKeyState()`, `GdkModifierType`, `CefEventFlags`, winit `ModifiersState`).
