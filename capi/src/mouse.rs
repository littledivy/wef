// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

use std::ffi::c_int;
use std::ffi::c_void;
use std::sync::{Mutex, OnceLock};

use crate::{api, KeyModifiers};

pub const WEF_MOUSE_BUTTON_LEFT: i32 = 0;
pub const WEF_MOUSE_BUTTON_RIGHT: i32 = 1;
pub const WEF_MOUSE_BUTTON_MIDDLE: i32 = 2;
pub const WEF_MOUSE_BUTTON_BACK: i32 = 3;
pub const WEF_MOUSE_BUTTON_FORWARD: i32 = 4;

pub const WEF_MOUSE_PRESSED: i32 = 0;
pub const WEF_MOUSE_RELEASED: i32 = 1;

pub const WEF_WHEEL_DELTA_PIXEL: i32 = 0;
pub const WEF_WHEEL_DELTA_LINE: i32 = 1;
pub const WEF_WHEEL_DELTA_PAGE: i32 = 2;

#[derive(Debug, Clone)]
pub struct MouseClickEvent {
  pub state: MouseButtonState,
  pub button: MouseButton,
  pub x: f64,
  pub y: f64,
  pub modifiers: KeyModifiers,
  /// 1 = single click, 2 = double click.
  pub click_count: i32,
}

impl MouseClickEvent {
  /// True when this is a "click" event (primary button released).
  pub fn is_click(&self) -> bool {
    self.state == MouseButtonState::Released
      && self.button == MouseButton::Left
      && self.click_count >= 1
  }

  /// True when this is a "dblclick" event (primary button released, double click).
  pub fn is_double_click(&self) -> bool {
    self.state == MouseButtonState::Released
      && self.button == MouseButton::Left
      && self.click_count >= 2
  }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MouseButtonState {
  Pressed,
  Released,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MouseButton {
  Left,
  Right,
  Middle,
  Back,
  Forward,
  Other(i32),
}

impl MouseButton {
  fn from_raw(button: i32) -> Self {
    match button {
      WEF_MOUSE_BUTTON_LEFT => MouseButton::Left,
      WEF_MOUSE_BUTTON_RIGHT => MouseButton::Right,
      WEF_MOUSE_BUTTON_MIDDLE => MouseButton::Middle,
      WEF_MOUSE_BUTTON_BACK => MouseButton::Back,
      WEF_MOUSE_BUTTON_FORWARD => MouseButton::Forward,
      other => MouseButton::Other(other),
    }
  }
}

static MOUSE_CLICK_HANDLER: OnceLock<
  Mutex<Option<Box<dyn Fn(MouseClickEvent) + Send + Sync>>>,
> = OnceLock::new();

fn mouse_click_handler_store(
) -> &'static Mutex<Option<Box<dyn Fn(MouseClickEvent) + Send + Sync>>> {
  MOUSE_CLICK_HANDLER.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn mouse_click_trampoline(
  _user_data: *mut c_void,
  state: c_int,
  button: c_int,
  x: f64,
  y: f64,
  modifiers: u32,
  click_count: i32,
) {
  let event = MouseClickEvent {
    state: if state == WEF_MOUSE_PRESSED {
      MouseButtonState::Pressed
    } else {
      MouseButtonState::Released
    },
    button: MouseButton::from_raw(button),
    x,
    y,
    modifiers: KeyModifiers::from_raw(modifiers),
    click_count,
  };

  let guard = mouse_click_handler_store().lock().unwrap();
  if let Some(handler) = guard.as_ref() {
    handler(event);
  }
}

/// Register a handler for mouse click events.
pub fn on_mouse_click<F>(handler: F)
where
  F: Fn(MouseClickEvent) + Send + Sync + 'static,
{
  *mouse_click_handler_store().lock().unwrap() = Some(Box::new(handler));

  let api = api();
  if let Some(set_handler) = api.set_mouse_click_handler {
    unsafe {
      set_handler(
        api.backend_data,
        Some(mouse_click_trampoline),
        std::ptr::null_mut(),
      );
    }
  }
}

#[derive(Debug, Clone, Copy)]
pub struct MouseMoveEvent {
  pub x: f64,
  pub y: f64,
  pub modifiers: KeyModifiers,
}

static MOUSE_MOVE_HANDLER: OnceLock<
  Mutex<Option<Box<dyn Fn(MouseMoveEvent) + Send + Sync>>>,
> = OnceLock::new();

fn mouse_move_handler_store(
) -> &'static Mutex<Option<Box<dyn Fn(MouseMoveEvent) + Send + Sync>>> {
  MOUSE_MOVE_HANDLER.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn mouse_move_trampoline(
  _user_data: *mut c_void,
  x: f64,
  y: f64,
  modifiers: u32,
) {
  let event = MouseMoveEvent {
    x,
    y,
    modifiers: KeyModifiers::from_raw(modifiers),
  };

  let guard = mouse_move_handler_store().lock().unwrap();
  if let Some(handler) = guard.as_ref() {
    handler(event);
  }
}

/// Register a handler for mouse move events.
pub fn on_mouse_move<F>(handler: F)
where
  F: Fn(MouseMoveEvent) + Send + Sync + 'static,
{
  *mouse_move_handler_store().lock().unwrap() = Some(Box::new(handler));

  let api = api();
  if let Some(set_handler) = api.set_mouse_move_handler {
    unsafe {
      set_handler(
        api.backend_data,
        Some(mouse_move_trampoline),
        std::ptr::null_mut(),
      );
    }
  }
}

// --- Wheel events ---

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum WheelDeltaMode {
  Pixel,
  Line,
  Page,
}

#[derive(Debug, Clone, Copy)]
pub struct WheelEvent {
  pub delta_x: f64,
  pub delta_y: f64,
  pub x: f64,
  pub y: f64,
  pub modifiers: KeyModifiers,
  pub delta_mode: WheelDeltaMode,
}

static WHEEL_HANDLER: OnceLock<
  Mutex<Option<Box<dyn Fn(WheelEvent) + Send + Sync>>>,
> = OnceLock::new();

fn wheel_handler_store(
) -> &'static Mutex<Option<Box<dyn Fn(WheelEvent) + Send + Sync>>> {
  WHEEL_HANDLER.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn wheel_trampoline(
  _user_data: *mut c_void,
  delta_x: f64,
  delta_y: f64,
  x: f64,
  y: f64,
  modifiers: u32,
  delta_mode: i32,
) {
  let event = WheelEvent {
    delta_x,
    delta_y,
    x,
    y,
    modifiers: KeyModifiers::from_raw(modifiers),
    delta_mode: match delta_mode {
      WEF_WHEEL_DELTA_LINE => WheelDeltaMode::Line,
      WEF_WHEEL_DELTA_PAGE => WheelDeltaMode::Page,
      _ => WheelDeltaMode::Pixel,
    },
  };

  let guard = wheel_handler_store().lock().unwrap();
  if let Some(handler) = guard.as_ref() {
    handler(event);
  }
}

/// Register a handler for wheel (scroll) events.
pub fn on_wheel<F>(handler: F)
where
  F: Fn(WheelEvent) + Send + Sync + 'static,
{
  *wheel_handler_store().lock().unwrap() = Some(Box::new(handler));

  let api = api();
  if let Some(set_handler) = api.set_wheel_handler {
    unsafe {
      set_handler(
        api.backend_data,
        Some(wheel_trampoline),
        std::ptr::null_mut(),
      );
    }
  }
}

// --- Cursor enter/leave events ---

#[derive(Debug, Clone, Copy)]
pub struct CursorEnterLeaveEvent {
  /// True when the cursor entered the window, false when it left.
  pub entered: bool,
  pub x: f64,
  pub y: f64,
  pub modifiers: KeyModifiers,
}

static CURSOR_ENTER_LEAVE_HANDLER: OnceLock<
  Mutex<Option<Box<dyn Fn(CursorEnterLeaveEvent) + Send + Sync>>>,
> = OnceLock::new();

fn cursor_enter_leave_handler_store(
) -> &'static Mutex<Option<Box<dyn Fn(CursorEnterLeaveEvent) + Send + Sync>>>
{
  CURSOR_ENTER_LEAVE_HANDLER.get_or_init(|| Mutex::new(None))
}

unsafe extern "C" fn cursor_enter_leave_trampoline(
  _user_data: *mut c_void,
  entered: c_int,
  x: f64,
  y: f64,
  modifiers: u32,
) {
  let event = CursorEnterLeaveEvent {
    entered: entered != 0,
    x,
    y,
    modifiers: KeyModifiers::from_raw(modifiers),
  };

  let guard = cursor_enter_leave_handler_store().lock().unwrap();
  if let Some(handler) = guard.as_ref() {
    handler(event);
  }
}

/// Register a handler for cursor enter/leave events.
pub fn on_cursor_enter_leave<F>(handler: F)
where
  F: Fn(CursorEnterLeaveEvent) + Send + Sync + 'static,
{
  *cursor_enter_leave_handler_store().lock().unwrap() =
    Some(Box::new(handler));

  let api = api();
  if let Some(set_handler) = api.set_cursor_enter_leave_handler {
    unsafe {
      set_handler(
        api.backend_data,
        Some(cursor_enter_leave_trampoline),
        std::ptr::null_mut(),
      );
    }
  }
}
