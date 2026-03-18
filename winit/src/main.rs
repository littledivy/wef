// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::error::Error;
use std::ffi::c_void;
use std::sync::atomic::{AtomicPtr, Ordering};

use wef_backend_winit_common::{
    BackendAccess, CommonEvent, CommonState, WefBackendApi, WefJsResultFn,
    define_common_backend_fns, fill_common_api,
    winit,
};
use winit::application::ApplicationHandler;
use winit::dpi::{PhysicalPosition, PhysicalSize};
use winit::event::WindowEvent;
use winit::event_loop::{EventLoop, EventLoopProxy};
use winit::keyboard::ModifiersState;
use winit::window::Window;

// --- Backend state ---

static BACKEND_STATE: AtomicPtr<BackendState> = AtomicPtr::new(std::ptr::null_mut());

struct BackendState {
    event_proxy: EventLoopProxy<UserEvent>,
    common: CommonState,
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

// --- Backend-specific functions ---

unsafe extern "C" fn backend_navigate(
    _data: *mut c_void,
    _url: *const std::ffi::c_char,
) {
    // no-op — no web engine
}

unsafe extern "C" fn backend_execute_js(
    _data: *mut c_void,
    _script: *const std::ffi::c_char,
    _callback: Option<WefJsResultFn>,
    _callback_data: *mut c_void,
) {
    // no-op — no web engine
}

// --- API construction ---

fn create_backend_api() -> WefBackendApi {
    let mut api = wef_backend_winit_common::create_api_base();
    fill_common_api!(api);
    api.navigate = Some(backend_navigate);
    api.execute_js = Some(backend_execute_js);
    api
}

// --- Event loop ---

#[derive(Debug)]
enum UserEvent {
    Common(CommonEvent),
}

enum App {
    Initial,
    Running {
        window: Window,
        modifiers: ModifiersState,
    },
}

impl App {
    fn new() -> Self {
        Self::Initial
    }
}

impl ApplicationHandler<UserEvent> for App {
    fn resumed(&mut self, event_loop: &winit::event_loop::ActiveEventLoop) {
        if let Self::Initial = self {
            let state = BackendState::get().expect("BackendState not initialized");
            let attrs = wef_backend_winit_common::apply_pending_attrs(
                &state.common,
                Window::default_attributes(),
            );

            let window = event_loop
                .create_window(attrs)
                .expect("Failed to create winit Window");

            wef_backend_winit_common::apply_pending_post_create(&state.common, &window);
            wef_backend_winit_common::store_window_handles(&window);

            *self = Self::Running {
                window,
                modifiers: ModifiersState::default(),
            };
        }
    }

    fn user_event(&mut self, event_loop: &winit::event_loop::ActiveEventLoop, event: UserEvent) {
        let window = match self {
            Self::Running { window, .. } => window,
            Self::Initial => return,
        };

        match event {
            UserEvent::Common(ref common) => {
                if let CommonEvent::Quit = common {
                    event_loop.exit();
                } else {
                    wef_backend_winit_common::handle_common_event::<BackendState>(common, window);
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
        let modifiers = match self {
            Self::Running { modifiers, .. } => modifiers,
            Self::Initial => return,
        };

        match event {
            WindowEvent::CloseRequested => {
                event_loop.exit();
            }
            WindowEvent::Resized(PhysicalSize { width, height }) => {
                if let Some(state) = BackendState::get() {
                    *state.common.pending_size.lock().unwrap() =
                        Some((width as i32, height as i32));
                }
            }
            WindowEvent::Moved(PhysicalPosition { x, y }) => {
                if let Some(state) = BackendState::get() {
                    *state.common.pending_position.lock().unwrap() = Some((x, y));
                }
            }
            WindowEvent::ModifiersChanged(new_modifiers) => {
                *modifiers = new_modifiers.state();
            }
            WindowEvent::KeyboardInput {
                event: ref key_event,
                ..
            } => {
                if let Some(state) = BackendState::get() {
                    wef_backend_winit_common::dispatch_keyboard_event(
                        &state.common,
                        key_event,
                        *modifiers,
                    );
                }
            }
            WindowEvent::CursorMoved { position, .. } => {
                if let Some(state) = BackendState::get() {
                    *state.common.cursor_position.lock().unwrap() =
                        (position.x, position.y);
                    wef_backend_winit_common::dispatch_mouse_move_event(
                        &state.common,
                        position.x,
                        position.y,
                        *modifiers,
                    );
                }
            }
            WindowEvent::MouseInput {
                state: button_state,
                button,
                ..
            } => {
                if let Some(state) = BackendState::get() {
                    wef_backend_winit_common::dispatch_mouse_click_event(
                        &state.common,
                        button_state,
                        button,
                        *modifiers,
                    );
                }
            }
            _ => {}
        }
    }
}

fn main() -> Result<(), Box<dyn Error>> {
    let event_loop = EventLoop::with_user_event()
        .build()
        .expect("Failed to create EventLoop");

    let backend_state = Box::new(BackendState {
        event_proxy: event_loop.create_proxy(),
        common: CommonState::new(),
    });
    BACKEND_STATE.store(Box::into_raw(backend_state), Ordering::Release);

    wef_backend_winit_common::load_and_start_runtime(create_backend_api());

    let mut app = App::new();
    Ok(event_loop.run_app(&mut app)?)
}
