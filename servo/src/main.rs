// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::cell::{Cell, RefCell};
use std::env;
use std::error::Error;
use std::ffi::{c_char, c_int, c_void, CStr};
use std::path::PathBuf;
use std::rc::Rc;
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::Mutex;
use std::thread;

use euclid::Scale;
use libloading::{Library, Symbol};
use servo::{
    InputEvent, MouseButton as ServoMouseButton, MouseButtonAction, MouseButtonEvent,
    MouseMoveEvent, RenderingContext, Servo, ServoBuilder, WebView, WebViewBuilder, WheelDelta,
    WheelEvent, WheelMode, WindowRenderingContext,
};
use tracing::warn;
use url::Url;
use webrender_api::units::{DevicePoint, DeviceIntPoint};
use winit::application::ApplicationHandler;
use winit::event::{ElementState, MouseButton, MouseScrollDelta, WindowEvent};
use winit::event_loop::{EventLoop, EventLoopProxy};
use winit::raw_window_handle::{HasDisplayHandle, HasWindowHandle};
use winit::window::Window;

const WEF_API_VERSION: u32 = 2;

#[repr(C)]
struct WefValue {
    _opaque: [u8; 0],
}

type WefJsCallFn = unsafe extern "C" fn(*mut c_void, u64, *const c_char, *mut WefValue);

#[repr(C)]
struct WefBackendApi {
    version: u32,
    backend_data: *mut c_void,
    navigate: Option<unsafe extern "C" fn(*mut c_void, *const c_char)>,
    set_title: Option<unsafe extern "C" fn(*mut c_void, *const c_char)>,
    execute_js: Option<unsafe extern "C" fn(*mut c_void, *const c_char)>,
    quit: Option<unsafe extern "C" fn(*mut c_void)>,
    set_window_size: Option<unsafe extern "C" fn(*mut c_void, c_int, c_int)>,
    post_ui_task:
        Option<unsafe extern "C" fn(*mut c_void, Option<unsafe extern "C" fn(*mut c_void)>, *mut c_void)>,
    value_is_null: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    value_is_bool: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    value_is_int: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    value_is_double: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    value_is_string: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    value_is_list: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    value_is_dict: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    value_is_binary: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    value_is_callback: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    value_get_bool: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    value_get_int: Option<unsafe extern "C" fn(*mut WefValue) -> c_int>,
    value_get_double: Option<unsafe extern "C" fn(*mut WefValue) -> f64>,
    value_get_string: Option<unsafe extern "C" fn(*mut WefValue, *mut usize) -> *mut c_char>,
    value_free_string: Option<unsafe extern "C" fn(*mut c_char)>,
    value_list_size: Option<unsafe extern "C" fn(*mut WefValue) -> usize>,
    value_list_get: Option<unsafe extern "C" fn(*mut WefValue, usize) -> *mut WefValue>,
    value_dict_get: Option<unsafe extern "C" fn(*mut WefValue, *const c_char) -> *mut WefValue>,
    value_dict_has: Option<unsafe extern "C" fn(*mut WefValue, *const c_char) -> bool>,
    value_dict_size: Option<unsafe extern "C" fn(*mut WefValue) -> usize>,
    value_dict_keys: Option<unsafe extern "C" fn(*mut WefValue, *mut usize) -> *mut *mut c_char>,
    value_free_keys: Option<unsafe extern "C" fn(*mut *mut c_char, usize)>,
    value_get_binary: Option<unsafe extern "C" fn(*mut WefValue, *mut usize) -> *const c_void>,
    value_get_callback_id: Option<unsafe extern "C" fn(*mut WefValue) -> u64>,
    value_null: Option<unsafe extern "C" fn(*mut c_void) -> *mut WefValue>,
    value_bool: Option<unsafe extern "C" fn(*mut c_void, bool) -> *mut WefValue>,
    value_int: Option<unsafe extern "C" fn(*mut c_void, c_int) -> *mut WefValue>,
    value_double: Option<unsafe extern "C" fn(*mut c_void, f64) -> *mut WefValue>,
    value_string: Option<unsafe extern "C" fn(*mut c_void, *const c_char) -> *mut WefValue>,
    value_list: Option<unsafe extern "C" fn(*mut c_void) -> *mut WefValue>,
    value_dict: Option<unsafe extern "C" fn(*mut c_void) -> *mut WefValue>,
    value_binary: Option<unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> *mut WefValue>,
    value_list_append: Option<unsafe extern "C" fn(*mut WefValue, *mut WefValue) -> bool>,
    value_list_set: Option<unsafe extern "C" fn(*mut WefValue, usize, *mut WefValue) -> bool>,
    value_dict_set: Option<unsafe extern "C" fn(*mut WefValue, *const c_char, *mut WefValue) -> bool>,
    value_free: Option<unsafe extern "C" fn(*mut WefValue)>,
    set_js_call_handler: Option<unsafe extern "C" fn(*mut c_void, WefJsCallFn, *mut c_void)>,
    js_call_respond: Option<unsafe extern "C" fn(*mut c_void, u64, *mut WefValue, *const c_char)>,
    invoke_js_callback: Option<unsafe extern "C" fn(*mut c_void, u64, *mut WefValue)>,
    release_js_callback: Option<unsafe extern "C" fn(*mut c_void, u64)>,
}

type RuntimeInitFn = unsafe extern "C" fn(*const WefBackendApi) -> c_int;
type RuntimeStartFn = unsafe extern "C" fn() -> c_int;
#[allow(dead_code)]
type RuntimeShutdownFn = unsafe extern "C" fn();

static BACKEND_STATE: AtomicPtr<BackendState> = AtomicPtr::new(std::ptr::null_mut());

struct BackendState {
    event_proxy: EventLoopProxy<UserEvent>,
    pending_navigate: Mutex<Option<String>>,
    pending_title: Mutex<Option<String>>,
    pending_js: Mutex<Vec<String>>,
}

impl BackendState {
    fn get() -> Option<&'static Self> {
        let ptr = BACKEND_STATE.load(Ordering::Acquire);
        if ptr.is_null() {
            None
        } else {
            Some(unsafe { &*ptr })
        }
    }
}

unsafe extern "C" fn backend_navigate(_data: *mut c_void, url: *const c_char) {
    if url.is_null() {
        return;
    }
    let url_str = unsafe { CStr::from_ptr(url) }.to_string_lossy().into_owned();
    println!("[Servo] backend_navigate called with: {}", &url_str[..url_str.len().min(100)]);
    if let Some(state) = BackendState::get() {
        *state.pending_navigate.lock().unwrap() = Some(url_str);
        let _ = state.event_proxy.send_event(UserEvent::Navigate);
    }
}

unsafe extern "C" fn backend_set_title(_data: *mut c_void, title: *const c_char) {
    if title.is_null() {
        return;
    }
    let title_str = unsafe { CStr::from_ptr(title) }.to_string_lossy().into_owned();
    if let Some(state) = BackendState::get() {
        *state.pending_title.lock().unwrap() = Some(title_str);
        let _ = state.event_proxy.send_event(UserEvent::SetTitle);
    }
}

unsafe extern "C" fn backend_execute_js(_data: *mut c_void, script: *const c_char) {
    if script.is_null() {
        return;
    }
    let script_str = unsafe { CStr::from_ptr(script) }.to_string_lossy().into_owned();
    if let Some(state) = BackendState::get() {
        state.pending_js.lock().unwrap().push(script_str);
        let _ = state.event_proxy.send_event(UserEvent::ExecuteJs);
    }
}

unsafe extern "C" fn backend_quit(_data: *mut c_void) {
    if let Some(state) = BackendState::get() {
        let _ = state.event_proxy.send_event(UserEvent::Quit);
    }
}

unsafe extern "C" fn backend_set_window_size(_data: *mut c_void, _width: c_int, _height: c_int) {
}

unsafe extern "C" fn backend_post_ui_task(
    _data: *mut c_void,
    task: Option<unsafe extern "C" fn(*mut c_void)>,
    task_data: *mut c_void,
) {
    if let Some(task_fn) = task {
        if let Some(state) = BackendState::get() {
            let _ = state.event_proxy.send_event(UserEvent::UiTask {
                task: task_fn,
                data: task_data as usize,
            });
        }
    }
}

unsafe extern "C" fn value_is_null(_val: *mut WefValue) -> bool { true }
unsafe extern "C" fn value_is_bool(_val: *mut WefValue) -> bool { false }
unsafe extern "C" fn value_is_int(_val: *mut WefValue) -> bool { false }
unsafe extern "C" fn value_is_double(_val: *mut WefValue) -> bool { false }
unsafe extern "C" fn value_is_string(_val: *mut WefValue) -> bool { false }
unsafe extern "C" fn value_is_list(_val: *mut WefValue) -> bool { false }
unsafe extern "C" fn value_is_dict(_val: *mut WefValue) -> bool { false }
unsafe extern "C" fn value_is_binary(_val: *mut WefValue) -> bool { false }
unsafe extern "C" fn value_is_callback(_val: *mut WefValue) -> bool { false }
unsafe extern "C" fn value_get_bool(_val: *mut WefValue) -> bool { false }
unsafe extern "C" fn value_get_int(_val: *mut WefValue) -> c_int { 0 }
unsafe extern "C" fn value_get_double(_val: *mut WefValue) -> f64 { 0.0 }
unsafe extern "C" fn value_get_string(_val: *mut WefValue, len: *mut usize) -> *mut c_char {
    if !len.is_null() { unsafe { *len = 0; } }
    std::ptr::null_mut()
}
unsafe extern "C" fn value_free_string(_s: *mut c_char) {}
unsafe extern "C" fn value_list_size(_val: *mut WefValue) -> usize { 0 }
unsafe extern "C" fn value_list_get(_val: *mut WefValue, _idx: usize) -> *mut WefValue { std::ptr::null_mut() }
unsafe extern "C" fn value_dict_get(_val: *mut WefValue, _key: *const c_char) -> *mut WefValue { std::ptr::null_mut() }
unsafe extern "C" fn value_dict_has(_val: *mut WefValue, _key: *const c_char) -> bool { false }
unsafe extern "C" fn value_dict_size(_val: *mut WefValue) -> usize { 0 }
unsafe extern "C" fn value_dict_keys(_val: *mut WefValue, count: *mut usize) -> *mut *mut c_char {
    if !count.is_null() { unsafe { *count = 0; } }
    std::ptr::null_mut()
}
unsafe extern "C" fn value_free_keys(_keys: *mut *mut c_char, _count: usize) {}
unsafe extern "C" fn value_get_binary(_val: *mut WefValue, len: *mut usize) -> *const c_void {
    if !len.is_null() { unsafe { *len = 0; } }
    std::ptr::null()
}
unsafe extern "C" fn value_get_callback_id(_val: *mut WefValue) -> u64 { 0 }
unsafe extern "C" fn value_null(_data: *mut c_void) -> *mut WefValue { std::ptr::null_mut() }
unsafe extern "C" fn value_bool(_data: *mut c_void, _v: bool) -> *mut WefValue { std::ptr::null_mut() }
unsafe extern "C" fn value_int(_data: *mut c_void, _v: c_int) -> *mut WefValue { std::ptr::null_mut() }
unsafe extern "C" fn value_double(_data: *mut c_void, _v: f64) -> *mut WefValue { std::ptr::null_mut() }
unsafe extern "C" fn value_string(_data: *mut c_void, _v: *const c_char) -> *mut WefValue { std::ptr::null_mut() }
unsafe extern "C" fn value_list(_data: *mut c_void) -> *mut WefValue { std::ptr::null_mut() }
unsafe extern "C" fn value_dict(_data: *mut c_void) -> *mut WefValue { std::ptr::null_mut() }
unsafe extern "C" fn value_binary(_data: *mut c_void, _d: *const c_void, _len: usize) -> *mut WefValue { std::ptr::null_mut() }
unsafe extern "C" fn value_list_append(_list: *mut WefValue, _val: *mut WefValue) -> bool { false }
unsafe extern "C" fn value_list_set(_list: *mut WefValue, _idx: usize, _val: *mut WefValue) -> bool { false }
unsafe extern "C" fn value_dict_set(_dict: *mut WefValue, _key: *const c_char, _val: *mut WefValue) -> bool { false }
unsafe extern "C" fn value_free(_val: *mut WefValue) {}
unsafe extern "C" fn set_js_call_handler(_data: *mut c_void, _handler: WefJsCallFn, _user_data: *mut c_void) {}
unsafe extern "C" fn js_call_respond(_data: *mut c_void, _call_id: u64, _result: *mut WefValue, _error: *const c_char) {}
unsafe extern "C" fn invoke_js_callback(_data: *mut c_void, _cb_id: u64, _args: *mut WefValue) {}
unsafe extern "C" fn release_js_callback(_data: *mut c_void, _cb_id: u64) {}

fn create_backend_api() -> WefBackendApi {
    WefBackendApi {
        version: WEF_API_VERSION,
        backend_data: std::ptr::null_mut(),
        navigate: Some(backend_navigate),
        set_title: Some(backend_set_title),
        execute_js: Some(backend_execute_js),
        quit: Some(backend_quit),
        set_window_size: Some(backend_set_window_size),
        post_ui_task: Some(backend_post_ui_task),
        value_is_null: Some(value_is_null),
        value_is_bool: Some(value_is_bool),
        value_is_int: Some(value_is_int),
        value_is_double: Some(value_is_double),
        value_is_string: Some(value_is_string),
        value_is_list: Some(value_is_list),
        value_is_dict: Some(value_is_dict),
        value_is_binary: Some(value_is_binary),
        value_is_callback: Some(value_is_callback),
        value_get_bool: Some(value_get_bool),
        value_get_int: Some(value_get_int),
        value_get_double: Some(value_get_double),
        value_get_string: Some(value_get_string),
        value_free_string: Some(value_free_string),
        value_list_size: Some(value_list_size),
        value_list_get: Some(value_list_get),
        value_dict_get: Some(value_dict_get),
        value_dict_has: Some(value_dict_has),
        value_dict_size: Some(value_dict_size),
        value_dict_keys: Some(value_dict_keys),
        value_free_keys: Some(value_free_keys),
        value_get_binary: Some(value_get_binary),
        value_get_callback_id: Some(value_get_callback_id),
        value_null: Some(value_null),
        value_bool: Some(value_bool),
        value_int: Some(value_int),
        value_double: Some(value_double),
        value_string: Some(value_string),
        value_list: Some(value_list),
        value_dict: Some(value_dict),
        value_binary: Some(value_binary),
        value_list_append: Some(value_list_append),
        value_list_set: Some(value_list_set),
        value_dict_set: Some(value_dict_set),
        value_free: Some(value_free),
        set_js_call_handler: Some(set_js_call_handler),
        js_call_respond: Some(js_call_respond),
        invoke_js_callback: Some(invoke_js_callback),
        release_js_callback: Some(release_js_callback),
    }
}

fn find_runtime_library() -> Option<PathBuf> {
    if let Ok(path) = env::var("WEF_RUNTIME_PATH") {
        let p = PathBuf::from(path);
        if p.exists() {
            return Some(p);
        }
    }

    let search_paths = [
        "./libruntime.dylib",
        "./libruntime.so",
        "./target/debug/libhello.dylib",
        "./target/release/libhello.dylib",
        "./target/debug/libhello.so",
        "./target/release/libhello.so",
    ];

    for path in &search_paths {
        let p = PathBuf::from(path);
        if p.exists() {
            return Some(p);
        }
    }

    None
}

fn main() -> Result<(), Box<dyn Error>> {
    rustls::crypto::aws_lc_rs::default_provider()
        .install_default()
        .expect("Failed to install crypto provider");

    let event_loop = EventLoop::with_user_event()
        .build()
        .expect("Failed to create EventLoop");

    let backend_state = Box::new(BackendState {
        event_proxy: event_loop.create_proxy(),
        pending_navigate: Mutex::new(None),
        pending_title: Mutex::new(None),
        pending_js: Mutex::new(Vec::new()),
    });
    BACKEND_STATE.store(Box::into_raw(backend_state), Ordering::Release);

    let runtime_path = find_runtime_library();
    if let Some(path) = runtime_path {
        println!("Loading runtime from: {}", path.display());

        thread::spawn(move || {
            unsafe {
                let lib = match Library::new(&path) {
                    Ok(l) => l,
                    Err(e) => {
                        eprintln!("Failed to load runtime: {}", e);
                        return;
                    }
                };

                let init: Symbol<RuntimeInitFn> = match lib.get(b"wef_runtime_init\0") {
                    Ok(f) => f,
                    Err(e) => {
                        eprintln!("Failed to find wef_runtime_init: {}", e);
                        return;
                    }
                };

                let start: Symbol<RuntimeStartFn> = match lib.get(b"wef_runtime_start\0") {
                    Ok(f) => f,
                    Err(e) => {
                        eprintln!("Failed to find wef_runtime_start: {}", e);
                        return;
                    }
                };

                let api = create_backend_api();
                let result = init(&api);
                if result != 0 {
                    eprintln!("Runtime init failed with code: {}", result);
                    return;
                }

                println!("Runtime initialized, starting...");
                let result = start();
                if result != 0 {
                    eprintln!("Runtime start failed with code: {}", result);
                }

                std::mem::forget(lib);
            }
        });
    } else {
        println!("No runtime library found. Set WEF_RUNTIME_PATH or place libruntime in current directory.");
        println!("Starting Servo without runtime integration...");
    }

    let mut app = App::new(&event_loop);
    Ok(event_loop.run_app(&mut app)?)
}

#[derive(Debug)]
enum UserEvent {
    Waker,
    Navigate,
    SetTitle,
    ExecuteJs,
    Quit,
    UiTask { task: unsafe extern "C" fn(*mut c_void), data: usize },
}

struct AppState {
    window: Window,
    servo: Servo,
    rendering_context: Rc<WindowRenderingContext>,
    webviews: RefCell<Vec<WebView>>,
    mouse_position: Cell<DeviceIntPoint>,
}

impl servo::WebViewDelegate for AppState {
    fn notify_new_frame_ready(&self, _webview: WebView) {
        self.window.request_redraw();
    }
}

enum App {
    Initial(Waker),
    Running(Rc<AppState>),
}

impl App {
    fn new(event_loop: &EventLoop<UserEvent>) -> Self {
        Self::Initial(Waker::new(event_loop))
    }
}

impl ApplicationHandler<UserEvent> for App {
    fn resumed(&mut self, event_loop: &winit::event_loop::ActiveEventLoop) {
        if let Self::Initial(waker) = self {
            let display_handle = event_loop
                .display_handle()
                .expect("Failed to get display handle");
            let window = event_loop
                .create_window(Window::default_attributes())
                .expect("Failed to create winit Window");
            let window_handle = window.window_handle().expect("Failed to get window handle");

            let rendering_context = Rc::new(
                WindowRenderingContext::new(display_handle, window_handle, window.inner_size())
                    .expect("Could not create RenderingContext for window."),
            );

            let _ = rendering_context.make_current();

            let servo = ServoBuilder::default()
                .event_loop_waker(Box::new(waker.clone()))
                .build();
            servo.setup_logging();

            let app_state = Rc::new(AppState {
                window,
                servo,
                rendering_context,
                webviews: Default::default(),
                mouse_position: Cell::new(DeviceIntPoint::zero()),
            });

            if let Some(backend_state) = BackendState::get() {
                if backend_state.pending_navigate.lock().unwrap().is_some() {
                    println!("[Servo] Found pending navigate, re-sending event");
                    let _ = backend_state.event_proxy.send_event(UserEvent::Navigate);
                }
                if backend_state.pending_title.lock().unwrap().is_some() {
                    let _ = backend_state.event_proxy.send_event(UserEvent::SetTitle);
                }
            }

            *self = Self::Running(app_state);
        }
    }

    fn user_event(&mut self, event_loop: &winit::event_loop::ActiveEventLoop, event: UserEvent) {
        match event {
            UserEvent::Waker => {
                if let Self::Running(state) = self {
                    state.servo.spin_event_loop();
                }
            }
            UserEvent::Navigate => {
                if let Self::Running(state) = self {
                    if let Some(backend_state) = BackendState::get() {
                        if let Some(url_str) = backend_state.pending_navigate.lock().unwrap().take() {
                            println!("[Servo] UserEvent::Navigate processing: {}", &url_str[..url_str.len().min(100)]);
                            let url = match Url::parse(&url_str) {
                                Ok(u) => u,
                                Err(e) => {
                                    println!("[Servo] Failed to parse URL: {}", e);
                                    return;
                                }
                            };

                            println!("[Servo] Creating webview with URL");
                            let webview = WebViewBuilder::new(&state.servo, state.rendering_context.clone())
                                .url(url)
                                .hidpi_scale_factor(Scale::new(state.window.scale_factor() as f32))
                                .delegate(state.clone())
                                .build();

                            state.webviews.borrow_mut().push(webview);
                            state.servo.spin_event_loop();
                            state.window.request_redraw();
                        }
                    }
                }
            }
            UserEvent::SetTitle => {
                if let Self::Running(state) = self {
                    if let Some(backend_state) = BackendState::get() {
                        if let Some(title) = backend_state.pending_title.lock().unwrap().take() {
                            state.window.set_title(&title);
                        }
                    }
                }
            }
            UserEvent::ExecuteJs => {
                if let Some(backend_state) = BackendState::get() {
                    let _scripts = std::mem::take(&mut *backend_state.pending_js.lock().unwrap());
                }
            }
            UserEvent::Quit => {
                event_loop.exit();
            }
            UserEvent::UiTask { task, data } => {
                unsafe { task(data as *mut c_void) };
            }
        }
    }

    fn window_event(
        &mut self,
        event_loop: &winit::event_loop::ActiveEventLoop,
        _window_id: winit::window::WindowId,
        event: WindowEvent,
    ) {
        if let Self::Running(state) = self {
            state.servo.spin_event_loop();
        }

        match event {
            WindowEvent::CloseRequested => {
                event_loop.exit();
            }
            WindowEvent::RedrawRequested => {
                if let Self::Running(state) = self {
                    state.webviews.borrow().last().unwrap().paint();
                    state.rendering_context.present();
                }
            }
            WindowEvent::MouseWheel { delta, .. } => {
                if let Self::Running(state) = self {
                    if let Some(webview) = state.webviews.borrow().last() {
                        let (delta_x, delta_y, mode) = match delta {
                            MouseScrollDelta::LineDelta(dx, dy) => {
                                ((dx * 76.0) as f64, (dy * 76.0) as f64, WheelMode::DeltaLine)
                            }
                            MouseScrollDelta::PixelDelta(delta) => {
                                (delta.x, delta.y, WheelMode::DeltaPixel)
                            }
                        };

                        webview.notify_input_event(InputEvent::Wheel(WheelEvent::new(
                            WheelDelta {
                                x: delta_x,
                                y: delta_y,
                                z: 0.0,
                                mode,
                            },
                            DevicePoint::default().into(),
                        )));
                    }
                }
            }
            WindowEvent::Resized(new_size) => {
                if let Self::Running(state) = self {
                    if let Some(webview) = state.webviews.borrow().last() {
                        webview.resize(new_size);
                    }
                }
            }
            WindowEvent::CursorMoved { position, .. } => {
                if let Self::Running(state) = self {
                    let point = DeviceIntPoint::new(position.x as i32, position.y as i32);
                    state.mouse_position.set(point);
                    if let Some(webview) = state.webviews.borrow().last() {
                        webview.notify_input_event(InputEvent::MouseMove(
                            MouseMoveEvent::new(point.to_f32().into()),
                        ));
                    }
                }
            }
            WindowEvent::MouseInput { state: button_state, button, .. } => {
                if let Self::Running(state) = self {
                    if let Some(webview) = state.webviews.borrow().last() {
                        let mouse_button = match button {
                            MouseButton::Left => ServoMouseButton::Left,
                            MouseButton::Right => ServoMouseButton::Right,
                            MouseButton::Middle => ServoMouseButton::Middle,
                            MouseButton::Back => ServoMouseButton::Back,
                            MouseButton::Forward => ServoMouseButton::Forward,
                            MouseButton::Other(v) => ServoMouseButton::Other(v),
                        };
                        let action = match button_state {
                            ElementState::Pressed => MouseButtonAction::Down,
                            ElementState::Released => MouseButtonAction::Up,
                        };
                        let point = state.mouse_position.get();
                        webview.notify_input_event(InputEvent::MouseButton(
                            MouseButtonEvent::new(action, mouse_button, point.to_f32().into()),
                        ));
                    }
                }
            }
            _ => (),
        }
    }
}

#[derive(Clone)]
struct Waker(EventLoopProxy<UserEvent>);

impl Waker {
    fn new(event_loop: &EventLoop<UserEvent>) -> Self {
        Self(event_loop.create_proxy())
    }
}

impl embedder_traits::EventLoopWaker for Waker {
    fn clone_box(&self) -> Box<dyn embedder_traits::EventLoopWaker> {
        Box::new(Self(self.0.clone()))
    }

    fn wake(&self) {
        if let Err(error) = self.0.send_event(UserEvent::Waker) {
            warn!(?error, "Failed to wake event loop");
        }
    }
}
