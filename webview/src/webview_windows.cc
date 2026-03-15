// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"
#include "webview_value.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl.h>
#include <wil/com.h>

// WebView2 headers
#include "WebView2.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>

using namespace Microsoft::WRL;

// JSON serialization (same as other platforms)
namespace json {

std::string Escape(const std::string& s) {
  std::string result;
  for (char c : s) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

std::string Serialize(const webview::ValuePtr& value);

std::string SerializeList(const webview::ValueList& list) {
  std::ostringstream ss;
  ss << "[";
  for (size_t i = 0; i < list.size(); ++i) {
    if (i > 0) ss << ",";
    ss << Serialize(list[i]);
  }
  ss << "]";
  return ss.str();
}

std::string SerializeDict(const webview::ValueDict& dict) {
  std::ostringstream ss;
  ss << "{";
  bool first = true;
  for (const auto& pair : dict) {
    if (!first) ss << ",";
    first = false;
    ss << "\"" << Escape(pair.first) << "\":" << Serialize(pair.second);
  }
  ss << "}";
  return ss.str();
}

std::string Serialize(const webview::ValuePtr& value) {
  if (!value) return "null";
  switch (value->type) {
    case webview::ValueType::Null:
      return "null";
    case webview::ValueType::Bool:
      return value->GetBool() ? "true" : "false";
    case webview::ValueType::Int:
      return std::to_string(value->GetInt());
    case webview::ValueType::Double: {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.17g", value->GetDouble());
      return buf;
    }
    case webview::ValueType::String:
      return "\"" + Escape(value->GetString()) + "\"";
    case webview::ValueType::Binary: {
      const auto& binary = value->GetBinary();
      std::string base64;
      static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      size_t i = 0;
      const uint8_t* data = binary.data.data();
      size_t len = binary.data.size();
      while (i < len) {
        uint32_t n = (data[i] << 16);
        if (i + 1 < len) n |= (data[i + 1] << 8);
        if (i + 2 < len) n |= data[i + 2];
        base64 += chars[(n >> 18) & 0x3F];
        base64 += chars[(n >> 12) & 0x3F];
        base64 += (i + 1 < len) ? chars[(n >> 6) & 0x3F] : '=';
        base64 += (i + 2 < len) ? chars[n & 0x3F] : '=';
        i += 3;
      }
      return "{\"__binary__\":\"" + base64 + "\"}";
    }
    case webview::ValueType::List:
      return SerializeList(value->GetList());
    case webview::ValueType::Dict:
      return SerializeDict(value->GetDict());
    case webview::ValueType::Callback:
      return "{\"__callback__\":\"" + std::to_string(value->GetCallbackId()) + "\"}";
  }
  return "null";
}

// Parse JSON to Value
webview::ValuePtr Parse(const char*& p);

void SkipWhitespace(const char*& p) {
  while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
}

std::string ParseString(const char*& p) {
  std::string result;
  ++p;
  while (*p && *p != '"') {
    if (*p == '\\' && *(p + 1)) {
      ++p;
      switch (*p) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'u': {
          if (p[1] && p[2] && p[3] && p[4]) {
            char hex[5] = {p[1], p[2], p[3], p[4], 0};
            int codepoint = (int)strtol(hex, nullptr, 16);
            if (codepoint < 0x80) {
              result += (char)codepoint;
            } else if (codepoint < 0x800) {
              result += (char)(0xC0 | (codepoint >> 6));
              result += (char)(0x80 | (codepoint & 0x3F));
            } else {
              result += (char)(0xE0 | (codepoint >> 12));
              result += (char)(0x80 | ((codepoint >> 6) & 0x3F));
              result += (char)(0x80 | (codepoint & 0x3F));
            }
            p += 4;
          }
          break;
        }
        default: result += *p;
      }
    } else {
      result += *p;
    }
    ++p;
  }
  if (*p == '"') ++p;
  return result;
}

webview::ValuePtr ParseArray(const char*& p) {
  auto list = webview::Value::List();
  ++p;
  SkipWhitespace(p);
  while (*p && *p != ']') {
    list->GetList().push_back(Parse(p));
    SkipWhitespace(p);
    if (*p == ',') ++p;
    SkipWhitespace(p);
  }
  if (*p == ']') ++p;
  return list;
}

webview::ValuePtr ParseObject(const char*& p) {
  auto dict = webview::Value::Dict();
  ++p;
  SkipWhitespace(p);
  while (*p && *p != '}') {
    SkipWhitespace(p);
    if (*p != '"') break;
    std::string key = ParseString(p);
    SkipWhitespace(p);
    if (*p == ':') ++p;
    SkipWhitespace(p);
    dict->GetDict()[key] = Parse(p);
    SkipWhitespace(p);
    if (*p == ',') ++p;
    SkipWhitespace(p);
  }
  if (*p == '}') ++p;

  const auto& d = dict->GetDict();
  auto it = d.find("__callback__");
  if (it != d.end() && it->second->IsString()) {
    uint64_t id = std::stoull(it->second->GetString());
    return webview::Value::Callback(id);
  }
  it = d.find("__binary__");
  if (it != d.end() && it->second->IsString()) {
    const std::string& base64 = it->second->GetString();
    std::vector<uint8_t> data;
    static const int decode[256] = {
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
      52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
      -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
      15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
      -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
      41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    int val = 0, bits = -8;
    for (char c : base64) {
      if (c == '=') break;
      int d = decode[(unsigned char)c];
      if (d < 0) continue;
      val = (val << 6) | d;
      bits += 6;
      if (bits >= 0) {
        data.push_back((val >> bits) & 0xFF);
        bits -= 8;
      }
    }
    return webview::Value::Binary(data.data(), data.size());
  }

  return dict;
}

webview::ValuePtr Parse(const char*& p) {
  SkipWhitespace(p);
  if (!*p) return webview::Value::Null();

  if (*p == 'n' && strncmp(p, "null", 4) == 0) {
    p += 4;
    return webview::Value::Null();
  }
  if (*p == 't' && strncmp(p, "true", 4) == 0) {
    p += 4;
    return webview::Value::Bool(true);
  }
  if (*p == 'f' && strncmp(p, "false", 5) == 0) {
    p += 5;
    return webview::Value::Bool(false);
  }
  if (*p == '"') {
    return webview::Value::String(ParseString(p));
  }
  if (*p == '[') {
    return ParseArray(p);
  }
  if (*p == '{') {
    return ParseObject(p);
  }
  if (*p == '-' || (*p >= '0' && *p <= '9')) {
    char* end;
    double d = strtod(p, &end);
    bool isInt = true;
    for (const char* c = p; c < end; ++c) {
      if (*c == '.' || *c == 'e' || *c == 'E') {
        isInt = false;
        break;
      }
    }
    p = end;
    if (isInt && d >= INT_MIN && d <= INT_MAX) {
      return webview::Value::Int((int)d);
    }
    return webview::Value::Double(d);
  }

  return webview::Value::Null();
}

webview::ValuePtr ParseJson(const std::string& json) {
  const char* p = json.c_str();
  return Parse(p);
}

// Convert UTF-8 to wide string
std::wstring Utf8ToWide(const std::string& str) {
  if (str.empty()) return std::wstring();
  int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, nullptr, 0);
  std::wstring result(size - 1, 0);
  MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &result[0], size);
  return result;
}

// Convert wide string to UTF-8
std::string WideToUtf8(const std::wstring& wstr) {
  if (wstr.empty()) return std::string();
  int size = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, nullptr, 0, nullptr, nullptr);
  std::string result(size - 1, 0);
  WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), -1, &result[0], size, nullptr, nullptr);
  return result;
}

} // namespace json

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
  void GetWindowSize(int* width, int* height) override;
  void SetWindowPosition(int x, int y) override;
  void GetWindowPosition(int* x, int* y) override;
  void SetResizable(bool resizable) override;
  bool IsResizable() override;
  void SetAlwaysOnTop(bool always_on_top) override;
  bool IsAlwaysOnTop() override;
  bool IsVisible() override;
  void Show() override;
  void Hide() override;
  void Focus() override;
  void PostUiTask(void (*task)(void*), void* data) override;

  void InvokeJsCallback(uint64_t callback_id, webview::ValuePtr args) override;
  void ReleaseJsCallback(uint64_t callback_id) override;
  void RespondToJsCall(uint64_t call_id, webview::ValuePtr result, webview::ValuePtr error) override;

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

  std::atomic<uint64_t> next_callback_id_{1};
  std::map<uint64_t, bool> stored_callbacks_;
  std::mutex callbacks_mutex_;

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
  std::wstring wtitle = json::Utf8ToWide(title);
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
  std::wstring wurl = json::Utf8ToWide(url);
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
  std::wstring wtitle = json::Utf8ToWide(title);
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
  std::wstring wscript = json::Utf8ToWide(script);
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

void WebView2Backend::GetWindowSize(int* width, int* height) {
  RECT rect;
  if (GetWindowRect(hwnd_, &rect)) {
    if (width) *width = rect.right - rect.left;
    if (height) *height = rect.bottom - rect.top;
  }
}

void WebView2Backend::SetWindowPosition(int x, int y) {
  PostMessage(hwnd_, WM_UI_TASK, 0,
      reinterpret_cast<LPARAM>(new UiTaskData{
          [](void* data) {
            auto* tuple = static_cast<std::tuple<HWND, int, int>*>(data);
            SetWindowPos(std::get<0>(*tuple), nullptr,
                         std::get<1>(*tuple), std::get<2>(*tuple),
                         0, 0, SWP_NOSIZE | SWP_NOZORDER);
            delete tuple;
          },
          new std::tuple<HWND, int, int>(hwnd_, x, y)
      }));
}

void WebView2Backend::GetWindowPosition(int* x, int* y) {
  RECT rect;
  if (GetWindowRect(hwnd_, &rect)) {
    if (x) *x = rect.left;
    if (y) *y = rect.top;
  }
}

void WebView2Backend::SetResizable(bool resizable) {
  PostMessage(hwnd_, WM_UI_TASK, 0,
      reinterpret_cast<LPARAM>(new UiTaskData{
          [](void* data) {
            auto* tuple = static_cast<std::tuple<HWND, bool>*>(data);
            HWND h = std::get<0>(*tuple);
            bool r = std::get<1>(*tuple);
            LONG style = GetWindowLong(h, GWL_STYLE);
            if (r) {
              style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
            } else {
              style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
            }
            SetWindowLong(h, GWL_STYLE, style);
            delete tuple;
          },
          new std::tuple<HWND, bool>(hwnd_, resizable)
      }));
}

bool WebView2Backend::IsResizable() {
  LONG style = GetWindowLong(hwnd_, GWL_STYLE);
  return (style & WS_THICKFRAME) != 0;
}

void WebView2Backend::SetAlwaysOnTop(bool always_on_top) {
  PostMessage(hwnd_, WM_UI_TASK, 0,
      reinterpret_cast<LPARAM>(new UiTaskData{
          [](void* data) {
            auto* tuple = static_cast<std::tuple<HWND, bool>*>(data);
            SetWindowPos(std::get<0>(*tuple),
                         std::get<1>(*tuple) ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            delete tuple;
          },
          new std::tuple<HWND, bool>(hwnd_, always_on_top)
      }));
}

bool WebView2Backend::IsAlwaysOnTop() {
  LONG ex_style = GetWindowLong(hwnd_, GWL_EXSTYLE);
  return (ex_style & WS_EX_TOPMOST) != 0;
}

bool WebView2Backend::IsVisible() {
  return IsWindowVisible(hwnd_) != FALSE;
}

void WebView2Backend::Show() {
  PostMessage(hwnd_, WM_UI_TASK, 0,
      reinterpret_cast<LPARAM>(new UiTaskData{
          [](void* data) {
            ShowWindow(static_cast<HWND>(data), SW_SHOW);
          },
          hwnd_
      }));
}

void WebView2Backend::Hide() {
  PostMessage(hwnd_, WM_UI_TASK, 0,
      reinterpret_cast<LPARAM>(new UiTaskData{
          [](void* data) {
            ShowWindow(static_cast<HWND>(data), SW_HIDE);
          },
          hwnd_
      }));
}

void WebView2Backend::Focus() {
  PostMessage(hwnd_, WM_UI_TASK, 0,
      reinterpret_cast<LPARAM>(new UiTaskData{
          [](void* data) {
            HWND h = static_cast<HWND>(data);
            ShowWindow(h, SW_SHOW);
            SetForegroundWindow(h);
            SetFocus(h);
          },
          hwnd_
      }));
}

void WebView2Backend::PostUiTask(void (*task)(void*), void* data) {
  PostMessage(hwnd_, WM_UI_TASK, 0,
      reinterpret_cast<LPARAM>(new UiTaskData{task, data}));
}

void WebView2Backend::InvokeJsCallback(uint64_t callback_id, webview::ValuePtr args) {
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

void WebView2Backend::RespondToJsCall(uint64_t call_id, webview::ValuePtr result, webview::ValuePtr error) {
  std::string resultJson = json::Serialize(result);
  std::string script;
  if (error) {
    std::string errorJson = json::Serialize(error);
    script = "window.__wefRespond(" + std::to_string(call_id) + ", null, " +
             errorJson + ");";
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
  std::string jsonStr = json::WideToUtf8(json);
  webview::ValuePtr msg = json::ParseJson(jsonStr);
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
  webview::ValuePtr args = (argsIt != dict.end()) ? argsIt->second : webview::Value::List();

  RuntimeLoader::GetInstance()->OnJsCall(call_id, method, args);
}

// ============================================================================
// Factory Function
// ============================================================================

WebviewBackend* CreateWebviewBackend(int width, int height, const std::string& title) {
  return new WebView2Backend(width, height, title);
}
