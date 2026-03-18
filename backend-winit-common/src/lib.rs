// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

pub use winit;

use std::env;
use std::ffi::{c_char, c_int, c_void};
use std::path::PathBuf;
use std::sync::atomic::{AtomicI32, AtomicPtr, Ordering};
use std::sync::Mutex;
use std::thread;

use libloading::{Library, Symbol};
#[allow(unused_imports)]
use raw_window_handle::{HasDisplayHandle, HasWindowHandle, RawDisplayHandle, RawWindowHandle};
use winit::dpi::{LogicalPosition, LogicalSize};
use winit::event_loop::EventLoopProxy;
use winit::window::{Window, WindowLevel};

// --- Constants ---

pub const WEF_API_VERSION: u32 = 6;

#[allow(dead_code)]
pub const WEF_WINDOW_HANDLE_UNKNOWN: c_int = 0;
#[allow(dead_code)]
pub const WEF_WINDOW_HANDLE_APPKIT: c_int = 1;
#[allow(dead_code)]
pub const WEF_WINDOW_HANDLE_WIN32: c_int = 2;
#[allow(dead_code)]
pub const WEF_WINDOW_HANDLE_X11: c_int = 3;
#[allow(dead_code)]
pub const WEF_WINDOW_HANDLE_WAYLAND: c_int = 4;

// --- FFI Types ---

#[repr(C)]
pub struct WefValue {
    _opaque: [u8; 0],
}

pub type WefJsCallFn = unsafe extern "C" fn(*mut c_void, u64, *const c_char, *mut WefValue);
pub type WefJsResultFn = unsafe extern "C" fn(*mut WefValue, *mut WefValue, *mut c_void);
pub type WefKeyboardEventFn = unsafe extern "C" fn(
    *mut c_void,   // user_data
    c_int,         // state (0=pressed, 1=released)
    *const c_char, // key
    *const c_char, // code
    u32,           // modifiers
    bool,          // repeat
);
pub type WefMouseClickFn = unsafe extern "C" fn(
    *mut c_void, // user_data
    c_int,       // state (0=pressed, 1=released)
    c_int,       // button
    f64,         // x
    f64,         // y
    u32,         // modifiers
    i32,         // click_count
);

#[repr(C)]
pub struct WefBackendApi {
    pub version: u32,
    pub backend_data: *mut c_void,
    pub navigate: Option<unsafe extern "C" fn(*mut c_void, *const c_char)>,
    pub set_title: Option<unsafe extern "C" fn(*mut c_void, *const c_char)>,
    pub execute_js: Option<
        unsafe extern "C" fn(*mut c_void, *const c_char, Option<WefJsResultFn>, *mut c_void),
    >,
    pub quit: Option<unsafe extern "C" fn(*mut c_void)>,
    pub set_window_size: Option<unsafe extern "C" fn(*mut c_void, c_int, c_int)>,
    pub get_window_size: Option<unsafe extern "C" fn(*mut c_void, *mut c_int, *mut c_int)>,
    pub set_window_position: Option<unsafe extern "C" fn(*mut c_void, c_int, c_int)>,
    pub get_window_position: Option<unsafe extern "C" fn(*mut c_void, *mut c_int, *mut c_int)>,
    pub set_resizable: Option<unsafe extern "C" fn(*mut c_void, bool)>,
    pub is_resizable: Option<unsafe extern "C" fn(*mut c_void) -> bool>,
    pub set_always_on_top: Option<unsafe extern "C" fn(*mut c_void, bool)>,
    pub is_always_on_top: Option<unsafe extern "C" fn(*mut c_void) -> bool>,
    pub is_visible: Option<unsafe extern "C" fn(*mut c_void) -> bool>,
    pub show: Option<unsafe extern "C" fn(*mut c_void)>,
    pub hide: Option<unsafe extern "C" fn(*mut c_void)>,
    pub focus: Option<unsafe extern "C" fn(*mut c_void)>,
    pub post_ui_task: Option<
        unsafe extern "C" fn(
            *mut c_void,
            Option<unsafe extern "C" fn(*mut c_void)>,
            *mut c_void,
        ),
    >,
    pub value_is_null: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    pub value_is_bool: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    pub value_is_int: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    pub value_is_double: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    pub value_is_string: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    pub value_is_list: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    pub value_is_dict: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    pub value_is_binary: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    pub value_is_callback: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    pub value_get_bool: Option<unsafe extern "C" fn(*mut WefValue) -> bool>,
    pub value_get_int: Option<unsafe extern "C" fn(*mut WefValue) -> c_int>,
    pub value_get_double: Option<unsafe extern "C" fn(*mut WefValue) -> f64>,
    pub value_get_string:
        Option<unsafe extern "C" fn(*mut WefValue, *mut usize) -> *mut c_char>,
    pub value_free_string: Option<unsafe extern "C" fn(*mut c_char)>,
    pub value_list_size: Option<unsafe extern "C" fn(*mut WefValue) -> usize>,
    pub value_list_get: Option<unsafe extern "C" fn(*mut WefValue, usize) -> *mut WefValue>,
    pub value_dict_get:
        Option<unsafe extern "C" fn(*mut WefValue, *const c_char) -> *mut WefValue>,
    pub value_dict_has: Option<unsafe extern "C" fn(*mut WefValue, *const c_char) -> bool>,
    pub value_dict_size: Option<unsafe extern "C" fn(*mut WefValue) -> usize>,
    pub value_dict_keys:
        Option<unsafe extern "C" fn(*mut WefValue, *mut usize) -> *mut *mut c_char>,
    pub value_free_keys: Option<unsafe extern "C" fn(*mut *mut c_char, usize)>,
    pub value_get_binary:
        Option<unsafe extern "C" fn(*mut WefValue, *mut usize) -> *const c_void>,
    pub value_get_callback_id: Option<unsafe extern "C" fn(*mut WefValue) -> u64>,
    pub value_null: Option<unsafe extern "C" fn(*mut c_void) -> *mut WefValue>,
    pub value_bool: Option<unsafe extern "C" fn(*mut c_void, bool) -> *mut WefValue>,
    pub value_int: Option<unsafe extern "C" fn(*mut c_void, c_int) -> *mut WefValue>,
    pub value_double: Option<unsafe extern "C" fn(*mut c_void, f64) -> *mut WefValue>,
    pub value_string: Option<unsafe extern "C" fn(*mut c_void, *const c_char) -> *mut WefValue>,
    pub value_list: Option<unsafe extern "C" fn(*mut c_void) -> *mut WefValue>,
    pub value_dict: Option<unsafe extern "C" fn(*mut c_void) -> *mut WefValue>,
    pub value_binary:
        Option<unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> *mut WefValue>,
    pub value_list_append: Option<unsafe extern "C" fn(*mut WefValue, *mut WefValue) -> bool>,
    pub value_list_set:
        Option<unsafe extern "C" fn(*mut WefValue, usize, *mut WefValue) -> bool>,
    pub value_dict_set:
        Option<unsafe extern "C" fn(*mut WefValue, *const c_char, *mut WefValue) -> bool>,
    pub value_free: Option<unsafe extern "C" fn(*mut WefValue)>,
    pub set_js_call_handler:
        Option<unsafe extern "C" fn(*mut c_void, WefJsCallFn, *mut c_void)>,
    pub js_call_respond:
        Option<unsafe extern "C" fn(*mut c_void, u64, *mut WefValue, *mut WefValue)>,
    pub invoke_js_callback: Option<unsafe extern "C" fn(*mut c_void, u64, *mut WefValue)>,
    pub release_js_callback: Option<unsafe extern "C" fn(*mut c_void, u64)>,
    pub get_window_handle: Option<unsafe extern "C" fn(*mut c_void) -> *mut c_void>,
    pub get_display_handle: Option<unsafe extern "C" fn(*mut c_void) -> *mut c_void>,
    pub get_window_handle_type: Option<unsafe extern "C" fn(*mut c_void) -> c_int>,
    pub set_keyboard_event_handler:
        Option<unsafe extern "C" fn(*mut c_void, Option<WefKeyboardEventFn>, *mut c_void)>,
    pub set_mouse_click_handler:
        Option<unsafe extern "C" fn(*mut c_void, Option<WefMouseClickFn>, *mut c_void)>,
    pub poll_js_calls: Option<unsafe extern "C" fn(*mut c_void)>,
    pub set_js_call_notify: Option<
        unsafe extern "C" fn(
            *mut c_void,
            Option<unsafe extern "C" fn(*mut c_void)>,
            *mut c_void,
        ),
    >,
    pub set_application_menu: Option<
        unsafe extern "C" fn(
            *mut c_void,
            *mut WefValue,
            Option<unsafe extern "C" fn(*mut c_void, *const c_char)>,
            *mut c_void,
        ),
    >,
}

unsafe impl Send for WefBackendApi {}

pub type RuntimeInitFn = unsafe extern "C" fn(*const WefBackendApi) -> c_int;
pub type RuntimeStartFn = unsafe extern "C" fn() -> c_int;
#[allow(dead_code)]
pub type RuntimeShutdownFn = unsafe extern "C" fn();

// --- Value stub functions ---

pub unsafe extern "C" fn value_is_null(_val: *mut WefValue) -> bool { true }
pub unsafe extern "C" fn value_is_bool(_val: *mut WefValue) -> bool { false }
pub unsafe extern "C" fn value_is_int(_val: *mut WefValue) -> bool { false }
pub unsafe extern "C" fn value_is_double(_val: *mut WefValue) -> bool { false }
pub unsafe extern "C" fn value_is_string(_val: *mut WefValue) -> bool { false }
pub unsafe extern "C" fn value_is_list(_val: *mut WefValue) -> bool { false }
pub unsafe extern "C" fn value_is_dict(_val: *mut WefValue) -> bool { false }
pub unsafe extern "C" fn value_is_binary(_val: *mut WefValue) -> bool { false }
pub unsafe extern "C" fn value_is_callback(_val: *mut WefValue) -> bool { false }
pub unsafe extern "C" fn value_get_bool(_val: *mut WefValue) -> bool { false }
pub unsafe extern "C" fn value_get_int(_val: *mut WefValue) -> c_int { 0 }
pub unsafe extern "C" fn value_get_double(_val: *mut WefValue) -> f64 { 0.0 }
pub unsafe extern "C" fn value_get_string(
    _val: *mut WefValue,
    len: *mut usize,
) -> *mut c_char {
    if !len.is_null() {
        unsafe { *len = 0 };
    }
    std::ptr::null_mut()
}
pub unsafe extern "C" fn value_free_string(_s: *mut c_char) {}
pub unsafe extern "C" fn value_list_size(_val: *mut WefValue) -> usize { 0 }
pub unsafe extern "C" fn value_list_get(
    _val: *mut WefValue,
    _idx: usize,
) -> *mut WefValue {
    std::ptr::null_mut()
}
pub unsafe extern "C" fn value_dict_get(
    _val: *mut WefValue,
    _key: *const c_char,
) -> *mut WefValue {
    std::ptr::null_mut()
}
pub unsafe extern "C" fn value_dict_has(
    _val: *mut WefValue,
    _key: *const c_char,
) -> bool {
    false
}
pub unsafe extern "C" fn value_dict_size(_val: *mut WefValue) -> usize { 0 }
pub unsafe extern "C" fn value_dict_keys(
    _val: *mut WefValue,
    count: *mut usize,
) -> *mut *mut c_char {
    if !count.is_null() {
        unsafe { *count = 0 };
    }
    std::ptr::null_mut()
}
pub unsafe extern "C" fn value_free_keys(_keys: *mut *mut c_char, _count: usize) {}
pub unsafe extern "C" fn value_get_binary(
    _val: *mut WefValue,
    len: *mut usize,
) -> *const c_void {
    if !len.is_null() {
        unsafe { *len = 0 };
    }
    std::ptr::null()
}
pub unsafe extern "C" fn value_get_callback_id(_val: *mut WefValue) -> u64 { 0 }
pub unsafe extern "C" fn value_null(_data: *mut c_void) -> *mut WefValue {
    std::ptr::null_mut()
}
pub unsafe extern "C" fn value_bool(_data: *mut c_void, _v: bool) -> *mut WefValue {
    std::ptr::null_mut()
}
pub unsafe extern "C" fn value_int(_data: *mut c_void, _v: c_int) -> *mut WefValue {
    std::ptr::null_mut()
}
pub unsafe extern "C" fn value_double(_data: *mut c_void, _v: f64) -> *mut WefValue {
    std::ptr::null_mut()
}
pub unsafe extern "C" fn value_string(
    _data: *mut c_void,
    _v: *const c_char,
) -> *mut WefValue {
    std::ptr::null_mut()
}
pub unsafe extern "C" fn value_list(_data: *mut c_void) -> *mut WefValue {
    std::ptr::null_mut()
}
pub unsafe extern "C" fn value_dict(_data: *mut c_void) -> *mut WefValue {
    std::ptr::null_mut()
}
pub unsafe extern "C" fn value_binary(
    _data: *mut c_void,
    _d: *const c_void,
    _len: usize,
) -> *mut WefValue {
    std::ptr::null_mut()
}
pub unsafe extern "C" fn value_list_append(
    _list: *mut WefValue,
    _val: *mut WefValue,
) -> bool {
    false
}
pub unsafe extern "C" fn value_list_set(
    _list: *mut WefValue,
    _idx: usize,
    _val: *mut WefValue,
) -> bool {
    false
}
pub unsafe extern "C" fn value_dict_set(
    _dict: *mut WefValue,
    _key: *const c_char,
    _val: *mut WefValue,
) -> bool {
    false
}
pub unsafe extern "C" fn value_free(_val: *mut WefValue) {}
pub unsafe extern "C" fn set_js_call_handler(
    _data: *mut c_void,
    _handler: WefJsCallFn,
    _user_data: *mut c_void,
) {
}
pub unsafe extern "C" fn js_call_respond(
    _data: *mut c_void,
    _call_id: u64,
    _result: *mut WefValue,
    _error: *mut WefValue,
) {
}
pub unsafe extern "C" fn invoke_js_callback(
    _data: *mut c_void,
    _cb_id: u64,
    _args: *mut WefValue,
) {
}
pub unsafe extern "C" fn release_js_callback(_data: *mut c_void, _cb_id: u64) {}

/// Fill the value/JS stub fields of a WefBackendApi. Backend-specific fields
/// (navigate, execute_js, quit, window management, handles) are left as None
/// and must be set by the caller.
pub fn create_api_base() -> WefBackendApi {
    WefBackendApi {
        version: WEF_API_VERSION,
        backend_data: std::ptr::null_mut(),
        navigate: None,
        set_title: None,
        execute_js: None,
        quit: None,
        set_window_size: None,
        get_window_size: None,
        set_window_position: None,
        get_window_position: None,
        set_resizable: None,
        is_resizable: None,
        set_always_on_top: None,
        is_always_on_top: None,
        is_visible: None,
        show: None,
        hide: None,
        focus: None,
        post_ui_task: None,
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
        get_window_handle: None,
        get_display_handle: None,
        get_window_handle_type: None,
        set_keyboard_event_handler: None,
        set_mouse_click_handler: None,
        poll_js_calls: None,
        set_js_call_notify: None,
        set_application_menu: None,
    }
}

// --- Window handle statics ---

static WINDOW_HANDLE: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());
static DISPLAY_HANDLE: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());
static WINDOW_HANDLE_TYPE: AtomicI32 = AtomicI32::new(0);

pub unsafe extern "C" fn backend_get_window_handle(_data: *mut c_void) -> *mut c_void {
    WINDOW_HANDLE.load(Ordering::Acquire)
}

pub unsafe extern "C" fn backend_get_display_handle(_data: *mut c_void) -> *mut c_void {
    DISPLAY_HANDLE.load(Ordering::Acquire)
}

pub unsafe extern "C" fn backend_get_window_handle_type(_data: *mut c_void) -> c_int {
    WINDOW_HANDLE_TYPE.load(Ordering::Acquire)
}

pub fn store_window_handles(window: &Window) {
    if let Ok(wh) = window.window_handle() {
        match wh.as_raw() {
            #[cfg(target_os = "macos")]
            RawWindowHandle::AppKit(handle) => {
                WINDOW_HANDLE
                    .store(handle.ns_view.as_ptr() as *mut c_void, Ordering::Release);
                WINDOW_HANDLE_TYPE.store(WEF_WINDOW_HANDLE_APPKIT, Ordering::Release);
            }
            #[cfg(target_os = "windows")]
            RawWindowHandle::Win32(handle) => {
                WINDOW_HANDLE
                    .store(handle.hwnd.get() as *mut c_void, Ordering::Release);
                WINDOW_HANDLE_TYPE.store(WEF_WINDOW_HANDLE_WIN32, Ordering::Release);
            }
            #[cfg(target_os = "linux")]
            RawWindowHandle::Xlib(handle) => {
                WINDOW_HANDLE
                    .store(handle.window as *mut c_void, Ordering::Release);
                WINDOW_HANDLE_TYPE.store(WEF_WINDOW_HANDLE_X11, Ordering::Release);
            }
            #[cfg(target_os = "linux")]
            RawWindowHandle::Wayland(handle) => {
                WINDOW_HANDLE.store(
                    handle.surface.as_ptr() as *mut c_void,
                    Ordering::Release,
                );
                WINDOW_HANDLE_TYPE.store(WEF_WINDOW_HANDLE_WAYLAND, Ordering::Release);
            }
            _ => {}
        }
    }

    if let Ok(dh) = window.display_handle() {
        match dh.as_raw() {
            #[cfg(target_os = "linux")]
            RawDisplayHandle::Xlib(handle) => {
                if let Some(display) = handle.display {
                    DISPLAY_HANDLE
                        .store(display.as_ptr() as *mut c_void, Ordering::Release);
                }
            }
            #[cfg(target_os = "linux")]
            RawDisplayHandle::Wayland(handle) => {
                DISPLAY_HANDLE
                    .store(handle.display.as_ptr() as *mut c_void, Ordering::Release);
            }
            _ => {}
        }
    }
}

// --- Common backend state ---

pub struct CommonState {
    pub pending_title: Mutex<Option<String>>,
    pub pending_size: Mutex<Option<(i32, i32)>>,
    pub pending_position: Mutex<Option<(i32, i32)>>,
    pub pending_resizable: Mutex<Option<bool>>,
    pub pending_always_on_top: Mutex<Option<bool>>,
    pub pending_visible: Mutex<Option<bool>>,
    pub keyboard_handler: Mutex<Option<(WefKeyboardEventFn, usize)>>,
    pub mouse_click_handler: Mutex<Option<(WefMouseClickFn, usize)>>,
    pub cursor_position: Mutex<(f64, f64)>,
    // Double-click tracking for winit (which doesn't provide click_count natively)
    pub last_press_time: Mutex<Option<std::time::Instant>>,
    pub last_press_button: Mutex<Option<winit::event::MouseButton>>,
    pub click_count: Mutex<i32>,
}

impl CommonState {
    pub fn new() -> Self {
        Self {
            pending_title: Mutex::new(None),
            pending_size: Mutex::new(None),
            pending_position: Mutex::new(None),
            pending_resizable: Mutex::new(None),
            pending_always_on_top: Mutex::new(None),
            pending_visible: Mutex::new(None),
            keyboard_handler: Mutex::new(None),
            mouse_click_handler: Mutex::new(None),
            cursor_position: Mutex::new((0.0, 0.0)),
            last_press_time: Mutex::new(None),
            last_press_button: Mutex::new(None),
            click_count: Mutex::new(0),
        }
    }
}

impl Default for CommonState {
    fn default() -> Self {
        Self::new()
    }
}

// --- Common event enum ---

#[derive(Debug)]
pub enum CommonEvent {
    SetTitle,
    SetWindowSize,
    SetWindowPosition,
    SetResizable,
    SetAlwaysOnTop,
    Show,
    Hide,
    Focus,
    Quit,
    UiTask {
        task: unsafe extern "C" fn(*mut c_void),
        data: usize,
    },
}

// --- Trait for backend state access ---

pub trait BackendAccess: Sized + 'static {
    type Event: 'static;

    fn get() -> Option<&'static Self>;
    fn proxy(&self) -> &EventLoopProxy<Self::Event>;
    fn common(&self) -> &CommonState;
    fn common_event(event: CommonEvent) -> Self::Event;
}

// --- Macro to generate C ABI backend functions ---

#[macro_export]
macro_rules! define_common_backend_fns {
    ($B:ty) => {
        unsafe extern "C" fn backend_set_title(
            _data: *mut ::std::ffi::c_void,
            title: *const ::std::ffi::c_char,
        ) {
            if title.is_null() {
                return;
            }
            let title_str = ::std::ffi::CStr::from_ptr(title)
                .to_string_lossy()
                .into_owned();
            if let Some(state) = <$B as $crate::BackendAccess>::get() {
                *state.common().pending_title.lock().unwrap() = Some(title_str);
                let _ = state.proxy().send_event(<$B as $crate::BackendAccess>::common_event(
                    $crate::CommonEvent::SetTitle,
                ));
            }
        }

        unsafe extern "C" fn backend_quit(_data: *mut ::std::ffi::c_void) {
            if let Some(state) = <$B as $crate::BackendAccess>::get() {
                let _ = state.proxy().send_event(<$B as $crate::BackendAccess>::common_event(
                    $crate::CommonEvent::Quit,
                ));
            }
        }

        unsafe extern "C" fn backend_set_window_size(
            _data: *mut ::std::ffi::c_void,
            width: ::std::ffi::c_int,
            height: ::std::ffi::c_int,
        ) {
            if let Some(state) = <$B as $crate::BackendAccess>::get() {
                *state.common().pending_size.lock().unwrap() = Some((width, height));
                let _ = state.proxy().send_event(<$B as $crate::BackendAccess>::common_event(
                    $crate::CommonEvent::SetWindowSize,
                ));
            }
        }

        unsafe extern "C" fn backend_get_window_size(
            _data: *mut ::std::ffi::c_void,
            width: *mut ::std::ffi::c_int,
            height: *mut ::std::ffi::c_int,
        ) {
            if let Some(state) = <$B as $crate::BackendAccess>::get() {
                if let Some((w, h)) = *state.common().pending_size.lock().unwrap() {
                    if !width.is_null() {
                        *width = w;
                    }
                    if !height.is_null() {
                        *height = h;
                    }
                    return;
                }
            }
            if !width.is_null() {
                *width = 0;
            }
            if !height.is_null() {
                *height = 0;
            }
        }

        unsafe extern "C" fn backend_set_window_position(
            _data: *mut ::std::ffi::c_void,
            x: ::std::ffi::c_int,
            y: ::std::ffi::c_int,
        ) {
            if let Some(state) = <$B as $crate::BackendAccess>::get() {
                *state.common().pending_position.lock().unwrap() = Some((x, y));
                let _ = state.proxy().send_event(<$B as $crate::BackendAccess>::common_event(
                    $crate::CommonEvent::SetWindowPosition,
                ));
            }
        }

        unsafe extern "C" fn backend_get_window_position(
            _data: *mut ::std::ffi::c_void,
            x: *mut ::std::ffi::c_int,
            y: *mut ::std::ffi::c_int,
        ) {
            if let Some(state) = <$B as $crate::BackendAccess>::get() {
                if let Some((px, py)) = *state.common().pending_position.lock().unwrap() {
                    if !x.is_null() {
                        *x = px;
                    }
                    if !y.is_null() {
                        *y = py;
                    }
                    return;
                }
            }
            if !x.is_null() {
                *x = 0;
            }
            if !y.is_null() {
                *y = 0;
            }
        }

        unsafe extern "C" fn backend_set_resizable(
            _data: *mut ::std::ffi::c_void,
            resizable: bool,
        ) {
            if let Some(state) = <$B as $crate::BackendAccess>::get() {
                *state.common().pending_resizable.lock().unwrap() = Some(resizable);
                let _ = state.proxy().send_event(<$B as $crate::BackendAccess>::common_event(
                    $crate::CommonEvent::SetResizable,
                ));
            }
        }

        unsafe extern "C" fn backend_is_resizable(_data: *mut ::std::ffi::c_void) -> bool {
            <$B as $crate::BackendAccess>::get()
                .and_then(|s| *s.common().pending_resizable.lock().unwrap())
                .unwrap_or(true)
        }

        unsafe extern "C" fn backend_set_always_on_top(
            _data: *mut ::std::ffi::c_void,
            always_on_top: bool,
        ) {
            if let Some(state) = <$B as $crate::BackendAccess>::get() {
                *state.common().pending_always_on_top.lock().unwrap() = Some(always_on_top);
                let _ = state.proxy().send_event(<$B as $crate::BackendAccess>::common_event(
                    $crate::CommonEvent::SetAlwaysOnTop,
                ));
            }
        }

        unsafe extern "C" fn backend_is_always_on_top(
            _data: *mut ::std::ffi::c_void,
        ) -> bool {
            <$B as $crate::BackendAccess>::get()
                .and_then(|s| *s.common().pending_always_on_top.lock().unwrap())
                .unwrap_or(false)
        }

        unsafe extern "C" fn backend_is_visible(_data: *mut ::std::ffi::c_void) -> bool {
            <$B as $crate::BackendAccess>::get()
                .and_then(|s| *s.common().pending_visible.lock().unwrap())
                .unwrap_or(true)
        }

        unsafe extern "C" fn backend_show(_data: *mut ::std::ffi::c_void) {
            if let Some(state) = <$B as $crate::BackendAccess>::get() {
                *state.common().pending_visible.lock().unwrap() = Some(true);
                let _ = state.proxy().send_event(<$B as $crate::BackendAccess>::common_event(
                    $crate::CommonEvent::Show,
                ));
            }
        }

        unsafe extern "C" fn backend_hide(_data: *mut ::std::ffi::c_void) {
            if let Some(state) = <$B as $crate::BackendAccess>::get() {
                *state.common().pending_visible.lock().unwrap() = Some(false);
                let _ = state.proxy().send_event(<$B as $crate::BackendAccess>::common_event(
                    $crate::CommonEvent::Hide,
                ));
            }
        }

        unsafe extern "C" fn backend_focus(_data: *mut ::std::ffi::c_void) {
            if let Some(state) = <$B as $crate::BackendAccess>::get() {
                let _ = state.proxy().send_event(<$B as $crate::BackendAccess>::common_event(
                    $crate::CommonEvent::Focus,
                ));
            }
        }

        unsafe extern "C" fn backend_post_ui_task(
            _data: *mut ::std::ffi::c_void,
            task: Option<unsafe extern "C" fn(*mut ::std::ffi::c_void)>,
            task_data: *mut ::std::ffi::c_void,
        ) {
            if let Some(task_fn) = task {
                if let Some(state) = <$B as $crate::BackendAccess>::get() {
                    let _ =
                        state.proxy().send_event(<$B as $crate::BackendAccess>::common_event(
                            $crate::CommonEvent::UiTask {
                                task: task_fn,
                                data: task_data as usize,
                            },
                        ));
                }
            }
        }

        unsafe extern "C" fn backend_set_keyboard_event_handler(
            _data: *mut ::std::ffi::c_void,
            handler: Option<$crate::WefKeyboardEventFn>,
            user_data: *mut ::std::ffi::c_void,
        ) {
            if let Some(state) = <$B as $crate::BackendAccess>::get() {
                *state.common().keyboard_handler.lock().unwrap() =
                    handler.map(|h| (h, user_data as usize));
            }
        }

        unsafe extern "C" fn backend_set_mouse_click_handler(
            _data: *mut ::std::ffi::c_void,
            handler: Option<$crate::WefMouseClickFn>,
            user_data: *mut ::std::ffi::c_void,
        ) {
            if let Some(state) = <$B as $crate::BackendAccess>::get() {
                *state.common().mouse_click_handler.lock().unwrap() =
                    handler.map(|h| (h, user_data as usize));
            }
        }
    };
}

/// Fill the common (window management) backend function pointers into an API struct.
/// Call this after `create_api_base()`.
///
/// The function pointer names must match those generated by `define_common_backend_fns!`.
#[macro_export]
macro_rules! fill_common_api {
    ($api:expr) => {
        $api.set_title = Some(backend_set_title);
        $api.quit = Some(backend_quit);
        $api.set_window_size = Some(backend_set_window_size);
        $api.get_window_size = Some(backend_get_window_size);
        $api.set_window_position = Some(backend_set_window_position);
        $api.get_window_position = Some(backend_get_window_position);
        $api.set_resizable = Some(backend_set_resizable);
        $api.is_resizable = Some(backend_is_resizable);
        $api.set_always_on_top = Some(backend_set_always_on_top);
        $api.is_always_on_top = Some(backend_is_always_on_top);
        $api.is_visible = Some(backend_is_visible);
        $api.show = Some(backend_show);
        $api.hide = Some(backend_hide);
        $api.focus = Some(backend_focus);
        $api.post_ui_task = Some(backend_post_ui_task);
        $api.get_window_handle = Some($crate::backend_get_window_handle);
        $api.get_display_handle = Some($crate::backend_get_display_handle);
        $api.get_window_handle_type = Some($crate::backend_get_window_handle_type);
        $api.set_keyboard_event_handler = Some(backend_set_keyboard_event_handler);
        $api.set_mouse_click_handler = Some(backend_set_mouse_click_handler);
    };
}

// --- Common event handling ---

/// Handle common window management events. Returns true if the event was handled.
pub fn handle_common_event<B: BackendAccess>(
    event: &CommonEvent,
    window: &Window,
) -> bool {
    match event {
        CommonEvent::SetTitle => {
            if let Some(state) = B::get() {
                if let Some(title) = state.common().pending_title.lock().unwrap().take() {
                    window.set_title(&title);
                }
            }
            true
        }
        CommonEvent::SetWindowSize => {
            if let Some(state) = B::get() {
                if let Some((w, h)) = state.common().pending_size.lock().unwrap().take() {
                    let _ = window.request_inner_size(LogicalSize::new(w, h));
                }
            }
            true
        }
        CommonEvent::SetWindowPosition => {
            if let Some(state) = B::get() {
                if let Some((x, y)) = state.common().pending_position.lock().unwrap().take() {
                    window.set_outer_position(LogicalPosition::new(x, y));
                }
            }
            true
        }
        CommonEvent::SetResizable => {
            if let Some(state) = B::get() {
                if let Some(resizable) = *state.common().pending_resizable.lock().unwrap() {
                    window.set_resizable(resizable);
                }
            }
            true
        }
        CommonEvent::SetAlwaysOnTop => {
            if let Some(state) = B::get() {
                if let Some(on_top) = *state.common().pending_always_on_top.lock().unwrap() {
                    window.set_window_level(if on_top {
                        WindowLevel::AlwaysOnTop
                    } else {
                        WindowLevel::Normal
                    });
                }
            }
            true
        }
        CommonEvent::Show => {
            window.set_visible(true);
            true
        }
        CommonEvent::Hide => {
            window.set_visible(false);
            true
        }
        CommonEvent::Focus => {
            window.focus_window();
            true
        }
        CommonEvent::Quit => false, // caller handles exit
        CommonEvent::UiTask { task, data } => {
            unsafe { task(*data as *mut c_void) };
            true
        }
    }
}

/// Apply pending state to window attributes before creation.
pub fn apply_pending_attrs(
    common: &CommonState,
    mut attrs: winit::window::WindowAttributes,
) -> winit::window::WindowAttributes {
    if let Some((w, h)) = *common.pending_size.lock().unwrap() {
        attrs = attrs.with_inner_size(LogicalSize::new(w, h));
    }
    if let Some((x, y)) = *common.pending_position.lock().unwrap() {
        attrs = attrs.with_position(LogicalPosition::new(x, y));
    }
    if let Some(resizable) = *common.pending_resizable.lock().unwrap() {
        attrs = attrs.with_resizable(resizable);
    }
    if let Some(title) = common.pending_title.lock().unwrap().as_ref() {
        attrs = attrs.with_title(title.clone());
    }
    attrs
}

/// Apply post-creation pending state (e.g. always-on-top).
pub fn apply_pending_post_create(common: &CommonState, window: &Window) {
    if let Some(true) = *common.pending_always_on_top.lock().unwrap() {
        window.set_window_level(WindowLevel::AlwaysOnTop);
    }
}

// --- Keyboard modifier flags ---

pub const WEF_MOD_SHIFT: u32 = 1 << 0;
pub const WEF_MOD_CONTROL: u32 = 1 << 1;
pub const WEF_MOD_ALT: u32 = 1 << 2;
pub const WEF_MOD_META: u32 = 1 << 3;

pub const WEF_KEY_PRESSED: c_int = 0;
pub const WEF_KEY_RELEASED: c_int = 1;

pub const WEF_MOUSE_BUTTON_LEFT: c_int = 0;
pub const WEF_MOUSE_BUTTON_RIGHT: c_int = 1;
pub const WEF_MOUSE_BUTTON_MIDDLE: c_int = 2;
pub const WEF_MOUSE_BUTTON_BACK: c_int = 3;
pub const WEF_MOUSE_BUTTON_FORWARD: c_int = 4;

pub const WEF_MOUSE_PRESSED: c_int = 0;
pub const WEF_MOUSE_RELEASED: c_int = 1;

/// Convert winit modifier state to WEF modifier bitmask.
pub fn modifiers_to_wef(mods: winit::keyboard::ModifiersState) -> u32 {
    let mut flags = 0u32;
    if mods.shift_key() {
        flags |= WEF_MOD_SHIFT;
    }
    if mods.control_key() {
        flags |= WEF_MOD_CONTROL;
    }
    if mods.alt_key() {
        flags |= WEF_MOD_ALT;
    }
    if mods.super_key() {
        flags |= WEF_MOD_META;
    }
    flags
}

/// Convert a winit logical key to its W3C UI Events `key` string representation.
pub fn winit_key_to_string(key: &winit::keyboard::Key) -> String {
    match key {
        winit::keyboard::Key::Character(c) => c.to_string(),
        winit::keyboard::Key::Named(named) => format!("{named:?}"),
        winit::keyboard::Key::Unidentified(_) => "Unidentified".to_string(),
        winit::keyboard::Key::Dead(c) => {
            if let Some(ch) = c {
                format!("Dead({ch})")
            } else {
                "Dead".to_string()
            }
        }
    }
}

/// Convert a winit physical key to its W3C UI Events `code` string representation.
pub fn winit_code_to_string(physical: &winit::keyboard::PhysicalKey) -> String {
    match physical {
        winit::keyboard::PhysicalKey::Code(code) => format!("{code:?}"),
        winit::keyboard::PhysicalKey::Unidentified(_) => "Unidentified".to_string(),
    }
}

/// Dispatch a keyboard event to the registered handler on the CommonState.
/// Call this from backend `window_event` handlers when processing `WindowEvent::KeyboardInput`.
pub fn dispatch_keyboard_event(
    common: &CommonState,
    key_event: &winit::event::KeyEvent,
    modifiers: winit::keyboard::ModifiersState,
) {
    let handler = common.keyboard_handler.lock().unwrap();
    if let Some((cb, user_data)) = *handler {
        let state = match key_event.state {
            winit::event::ElementState::Pressed => WEF_KEY_PRESSED,
            winit::event::ElementState::Released => WEF_KEY_RELEASED,
        };
        let key_str = winit_key_to_string(&key_event.logical_key);
        let code_str = winit_code_to_string(&key_event.physical_key);
        let mods = modifiers_to_wef(modifiers);

        let c_key = std::ffi::CString::new(key_str).unwrap_or_default();
        let c_code = std::ffi::CString::new(code_str).unwrap_or_default();

        unsafe {
            cb(
                user_data as *mut c_void,
                state,
                c_key.as_ptr(),
                c_code.as_ptr(),
                mods,
                key_event.repeat,
            );
        }
    }
}

/// Convert a winit mouse button to a WEF mouse button constant.
pub fn winit_button_to_wef(button: winit::event::MouseButton) -> c_int {
    match button {
        winit::event::MouseButton::Left => WEF_MOUSE_BUTTON_LEFT,
        winit::event::MouseButton::Right => WEF_MOUSE_BUTTON_RIGHT,
        winit::event::MouseButton::Middle => WEF_MOUSE_BUTTON_MIDDLE,
        winit::event::MouseButton::Back => WEF_MOUSE_BUTTON_BACK,
        winit::event::MouseButton::Forward => WEF_MOUSE_BUTTON_FORWARD,
        winit::event::MouseButton::Other(_) => -1,
    }
}

/// Dispatch a mouse click event to the registered handler on the CommonState.
/// Double-click interval (500ms is the standard across most platforms).
const DOUBLE_CLICK_INTERVAL: std::time::Duration = std::time::Duration::from_millis(500);

pub fn dispatch_mouse_click_event(
    common: &CommonState,
    button_state: winit::event::ElementState,
    button: winit::event::MouseButton,
    modifiers: winit::keyboard::ModifiersState,
) {
    let handler = common.mouse_click_handler.lock().unwrap();
    if let Some((cb, user_data)) = *handler {
        let state = match button_state {
            winit::event::ElementState::Pressed => WEF_MOUSE_PRESSED,
            winit::event::ElementState::Released => WEF_MOUSE_RELEASED,
        };
        let wef_button = winit_button_to_wef(button);
        if wef_button < 0 {
            return;
        }
        let (x, y) = *common.cursor_position.lock().unwrap();
        let mods = modifiers_to_wef(modifiers);

        // Compute click_count on press events (winit doesn't provide this natively)
        let click_count = if button_state == winit::event::ElementState::Pressed {
            let now = std::time::Instant::now();
            let mut last_time = common.last_press_time.lock().unwrap();
            let mut last_btn = common.last_press_button.lock().unwrap();
            let mut count = common.click_count.lock().unwrap();

            if *last_btn == Some(button)
                && *count < 2
                && last_time.map_or(false, |t| now.duration_since(t) < DOUBLE_CLICK_INTERVAL)
            {
                *count = 2;
            } else if *count >= 2 || *last_btn != Some(button) {
                *count = 1;
            }
            *last_time = Some(now);
            *last_btn = Some(button);
            *count
        } else {
            // Released: use the click_count from the most recent press
            *common.click_count.lock().unwrap()
        };

        unsafe {
            cb(
                user_data as *mut c_void,
                state,
                wef_button,
                x,
                y,
                mods,
                click_count,
            );
        }
    }
}

// --- Runtime loading ---

pub fn find_runtime_library() -> Option<PathBuf> {
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

pub fn load_and_start_runtime(api: WefBackendApi) {
    let runtime_path = find_runtime_library();
    match runtime_path {
        Some(path) => {
            println!("Loading runtime from: {}", path.display());
            thread::spawn(move || unsafe {
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
            });
        }
        None => {
            println!("No runtime library found. Set WEF_RUNTIME_PATH or place libruntime in current directory.");
            println!("Starting without runtime integration...");
        }
    }
}
