// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_void, CStr, CString};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Mutex, OnceLock};

#[allow(non_upper_case_globals)]
#[allow(non_camel_case_types)]
#[allow(non_snake_case)]
#[allow(dead_code)]
mod ffi {
    include!(concat!(env!("OUT_DIR"), "/bindings.rs"));
}

pub const WEF_API_VERSION: u32 = 1;
pub type WefValue = ffi::wef_value_t;
pub type WefBackendApi = ffi::wef_backend_api_t;

unsafe impl Send for WefBackendApi {}
unsafe impl Sync for WefBackendApi {}

static BACKEND_API: OnceLock<&'static WefBackendApi> = OnceLock::new();
static SHUTDOWN_FLAG: AtomicBool = AtomicBool::new(false);
static BINDINGS: OnceLock<Mutex<HashMap<String, Box<dyn Fn(JsCall) + Send + Sync>>>> =
    OnceLock::new();

fn api() -> &'static WefBackendApi {
    BACKEND_API.get().expect("Backend API not initialized")
}

fn bindings() -> &'static Mutex<HashMap<String, Box<dyn Fn(JsCall) + Send + Sync>>> {
    BINDINGS.get_or_init(|| Mutex::new(HashMap::new()))
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
                    api.value_string
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
    call_id: u64,
    pub method: String,
    pub args: Vec<Value>,
}

impl JsCall {
    pub fn resolve(self, value: Value) {
        let api = api();
        if let Some(respond) = api.js_call_respond {
            let raw = value.to_raw();
            unsafe { respond(api.backend_data, self.call_id, raw, std::ptr::null()) };
        }
    }

    pub fn reject(self, error: &str) {
        let api = api();
        if let Some(respond) = api.js_call_respond {
            let c_err = CString::new(error).unwrap();
            unsafe {
                respond(
                    api.backend_data,
                    self.call_id,
                    std::ptr::null_mut(),
                    c_err.as_ptr(),
                )
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
        handler(call);
    } else {
        drop(bindings);
        call.reject(&format!("No binding for '{}'", method));
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

    pub fn load_url(self, url: &str) -> Self {
        let api = api();
        if let Some(f) = api.navigate {
            let c_url = CString::new(url).expect("Invalid URL");
            unsafe { f(api.backend_data, c_url.as_ptr()) };
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

    pub fn bind<F>(self, name: &str, handler: F) -> Self
    where
        F: Fn(JsCall) + Send + Sync + 'static,
    {
        static HANDLER_REGISTERED: AtomicBool = AtomicBool::new(false);
        if !HANDLER_REGISTERED.swap(true, Ordering::SeqCst) {
            register_js_handler();
        }

        let mut bindings = bindings().lock().unwrap();
        bindings.insert(name.to_string(), Box::new(handler));
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

pub fn execute_js(script: &str) {
    let api = api();
    if let Some(f) = api.execute_js {
        let c_script = CString::new(script).expect("Invalid script");
        unsafe { f(api.backend_data, c_script.as_ptr()) };
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

pub fn bind<F>(name: &str, handler: F)
where
    F: Fn(JsCall) + Send + Sync + 'static,
{
    static HANDLER_REGISTERED: AtomicBool = AtomicBool::new(false);
    if !HANDLER_REGISTERED.swap(true, Ordering::SeqCst) {
        register_js_handler();
    }

    let mut bindings = bindings().lock().unwrap();
    bindings.insert(name.to_string(), Box::new(handler));
}

#[macro_export]
macro_rules! main {
    ($main_fn:expr) => {
        #[no_mangle]
        pub extern "C" fn wef_runtime_init(api: *const $crate::WefBackendApi) -> std::ffi::c_int {
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
