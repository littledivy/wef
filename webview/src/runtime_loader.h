// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef WEF_RUNTIME_LOADER_H_
#define WEF_RUNTIME_LOADER_H_

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <queue>

#include "wef.h"
#include "webview_value.h"

class WefBackend;

class RuntimeLoader {
 public:
  static RuntimeLoader* GetInstance();

  bool Load(const std::string& path);

  bool Start();

  void Shutdown();

  void SetBackend(WefBackend* backend) { backend_ = backend; }
  WefBackend* GetBackend() { return backend_; }
  const wef_backend_api_t& GetBackendApi() const { return backend_api_; }

  void OnJsCall(uint64_t call_id, const std::string& method_path,
                wef::ValuePtr args);

  void PollPendingJsCalls();

  void JsCallRespond(uint64_t call_id, wef::ValuePtr result, wef::ValuePtr error);

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

  WefBackend* backend_ = nullptr;
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

  void (*js_call_notify_fn_)(void*) = nullptr;
  void* js_call_notify_data_ = nullptr;
  std::mutex notify_mutex_;

  struct PendingJsCall {
    uint64_t call_id;
    std::string method_path;
    wef::ValuePtr args;
  };
  std::queue<PendingJsCall> pending_js_calls_;
  std::mutex pending_mutex_;

  static RuntimeLoader* instance_;
};

class WefBackend {
 public:
  virtual ~WefBackend() = default;

  virtual void Navigate(const std::string& url) = 0;
  virtual void SetTitle(const std::string& title) = 0;
  virtual void ExecuteJs(const std::string& script) = 0;
  virtual void Quit() = 0;
  virtual void SetWindowSize(int width, int height) = 0;
  virtual void GetWindowSize(int* width, int* height) = 0;
  virtual void SetWindowPosition(int x, int y) = 0;
  virtual void GetWindowPosition(int* x, int* y) = 0;
  virtual void SetResizable(bool resizable) = 0;
  virtual bool IsResizable() = 0;
  virtual void SetAlwaysOnTop(bool always_on_top) = 0;
  virtual bool IsAlwaysOnTop() = 0;
  virtual bool IsVisible() = 0;
  virtual void Show() = 0;
  virtual void Hide() = 0;
  virtual void Focus() = 0;
  virtual void PostUiTask(void (*task)(void*), void* data) = 0;

  virtual void InvokeJsCallback(uint64_t callback_id, wef::ValuePtr args) = 0;
  virtual void ReleaseJsCallback(uint64_t callback_id) = 0;

  virtual void RespondToJsCall(uint64_t call_id, wef::ValuePtr result, wef::ValuePtr error) = 0;

  virtual void Run() = 0;

  virtual void SetApplicationMenu(wef_value_t* menu_template,
                                  const wef_backend_api_t* api,
                                  wef_menu_click_fn on_click,
                                  void* on_click_data) = 0;
};

WefBackend* CreateWefBackend(int width, int height, const std::string& title);

#endif // WEF_RUNTIME_LOADER_H_
