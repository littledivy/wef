// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::future::Future;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};

use tokio::sync::Notify;

#[allow(non_upper_case_globals)]
#[allow(non_camel_case_types)]
#[allow(non_snake_case)]
#[allow(dead_code)]
mod ffi {
  include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

mod keyboard;
pub use keyboard::*;

mod mouse;
pub use mouse::*;

pub const WEF_API_VERSION: u32 = 8;

pub const WEF_WINDOW_HANDLE_UNKNOWN: i32 = 0;
pub const WEF_WINDOW_HANDLE_APPKIT: i32 = 1;
pub const WEF_WINDOW_HANDLE_WIN32: i32 = 2;
pub const WEF_WINDOW_HANDLE_X11: i32 = 3;
pub const WEF_WINDOW_HANDLE_WAYLAND: i32 = 4;
pub type WefValue = ffi::wef_value_t;
pub type WefBackendApi = ffi::wef_backend_api_t;

unsafe impl Send for WefBackendApi {}
unsafe impl Sync for WefBackendApi {}

static BACKEND_API: OnceLock<&'static WefBackendApi> = OnceLock::new();
static SHUTDOWN_FLAG: AtomicBool = AtomicBool::new(false);
static BINDINGS: OnceLock<Mutex<HashMap<String, BindingHandler>>> =
  OnceLock::new();
static JS_CALL_NOTIFY: OnceLock<Notify> = OnceLock::new();
static MENU_CLICK_HANDLER: OnceLock<
  Mutex<Option<Box<dyn Fn(&str) + Send + Sync>>>,
> = OnceLock::new();

enum BindingHandler {
  Sync(Box<dyn Fn(JsCall) + Send + Sync>),
  Async(
    Box<
      dyn Fn(JsCall) -> std::pin::Pin<Box<dyn Future<Output = ()> + Send>>
        + Send
        + Sync,
    >,
  ),
}

fn api() -> &'static WefBackendApi {
  BACKEND_API.get().expect("Backend API not initialized")
}

fn bindings() -> &'static Mutex<HashMap<String, BindingHandler>> {
  BINDINGS.get_or_init(|| Mutex::new(HashMap::new()))
}

fn js_call_notify() -> &'static Notify {
  JS_CALL_NOTIFY.get_or_init(Notify::new)
}

pub fn init_api(api: *const WefBackendApi) -> c_int {
  if api.is_null() {
    return -1;
  }
  let api_ref: &'static WefBackendApi = unsafe { &*api };
  if api_ref.version != WEF_API_VERSION {
    eprintln!(
      "API version mismatch: expected {}, got {}",
      WEF_API_VERSION, api_ref.version
    );
    return -2;
  }
  match BACKEND_API.set(api_ref) {
    Ok(_) => 0,
    Err(_) => -3,
  }
}

pub fn shutdown() {
  SHUTDOWN_FLAG.store(true, Ordering::SeqCst);
  // Wake up run() so it can exit
  if let Some(notify) = JS_CALL_NOTIFY.get() {
    notify.notify_one();
  }
}

pub fn should_shutdown() -> bool {
  SHUTDOWN_FLAG.load(Ordering::SeqCst)
}

pub enum Value {
  Null,
  Bool(bool),
  Int(i32),
  Double(f64),
  String(String),
  List(Vec<Value>),
  Dict(HashMap<String, Value>),
  Binary(Vec<u8>),
}

impl Value {
  pub unsafe fn from_raw(ptr: *mut WefValue) -> Option<Self> {
    if ptr.is_null() {
      return None;
    }
    let api = api();

    if api.value_is_null.map(|f| f(ptr)).unwrap_or(false) {
      return Some(Value::Null);
    }
    if api.value_is_bool.map(|f| f(ptr)).unwrap_or(false) {
      return Some(Value::Bool(
        api.value_get_bool.map(|f| f(ptr)).unwrap_or(false),
      ));
    }
    if api.value_is_int.map(|f| f(ptr)).unwrap_or(false) {
      return Some(Value::Int(api.value_get_int.map(|f| f(ptr)).unwrap_or(0)));
    }
    if api.value_is_double.map(|f| f(ptr)).unwrap_or(false) {
      return Some(Value::Double(
        api.value_get_double.map(|f| f(ptr)).unwrap_or(0.0),
      ));
    }
    if api.value_is_string.map(|f| f(ptr)).unwrap_or(false) {
      let mut len: usize = 0;
      if let Some(get_str) = api.value_get_string {
        let c_str = get_str(ptr, &mut len);
        if !c_str.is_null() {
          let s = CStr::from_ptr(c_str).to_string_lossy().into_owned();
          if let Some(free_str) = api.value_free_string {
            free_str(c_str);
          }
          return Some(Value::String(s));
        }
      }
      return Some(Value::String(String::new()));
    }
    if api.value_is_list.map(|f| f(ptr)).unwrap_or(false) {
      let size = api.value_list_size.map(|f| f(ptr)).unwrap_or(0);
      let mut list = Vec::with_capacity(size);
      if let Some(get_item) = api.value_list_get {
        for i in 0..size {
          let item = get_item(ptr, i);
          if let Some(v) = Value::from_raw(item) {
            list.push(v);
          }
        }
      }
      return Some(Value::List(list));
    }
    if api.value_is_dict.map(|f| f(ptr)).unwrap_or(false) {
      let mut dict = HashMap::new();
      let mut count: usize = 0;
      if let Some(get_keys) = api.value_dict_keys {
        let keys = get_keys(ptr, &mut count);
        if !keys.is_null() {
          for i in 0..count {
            let key_ptr = *keys.add(i);
            if !key_ptr.is_null() {
              let key = CStr::from_ptr(key_ptr).to_string_lossy().into_owned();
              if let Some(get_val) = api.value_dict_get {
                let c_key = CString::new(key.as_str()).unwrap();
                let val = get_val(ptr, c_key.as_ptr());
                if let Some(v) = Value::from_raw(val) {
                  dict.insert(key, v);
                }
              }
            }
          }
          if let Some(free_keys) = api.value_free_keys {
            free_keys(keys, count);
          }
        }
      }
      return Some(Value::Dict(dict));
    }
    if api.value_is_binary.map(|f| f(ptr)).unwrap_or(false) {
      let mut len: usize = 0;
      if let Some(get_bin) = api.value_get_binary {
        let data = get_bin(ptr, &mut len);
        if !data.is_null() && len > 0 {
          let slice = std::slice::from_raw_parts(data as *const u8, len);
          return Some(Value::Binary(slice.to_vec()));
        }
      }
      return Some(Value::Binary(Vec::new()));
    }

    Some(Value::Null)
  }

  pub fn to_raw(&self) -> *mut WefValue {
    let api = api();
    let bd = api.backend_data;

    unsafe {
      match self {
        Value::Null => api
          .value_null
          .map(|f| f(bd))
          .unwrap_or(std::ptr::null_mut()),
        Value::Bool(v) => api
          .value_bool
          .map(|f| f(bd, *v))
          .unwrap_or(std::ptr::null_mut()),
        Value::Int(v) => api
          .value_int
          .map(|f| f(bd, *v))
          .unwrap_or(std::ptr::null_mut()),
        Value::Double(v) => api
          .value_double
          .map(|f| f(bd, *v))
          .unwrap_or(std::ptr::null_mut()),
        Value::String(s) => {
          let c_str = CString::new(s.as_str()).unwrap();
          api
            .value_string
            .map(|f| f(bd, c_str.as_ptr()))
            .unwrap_or(std::ptr::null_mut())
        }
        Value::List(items) => {
          let list = api
            .value_list
            .map(|f| f(bd))
            .unwrap_or(std::ptr::null_mut());
          if !list.is_null() {
            if let Some(append) = api.value_list_append {
              for item in items {
                let raw = item.to_raw();
                append(list, raw);
              }
            }
          }
          list
        }
        Value::Dict(map) => {
          let dict = api
            .value_dict
            .map(|f| f(bd))
            .unwrap_or(std::ptr::null_mut());
          if !dict.is_null() {
            if let Some(set) = api.value_dict_set {
              for (k, v) in map {
                let c_key = CString::new(k.as_str()).unwrap();
                let raw = v.to_raw();
                set(dict, c_key.as_ptr(), raw);
              }
            }
          }
          dict
        }
        Value::Binary(data) => api
          .value_binary
          .map(|f| f(bd, data.as_ptr() as *const c_void, data.len()))
          .unwrap_or(std::ptr::null_mut()),
      }
    }
  }

  pub fn as_string(&self) -> Option<&str> {
    match self {
      Value::String(s) => Some(s.as_str()),
      _ => None,
    }
  }

  pub fn as_int(&self) -> Option<i32> {
    match self {
      Value::Int(i) => Some(*i),
      _ => None,
    }
  }

  pub fn as_bool(&self) -> Option<bool> {
    match self {
      Value::Bool(b) => Some(*b),
      _ => None,
    }
  }

  pub fn as_list(&self) -> Option<&Vec<Value>> {
    match self {
      Value::List(l) => Some(l),
      _ => None,
    }
  }

  pub fn as_dict(&self) -> Option<&HashMap<String, Value>> {
    match self {
      Value::Dict(d) => Some(d),
      _ => None,
    }
  }
}

pub struct JsCall {
  pub call_id: u64,
  pub method: String,
  pub args: Vec<Value>,
}

impl JsCall {
  pub fn resolve(self, value: Value) {
    let api = api();
    if let Some(respond) = api.js_call_respond {
      let raw = value.to_raw();
      unsafe {
        respond(api.backend_data, self.call_id, raw, std::ptr::null_mut())
      };
    }
  }

  pub fn reject(self, error: Value) {
    let api = api();
    if let Some(respond) = api.js_call_respond {
      let raw = error.to_raw();
      unsafe {
        respond(api.backend_data, self.call_id, std::ptr::null_mut(), raw)
      };
    }
  }
}

unsafe extern "C" fn js_call_handler(
  _user_data: *mut c_void,
  call_id: u64,
  method_path: *const c_char,
  args: *mut WefValue,
) {
  let method = if method_path.is_null() {
    String::new()
  } else {
    CStr::from_ptr(method_path).to_string_lossy().into_owned()
  };

  let args_vec = if args.is_null() {
    Vec::new()
  } else {
    match Value::from_raw(args) {
      Some(Value::List(l)) => l,
      _ => Vec::new(),
    }
  };

  let call = JsCall {
    call_id,
    method: method.clone(),
    args: args_vec,
  };

  let bindings = bindings().lock().unwrap();
  if let Some(handler) = bindings.get(&method) {
    match handler {
      BindingHandler::Sync(f) => f(call),
      BindingHandler::Async(f) => {
        let fut = f(call);
        tokio::spawn(fut);
      }
    }
  } else {
    drop(bindings);
    call.reject(Value::String(format!("No binding for '{}'", method)));
  }
}

fn register_js_handler() {
  let api = api();
  if let Some(set_handler) = api.set_js_call_handler {
    unsafe {
      set_handler(
        api.backend_data,
        Some(js_call_handler),
        std::ptr::null_mut(),
      );
    }
  }
}

unsafe extern "C" fn js_call_notify_callback(_user_data: *mut c_void) {
  js_call_notify().notify_one();
}

fn register_js_notify() {
  let api = api();
  if let Some(set_notify) = api.set_js_call_notify {
    unsafe {
      set_notify(
        api.backend_data,
        Some(js_call_notify_callback),
        std::ptr::null_mut(),
      );
    }
  }
}

fn ensure_js_handler() {
  static HANDLER_REGISTERED: AtomicBool = AtomicBool::new(false);
  if !HANDLER_REGISTERED.swap(true, Ordering::SeqCst) {
    register_js_handler();
    register_js_notify();
  }
}

/// Async event loop that dispatches JS calls as they arrive.
/// Blocks until `should_shutdown()` returns true.
pub async fn run() {
  ensure_js_handler();
  loop {
    js_call_notify().notified().await;
    poll_js_calls();
    if should_shutdown() {
      break;
    }
  }
}

pub struct Window;

impl Window {
  pub fn new(width: i32, height: i32) -> Self {
    let api = api();
    if let Some(f) = api.set_window_size {
      unsafe { f(api.backend_data, width, height) };
    }
    Window
  }

  pub fn title(self, title: &str) -> Self {
    let api = api();
    if let Some(f) = api.set_title {
      let c_title = CString::new(title).expect("Invalid title");
      unsafe { f(api.backend_data, c_title.as_ptr()) };
    }
    self
  }

  pub fn load(self, path: &str) -> Self {
    let api = api();
    if let Some(f) = api.navigate {
      let c_path = CString::new(path).expect("Invalid path");
      unsafe { f(api.backend_data, c_path.as_ptr()) };
    }
    self
  }

  pub fn size(self, width: i32, height: i32) -> Self {
    let api = api();
    if let Some(f) = api.set_window_size {
      unsafe { f(api.backend_data, width, height) };
    }
    self
  }

  pub fn get_size(&self) -> (i32, i32) {
    get_window_size()
  }

  pub fn position(self, x: i32, y: i32) -> Self {
    set_window_position(x, y);
    self
  }

  pub fn get_position(&self) -> (i32, i32) {
    get_window_position()
  }

  pub fn resizable(self, resizable: bool) -> Self {
    set_resizable(resizable);
    self
  }

  pub fn get_resizable(&self) -> bool {
    is_resizable()
  }

  pub fn always_on_top(self, always_on_top: bool) -> Self {
    set_always_on_top(always_on_top);
    self
  }

  pub fn get_always_on_top(&self) -> bool {
    is_always_on_top()
  }

  pub fn get_visible(&self) -> bool {
    is_visible()
  }

  pub fn show(&self) {
    show();
  }

  pub fn hide(&self) {
    hide();
  }

  pub fn focus(&self) {
    focus();
  }

  pub fn get_window_handle(&self) -> *mut c_void {
    get_window_handle()
  }

  pub fn get_display_handle(&self) -> *mut c_void {
    get_display_handle()
  }

  pub fn get_window_handle_type(&self) -> i32 {
    get_window_handle_type()
  }

  pub fn on_keyboard_event<F>(self, handler: F) -> Self
  where
    F: Fn(KeyboardEvent) + Send + Sync + 'static,
  {
    on_keyboard_event(handler);
    self
  }

  pub fn on_mouse_click<F>(self, handler: F) -> Self
  where
    F: Fn(MouseClickEvent) + Send + Sync + 'static,
  {
    on_mouse_click(handler);
    self
  }

  pub fn on_mouse_move<F>(self, handler: F) -> Self
  where
    F: Fn(MouseMoveEvent) + Send + Sync + 'static,
  {
    on_mouse_move(handler);
    self
  }

  pub fn on_wheel<F>(self, handler: F) -> Self
  where
    F: Fn(WheelEvent) + Send + Sync + 'static,
  {
    on_wheel(handler);
    self
  }

  pub fn on_cursor_enter_leave<F>(self, handler: F) -> Self
  where
    F: Fn(CursorEnterLeaveEvent) + Send + Sync + 'static,
  {
    on_cursor_enter_leave(handler);
    self
  }

  pub fn bind<F>(self, name: &str, handler: F) -> Self
  where
    F: Fn(JsCall) + Send + Sync + 'static,
  {
    ensure_js_handler();
    let mut bindings = bindings().lock().unwrap();
    bindings.insert(name.to_string(), BindingHandler::Sync(Box::new(handler)));
    self
  }

  pub fn bind_async<F, Fut>(self, name: &str, handler: F) -> Self
  where
    F: Fn(JsCall) -> Fut + Send + Sync + 'static,
    Fut: Future<Output = ()> + Send + 'static,
  {
    ensure_js_handler();
    let mut bindings = bindings().lock().unwrap();
    bindings.insert(
      name.to_string(),
      BindingHandler::Async(Box::new(move |call| Box::pin(handler(call)))),
    );
    self
  }
}

pub fn navigate(url: &str) {
  let api = api();
  if let Some(f) = api.navigate {
    let c_url = CString::new(url).expect("Invalid URL");
    unsafe { f(api.backend_data, c_url.as_ptr()) };
  }
}

pub fn set_title(title: &str) {
  let api = api();
  if let Some(f) = api.set_title {
    let c_title = CString::new(title).expect("Invalid title");
    unsafe { f(api.backend_data, c_title.as_ptr()) };
  }
}

pub fn execute_js<F>(script: &str, callback: Option<F>)
where
  F: FnOnce(Result<Value, Value>) + Send + 'static,
{
  let api = api();
  if let Some(f) = api.execute_js {
    let c_script = CString::new(script).expect("Invalid script");

    match callback {
      Some(cb_fn) => {
        unsafe extern "C" fn trampoline(
          result: *mut WefValue,
          error: *mut WefValue,
          user_data: *mut c_void,
        ) {
          let cb = Box::from_raw(
            user_data as *mut Box<dyn FnOnce(Result<Value, Value>) + Send>,
          );
          if !error.is_null() {
            if let Some(e) = Value::from_raw(error) {
              cb(Err(e));
              return;
            }
          }
          let val = Value::from_raw(result).unwrap_or(Value::Null);
          cb(Ok(val));
        }

        let cb: Box<Box<dyn FnOnce(Result<Value, Value>) + Send>> =
          Box::new(Box::new(cb_fn));
        let user_data = Box::into_raw(cb) as *mut c_void;

        unsafe {
          f(
            api.backend_data,
            c_script.as_ptr(),
            Some(trampoline),
            user_data,
          )
        };
      }
      None => {
        unsafe {
          f(
            api.backend_data,
            c_script.as_ptr(),
            None,
            std::ptr::null_mut(),
          )
        };
      }
    }
  }
}

pub fn quit() {
  let api = api();
  if let Some(f) = api.quit {
    unsafe { f(api.backend_data) };
  }
}

pub fn set_window_size(width: i32, height: i32) {
  let api = api();
  if let Some(f) = api.set_window_size {
    unsafe { f(api.backend_data, width, height) };
  }
}

pub fn get_window_size() -> (i32, i32) {
  let api = api();
  let mut width: c_int = 0;
  let mut height: c_int = 0;
  if let Some(f) = api.get_window_size {
    unsafe { f(api.backend_data, &mut width, &mut height) };
  }
  (width, height)
}

pub fn set_window_position(x: i32, y: i32) {
  let api = api();
  if let Some(f) = api.set_window_position {
    unsafe { f(api.backend_data, x, y) };
  }
}

pub fn get_window_position() -> (i32, i32) {
  let api = api();
  let mut x: c_int = 0;
  let mut y: c_int = 0;
  if let Some(f) = api.get_window_position {
    unsafe { f(api.backend_data, &mut x, &mut y) };
  }
  (x, y)
}

pub fn set_resizable(resizable: bool) {
  let api = api();
  if let Some(f) = api.set_resizable {
    unsafe { f(api.backend_data, resizable) };
  }
}

pub fn is_resizable() -> bool {
  let api = api();
  if let Some(f) = api.is_resizable {
    unsafe { f(api.backend_data) }
  } else {
    true
  }
}

pub fn set_always_on_top(always_on_top: bool) {
  let api = api();
  if let Some(f) = api.set_always_on_top {
    unsafe { f(api.backend_data, always_on_top) };
  }
}

pub fn is_always_on_top() -> bool {
  let api = api();
  if let Some(f) = api.is_always_on_top {
    unsafe { f(api.backend_data) }
  } else {
    false
  }
}

pub fn is_visible() -> bool {
  let api = api();
  if let Some(f) = api.is_visible {
    unsafe { f(api.backend_data) }
  } else {
    true
  }
}

pub fn show() {
  let api = api();
  if let Some(f) = api.show {
    unsafe { f(api.backend_data) };
  }
}

pub fn hide() {
  let api = api();
  if let Some(f) = api.hide {
    unsafe { f(api.backend_data) };
  }
}

pub fn focus() {
  let api = api();
  if let Some(f) = api.focus {
    unsafe { f(api.backend_data) };
  }
}

pub fn poll_js_calls() {
  let api = api();
  if let Some(f) = api.poll_js_calls {
    unsafe { f(api.backend_data) };
  }
}

pub fn bind<F>(name: &str, handler: F)
where
  F: Fn(JsCall) + Send + Sync + 'static,
{
  ensure_js_handler();
  let mut bindings = bindings().lock().unwrap();
  bindings.insert(name.to_string(), BindingHandler::Sync(Box::new(handler)));
}

pub fn bind_async<F, Fut>(name: &str, handler: F)
where
  F: Fn(JsCall) -> Fut + Send + Sync + 'static,
  Fut: Future<Output = ()> + Send + 'static,
{
  ensure_js_handler();
  let mut bindings = bindings().lock().unwrap();
  bindings.insert(
    name.to_string(),
    BindingHandler::Async(Box::new(move |call| Box::pin(handler(call)))),
  );
}

/// A menu item in an application menu template.
#[derive(Clone, Debug)]
pub enum MenuItem {
  /// A regular menu item with label and optional properties.
  Item {
    label: String,
    id: Option<String>,
    accelerator: Option<String>,
    enabled: bool,
  },
  /// A submenu containing child items.
  Submenu { label: String, items: Vec<MenuItem> },
  /// A separator line.
  Separator,
  /// A standard role-based item (quit, copy, paste, etc.)
  Role { role: String },
}

impl MenuItem {
  fn to_value(&self) -> Value {
    match self {
      MenuItem::Item {
        label,
        id,
        accelerator,
        enabled,
      } => {
        let mut dict = HashMap::new();
        dict.insert("label".to_string(), Value::String(label.clone()));
        if let Some(id) = id {
          dict.insert("id".to_string(), Value::String(id.clone()));
        }
        if let Some(accel) = accelerator {
          dict.insert("accelerator".to_string(), Value::String(accel.clone()));
        }
        if !enabled {
          dict.insert("enabled".to_string(), Value::Bool(false));
        }
        Value::Dict(dict)
      }
      MenuItem::Submenu { label, items } => {
        let mut dict = HashMap::new();
        dict.insert("label".to_string(), Value::String(label.clone()));
        dict.insert(
          "submenu".to_string(),
          Value::List(items.iter().map(|i| i.to_value()).collect()),
        );
        Value::Dict(dict)
      }
      MenuItem::Separator => {
        let mut dict = HashMap::new();
        dict.insert("type".to_string(), Value::String("separator".to_string()));
        Value::Dict(dict)
      }
      MenuItem::Role { role } => {
        let mut dict = HashMap::new();
        dict.insert("role".to_string(), Value::String(role.clone()));
        Value::Dict(dict)
      }
    }
  }
}

unsafe extern "C" fn menu_click_callback(
  _user_data: *mut c_void,
  item_id: *const c_char,
) {
  if item_id.is_null() {
    return;
  }
  let id = CStr::from_ptr(item_id).to_string_lossy();
  let handler = MENU_CLICK_HANDLER
    .get_or_init(|| Mutex::new(None))
    .lock()
    .unwrap();
  if let Some(ref f) = *handler {
    f(&id);
  }
}

/// Set the application menu from a template and register a click handler.
pub fn set_application_menu<F>(template: &[MenuItem], on_click: F)
where
  F: Fn(&str) + Send + Sync + 'static,
{
  {
    let mut handler = MENU_CLICK_HANDLER
      .get_or_init(|| Mutex::new(None))
      .lock()
      .unwrap();
    *handler = Some(Box::new(on_click));
  }

  let api = api();
  if let Some(f) = api.set_application_menu {
    let value = Value::List(template.iter().map(|i| i.to_value()).collect());
    let raw = value.to_raw();
    unsafe {
      f(
        api.backend_data,
        raw,
        Some(menu_click_callback),
        std::ptr::null_mut(),
      );
    }
  }
}

/// Set the menu click handler (called when a custom menu item is clicked).
pub fn set_menu_click_handler(handler: impl Fn(&str) + Send + Sync + 'static) {
  let mut h = MENU_CLICK_HANDLER
    .get_or_init(|| Mutex::new(None))
    .lock()
    .unwrap();
  *h = Some(Box::new(handler));
}

/// Set the application menu from a raw `Value` template.
pub fn set_application_menu_raw(template: Value) {
  let api = api();
  if let Some(f) = api.set_application_menu {
    let raw = template.to_raw();
    unsafe {
      f(
        api.backend_data,
        raw,
        Some(menu_click_callback),
        std::ptr::null_mut(),
      );
    }
  }
}

/// Returns the raw platform-specific window handle (e.g. NSView*, HWND, X11 Window, wl_surface*).
/// Returns null if the backend does not support this.
pub fn get_window_handle() -> *mut c_void {
  let api = api();
  if let Some(f) = api.get_window_handle {
    unsafe { f(api.backend_data) }
  } else {
    std::ptr::null_mut()
  }
}

/// Returns the raw platform-specific display handle (e.g. X11 Display*, wl_display*).
/// Returns null on platforms that don't need one (macOS, Windows).
pub fn get_display_handle() -> *mut c_void {
  let api = api();
  if let Some(f) = api.get_display_handle {
    unsafe { f(api.backend_data) }
  } else {
    std::ptr::null_mut()
  }
}

/// Returns the window handle type as a `WEF_WINDOW_HANDLE_*` constant.
pub fn get_window_handle_type() -> i32 {
  let api = api();
  if let Some(f) = api.get_window_handle_type {
    unsafe { f(api.backend_data) }
  } else {
    WEF_WINDOW_HANDLE_UNKNOWN
  }
}

pub const WEF_KEY_PRESSED: i32 = 0;
pub const WEF_KEY_RELEASED: i32 = 1;

pub const WEF_MOD_SHIFT: u32 = 1 << 0;
pub const WEF_MOD_CONTROL: u32 = 1 << 1;
pub const WEF_MOD_ALT: u32 = 1 << 2;
pub const WEF_MOD_META: u32 = 1 << 3;

#[derive(Debug, Clone, Copy, Default)]
pub struct KeyModifiers {
  pub shift: bool,
  pub control: bool,
  pub alt: bool,
  pub meta: bool,
}

impl KeyModifiers {
  pub(crate) fn from_raw(flags: u32) -> Self {
    Self {
      shift: flags & WEF_MOD_SHIFT != 0,
      control: flags & WEF_MOD_CONTROL != 0,
      alt: flags & WEF_MOD_ALT != 0,
      meta: flags & WEF_MOD_META != 0,
    }
  }
}

pub fn unbind(name: &str) {
  static HANDLER_REGISTERED: AtomicBool = AtomicBool::new(false);
  if !HANDLER_REGISTERED.swap(true, Ordering::SeqCst) {
    register_js_handler();
  }

  let mut bindings = bindings().lock().unwrap();
  bindings.remove(&name.to_string());
}

#[macro_export]
macro_rules! main {
  ($main_fn:expr) => {
    #[no_mangle]
    pub extern "C" fn wef_runtime_init(
      api: *const $crate::WefBackendApi,
    ) -> std::ffi::c_int {
      $crate::init_api(api)
    }

    #[no_mangle]
    pub extern "C" fn wef_runtime_start() -> std::ffi::c_int {
      let main_fn: fn() = $main_fn;
      main_fn();
      0
    }

    #[no_mangle]
    pub extern "C" fn wef_runtime_shutdown() {
      $crate::shutdown();
    }
  };
}
