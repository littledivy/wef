// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::cell::{Cell, RefCell};
use std::ffi::{c_char, c_void, CStr};
use std::rc::Rc;
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::Mutex;

use euclid::Scale;
use servo::{
    InputEvent, MouseButton as ServoMouseButton, MouseButtonAction, MouseButtonEvent,
    MouseMoveEvent, RenderingContext, Servo, ServoBuilder, WebView, WebViewBuilder, WheelDelta,
    WheelEvent, WheelMode, WindowRenderingContext,
};
use tracing::warn;
use url::Url;
use wef_backend_winit_common::{
    BackendAccess, CommonEvent, CommonState, WefBackendApi, WefJsResultFn,
    define_common_backend_fns, fill_common_api,
};
use webrender_api::units::{DeviceIntPoint, DevicePoint};
use winit::application::ApplicationHandler;
use winit::event::{ElementState, MouseButton, MouseScrollDelta, WindowEvent};
use winit::event_loop::{EventLoop, EventLoopProxy};
use winit::raw_window_handle::{HasDisplayHandle, HasWindowHandle};
use winit::window::Window;

// --- Backend state ---

static BACKEND_STATE: AtomicPtr<BackendState> = AtomicPtr::new(std::ptr::null_mut());

struct BackendState {
    event_proxy: EventLoopProxy<UserEvent>,
    common: CommonState,
    pending_navigate: Mutex<Option<String>>,
    pending_js: Mutex<Vec<(String, Option<WefJsResultFn>, usize)>>,
}

impl BackendAccess for BackendState {
    type Event = UserEvent;

    fn get() -> Option<&'static Self> {
        let ptr = BACKEND_STATE.load(Ordering::Acquire);
        if ptr.is_null() { None } else { Some(unsafe { &*ptr }) }
    }

    fn proxy(&self) -> &EventLoopProxy<UserEvent> {
        &self.event_proxy
    }

    fn common(&self) -> &CommonState {
        &self.common
    }

    fn common_event(event: CommonEvent) -> UserEvent {
        UserEvent::Common(event)
    }
}

// Generate all common backend C ABI functions
define_common_backend_fns!(BackendState);

// --- Servo-specific backend functions ---

unsafe extern "C" fn backend_navigate(_data: *mut c_void, url: *const c_char) {
    if url.is_null() {
        return;
    }
    let url_str = unsafe { CStr::from_ptr(url) }.to_string_lossy().into_owned();
    println!(
        "[Servo] backend_navigate called with: {}",
        &url_str[..url_str.len().min(100)]
    );
    if let Some(state) = BackendState::get() {
        *state.pending_navigate.lock().unwrap() = Some(url_str);
        let _ = state.event_proxy.send_event(UserEvent::Navigate);
    }
}

unsafe extern "C" fn backend_execute_js(
    _data: *mut c_void,
    script: *const c_char,
    callback: Option<WefJsResultFn>,
    callback_data: *mut c_void,
) {
    if script.is_null() {
        return;
    }
    let script_str = unsafe { CStr::from_ptr(script) }.to_string_lossy().into_owned();
    if let Some(state) = BackendState::get() {
        state
            .pending_js
            .lock()
            .unwrap()
            .push((script_str, callback, callback_data as usize));
        let _ = state.event_proxy.send_event(UserEvent::ExecuteJs);
    }
}

// --- API construction ---

fn create_backend_api() -> WefBackendApi {
    let mut api = wef_backend_common::create_api_base();
    fill_common_api!(api);
    api.navigate = Some(backend_navigate);
    api.execute_js = Some(backend_execute_js);
    api
}

// --- Event loop ---

#[derive(Debug)]
enum UserEvent {
    Common(CommonEvent),
    Waker,
    Navigate,
    ExecuteJs,
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

            let state = BackendState::get().expect("BackendState not initialized");
            let attrs = wef_backend_winit_common::apply_pending_attrs(
                &state.common,
                Window::default_attributes(),
            );

            let window = event_loop
                .create_window(attrs)
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

            wef_backend_winit_common::apply_pending_post_create(&state.common, &window);
            wef_backend_winit_common::store_window_handles(&window);

            let app_state = Rc::new(AppState {
                window,
                servo,
                rendering_context,
                webviews: Default::default(),
                mouse_position: Cell::new(DeviceIntPoint::zero()),
            });

            if state.pending_navigate.lock().unwrap().is_some() {
                println!("[Servo] Found pending navigate, re-sending event");
                let _ = state.event_proxy.send_event(UserEvent::Navigate);
            }

            *self = Self::Running(app_state);
        }
    }

    fn user_event(&mut self, event_loop: &winit::event_loop::ActiveEventLoop, event: UserEvent) {
        match event {
            UserEvent::Common(ref common) => {
                if let CommonEvent::Quit = common {
                    event_loop.exit();
                } else if let Self::Running(state) = self {
                    wef_backend_winit_common::handle_common_event::<BackendState>(
                        common,
                        &state.window,
                    );
                }
                return;
            }
            UserEvent::Waker => {
                if let Self::Running(state) = self {
                    state.servo.spin_event_loop();
                }
            }
            UserEvent::Navigate => {
                if let Self::Running(state) = self {
                    if let Some(backend_state) = BackendState::get() {
                        if let Some(url_str) =
                            backend_state.pending_navigate.lock().unwrap().take()
                        {
                            println!(
                                "[Servo] UserEvent::Navigate processing: {}",
                                &url_str[..url_str.len().min(100)]
                            );
                            let url = match Url::parse(&url_str) {
                                Ok(u) => u,
                                Err(e) => {
                                    println!("[Servo] Failed to parse URL: {}", e);
                                    return;
                                }
                            };

                            println!("[Servo] Creating webview with URL");
                            let webview = WebViewBuilder::new(
                                &state.servo,
                                state.rendering_context.clone(),
                            )
                            .url(url)
                            .hidpi_scale_factor(Scale::new(
                                state.window.scale_factor() as f32,
                            ))
                            .delegate(state.clone())
                            .build();

                            state.webviews.borrow_mut().push(webview);
                            state.servo.spin_event_loop();
                            state.window.request_redraw();
                        }
                    }
                }
            }
            UserEvent::ExecuteJs => {
                if let Self::Running(state) = self {
                    if let Some(backend_state) = BackendState::get() {
                        let scripts =
                            std::mem::take(&mut *backend_state.pending_js.lock().unwrap());
                        if let Some(webview) = state.webviews.borrow().last() {
                            for (script, callback, callback_data) in scripts {
                                match callback {
                                    Some(cb) => {
                                        let data = callback_data;
                                        webview.evaluate_javascript(
                                            script,
                                            move |result| match result {
                                                Ok(_js_value) => {
                                                    // TODO: convert JSValue to WefValue
                                                    unsafe {
                                                        cb(
                                                            std::ptr::null_mut(),
                                                            std::ptr::null_mut(),
                                                            data as *mut c_void,
                                                        )
                                                    };
                                                }
                                                Err(_err) => {
                                                    // TODO: convert error to WefValue
                                                    unsafe {
                                                        cb(
                                                            std::ptr::null_mut(),
                                                            std::ptr::null_mut(),
                                                            data as *mut c_void,
                                                        )
                                                    };
                                                }
                                            },
                                        );
                                    }
                                    None => {
                                        webview.evaluate_javascript(script, |_result| {});
                                    }
                                }
                            }
                            state.servo.spin_event_loop();
                        }
                    }
                }
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
                    if let Some(backend_state) = BackendState::get() {
                        *backend_state.common.pending_size.lock().unwrap() =
                            Some((new_size.width as i32, new_size.height as i32));
                    }
                    if let Some(webview) = state.webviews.borrow().last() {
                        webview.resize(new_size);
                    }
                }
            }
            WindowEvent::Moved(position) => {
                if let Some(backend_state) = BackendState::get() {
                    *backend_state.common.pending_position.lock().unwrap() =
                        Some((position.x, position.y));
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
            WindowEvent::MouseInput {
                state: button_state,
                button,
                ..
            } => {
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
                            MouseButtonEvent::new(
                                action,
                                mouse_button,
                                point.to_f32().into(),
                            ),
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

fn main() -> Result<(), Box<dyn std::error::Error>> {
    rustls::crypto::aws_lc_rs::default_provider()
        .install_default()
        .expect("Failed to install crypto provider");

    let event_loop = EventLoop::with_user_event()
        .build()
        .expect("Failed to create EventLoop");

    let backend_state = Box::new(BackendState {
        event_proxy: event_loop.create_proxy(),
        common: CommonState::new(),
        pending_navigate: Mutex::new(None),
        pending_js: Mutex::new(Vec::new()),
    });
    BACKEND_STATE.store(Box::into_raw(backend_state), Ordering::Release);

    wef_backend_winit_common::load_and_start_runtime(create_backend_api());

    let mut app = App::new(&event_loop);
    Ok(event_loop.run_app(&mut app)?)
}
