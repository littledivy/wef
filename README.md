![wef-doodle](https://github.com/user-attachments/assets/c7d55981-ab29-4bea-a4c6-28feeb1b4520)

wef ("Web embedded framework") lets you build cross platform apps using web technologies with your choice of browser engine.

### How it works

wef is built around a C ABI that separates browser engines (backends) from application logic (runtimes). Prebuilt backends for [CEF](./cef), system [WebView](./webview) and [Servo](./servo) implement the `wef_backend_api_t` interface defined in [`capi/include/wef.h`](capi/include/wef.h)

Runtimes are shared libraries compiled with user application logic. When the backend starts, it loads the runtime dylib and hands control to the runtime. The runtime uses the interface to create windows, register JavaScript bindings, and respond to calls from the web content. 

wef also handles bidirectional marshalling that abstracts message passing between JS and native code. This modular approach leads to fast development and packaging of wef applications.

```rust
use wef::{Window, Value};

fn main() {
    Window::new(800, 600)
      .title("My App")
      .bind("greet", |call| {
         let name = call.args.get(0)
             .and_then(|v| v.as_string())
             .unwrap_or("World");
         call.resolve(Value::String(format!("Hello, {}!", name)));
      })
      .load_url("https://example.com");
}

wef::main!(main);
```

### Backend matrix

wef ships three backends today. The [CEF](./cef) backend embeds Chromium 144 and runs a multi-process architecture identical to a full browser — you get the same renderer, GPU process, and DevTools you would in Chrome, bundled into your app. The system [WebView](./webview) backend delegates to the platform's native web engine (WKWebView on macOS, WebView2 on Windows, WebKitGTK on Linux), so the engine is never bundled and your app stays small. The [Servo](./servo) backend uses Mozilla's experimental Rust-based engine with GPU-accelerated rendering via WebRender; it is still early and the JS bridge is stubbed out.

| | CEF | WebView | Servo | Winit |
|---|---|---|---|---|
| **macOS** | x86_64, aarch64 | x86_64, aarch64 | x86_64, aarch64 | x86_64, aarch64 |
| **Windows** | x86_64 | x86_64 | - | x86_64 |
| **Linux** | x86_64, aarch64 | x86_64, aarch64 | x86_64, aarch64 | x86_64, aarch64 |
| **Android** | - | 🔜 | - | - |

| | Engine | Multi-process | Bundled | JS bridge |
|---|---|---|---|---|
| **CEF** | Chromium 144 | Yes | Yes | Full |
| **WebView** | System native | No | No | Full |
| **Servo** | Servo | No | Yes | Stub |

### Feature support matrix

Features marked with platform qualifiers indicate per-platform differences within a backend.

#### Window management

| Feature | CEF | WebView | Servo | Winit |
|---|---|---|---|---|
| Create / close window | Yes | Yes | Yes | Yes |
| Navigate (load URL) | Yes | Yes | Yes | - |
| Set title | Yes | Yes | Yes | Yes |
| Execute JavaScript | Yes | Yes | Yes | - |
| Set / get window size | Yes | Yes | Yes | Yes |
| Set / get window position | Yes | Yes | Yes | Yes |
| Set / get resizable | - | Yes | Yes | Yes |
| Set / get always on top | Yes | Yes | Yes | Yes |
| Show / hide / is visible | Yes | Yes | Yes | Yes |
| Focus | Yes | Yes | Yes | Yes |
| Quit | Yes | Yes | Yes | Yes |
| Post UI task | Yes | Yes | Yes | Yes |

#### JavaScript interop

| Feature | CEF | WebView | Servo | Winit |
|---|---|---|---|---|
| JS call handler (native bindings) | Yes | Yes | Yes | - |
| Respond to JS calls | Yes | Yes | Yes | - |
| Invoke JS callbacks | Yes | Yes | Yes | - |
| Release JS callbacks | Yes | Yes | Yes | - |

#### Menus

| Feature | CEF | WebView | Servo | Winit |
|---|---|---|---|---|
| Application menu | macOS, Windows | Yes | - | - |
| Context menu | macOS, Windows | Yes | - | - |

CEF on Linux cannot support menus because CEF Views creates raw X11 windows without a GTK container.

#### Native dialogs

| Feature | CEF | WebView | Servo | Winit |
|---|---|---|---|---|
| Alert | Yes | Yes | - | - |
| Confirm | Yes | Yes | - | - |
| Prompt | Yes | Yes | - | - |

#### Input events

| Feature | CEF | WebView | Servo | Winit |
|---|---|---|---|---|
| Keyboard events | Yes | Yes | Yes | Yes |
| Mouse click events | Yes | Yes | Yes | Yes |
| Mouse move events | Yes | Yes | Yes | Yes |
| Wheel / scroll events | Yes | Yes | Yes | Yes |
| Cursor enter / leave | Yes | Yes | Yes | Yes |

#### Window events

| Feature | CEF | WebView | Servo | Winit |
|---|---|---|---|---|
| Focus / blur | Yes | Yes | Yes | Yes |
| Resize | Yes | Yes | Yes | Yes |
| Move | Yes | Yes | Yes | Yes |
| Close requested | Yes | Yes | Yes | Yes |

#### Window handles (GPU surface creation)

| Feature | CEF | WebView | Servo | Winit |
|---|---|---|---|---|
| Get window handle | - | - | - | Yes |
| Get display handle | - | - | - | Yes |
| Get window handle type | - | - | - | Yes |

