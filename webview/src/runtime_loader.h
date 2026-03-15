// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef WEF_RUNTIME_LOADER_H_
#define WEF_RUNTIME_LOADER_H_

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <map>

#include "wef.h"
#include "webview_value.h"

#ifdef __APPLE__
#ifdef __OBJC__
@class WKWebView;
@class NSWindow;
#else
typedef void* WKWebView;
typedef void* NSWindow;
#endif
#elif defined(__linux__)
typedef struct _WebKitWebView WebKitWebView;
typedef struct _GtkWindow GtkWindow;
#elif defined(_WIN32)
struct ICoreWebView2;
struct ICoreWebView2Controller;
struct HWND__;
typedef HWND__* HWND;
#endif

class WefBackend;

class RuntimeLoader {
 public:
  static RuntimeLoader* GetInstance();

  bool Load(const std::string& path);

  bool Start();

  void Shutdown();

  void SetBackend(WefBackend* backend) { backend_ = backend; }
  WefBackend* GetBackend() { return backend_; }

  void OnJsCall(uint64_t call_id, const std::string& method_path,
                wef::ValuePtr args);

  void JsCallRespond(uint64_t call_id, wef::ValuePtr result, wef::ValuePtr error);

  wef_js_call_fn GetJsCallHandler() const { return js_call_handler_; }
  void* GetJsCallUserData() const { return js_call_user_data_; }

  void SetJsCallHandler(wef_js_call_fn handler, void* user_data) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    js_call_handler_ = handler;
    js_call_user_data_ = user_data;
  }

  const wef_backend_api_t* GetBackendApi() const { return &backend_api_; }

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
};

WefBackend* CreateWefBackend(int width, int height, const std::string& title);

#endif // WEF_RUNTIME_LOADER_H_
