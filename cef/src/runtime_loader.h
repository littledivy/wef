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

#endif
