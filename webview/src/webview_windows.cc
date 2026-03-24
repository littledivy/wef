// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"
#include "wef_json.h"
#include <win32_menu.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <wrl.h>
#include <wil/com.h>

// WebView2 headers
#include "WebView2.h"

#include <iostream>
#include <string>
#include <map>
#include <mutex>

using namespace Microsoft::WRL;

namespace keyboard {

std::string VirtualKeyToKey(WPARAM vk, LPARAM lParam) {
  switch (vk) {
    case VK_BACK: return "Backspace";
    case VK_TAB: return "Tab";
    case VK_RETURN: return "Enter";
    case VK_SHIFT: return "Shift";
    case VK_CONTROL: return "Control";
    case VK_MENU: return "Alt";
    case VK_PAUSE: return "Pause";
    case VK_CAPITAL: return "CapsLock";
    case VK_ESCAPE: return "Escape";
    case VK_SPACE: return " ";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_END: return "End";
    case VK_HOME: return "Home";
    case VK_LEFT: return "ArrowLeft";
    case VK_UP: return "ArrowUp";
    case VK_RIGHT: return "ArrowRight";
    case VK_DOWN: return "ArrowDown";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_LWIN: case VK_RWIN: return "Meta";
    case VK_F1: return "F1";
    case VK_F2: return "F2";
    case VK_F3: return "F3";
    case VK_F4: return "F4";
    case VK_F5: return "F5";
    case VK_F6: return "F6";
    case VK_F7: return "F7";
    case VK_F8: return "F8";
    case VK_F9: return "F9";
    case VK_F10: return "F10";
    case VK_F11: return "F11";
    case VK_F12: return "F12";
    case VK_NUMLOCK: return "NumLock";
    case VK_SCROLL: return "ScrollLock";
    default:
      if (vk >= 'A' && vk <= 'Z') {
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        bool caps = (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
        char c = static_cast<char>(vk);
        if (!(shift ^ caps)) c += 32; // lowercase
        return std::string(1, c);
      }
      if (vk >= '0' && vk <= '9') {
        return std::string(1, static_cast<char>(vk));
      }
      return "Unidentified";
  }
}

std::string VirtualKeyToCode(WPARAM vk, LPARAM lParam) {
  bool isExtended = (lParam & (1 << 24)) != 0;
  switch (vk) {
    case VK_BACK: return "Backspace";
    case VK_TAB: return "Tab";
    case VK_RETURN: return isExtended ? "NumpadEnter" : "Enter";
    case VK_SHIFT: return (MapVirtualKey((lParam >> 16) & 0xFF, MAPVK_VSC_TO_VK_EX) == VK_RSHIFT) ? "ShiftRight" : "ShiftLeft";
    case VK_CONTROL: return isExtended ? "ControlRight" : "ControlLeft";
    case VK_MENU: return isExtended ? "AltRight" : "AltLeft";
    case VK_PAUSE: return "Pause";
    case VK_CAPITAL: return "CapsLock";
    case VK_ESCAPE: return "Escape";
    case VK_SPACE: return "Space";
    case VK_PRIOR: return "PageUp";
    case VK_NEXT: return "PageDown";
    case VK_END: return "End";
    case VK_HOME: return "Home";
    case VK_LEFT: return "ArrowLeft";
    case VK_UP: return "ArrowUp";
    case VK_RIGHT: return "ArrowRight";
    case VK_DOWN: return "ArrowDown";
    case VK_INSERT: return "Insert";
    case VK_DELETE: return "Delete";
    case VK_LWIN: return "MetaLeft";
    case VK_RWIN: return "MetaRight";
    case VK_F1: return "F1";
    case VK_F2: return "F2";
    case VK_F3: return "F3";
    case VK_F4: return "F4";
    case VK_F5: return "F5";
    case VK_F6: return "F6";
    case VK_F7: return "F7";
    case VK_F8: return "F8";
    case VK_F9: return "F9";
    case VK_F10: return "F10";
    case VK_F11: return "F11";
    case VK_F12: return "F12";
    case VK_NUMLOCK: return "NumLock";
    case VK_SCROLL: return "ScrollLock";
    case VK_OEM_1: return "Semicolon";
    case VK_OEM_PLUS: return "Equal";
    case VK_OEM_COMMA: return "Comma";
    case VK_OEM_MINUS: return "Minus";
    case VK_OEM_PERIOD: return "Period";
    case VK_OEM_2: return "Slash";
    case VK_OEM_3: return "Backquote";
    case VK_OEM_4: return "BracketLeft";
    case VK_OEM_5: return "Backslash";
    case VK_OEM_6: return "BracketRight";
    case VK_OEM_7: return "Quote";
    default:
      if (vk >= 'A' && vk <= 'Z') {
        return "Key" + std::string(1, static_cast<char>(vk));
      }
      if (vk >= '0' && vk <= '9') {
        return "Digit" + std::string(1, static_cast<char>(vk));
      }
      return "Unidentified";
  }
}

uint32_t GetWefModifiers() {
  uint32_t modifiers = 0;
  if (GetKeyState(VK_SHIFT) & 0x8000) modifiers |= WEF_MOD_SHIFT;
  if (GetKeyState(VK_CONTROL) & 0x8000) modifiers |= WEF_MOD_CONTROL;
  if (GetKeyState(VK_MENU) & 0x8000) modifiers |= WEF_MOD_ALT;
  if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) modifiers |= WEF_MOD_META;
  return modifiers;
}

} // namespace keyboard

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

// HWND → wef_id mapping
static std::map<HWND, uint32_t> g_hwnd_to_wef_id;
static std::mutex g_hwnd_mutex;

static uint32_t WefIdForHwnd(HWND hwnd) {
  if (!hwnd) return 0;
  std::lock_guard<std::mutex> lock(g_hwnd_mutex);
  auto it = g_hwnd_to_wef_id.find(hwnd);
  return it != g_hwnd_to_wef_id.end() ? it->second : 0;
}

// Per-window state
struct WinWindowState {
  uint32_t window_id;
  HWND hwnd;
  ComPtr<ICoreWebView2Controller> controller;
  ComPtr<ICoreWebView2> webview;
  bool webview_ready = false;
  std::wstring pending_url;
  std::wstring pending_title;
};

// Custom window message for UI tasks
#define WM_UI_TASK (WM_USER + 1)

struct UiTaskData {
  void (*task)(void*);
  void* data;
};

// ============================================================================
// WebView2 Backend
// ============================================================================

class WebView2Backend;
static WebView2Backend* g_win_backend = nullptr;

class WebView2Backend : public WefBackend {
 public:
  WebView2Backend();
  ~WebView2Backend() override;

  void CreateWindow(uint32_t window_id, int width, int height) override;
  void CloseWindow(uint32_t window_id) override;

  void Navigate(uint32_t window_id, const std::string& url) override;
  void SetTitle(uint32_t window_id, const std::string& title) override;
  void ExecuteJs(uint32_t window_id, const std::string& script) override;
  void Quit() override;
  void SetWindowSize(uint32_t window_id, int width, int height) override;
  void GetWindowSize(uint32_t window_id, int* width, int* height) override;
  void SetWindowPosition(uint32_t window_id, int x, int y) override;
  void GetWindowPosition(uint32_t window_id, int* x, int* y) override;
  void SetResizable(uint32_t window_id, bool resizable) override;
  bool IsResizable(uint32_t window_id) override;
  void SetAlwaysOnTop(uint32_t window_id, bool always_on_top) override;
  bool IsAlwaysOnTop(uint32_t window_id) override;
  bool IsVisible(uint32_t window_id) override;
  void Show(uint32_t window_id) override;
  void Hide(uint32_t window_id) override;
  void Focus(uint32_t window_id) override;
  void PostUiTask(void (*task)(void*), void* data) override;

  void InvokeJsCallback(uint32_t window_id, uint64_t callback_id, wef::ValuePtr args) override;
  void ReleaseJsCallback(uint32_t window_id, uint64_t callback_id) override;
  void RespondToJsCall(uint32_t window_id, uint64_t call_id, wef::ValuePtr result, wef::ValuePtr error) override;

  void Run() override;

  void SetApplicationMenu(wef_value_t*, const wef_backend_api_t*, wef_menu_click_fn, void*) override {}

  void HandleJsMessage(uint32_t window_id, const std::wstring& json);

 private:
  WinWindowState* GetWindow(uint32_t window_id);
  void InitializeWebViewForWindow(uint32_t window_id, HWND hwnd);
  static LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  std::map<uint32_t, WinWindowState> windows_;
  std::mutex windows_mutex_;
  bool class_registered_ = false;
};

LRESULT CALLBACK WebView2Backend::WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
  uint32_t wid = WefIdForHwnd(hwnd);

  switch (msg) {
    case WM_SIZE: {
      if (g_win_backend && wid > 0) {
        std::lock_guard<std::mutex> lock(g_win_backend->windows_mutex_);
        auto* state = g_win_backend->GetWindow(wid);
        if (state && state->controller) {
          RECT bounds;
          GetClientRect(hwnd, &bounds);
          state->controller->put_Bounds(bounds);
        }
      }
      if (wid > 0) {
        RECT rect;
        GetClientRect(hwnd, &rect);
        RuntimeLoader::GetInstance()->DispatchResizeEvent(
            wid, rect.right - rect.left, rect.bottom - rect.top);
      }
      return 0;
    }
    case WM_MOVE:
      if (wid > 0) {
        RuntimeLoader::GetInstance()->DispatchMoveEvent(
            wid, (int)(short)LOWORD(lParam), (int)(short)HIWORD(lParam));
      }
      return 0;
    case WM_SETFOCUS:
      if (wid > 0) RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 1);
      return 0;
    case WM_KILLFOCUS:
      if (wid > 0) RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 0);
      return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP: {
      if (wid == 0) break;
      int state = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN ||
                   msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN)
                      ? WEF_MOUSE_PRESSED : WEF_MOUSE_RELEASED;
      int button;
      switch (msg) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP:
          button = WEF_MOUSE_BUTTON_LEFT; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP:
          button = WEF_MOUSE_BUTTON_RIGHT; break;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP:
          button = WEF_MOUSE_BUTTON_MIDDLE; break;
        default:
          button = (GET_XBUTTON_WPARAM(wParam) == XBUTTON1)
                       ? WEF_MOUSE_BUTTON_BACK : WEF_MOUSE_BUTTON_FORWARD;
          break;
      }
      double x = static_cast<double>(GET_X_LPARAM(lParam));
      double y = static_cast<double>(GET_Y_LPARAM(lParam));
      uint32_t modifiers = keyboard::GetWefModifiers();
      RuntimeLoader::GetInstance()->DispatchMouseClickEvent(
          wid, state, button, x, y, modifiers, 1);
      break;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      if (wid == 0) break;
      std::string key = keyboard::VirtualKeyToKey(wParam, lParam);
      std::string code = keyboard::VirtualKeyToCode(wParam, lParam);
      uint32_t modifiers = keyboard::GetWefModifiers();
      bool repeat = (lParam & (1 << 30)) != 0;
      RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
          wid, WEF_KEY_PRESSED, key.c_str(), code.c_str(), modifiers, repeat);
      break;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
      if (wid == 0) break;
      std::string key = keyboard::VirtualKeyToKey(wParam, lParam);
      std::string code = keyboard::VirtualKeyToCode(wParam, lParam);
      uint32_t modifiers = keyboard::GetWefModifiers();
      RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
          wid, WEF_KEY_RELEASED, key.c_str(), code.c_str(), modifiers, false);
      break;
    }
    case WM_CLOSE:
      if (wid > 0) {
        RuntimeLoader::GetInstance()->DispatchCloseRequestedEvent(wid);
      }
      // Check if any windows remain
      {
        std::lock_guard<std::mutex> lock(g_hwnd_mutex);
        // Remove this window from map
        g_hwnd_to_wef_id.erase(hwnd);
        if (g_hwnd_to_wef_id.empty()) {
          PostQuitMessage(0);
        }
      }
      DestroyWindow(hwnd);
      return 0;
    case WM_COMMAND:
      if (win32_menu::HandleMenuCommand(wParam))
        return 0;
      break;
    case WM_DESTROY:
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

WebView2Backend::WebView2Backend() {
  g_win_backend = this;

  // Register window class
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(WNDCLASSEXW);
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.lpszClassName = L"WefWebView2";
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
  RegisterClassExW(&wc);
  class_registered_ = true;
}

WebView2Backend::~WebView2Backend() {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    if (state.controller) state.controller->Close();
    {
      std::lock_guard<std::mutex> hlock(g_hwnd_mutex);
      g_hwnd_to_wef_id.erase(state.hwnd);
    }
  }
  windows_.clear();
  g_win_backend = nullptr;
}

WinWindowState* WebView2Backend::GetWindow(uint32_t window_id) {
  auto it = windows_.find(window_id);
  return it != windows_.end() ? &it->second : nullptr;
}

void WebView2Backend::CreateWindow(uint32_t window_id, int width, int height) {
  HWND hwnd = CreateWindowExW(
      0,
      L"WefWebView2",
      L"",
      WS_OVERLAPPEDWINDOW,
      CW_USEDEFAULT, CW_USEDEFAULT,
      width, height,
      nullptr, nullptr,
      GetModuleHandle(nullptr),
      nullptr
  );

  {
    std::lock_guard<std::mutex> lock(g_hwnd_mutex);
    g_hwnd_to_wef_id[hwnd] = window_id;
  }

  WinWindowState state;
  state.window_id = window_id;
  state.hwnd = hwnd;

  {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    windows_[window_id] = state;
  }

  InitializeWebViewForWindow(window_id, hwnd);

  ShowWindow(hwnd, SW_SHOW);
  UpdateWindow(hwnd);
}

void WebView2Backend::InitializeWebViewForWindow(uint32_t window_id, HWND hwnd) {
  CreateCoreWebView2EnvironmentWithOptions(
      nullptr, nullptr, nullptr,
      Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
          [this, window_id, hwnd](HRESULT result, ICoreWebView2Environment* env) -> HRESULT {
            if (FAILED(result)) {
              std::cerr << "Failed to create WebView2 environment" << std::endl;
              return result;
            }

            env->CreateCoreWebView2Controller(
                hwnd,
                Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                    [this, window_id, hwnd](HRESULT result, ICoreWebView2Controller* controller) -> HRESULT {
                      if (FAILED(result) || !controller) {
                        std::cerr << "Failed to create WebView2 controller" << std::endl;
                        return result;
                      }

                      std::lock_guard<std::mutex> lock(windows_mutex_);
                      auto* state = GetWindow(window_id);
                      if (!state) return S_OK;

                      state->controller = controller;
                      controller->get_CoreWebView2(&state->webview);

                      RECT bounds;
                      GetClientRect(hwnd, &bounds);
                      controller->put_Bounds(bounds);

                      state->webview->AddScriptToExecuteOnDocumentCreated(
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

                      uint32_t wid = window_id;
                      state->webview->add_WebMessageReceived(
                          Callback<ICoreWebView2WebMessageReceivedEventHandler>(
                              [this, wid](ICoreWebView2* sender, ICoreWebView2WebMessageReceivedEventArgs* args) -> HRESULT {
                                LPWSTR messageRaw;
                                args->TryGetWebMessageAsString(&messageRaw);
                                if (messageRaw) {
                                  HandleJsMessage(wid, messageRaw);
                                  CoTaskMemFree(messageRaw);
                                }
                                return S_OK;
                              }).Get(),
                          nullptr);

                      state->webview_ready = true;

                      if (!state->pending_url.empty()) {
                        state->webview->Navigate(state->pending_url.c_str());
                        state->pending_url.clear();
                      }
                      if (!state->pending_title.empty()) {
                        SetWindowTextW(hwnd, state->pending_title.c_str());
                        state->pending_title.clear();
                      }

                      return S_OK;
                    }).Get());

            return S_OK;
          }).Get());
}

void WebView2Backend::CloseWindow(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    if (state->controller) state->controller->Close();
    {
      std::lock_guard<std::mutex> hlock(g_hwnd_mutex);
      g_hwnd_to_wef_id.erase(state->hwnd);
    }
    DestroyWindow(state->hwnd);
    windows_.erase(window_id);
  }
}

void WebView2Backend::Navigate(uint32_t window_id, const std::string& url) {
  std::wstring wurl = Utf8ToWide(url);
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state) return;
  if (state->webview_ready && state->webview) {
    state->webview->Navigate(wurl.c_str());
  } else {
    state->pending_url = wurl;
  }
}

void WebView2Backend::SetTitle(uint32_t window_id, const std::string& title) {
  std::wstring wtitle = Utf8ToWide(title);
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (!state) return;
  if (state->webview_ready) {
    SetWindowTextW(state->hwnd, wtitle.c_str());
  } else {
    state->pending_title = wtitle;
  }
}

void WebView2Backend::ExecuteJs(uint32_t window_id, const std::string& script) {
  std::wstring wscript = Utf8ToWide(script);
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state && state->webview_ready && state->webview) {
    state->webview->ExecuteScript(wscript.c_str(), nullptr);
  }
}

void WebView2Backend::Quit() {
  PostQuitMessage(0);
}

void WebView2Backend::SetWindowSize(uint32_t window_id, int width, int height) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    SetWindowPos(state->hwnd, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER);
  }
}

void WebView2Backend::GetWindowSize(uint32_t window_id, int* width, int* height) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    RECT rect;
    if (GetWindowRect(state->hwnd, &rect)) {
      if (width) *width = rect.right - rect.left;
      if (height) *height = rect.bottom - rect.top;
    }
  }
}

void WebView2Backend::SetWindowPosition(uint32_t window_id, int x, int y) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    SetWindowPos(state->hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
  }
}

void WebView2Backend::GetWindowPosition(uint32_t window_id, int* x, int* y) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    RECT rect;
    if (GetWindowRect(state->hwnd, &rect)) {
      if (x) *x = rect.left;
      if (y) *y = rect.top;
    }
  }
}

void WebView2Backend::SetResizable(uint32_t window_id, bool resizable) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    LONG style = GetWindowLong(state->hwnd, GWL_STYLE);
    if (resizable) {
      style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    } else {
      style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
    }
    SetWindowLong(state->hwnd, GWL_STYLE, style);
  }
}

bool WebView2Backend::IsResizable(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? (GetWindowLong(state->hwnd, GWL_STYLE) & WS_THICKFRAME) != 0 : false;
}

void WebView2Backend::SetAlwaysOnTop(uint32_t window_id, bool always_on_top) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    SetWindowPos(state->hwnd, always_on_top ? HWND_TOPMOST : HWND_NOTOPMOST,
                 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
  }
}

bool WebView2Backend::IsAlwaysOnTop(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? (GetWindowLong(state->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0 : false;
}

bool WebView2Backend::IsVisible(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? IsWindowVisible(state->hwnd) != FALSE : false;
}

void WebView2Backend::Show(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) ShowWindow(state->hwnd, SW_SHOW);
}

void WebView2Backend::Hide(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) ShowWindow(state->hwnd, SW_HIDE);
}

void WebView2Backend::Focus(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    ShowWindow(state->hwnd, SW_SHOW);
    SetForegroundWindow(state->hwnd);
    SetFocus(state->hwnd);
  }
}

void WebView2Backend::PostUiTask(void (*task)(void*), void* data) {
  // Post to the first available window, or use a message-only window
  std::lock_guard<std::mutex> lock(windows_mutex_);
  if (!windows_.empty()) {
    PostMessage(windows_.begin()->second.hwnd, WM_UI_TASK, 0,
                reinterpret_cast<LPARAM>(new UiTaskData{task, data}));
  }
}

void WebView2Backend::InvokeJsCallback(uint32_t window_id, uint64_t callback_id, wef::ValuePtr args) {
  std::string argsJson = json::Serialize(args);
  std::string script = "window.__wefInvokeCallback(" +
                       std::to_string(callback_id) + ", " + argsJson + ");";
  std::wstring wscript = Utf8ToWide(script);
  std::lock_guard<std::mutex> lock(windows_mutex_);
  if (window_id == 0) {
    for (auto& [wid, state] : windows_) {
      if (state.webview_ready && state.webview) {
        state.webview->ExecuteScript(wscript.c_str(), nullptr);
      }
    }
  } else {
    auto* state = GetWindow(window_id);
    if (state && state->webview_ready && state->webview) {
      state->webview->ExecuteScript(wscript.c_str(), nullptr);
    }
  }
}

void WebView2Backend::ReleaseJsCallback(uint32_t window_id, uint64_t callback_id) {
  std::string script = "window.__wefReleaseCallback(" +
                       std::to_string(callback_id) + ");";
  std::wstring wscript = Utf8ToWide(script);
  std::lock_guard<std::mutex> lock(windows_mutex_);
  if (window_id == 0) {
    for (auto& [wid, state] : windows_) {
      if (state.webview_ready && state.webview) {
        state.webview->ExecuteScript(wscript.c_str(), nullptr);
      }
    }
  } else {
    auto* state = GetWindow(window_id);
    if (state && state->webview_ready && state->webview) {
      state->webview->ExecuteScript(wscript.c_str(), nullptr);
    }
  }
}

void WebView2Backend::RespondToJsCall(uint32_t window_id, uint64_t call_id,
                                       wef::ValuePtr result, wef::ValuePtr error) {
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
  std::wstring wscript = Utf8ToWide(script);
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state && state->webview_ready && state->webview) {
    state->webview->ExecuteScript(wscript.c_str(), nullptr);
  }
}

void WebView2Backend::Run() {
  MSG msg;
  while (GetMessage(&msg, nullptr, 0, 0)) {
    TranslateMessage(&msg);
    DispatchMessage(&msg);
  }
}

void WebView2Backend::HandleJsMessage(uint32_t window_id, const std::wstring& json) {
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

  RuntimeLoader::GetInstance()->OnJsCall(window_id, call_id, method, args);
}

// ============================================================================
// Factory Function
// ============================================================================

WefBackend* CreateWefBackend() {
  return new WebView2Backend();
}
