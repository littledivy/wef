// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef WEF_RUNTIME_LOADER_H_
#define WEF_RUNTIME_LOADER_H_

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <map>

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

  void OnJsCall(uint64_t call_id, const std::string& method_path,
                CefRefPtr<CefListValue> args);

  wef_js_call_fn GetJsCallHandler() const { return js_call_handler_; }
  void* GetJsCallUserData() const { return js_call_user_data_; }

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
                               uint32_t modifiers) {
    std::lock_guard<std::mutex> lock(mouse_mutex_);
    if (mouse_click_handler_) {
      mouse_click_handler_(mouse_click_user_data_, state, button, x, y, modifiers);
    }
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

  static RuntimeLoader* instance_;
};

// Platform-specific native event monitor hooks.
// Implemented in the platform-specific main file (e.g. main_mac.mm).
void InstallNativeMouseMonitor();
void RemoveNativeMouseMonitor();

#endif
