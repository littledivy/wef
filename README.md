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

TODO


