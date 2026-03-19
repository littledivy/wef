// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef WEF_RUNTIME_LOADER_H_
#define WEF_RUNTIME_LOADER_H_

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>

#include "include/cef_browser.h"
#include "include/cef_values.h"
#include <wef.h>

struct wef_value {
  CefRefPtr<CefValue> value;
  bool is_callback;
  uint64_t callback_id;

  wef_value() : is_callback(false), callback_id(0) {}
  explicit wef_value(CefRefPtr<CefValue> v) : value(v), is_callback(false), callback_id(0) {}
  static wef_value* CreateCallback(uint64_t id) {
    wef_value* v = new wef_value();
    v->is_callback = true;
    v->callback_id = id;
    return v;
  }
};

class RuntimeLoader {
 public:
  static RuntimeLoader* GetInstance();

  bool Load(const std::string& path);

  bool Start();

  void Shutdown();

  void SetBrowser(CefRefPtr<CefBrowser> browser);

  CefRefPtr<CefBrowser> GetBrowser();

  const wef_backend_api_t& GetBackendApi() const { return backend_api_; }

  void OnJsCall(uint64_t call_id, const std::string& method_path,
                CefRefPtr<CefListValue> args);

  void PollPendingJsCalls();

  void SetJsCallHandler(wef_js_call_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    js_call_handler_ = handler;
    js_call_user_data_ = user_data;
  }

  void SetKeyboardEventHandler(wef_keyboard_event_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(keyboard_mutex_);
    keyboard_handler_ = handler;
    keyboard_user_data_ = user_data;
  }

  void DispatchKeyboardEvent(int state, const char* key, const char* code,
                             uint32_t modifiers, bool repeat) {
    std::lock_guard<std::mutex> lock(keyboard_mutex_);
    if (keyboard_handler_) {
      keyboard_handler_(keyboard_user_data_, state, key, code, modifiers, repeat);
    }
  }

  void SetMouseClickHandler(wef_mouse_click_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(mouse_mutex_);
    mouse_click_handler_ = handler;
    mouse_click_user_data_ = user_data;
  }

  void DispatchMouseClickEvent(int state, int button, double x, double y,
                               uint32_t modifiers, int32_t click_count) {
    std::lock_guard<std::mutex> lock(mouse_mutex_);
    if (mouse_click_handler_) {
      mouse_click_handler_(mouse_click_user_data_, state, button, x, y, modifiers, click_count);
    }
  }

  void SetMouseMoveHandler(wef_mouse_move_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(mouse_move_mutex_);
    mouse_move_handler_ = handler;
    mouse_move_user_data_ = user_data;
  }

  void DispatchMouseMoveEvent(double x, double y, uint32_t modifiers) {
    std::lock_guard<std::mutex> lock(mouse_move_mutex_);
    if (mouse_move_handler_) {
      mouse_move_handler_(mouse_move_user_data_, x, y, modifiers);
    }
  }

  void SetWheelHandler(wef_wheel_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(wheel_mutex_);
    wheel_handler_ = handler;
    wheel_user_data_ = user_data;
  }

  void DispatchWheelEvent(double delta_x, double delta_y, double x, double y,
                          uint32_t modifiers, int32_t delta_mode) {
    std::lock_guard<std::mutex> lock(wheel_mutex_);
    if (wheel_handler_) {
      wheel_handler_(wheel_user_data_, delta_x, delta_y, x, y, modifiers, delta_mode);
    }
  }

  void SetCursorEnterLeaveHandler(wef_cursor_enter_leave_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(cursor_enter_leave_mutex_);
    cursor_enter_leave_handler_ = handler;
    cursor_enter_leave_user_data_ = user_data;
  }

  void DispatchCursorEnterLeaveEvent(int entered, double x, double y,
                                     uint32_t modifiers) {
    std::lock_guard<std::mutex> lock(cursor_enter_leave_mutex_);
    if (cursor_enter_leave_handler_) {
      cursor_enter_leave_handler_(cursor_enter_leave_user_data_, entered, x, y, modifiers);
    }
  }

  void SetJsCallNotify(void (*notify_fn)(void*), void* notify_data) {
    std::lock_guard<std::mutex> lock(notify_mutex_);
    js_call_notify_fn_ = notify_fn;
    js_call_notify_data_ = notify_data;
  }

 private:
  RuntimeLoader();
  ~RuntimeLoader();

  void RuntimeThread();
  void InitializeBackendApi();

  void* library_handle_ = nullptr;
  wef_runtime_init_fn init_fn_ = nullptr;
  wef_runtime_start_fn start_fn_ = nullptr;
  wef_runtime_shutdown_fn shutdown_fn_ = nullptr;

  std::thread runtime_thread_;
  std::atomic<bool> running_{false};

  CefRefPtr<CefBrowser> browser_;
  wef_backend_api_t backend_api_;

  wef_js_call_fn js_call_handler_ = nullptr;
  void* js_call_user_data_ = nullptr;
  std::mutex handler_mutex_;

  wef_keyboard_event_fn keyboard_handler_ = nullptr;
  void* keyboard_user_data_ = nullptr;
  std::mutex keyboard_mutex_;

  wef_mouse_click_fn mouse_click_handler_ = nullptr;
  void* mouse_click_user_data_ = nullptr;
  std::mutex mouse_mutex_;

  wef_mouse_move_fn mouse_move_handler_ = nullptr;
  void* mouse_move_user_data_ = nullptr;
  std::mutex mouse_move_mutex_;

  wef_wheel_fn wheel_handler_ = nullptr;
  void* wheel_user_data_ = nullptr;
  std::mutex wheel_mutex_;

  wef_cursor_enter_leave_fn cursor_enter_leave_handler_ = nullptr;
  void* cursor_enter_leave_user_data_ = nullptr;
  std::mutex cursor_enter_leave_mutex_;

  void (*js_call_notify_fn_)(void*) = nullptr;
  void* js_call_notify_data_ = nullptr;
  std::mutex notify_mutex_;

  struct PendingJsCall {
    uint64_t call_id;
    std::string method_path;
    CefRefPtr<CefListValue> args;
  };
  std::queue<PendingJsCall> pending_js_calls_;
  std::mutex pending_mutex_;

  static RuntimeLoader* instance_;
};

// Platform-specific native event monitor hooks.
// Implemented in the platform-specific main file (e.g. main_mac.mm).
void InstallNativeMouseMonitor();
void RemoveNativeMouseMonitor();

#endif
