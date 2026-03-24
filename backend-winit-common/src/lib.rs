// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

pub use winit;

use std::collections::HashMap;
use std::env;
use std::ffi::{c_char, c_int, c_void};
use std::path::PathBuf;
use std::sync::atomic::{AtomicI32, AtomicPtr, AtomicU32, Ordering};
use std::sync::Mutex;
use std::thread;

use libloading::{Library, Symbol};
#[allow(unused_imports)]
use raw_window_handle::{
  HasDisplayHandle, HasWindowHandle, RawDisplayHandle, RawWindowHandle,
};
use winit::dpi::{LogicalPosition, LogicalSize};
use winit::event_loop::EventLoopProxy;
use winit::window::{Window, WindowLevel};

// --- Constants ---

pub const WEF_API_VERSION: u32 = 12;

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

pub type WefJsCallFn = unsafe extern "C" fn(
  *mut c_void,   // user_data
  u32,           // window_id
  u64,           // call_id
  *const c_char, // method_path
  *mut WefValue, // args
);
pub type WefJsResultFn =
  unsafe extern "C" fn(*mut WefValue, *mut WefValue, *mut c_void);
pub type WefKeyboardEventFn = unsafe extern "C" fn(
  *mut c_void,   // user_data
  u32,           // window_id
  c_int,         // state (0=pressed, 1=released)
  *const c_char, // key
  *const c_char, // code
  u32,           // modifiers
  bool,          // repeat
);
pub type WefMouseMoveFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  f64,         // x
  f64,         // y
  u32,         // modifiers
);
pub type WefMouseClickFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  c_int,       // state (0=pressed, 1=released)
  c_int,       // button
  f64,         // x
  f64,         // y
  u32,         // modifiers
  i32,         // click_count
);
pub type WefWheelFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  f64,         // delta_x
  f64,         // delta_y
  f64,         // x
  f64,         // y
  u32,         // modifiers
  i32,         // delta_mode
);
pub type WefCursorEnterLeaveFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  c_int,       // entered (1=entered, 0=left)
  f64,         // x
  f64,         // y
  u32,         // modifiers
);
pub type WefFocusedFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  c_int,       // focused (1=gained, 0=lost)
);
pub type WefResizeFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  c_int,       // width
  c_int,       // height
);
pub type WefMoveFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
  c_int,       // x
  c_int,       // y
);
pub type WefCloseRequestedFn = unsafe extern "C" fn(
  *mut c_void, // user_data
  u32,         // window_id
);

pub const WEF_WHEEL_DELTA_PIXEL: i32 = 0;
pub const WEF_WHEEL_DELTA_LINE: i32 = 1;

#[repr(C)]
pub struct WefBackendApi {
  pub version: u32,
  pub backend_data: *mut c_void,
  pub create_window: Option<unsafe extern "C" fn(*mut c_void) -> u32>,
  pub close_window: Option<unsafe extern "C" fn(*mut c_void, u32)>,
  pub navigate: Option<unsafe extern "C" fn(*mut c_void, u32, *const c_char)>,
  pub set_title: Option<unsafe extern "C" fn(*mut c_void, u32, *const c_char)>,
  pub execute_js: Option<
    unsafe extern "C" fn(
      *mut c_void,
      u32,
      *const c_char,
      Option<WefJsResultFn>,
      *mut c_void,
    ),
  >,
  pub quit: Option<unsafe extern "C" fn(*mut c_void)>,
  pub set_window_size:
    Option<unsafe extern "C" fn(*mut c_void, u32, c_int, c_int)>,
  pub get_window_size:
    Option<unsafe extern "C" fn(*mut c_void, u32, *mut c_int, *mut c_int)>,
  pub set_window_position:
    Option<unsafe extern "C" fn(*mut c_void, u32, c_int, c_int)>,
  pub get_window_position:
    Option<unsafe extern "C" fn(*mut c_void, u32, *mut c_int, *mut c_int)>,
  pub set_resizable: Option<unsafe extern "C" fn(*mut c_void, u32, bool)>,
  pub is_resizable: Option<unsafe extern "C" fn(*mut c_void, u32) -> bool>,
  pub set_always_on_top: Option<unsafe extern "C" fn(*mut c_void, u32, bool)>,
  pub is_always_on_top: Option<unsafe extern "C" fn(*mut c_void, u32) -> bool>,
  pub is_visible: Option<unsafe extern "C" fn(*mut c_void, u32) -> bool>,
  pub show: Option<unsafe extern "C" fn(*mut c_void, u32)>,
  pub hide: Option<unsafe extern "C" fn(*mut c_void, u32)>,
  pub focus: Option<unsafe extern "C" fn(*mut c_void, u32)>,
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
  pub value_list_get:
    Option<unsafe extern "C" fn(*mut WefValue, usize) -> *mut WefValue>,
  pub value_dict_get:
    Option<unsafe extern "C" fn(*mut WefValue, *const c_char) -> *mut WefValue>,
  pub value_dict_has:
    Option<unsafe extern "C" fn(*mut WefValue, *const c_char) -> bool>,
  pub value_dict_size: Option<unsafe extern "C" fn(*mut WefValue) -> usize>,
  pub value_dict_keys:
    Option<unsafe extern "C" fn(*mut WefValue, *mut usize) -> *mut *mut c_char>,
  pub value_free_keys: Option<unsafe extern "C" fn(*mut *mut c_char, usize)>,
  pub value_get_binary:
    Option<unsafe extern "C" fn(*mut WefValue, *mut usize) -> *const c_void>,
  pub value_get_callback_id: Option<unsafe extern "C" fn(*mut WefValue) -> u64>,
  pub value_null: Option<unsafe extern "C" fn(*mut c_void) -> *mut WefValue>,
  pub value_bool:
    Option<unsafe extern "C" fn(*mut c_void, bool) -> *mut WefValue>,
  pub value_int:
    Option<unsafe extern "C" fn(*mut c_void, c_int) -> *mut WefValue>,
  pub value_double:
    Option<unsafe extern "C" fn(*mut c_void, f64) -> *mut WefValue>,
  pub value_string:
    Option<unsafe extern "C" fn(*mut c_void, *const c_char) -> *mut WefValue>,
  pub value_list: Option<unsafe extern "C" fn(*mut c_void) -> *mut WefValue>,
  pub value_dict: Option<unsafe extern "C" fn(*mut c_void) -> *mut WefValue>,
  pub value_binary: Option<
    unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> *mut WefValue,
  >,
  pub value_list_append:
    Option<unsafe extern "C" fn(*mut WefValue, *mut WefValue) -> bool>,
  pub value_list_set:
    Option<unsafe extern "C" fn(*mut WefValue, usize, *mut WefValue) -> bool>,
  pub value_dict_set: Option<
    unsafe extern "C" fn(*mut WefValue, *const c_char, *mut WefValue) -> bool,
  >,
  pub value_free: Option<unsafe extern "C" fn(*mut WefValue)>,
  pub set_js_call_handler:
    Option<unsafe extern "C" fn(*mut c_void, WefJsCallFn, *mut c_void)>,
  pub js_call_respond: Option<
    unsafe extern "C" fn(*mut c_void, u64, *mut WefValue, *mut WefValue),
  >,
  pub invoke_js_callback:
    Option<unsafe extern "C" fn(*mut c_void, u64, *mut WefValue)>,
  pub release_js_callback: Option<unsafe extern "C" fn(*mut c_void, u64)>,
  pub get_window_handle:
    Option<unsafe extern "C" fn(*mut c_void, u32) -> *mut c_void>,
  pub get_display_handle:
    Option<unsafe extern "C" fn(*mut c_void, u32) -> *mut c_void>,
  pub get_window_handle_type:
    Option<unsafe extern "C" fn(*mut c_void, u32) -> c_int>,
  pub set_keyboard_event_handler: Option<
    unsafe extern "C" fn(*mut c_void, Option<WefKeyboardEventFn>, *mut c_void),
  >,
  pub set_mouse_click_handler: Option<
    unsafe extern "C" fn(*mut c_void, Option<WefMouseClickFn>, *mut c_void),
  >,
  pub set_mouse_move_handler: Option<
    unsafe extern "C" fn(*mut c_void, Option<WefMouseMoveFn>, *mut c_void),
  >,
  pub set_wheel_handler:
    Option<unsafe extern "C" fn(*mut c_void, Option<WefWheelFn>, *mut c_void)>,
  pub set_cursor_enter_leave_handler: Option<
    unsafe extern "C" fn(
      *mut c_void,
      Option<WefCursorEnterLeaveFn>,
      *mut c_void,
    ),
  >,
  pub set_focused_handler: Option<
    unsafe extern "C" fn(*mut c_void, Option<WefFocusedFn>, *mut c_void),
  >,
  pub set_resize_handler:
    Option<unsafe extern "C" fn(*mut c_void, Option<WefResizeFn>, *mut c_void)>,
  pub set_move_handler:
    Option<unsafe extern "C" fn(*mut c_void, Option<WefMoveFn>, *mut c_void)>,
  pub set_close_requested_handler: Option<
    unsafe extern "C" fn(*mut c_void, Option<WefCloseRequestedFn>, *mut c_void),
  >,
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

pub unsafe extern "C" fn value_is_null(_val: *mut WefValue) -> bool {
  true
}
pub unsafe extern "C" fn value_is_bool(_val: *mut WefValue) -> bool {
  false
}
pub unsafe extern "C" fn value_is_int(_val: *mut WefValue) -> bool {
  false
}
pub unsafe extern "C" fn value_is_double(_val: *mut WefValue) -> bool {
  false
}
pub unsafe extern "C" fn value_is_string(_val: *mut WefValue) -> bool {
  false
}
pub unsafe extern "C" fn value_is_list(_val: *mut WefValue) -> bool {
  false
}
pub unsafe extern "C" fn value_is_dict(_val: *mut WefValue) -> bool {
  false
}
pub unsafe extern "C" fn value_is_binary(_val: *mut WefValue) -> bool {
  false
}
pub unsafe extern "C" fn value_is_callback(_val: *mut WefValue) -> bool {
  false
}
pub unsafe extern "C" fn value_get_bool(_val: *mut WefValue) -> bool {
  false
}
pub unsafe extern "C" fn value_get_int(_val: *mut WefValue) -> c_int {
  0
}
pub unsafe extern "C" fn value_get_double(_val: *mut WefValue) -> f64 {
  0.0
}
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
pub unsafe extern "C" fn value_list_size(_val: *mut WefValue) -> usize {
  0
}
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
pub unsafe extern "C" fn value_dict_size(_val: *mut WefValue) -> usize {
  0
}
pub unsafe extern "C" fn value_dict_keys(
  _val: *mut WefValue,
  count: *mut usize,
) -> *mut *mut c_char {
  if !count.is_null() {
    unsafe { *count = 0 };
  }
  std::ptr::null_mut()
}
pub unsafe extern "C" fn value_free_keys(
  _keys: *mut *mut c_char,
  _count: usize,
) {
}
pub unsafe extern "C" fn value_get_binary(
  _val: *mut WefValue,
  len: *mut usize,
) -> *const c_void {
  if !len.is_null() {
    unsafe { *len = 0 };
  }
  std::ptr::null()
}
pub unsafe extern "C" fn value_get_callback_id(_val: *mut WefValue) -> u64 {
  0
}
pub unsafe extern "C" fn value_null(_data: *mut c_void) -> *mut WefValue {
  std::ptr::null_mut()
}
pub unsafe extern "C" fn value_bool(
  _data: *mut c_void,
  _v: bool,
) -> *mut WefValue {
  std::ptr::null_mut()
}
pub unsafe extern "C" fn value_int(
  _data: *mut c_void,
  _v: c_int,
) -> *mut WefValue {
  std::ptr::null_mut()
}
pub unsafe extern "C" fn value_double(
  _data: *mut c_void,
  _v: f64,
) -> *mut WefValue {
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
    create_window: None,
    close_window: None,
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
    set_mouse_move_handler: None,
    set_wheel_handler: None,
    set_cursor_enter_leave_handler: None,
    set_focused_handler: None,
    set_resize_handler: None,
    set_move_handler: None,
    set_close_requested_handler: None,
    poll_js_calls: None,
    set_js_call_notify: None,
    set_application_menu: None,
  }
}

// --- Window ID counter ---

static NEXT_WINDOW_ID: AtomicU32 = AtomicU32::new(1);

pub fn allocate_window_id() -> u32 {
  NEXT_WINDOW_ID.fetch_add(1, Ordering::Relaxed)
}

// --- Per-window handle storage ---

static WINDOW_HANDLES: Mutex<Option<HashMap<u32, WindowHandleInfo>>> =
  Mutex::new(None);
static DISPLAY_HANDLE: AtomicPtr<c_void> = AtomicPtr::new(std::ptr::null_mut());
static WINDOW_HANDLE_TYPE: AtomicI32 = AtomicI32::new(0);

struct WindowHandleInfo {
  handle: *mut c_void,
}

unsafe impl Send for WindowHandleInfo {}

pub unsafe extern "C" fn backend_get_window_handle(
  _data: *mut c_void,
  window_id: u32,
) -> *mut c_void {
  let map = WINDOW_HANDLES.lock().unwrap();
  map
    .as_ref()
    .and_then(|m| m.get(&window_id))
    .map(|info| info.handle)
    .unwrap_or(std::ptr::null_mut())
}

pub unsafe extern "C" fn backend_get_display_handle(
  _data: *mut c_void,
  _window_id: u32,
) -> *mut c_void {
  DISPLAY_HANDLE.load(Ordering::Acquire)
}

pub unsafe extern "C" fn backend_get_window_handle_type(
  _data: *mut c_void,
  _window_id: u32,
) -> c_int {
  WINDOW_HANDLE_TYPE.load(Ordering::Acquire)
}

pub fn store_window_handles(window_id: u32, window: &Window) {
  let mut handle_ptr = std::ptr::null_mut();

  if let Ok(wh) = window.window_handle() {
    match wh.as_raw() {
      #[cfg(target_os = "macos")]
      RawWindowHandle::AppKit(handle) => {
        handle_ptr = handle.ns_view.as_ptr() as *mut c_void;
        WINDOW_HANDLE_TYPE.store(WEF_WINDOW_HANDLE_APPKIT, Ordering::Release);
      }
      #[cfg(target_os = "windows")]
      RawWindowHandle::Win32(handle) => {
        handle_ptr = handle.hwnd.get() as *mut c_void;
        WINDOW_HANDLE_TYPE.store(WEF_WINDOW_HANDLE_WIN32, Ordering::Release);
      }
      #[cfg(target_os = "linux")]
      RawWindowHandle::Xlib(handle) => {
        handle_ptr = handle.window as *mut c_void;
        WINDOW_HANDLE_TYPE.store(WEF_WINDOW_HANDLE_X11, Ordering::Release);
      }
      #[cfg(target_os = "linux")]
      RawWindowHandle::Wayland(handle) => {
        handle_ptr = handle.surface.as_ptr() as *mut c_void;
        WINDOW_HANDLE_TYPE.store(WEF_WINDOW_HANDLE_WAYLAND, Ordering::Release);
      }
      _ => {}
    }
  }

  {
    let mut map = WINDOW_HANDLES.lock().unwrap();
    let map = map.get_or_insert_with(HashMap::new);
    map.insert(window_id, WindowHandleInfo { handle: handle_ptr });
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

pub fn remove_window_handles(window_id: u32) {
  let mut map = WINDOW_HANDLES.lock().unwrap();
  if let Some(m) = map.as_mut() {
    m.remove(&window_id);
  }
}

// --- Per-window common state ---

pub struct WindowState {
  pub pending_title: Mutex<Option<String>>,
  pub pending_size: Mutex<Option<(i32, i32)>>,
  pub pending_position: Mutex<Option<(i32, i32)>>,
  pub pending_resizable: Mutex<Option<bool>>,
  pub pending_always_on_top: Mutex<Option<bool>>,
  pub pending_visible: Mutex<Option<bool>>,
  pub cursor_position: Mutex<(f64, f64)>,
  pub last_press_time: Mutex<Option<std::time::Instant>>,
  pub last_press_button: Mutex<Option<winit::event::MouseButton>>,
  pub click_count: Mutex<i32>,
}

impl WindowState {
  pub fn new() -> Self {
    Self {
      pending_title: Mutex::new(None),
      pending_size: Mutex::new(None),
      pending_position: Mutex::new(None),
      pending_resizable: Mutex::new(None),
      pending_always_on_top: Mutex::new(None),
      pending_visible: Mutex::new(None),
      cursor_position: Mutex::new((0.0, 0.0)),
      last_press_time: Mutex::new(None),
      last_press_button: Mutex::new(None),
      click_count: Mutex::new(0),
    }
  }
}

impl Default for WindowState {
  fn default() -> Self {
    Self::new()
  }
}

// --- Global event handler state (registered once, dispatches for all windows) ---

pub struct EventHandlers {
  pub keyboard_handler: Mutex<Option<(WefKeyboardEventFn, usize)>>,
  pub mouse_click_handler: Mutex<Option<(WefMouseClickFn, usize)>>,
  pub mouse_move_handler: Mutex<Option<(WefMouseMoveFn, usize)>>,
  pub wheel_handler: Mutex<Option<(WefWheelFn, usize)>>,
  pub cursor_enter_leave_handler: Mutex<Option<(WefCursorEnterLeaveFn, usize)>>,
  pub focused_handler: Mutex<Option<(WefFocusedFn, usize)>>,
  pub resize_handler: Mutex<Option<(WefResizeFn, usize)>>,
  pub move_handler: Mutex<Option<(WefMoveFn, usize)>>,
  pub close_requested_handler: Mutex<Option<(WefCloseRequestedFn, usize)>>,
}

impl EventHandlers {
  pub fn new() -> Self {
    Self {
      keyboard_handler: Mutex::new(None),
      mouse_click_handler: Mutex::new(None),
      mouse_move_handler: Mutex::new(None),
      wheel_handler: Mutex::new(None),
      cursor_enter_leave_handler: Mutex::new(None),
      focused_handler: Mutex::new(None),
      resize_handler: Mutex::new(None),
      move_handler: Mutex::new(None),
      close_requested_handler: Mutex::new(None),
    }
  }
}

impl Default for EventHandlers {
  fn default() -> Self {
    Self::new()
  }
}

// --- Common backend state holding per-window state + global handlers ---

pub struct CommonState {
  pub windows: Mutex<HashMap<u32, WindowState>>,
  pub handlers: EventHandlers,
}

impl CommonState {
  pub fn new() -> Self {
    Self {
      windows: Mutex::new(HashMap::new()),
      handlers: EventHandlers::new(),
    }
  }

  pub fn add_window(&self, window_id: u32) {
    let mut windows = self.windows.lock().unwrap();
    windows.insert(window_id, WindowState::new());
  }

  pub fn remove_window(&self, window_id: u32) {
    let mut windows = self.windows.lock().unwrap();
    windows.remove(&window_id);
  }

  pub fn with_window<F, R>(&self, window_id: u32, f: F) -> Option<R>
  where
    F: FnOnce(&WindowState) -> R,
  {
    let windows = self.windows.lock().unwrap();
    windows.get(&window_id).map(f)
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
  CreateWindow {
    window_id: u32,
  },
  CloseWindow {
    window_id: u32,
  },
  SetTitle {
    window_id: u32,
  },
  SetWindowSize {
    window_id: u32,
  },
  SetWindowPosition {
    window_id: u32,
  },
  SetResizable {
    window_id: u32,
  },
  SetAlwaysOnTop {
    window_id: u32,
  },
  Show {
    window_id: u32,
  },
  Hide {
    window_id: u32,
  },
  Focus {
    window_id: u32,
  },
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
    unsafe extern "C" fn backend_create_window(
      _data: *mut ::std::ffi::c_void,
    ) -> u32 {
      let window_id = $crate::allocate_window_id();
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().add_window(window_id);
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::CreateWindow { window_id },
          ),
        );
      }
      window_id
    }

    unsafe extern "C" fn backend_close_window(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::CloseWindow { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_title(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      title: *const ::std::ffi::c_char,
    ) {
      if title.is_null() {
        return;
      }
      let title_str = ::std::ffi::CStr::from_ptr(title)
        .to_string_lossy()
        .into_owned();
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_title.lock().unwrap() = Some(title_str);
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::SetTitle { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_quit(_data: *mut ::std::ffi::c_void) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::Quit,
          ),
        );
      }
    }

    unsafe extern "C" fn backend_set_window_size(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      width: ::std::ffi::c_int,
      height: ::std::ffi::c_int,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_size.lock().unwrap() = Some((width, height));
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::SetWindowSize { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_get_window_size(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      width: *mut ::std::ffi::c_int,
      height: *mut ::std::ffi::c_int,
    ) {
      let mut found = false;
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        if let Some(()) = state.common().with_window(window_id, |ws| {
          if let Some((w, h)) = *ws.pending_size.lock().unwrap() {
            if !width.is_null() {
              *width = w;
            }
            if !height.is_null() {
              *height = h;
            }
          }
        }) {
          found = true;
        }
      }
      if !found {
        if !width.is_null() {
          *width = 0;
        }
        if !height.is_null() {
          *height = 0;
        }
      }
    }

    unsafe extern "C" fn backend_set_window_position(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      x: ::std::ffi::c_int,
      y: ::std::ffi::c_int,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_position.lock().unwrap() = Some((x, y));
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::SetWindowPosition { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_get_window_position(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      x: *mut ::std::ffi::c_int,
      y: *mut ::std::ffi::c_int,
    ) {
      let mut found = false;
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        if let Some(()) = state.common().with_window(window_id, |ws| {
          if let Some((px, py)) = *ws.pending_position.lock().unwrap() {
            if !x.is_null() {
              *x = px;
            }
            if !y.is_null() {
              *y = py;
            }
          }
        }) {
          found = true;
        }
      }
      if !found {
        if !x.is_null() {
          *x = 0;
        }
        if !y.is_null() {
          *y = 0;
        }
      }
    }

    unsafe extern "C" fn backend_set_resizable(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      resizable: bool,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_resizable.lock().unwrap() = Some(resizable);
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::SetResizable { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_is_resizable(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) -> bool {
      <$B as $crate::BackendAccess>::get()
        .and_then(|s| {
          s.common()
            .with_window(window_id, |ws| *ws.pending_resizable.lock().unwrap())
            .flatten()
        })
        .unwrap_or(true)
    }

    unsafe extern "C" fn backend_set_always_on_top(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
      always_on_top: bool,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_always_on_top.lock().unwrap() = Some(always_on_top);
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::SetAlwaysOnTop { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_is_always_on_top(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) -> bool {
      <$B as $crate::BackendAccess>::get()
        .and_then(|s| {
          s.common()
            .with_window(window_id, |ws| {
              *ws.pending_always_on_top.lock().unwrap()
            })
            .flatten()
        })
        .unwrap_or(false)
    }

    unsafe extern "C" fn backend_is_visible(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) -> bool {
      <$B as $crate::BackendAccess>::get()
        .and_then(|s| {
          s.common()
            .with_window(window_id, |ws| *ws.pending_visible.lock().unwrap())
            .flatten()
        })
        .unwrap_or(true)
    }

    unsafe extern "C" fn backend_show(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_visible.lock().unwrap() = Some(true);
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::Show { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_hide(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        state.common().with_window(window_id, |ws| {
          *ws.pending_visible.lock().unwrap() = Some(false);
        });
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::Hide { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_focus(
      _data: *mut ::std::ffi::c_void,
      window_id: u32,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        let _ = state.proxy().send_event(
          <$B as $crate::BackendAccess>::common_event(
            $crate::CommonEvent::Focus { window_id },
          ),
        );
      }
    }

    unsafe extern "C" fn backend_post_ui_task(
      _data: *mut ::std::ffi::c_void,
      task: Option<unsafe extern "C" fn(*mut ::std::ffi::c_void)>,
      task_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(task_fn) = task {
        if let Some(state) = <$B as $crate::BackendAccess>::get() {
          let _ = state.proxy().send_event(
            <$B as $crate::BackendAccess>::common_event(
              $crate::CommonEvent::UiTask {
                task: task_fn,
                data: task_data as usize,
              },
            ),
          );
        }
      }
    }

    unsafe extern "C" fn backend_set_keyboard_event_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::WefKeyboardEventFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.keyboard_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_mouse_click_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::WefMouseClickFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.mouse_click_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_mouse_move_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::WefMouseMoveFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.mouse_move_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_wheel_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::WefWheelFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.wheel_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_cursor_enter_leave_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::WefCursorEnterLeaveFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state
          .common()
          .handlers
          .cursor_enter_leave_handler
          .lock()
          .unwrap() = handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_focused_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::WefFocusedFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.focused_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_resize_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::WefResizeFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.resize_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_move_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::WefMoveFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state.common().handlers.move_handler.lock().unwrap() =
          handler.map(|h| (h, user_data as usize));
      }
    }

    unsafe extern "C" fn backend_set_close_requested_handler(
      _data: *mut ::std::ffi::c_void,
      handler: Option<$crate::WefCloseRequestedFn>,
      user_data: *mut ::std::ffi::c_void,
    ) {
      if let Some(state) = <$B as $crate::BackendAccess>::get() {
        *state
          .common()
          .handlers
          .close_requested_handler
          .lock()
          .unwrap() = handler.map(|h| (h, user_data as usize));
      }
    }
  };
}

/// Fill the common (window management) backend function pointers into an API struct.
#[macro_export]
macro_rules! fill_common_api {
  ($api:expr) => {
    $api.create_window = Some(backend_create_window);
    $api.close_window = Some(backend_close_window);
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
    $api.set_mouse_move_handler = Some(backend_set_mouse_move_handler);
    $api.set_wheel_handler = Some(backend_set_wheel_handler);
    $api.set_cursor_enter_leave_handler =
      Some(backend_set_cursor_enter_leave_handler);
    $api.set_focused_handler = Some(backend_set_focused_handler);
    $api.set_resize_handler = Some(backend_set_resize_handler);
    $api.set_move_handler = Some(backend_set_move_handler);
    $api.set_close_requested_handler =
      Some(backend_set_close_requested_handler);
  };
}

// --- Common event handling ---

/// Handle common window management events on a specific window.
pub fn handle_common_event<B: BackendAccess>(
  event: &CommonEvent,
  window_id: u32,
  window: &Window,
) -> bool {
  match event {
    CommonEvent::SetTitle { window_id: eid, .. } if *eid == window_id => {
      if let Some(state) = B::get() {
        state.common().with_window(window_id, |ws| {
          if let Some(title) = ws.pending_title.lock().unwrap().take() {
            window.set_title(&title);
          }
        });
      }
      true
    }
    CommonEvent::SetWindowSize { window_id: eid, .. } if *eid == window_id => {
      if let Some(state) = B::get() {
        state.common().with_window(window_id, |ws| {
          if let Some((w, h)) = ws.pending_size.lock().unwrap().take() {
            let _ = window.request_inner_size(LogicalSize::new(w, h));
          }
        });
      }
      true
    }
    CommonEvent::SetWindowPosition { window_id: eid, .. }
      if *eid == window_id =>
    {
      if let Some(state) = B::get() {
        state.common().with_window(window_id, |ws| {
          if let Some((x, y)) = ws.pending_position.lock().unwrap().take() {
            window.set_outer_position(LogicalPosition::new(x, y));
          }
        });
      }
      true
    }
    CommonEvent::SetResizable { window_id: eid, .. } if *eid == window_id => {
      if let Some(state) = B::get() {
        state.common().with_window(window_id, |ws| {
          if let Some(resizable) = *ws.pending_resizable.lock().unwrap() {
            window.set_resizable(resizable);
          }
        });
      }
      true
    }
    CommonEvent::SetAlwaysOnTop { window_id: eid, .. } if *eid == window_id => {
      if let Some(state) = B::get() {
        state.common().with_window(window_id, |ws| {
          if let Some(on_top) = *ws.pending_always_on_top.lock().unwrap() {
            window.set_window_level(if on_top {
              WindowLevel::AlwaysOnTop
            } else {
              WindowLevel::Normal
            });
          }
        });
      }
      true
    }
    CommonEvent::Show { window_id: eid, .. } if *eid == window_id => {
      window.set_visible(true);
      true
    }
    CommonEvent::Hide { window_id: eid, .. } if *eid == window_id => {
      window.set_visible(false);
      true
    }
    CommonEvent::Focus { window_id: eid, .. } if *eid == window_id => {
      window.focus_window();
      true
    }
    CommonEvent::UiTask { task, data } => {
      unsafe { task(*data as *mut c_void) };
      true
    }
    CommonEvent::Quit => false, // caller handles exit
    _ => false,
  }
}

/// Apply pending state to window attributes before creation.
pub fn apply_pending_attrs(
  ws: &WindowState,
  mut attrs: winit::window::WindowAttributes,
) -> winit::window::WindowAttributes {
  if let Some((w, h)) = *ws.pending_size.lock().unwrap() {
    attrs = attrs.with_inner_size(LogicalSize::new(w, h));
  }
  if let Some((x, y)) = *ws.pending_position.lock().unwrap() {
    attrs = attrs.with_position(LogicalPosition::new(x, y));
  }
  if let Some(resizable) = *ws.pending_resizable.lock().unwrap() {
    attrs = attrs.with_resizable(resizable);
  }
  if let Some(title) = ws.pending_title.lock().unwrap().as_ref() {
    attrs = attrs.with_title(title.clone());
  }
  attrs
}

/// Apply post-creation pending state (e.g. always-on-top).
pub fn apply_pending_post_create(ws: &WindowState, window: &Window) {
  if let Some(true) = *ws.pending_always_on_top.lock().unwrap() {
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

/// Dispatch a keyboard event to the registered handler.
pub fn dispatch_keyboard_event(
  handlers: &EventHandlers,
  window_id: u32,
  key_event: &winit::event::KeyEvent,
  modifiers: winit::keyboard::ModifiersState,
) {
  let handler = handlers.keyboard_handler.lock().unwrap();
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
        window_id,
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

/// Dispatch a mouse click event to the registered handler.
/// Double-click interval (500ms is the standard across most platforms).
const DOUBLE_CLICK_INTERVAL: std::time::Duration =
  std::time::Duration::from_millis(500);

pub fn dispatch_mouse_click_event(
  handlers: &EventHandlers,
  ws: &WindowState,
  window_id: u32,
  button_state: winit::event::ElementState,
  button: winit::event::MouseButton,
  modifiers: winit::keyboard::ModifiersState,
) {
  let handler = handlers.mouse_click_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    let state = match button_state {
      winit::event::ElementState::Pressed => WEF_MOUSE_PRESSED,
      winit::event::ElementState::Released => WEF_MOUSE_RELEASED,
    };
    let wef_button = winit_button_to_wef(button);
    if wef_button < 0 {
      return;
    }
    let (x, y) = *ws.cursor_position.lock().unwrap();
    let mods = modifiers_to_wef(modifiers);

    let click_count = if button_state == winit::event::ElementState::Pressed {
      let now = std::time::Instant::now();
      let mut last_time = ws.last_press_time.lock().unwrap();
      let mut last_btn = ws.last_press_button.lock().unwrap();
      let mut count = ws.click_count.lock().unwrap();

      if *last_btn == Some(button)
        && *count < 2
        && last_time
          .map_or(false, |t| now.duration_since(t) < DOUBLE_CLICK_INTERVAL)
      {
        *count = 2;
      } else if *count >= 2 || *last_btn != Some(button) {
        *count = 1;
      }
      *last_time = Some(now);
      *last_btn = Some(button);
      *count
    } else {
      *ws.click_count.lock().unwrap()
    };

    unsafe {
      cb(
        user_data as *mut c_void,
        window_id,
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

pub fn dispatch_mouse_move_event(
  handlers: &EventHandlers,
  window_id: u32,
  x: f64,
  y: f64,
  modifiers: winit::keyboard::ModifiersState,
) {
  let handler = handlers.mouse_move_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    let mods = modifiers_to_wef(modifiers);
    unsafe {
      cb(user_data as *mut c_void, window_id, x, y, mods);
    }
  }
}

pub fn dispatch_wheel_event(
  handlers: &EventHandlers,
  ws: &WindowState,
  window_id: u32,
  delta: winit::event::MouseScrollDelta,
  modifiers: winit::keyboard::ModifiersState,
) {
  let handler = handlers.wheel_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    let (delta_x, delta_y, delta_mode) = match delta {
      winit::event::MouseScrollDelta::LineDelta(dx, dy) => {
        (dx as f64, dy as f64, WEF_WHEEL_DELTA_LINE)
      }
      winit::event::MouseScrollDelta::PixelDelta(d) => {
        (d.x, d.y, WEF_WHEEL_DELTA_PIXEL)
      }
    };
    let (x, y) = *ws.cursor_position.lock().unwrap();
    let mods = modifiers_to_wef(modifiers);
    unsafe {
      cb(
        user_data as *mut c_void,
        window_id,
        delta_x,
        delta_y,
        x,
        y,
        mods,
        delta_mode,
      );
    }
  }
}

pub fn dispatch_cursor_enter_leave_event(
  handlers: &EventHandlers,
  ws: &WindowState,
  window_id: u32,
  entered: bool,
  modifiers: winit::keyboard::ModifiersState,
) {
  let handler = handlers.cursor_enter_leave_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    let (x, y) = *ws.cursor_position.lock().unwrap();
    let mods = modifiers_to_wef(modifiers);
    unsafe {
      cb(
        user_data as *mut c_void,
        window_id,
        if entered { 1 } else { 0 },
        x,
        y,
        mods,
      );
    }
  }
}

pub fn dispatch_focused_event(
  handlers: &EventHandlers,
  window_id: u32,
  focused: bool,
) {
  let handler = handlers.focused_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    unsafe {
      cb(
        user_data as *mut c_void,
        window_id,
        if focused { 1 } else { 0 },
      );
    }
  }
}

pub fn dispatch_resize_event(
  handlers: &EventHandlers,
  window_id: u32,
  width: i32,
  height: i32,
) {
  let handler = handlers.resize_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    unsafe {
      cb(user_data as *mut c_void, window_id, width, height);
    }
  }
}

pub fn dispatch_move_event(
  handlers: &EventHandlers,
  window_id: u32,
  x: i32,
  y: i32,
) {
  let handler = handlers.move_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    unsafe {
      cb(user_data as *mut c_void, window_id, x, y);
    }
  }
}

pub fn dispatch_close_requested_event(
  handlers: &EventHandlers,
  window_id: u32,
) {
  let handler = handlers.close_requested_handler.lock().unwrap();
  if let Some((cb, user_data)) = *handler {
    unsafe {
      cb(user_data as *mut c_void, window_id);
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

        let start: Symbol<RuntimeStartFn> =
          match lib.get(b"wef_runtime_start\0") {
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
