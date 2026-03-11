// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"
#include "wef_json.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl.h>
#include <wil/com.h>

// WebView2 headers
#include "WebView2.h"

#include <iostream>
#include <string>

using namespace Microsoft::WRL;

// Convert UTF-8 to wide string
static std::wstring Utf8ToWide(const std::string& str) {
  if (str.empty()) return std::wstring();
  int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
  std::wstring result(size - 1, 0);
  MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size);
  return result;
}

// Convert wide string to UTF-8
static std::string WideToUtf8(const std::wstring& wstr) {
  if (wstr.empty()) return std::string();
  int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string result(size - 1, 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], size, nullptr, nullptr);
  return result;
}

// ============================================================================
// WebView2 Backend
// ============================================================================

class WebView2Backend : public WebviewBackend {
 public:
  WebView2Backend(int width, int height, const std::string& title);
  ~WebView2Backend() override;

  void Navigate(const std::string& url) override;
  void SetTitle(const std::string& title) override;
  void ExecuteJs(const std::string& script) override;
  void Quit() override;
  void SetWindowSize(int width, int height) override;
  void PostUiTask(void (*task)(void*), void* data) override;

  void InvokeJsCallback(uint64_t callback_id, wef::ValuePtr args) override;
  void ReleaseJsCallback(uint64_t callback_id) override;
  void RespondToJsCall(uint64_t call_id, wef::ValuePtr result, const char* error) override;

  void Run() override;

  void HandleJsMessage(const std::wstring& json);

 private:
  bool InitializeWebView();
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  HWND hwnd_;
  ComPtr<ICoreWebView2Controller> controller_;
  ComPtr<ICoreWebView2> webview_;
  std::wstring pending_url_;
  std::wstring pending_title_;
  bool webview_ready_ = false;



  int initial_width_;
  int initial_height_;
  std::string initial_title_;
};

// Custom window message for UI tasks
#define WM_UI_TASK (WM_USER + 1)

struct UiTaskData {
  void (*task)(void*);
  void* data;
};

LRESULT CALLBACK WebView2Backend::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  WebView2Backend* backend = reinterpret_cast<WebView2Backend*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

  switch (msg) {
    case WM_CREATE: {
      CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
      SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      return 0;
    }
    case WM_SIZE:
      if (backend && backend->controller_) {
        RECT bounds;
        GetClientRect(hwnd, &bounds);
        backend->controller_->put_Bounds(bounds);
      }
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_UI_TASK: {
      UiTaskData* taskData = reinterpret_cast<UiTaskData*>(lParam);
      if (taskData) {
        taskData->task(taskData->data);
        delete taskData;
      }
      return 0;
    }
  }

  return DefWindowProc(hwnd, msg, wParam, lParam);
}

WebView2Backend::WebView2Backend(int width, int height, const std::string& title)
    : initial_width_(width), initial_height_(height), initial_title_(title) {

  // Register window class
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = L"WefWebView2";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  RegisterClassExW(&wc);

  // Create window
  std::wstring wtitle = Utf8ToWide(title);
  hwnd_ = CreateWindowExW(
      0,
      L"WefWebView2",
      wtitle.c_str(),
      WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT,
      width, height,
      nullptr,
      nullptr,
      GetModuleHandle(nullptr),
      this
  );

  // Initialize WebView2
  InitializeWebView();
}

WebView2Backend::~WebView2Backend() {
  if (controller_) {
    controller_->Close();
  }
  if (hwnd_) {
    DestroyWindow(hwnd_);
  }
}

bool WebView2Backend::InitializeWebView() {
  // Create WebView2 environment
  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
      nullptr, nullptr, nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result)) {
              std::cerr << "Failed to create WebView2 environment" << std::endl;
              return result;
            }

            // Create the WebView2 controller
            env->CreateCoreWebView2Controller(
                hwnd_,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(result) || !controller) {
                        std::cerr << "Failed to create WebView2 controller" << std::endl;
                        return result;
                      }

                      controller_ = controller;
                      controller_->get_CoreWebView2(&webview_);

                      // Set bounds
                      RECT bounds;
                      GetClientRect(hwnd_, &bounds);
                      controller_->put_Bounds(bounds);

                      // Add script message handler
                      webview_->AddScriptToExecuteOnDocumentCreated(
                          LR"JS(
(function() {
  const pendingCalls = new Map();
  let nextCallId = 1;

  function createWefProxy(path = []) {
    return new Proxy(function() {}, {
      get(target, prop) {
        if (prop === 'then' || prop === 'catch' || prop === 'finally' ||
            prop === 'constructor' || prop === Symbol.toStringTag) {
          return undefined;
        }
        return createWefProxy([...path, prop]);
      },
      apply(target, thisArg, args) {
        return new Promise((resolve, reject) => {
          const callId = nextCallId++;
          pendingCalls.set(callId, { resolve, reject });

          const processedArgs = args.map(arg => {
            if (typeof arg === 'function') {
              const cbId = nextCallId++;
              window.__wefCallbacks = window.__wefCallbacks || {};
              window.__wefCallbacks[cbId] = arg;
              return { __callback__: String(cbId) };
            }
            if (arg instanceof ArrayBuffer) {
              const bytes = new Uint8Array(arg);
              let binary = '';
              bytes.forEach(b => binary += String.fromCharCode(b));
              return { __binary__: btoa(binary) };
            }
            if (arg instanceof Uint8Array) {
              let binary = '';
              arg.forEach(b => binary += String.fromCharCode(b));
              return { __binary__: btoa(binary) };
            }
            return arg;
          });

          window.chrome.webview.postMessage(JSON.stringify({
            callId: callId,
            method: path.join('.'),
            args: processedArgs
          }));
        });
      }
    });
  }

  window.Wef = createWefProxy();

  window.__wefRespond = function(callId, result, error) {
    const pending = pendingCalls.get(callId);
    if (pending) {
      pendingCalls.delete(callId);
      if (error) {
        pending.reject(new Error(error));
      } else {
        function convertBinary(obj) {
          if (obj && typeof obj === 'object') {
            if (obj.__binary__) {
              const binary = atob(obj.__binary__);
              const bytes = new Uint8Array(binary.length);
              for (let i = 0; i < binary.length; i++) {
                bytes[i] = binary.charCodeAt(i);
              }
              return bytes.buffer;
            }
            if (Array.isArray(obj)) {
              return obj.map(convertBinary);
            }
            const result = {};
            for (const key in obj) {
              result[key] = convertBinary(obj[key]);
            }
            return result;
          }
          return obj;
        }
        pending.resolve(convertBinary(result));
      }
    }
  };

  window.__wefInvokeCallback = function(callbackId, args) {
    const cb = window.__wefCallbacks && window.__wefCallbacks[callbackId];
    if (cb) {
      cb.apply(null, args);
    }
  };

  window.__wefReleaseCallback = function(callbackId) {
    if (window.__wefCallbacks) {
      delete window.__wefCallbacks[callbackId];
    }
  };
})();
)JS",
                          nullptr);

                      // Set up web message handler
                      webview_->add_WebMessageReceived(
                          Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [this](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                LPWSTR messageRaw;
                                args->TryGetWebMessageAsString(&messageRaw);
                                if (messageRaw) {
                                  HandleJsMessage(messageRaw);
                                  CoTaskMemFree(messageRaw);
                                }
                                return S_OK;
                              }).Get(),
                          nullptr);

                      webview_ready_ = true;

                      // Apply pending operations
                      if (!pending_url_.empty()) {
                        webview_->Navigate(pending_url_.c_str());
                        pending_url_.clear();
                      }
                      if (!pending_title_.empty()) {
                        SetWindowTextW(hwnd_, pending_title_.c_str());
                        pending_title_.clear();
                      }

                      return S_OK;
                    }).Get());

            return S_OK;
          }).Get());

  return SUCCEEDED(hr);
}

void WebView2Backend::Navigate(const std::string& url) {
  std::wstring wurl = Utf8ToWide(url);
  if (webview_ready_ && webview_) {
    PostMessage(hwnd_, WM_UI_TASK, 0,
        reinterpret_cast<LPARAM>(new UiTaskData{
            [](void* data) {
              auto* pair = static_cast<std::pair<WebView2Backend*, std::wstring>*>(data);
              pair->first->webview_->Navigate(pair->second.c_str());
              delete pair;
            },
            new std::pair<WebView2Backend*, std::wstring>(this, wurl)
        }));
  } else {
    pending_url_ = wurl;
  }
}

void WebView2Backend::SetTitle(const std::string& title) {
  std::wstring wtitle = Utf8ToWide(title);
  if (webview_ready_) {
    PostMessage(hwnd_, WM_UI_TASK, 0,
        reinterpret_cast<LPARAM>(new UiTaskData{
            [](void* data) {
              auto* pair = static_cast<std::pair<HWND, std::wstring>*>(data);
              SetWindowTextW(pair->first, pair->second.c_str());
              delete pair;
            },
            new std::pair<HWND, std::wstring>(hwnd_, wtitle)
        }));
  } else {
    pending_title_ = wtitle;
  }
}

void WebView2Backend::ExecuteJs(const std::string& script) {
  if (!webview_ready_ || !webview_) return;
  std::wstring wscript = Utf8ToWide(script);
  PostMessage(hwnd_, WM_UI_TASK, 0,
      reinterpret_cast<LPARAM>(new UiTaskData{
          [](void* data) {
            auto* pair = static_cast<std::pair<WebView2Backend*, std::wstring>*>(data);
            pair->first->webview_->ExecuteScript(pair->second.c_str(), nullptr);
            delete pair;
          },
          new std::pair<WebView2Backend*, std::wstring>(this, wscript)
      }));
}

void WebView2Backend::Quit() {
  PostMessage(hwnd_, WM_CLOSE, 0, 0);
}

void WebView2Backend::SetWindowSize(int width, int height) {
  PostMessage(hwnd_, WM_UI_TASK, 0,
      reinterpret_cast<LPARAM>(new UiTaskData{
          [](void* data) {
            auto* tuple = static_cast<std::tuple<HWND, int, int>*>(data);
            SetWindowPos(std::get<0>(*tuple), nullptr, 0, 0,
                         std::get<1>(*tuple), std::get<2>(*tuple),
                         SWP_NOMOVE | SWP_NOZORDER);
            delete tuple;
          },
          new std::tuple<HWND, int, int>(hwnd_, width, height)
      }));
}

void WebView2Backend::PostUiTask(void (*task)(void*), void* data) {
  PostMessage(hwnd_, WM_UI_TASK, 0,
      reinterpret_cast<LPARAM>(new UiTaskData{task, data}));
}

void WebView2Backend::InvokeJsCallback(uint64_t callback_id, wef::ValuePtr args) {
  std::string argsJson = json::Serialize(args);
  std::string script = "window.__wefInvokeCallback(" +
                       std::to_string(callback_id) + ", " + argsJson + ");";
  ExecuteJs(script);
}

void WebView2Backend::ReleaseJsCallback(uint64_t callback_id) {
  std::string script = "window.__wefReleaseCallback(" +
                       std::to_string(callback_id) + ");";
  ExecuteJs(script);
}

void WebView2Backend::RespondToJsCall(uint64_t call_id, wef::ValuePtr result, const char* error) {
  std::string resultJson = json::Serialize(result);
  std::string script;
  if (error && strlen(error) > 0) {
    script = "window.__wefRespond(" + std::to_string(call_id) + ", null, \"" +
             json::Escape(error) + "\");";
  } else {
    script = "window.__wefRespond(" + std::to_string(call_id) + ", " +
             resultJson + ", null);";
  }
  ExecuteJs(script);
}

void WebView2Backend::Run() {
  ShowWindow(hwnd_, SW_SHOW);
  UpdateWindow(hwnd_);

  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

void WebView2Backend::HandleJsMessage(const std::wstring& json) {
  std::string jsonStr = WideToUtf8(json);
  wef::ValuePtr msg = json::ParseJson(jsonStr);
  if (!msg || !msg->IsDict()) return;

  const auto& dict = msg->GetDict();
  auto callIdIt = dict.find("callId");
  auto methodIt = dict.find("method");
  auto argsIt = dict.find("args");

  if (callIdIt == dict.end() || methodIt == dict.end()) return;

  uint64_t call_id = 0;
  if (callIdIt->second->IsInt()) {
    call_id = static_cast<uint64_t>(callIdIt->second->GetInt());
  } else if (callIdIt->second->IsDouble()) {
    call_id = static_cast<uint64_t>(callIdIt->second->GetDouble());
  }

  std::string method = methodIt->second->IsString() ? methodIt->second->GetString() : "";
  wef::ValuePtr args = (argsIt != dict.end()) ? argsIt->second : wef::Value::List();

  RuntimeLoader::GetInstance()->OnJsCall(call_id, method, args);
}

// ============================================================================
// Factory Function
// ============================================================================

WebviewBackend* CreateWebviewBackend(int width, int height, const std::string& title) {
  return new WebView2Backend(width, height, title);
}
