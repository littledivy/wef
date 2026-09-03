// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::collections::HashMap;
use std::ffi::{c_char, c_int, c_void, CStr};
use std::sync::{Mutex, OnceLock};

use crate::{api, LAUFEY_IME_START, LAUFEY_IME_UPDATE};

#[derive(Debug, Clone)]
pub struct ImeEvent {
  pub window_id: u32,
  pub state: ImeState,
  pub data: String,
}

/// Maps onto the W3C `compositionstart` / `compositionupdate` /
/// `compositionend` sequence. `data` is the current composition string
/// (empty on start, and empty on end when the session is cancelled).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ImeState {
  Start,
  Update,
  End,
}

impl ImeState {
  pub(crate) fn from_raw(raw: c_int) -> Self {
    if raw == LAUFEY_IME_START {
      Self::Start
    } else if raw == LAUFEY_IME_UPDATE {
      Self::Update
    } else {
      Self::End
    }
  }
}

static IME_HANDLERS: OnceLock<
  Mutex<HashMap<u32, Box<dyn Fn(ImeEvent) + Send + Sync>>>,
> = OnceLock::new();

fn ime_handlers_store(
) -> &'static Mutex<HashMap<u32, Box<dyn Fn(ImeEvent) + Send + Sync>>> {
  IME_HANDLERS.get_or_init(|| Mutex::new(HashMap::new()))
}

static HANDLER_REGISTERED: std::sync::atomic::AtomicBool =
  std::sync::atomic::AtomicBool::new(false);

fn ensure_ime_handler_registered() {
  if !HANDLER_REGISTERED.swap(true, std::sync::atomic::Ordering::SeqCst) {
    let api = api();
    if let Some(set_handler) = api.set_ime_event_handler {
      unsafe {
        set_handler(
          api.backend_data,
          Some(ime_event_trampoline),
          std::ptr::null_mut(),
        );
      }
    }
  }
}

unsafe extern "C" fn ime_event_trampoline(
  _user_data: *mut c_void,
  window_id: u32,
  state: c_int,
  data: *const c_char,
) {
  let data_str = if data.is_null() {
    String::new()
  } else {
    CStr::from_ptr(data).to_string_lossy().into_owned()
  };

  let event = ImeEvent {
    window_id,
    state: ImeState::from_raw(state),
    data: data_str,
  };

  let guard = ime_handlers_store().lock().unwrap();
  if let Some(handler) = guard.get(&window_id) {
    handler(event);
  }
}

/// Register a handler for IME composition events on a specific window.
/// Observed like keyboard events; the engine still owns composition.
pub fn on_ime_event<F>(window_id: u32, handler: F)
where
  F: Fn(ImeEvent) + Send + Sync + 'static,
{
  ensure_ime_handler_registered();
  ime_handlers_store()
    .lock()
    .unwrap()
    .insert(window_id, Box::new(handler));
}

#[cfg(test)]
mod tests {
  use super::*;
  use crate::LAUFEY_IME_END;

  #[test]
  fn ime_state_from_raw_known_values() {
    assert_eq!(ImeState::from_raw(LAUFEY_IME_START), ImeState::Start);
    assert_eq!(ImeState::from_raw(LAUFEY_IME_UPDATE), ImeState::Update);
    assert_eq!(ImeState::from_raw(LAUFEY_IME_END), ImeState::End);
  }

  #[test]
  fn ime_state_unknown_defaults_to_end() {
    // The trampoline collapses unknown raw values to End — pinning so a
    // future extra state can't be misread as Start or Update.
    assert_eq!(ImeState::from_raw(42), ImeState::End);
    assert_eq!(ImeState::from_raw(-1), ImeState::End);
  }

  #[test]
  fn ime_event_field_passthrough() {
    let ev = ImeEvent {
      window_id: 7,
      state: ImeState::Update,
      data: "あ".to_string(),
    };
    assert_eq!(ev.window_id, 7);
    assert_eq!(ev.state, ImeState::Update);
    assert_eq!(ev.data, "あ");
  }
}
