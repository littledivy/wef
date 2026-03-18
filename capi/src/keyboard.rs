// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::ffi::{c_char, c_int, c_void, CStr};
use std::sync::{Mutex, OnceLock};

use crate::{api, KeyModifiers, WEF_KEY_PRESSED};

#[derive(Debug, Clone)]
pub struct KeyboardEvent {
    pub state: KeyState,
    pub key: String,
    pub code: String,
    pub modifiers: KeyModifiers,
    pub repeat: bool,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum KeyState {
    Pressed,
    Released,
}

static KEYBOARD_HANDLER: OnceLock<Mutex<Option<Box<dyn Fn(KeyboardEvent) + Send + Sync>>>> =
    OnceLock::new();

fn keyboard_handler_store(
) -> &'static Mutex<Option<Box<dyn Fn(KeyboardEvent) + Send + Sync>>> {
    KEYBOARD_HANDLER.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn keyboard_event_trampoline(
    _user_data: *mut c_void,
    state: c_int,
    key: *const c_char,
    code: *const c_char,
    modifiers: u32,
    repeat: bool,
) {
    let key_str = if key.is_null() {
        String::new()
    } else {
        CStr::from_ptr(key).to_string_lossy().into_owned()
    };
    let code_str = if code.is_null() {
        String::new()
    } else {
        CStr::from_ptr(code).to_string_lossy().into_owned()
    };

    let event = KeyboardEvent {
        state: if state == WEF_KEY_PRESSED {
            KeyState::Pressed
        } else {
            KeyState::Released
        },
        key: key_str,
        code: code_str,
        modifiers: KeyModifiers::from_raw(modifiers),
        repeat,
    };

    let guard = keyboard_handler_store().lock().unwrap();
    if let Some(handler) = guard.as_ref() {
        handler(event);
    }
}

/// Register a handler for keyboard input events.
pub fn on_keyboard_event<F>(handler: F)
where
    F: Fn(KeyboardEvent) + Send + Sync + 'static,
{
    *keyboard_handler_store().lock().unwrap() = Some(Box::new(handler));

    let api = api();
    if let Some(set_handler) = api.set_keyboard_event_handler {
        unsafe {
            set_handler(
                api.backend_data,
                Some(keyboard_event_trampoline),
                std::ptr::null_mut(),
            );
        }
    }
}
