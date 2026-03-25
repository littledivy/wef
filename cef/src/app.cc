// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "app.h"
#include "runtime_loader.h"

#include <iostream>

#include "include/base/cef_callback.h"
#include "include/cef_browser.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

std::string g_runtime_path;
std::queue<uint32_t> g_pending_wef_ids;

namespace {
WefHandler* g_handler = nullptr;
}

// WefWindowDelegate implementation

void WefWindowDelegate::OnWindowCreated(CefRefPtr<CefWindow> window) {
  window->AddChildView(browser_view_);
  window->Show();
  InstallNativeMouseMonitor();

  // Register native window for event routing
  CefWindowHandle handle = window->GetWindowHandle();
  if (handle && wef_id_ > 0) {
#ifdef __APPLE__
    RegisterNSWindowForCefHandle(handle, wef_id_);
#elif defined(_WIN32)
    RuntimeLoader::GetInstance()->RegisterNativeHandle((void*)handle, wef_id_);
#endif
  }
}

void WefWindowDelegate::OnWindowDestroyed(CefRefPtr<CefWindow> window) {
  // Unregister native window
  CefWindowHandle handle = window->GetWindowHandle();
  if (handle) {
#ifdef __APPLE__
    UnregisterNSWindowForCefHandle(handle);
#elif defined(_WIN32)
    RuntimeLoader::GetInstance()->UnregisterNativeHandle((void*)handle);
#endif
  }
  if (wef_id_ > 0) {
    RuntimeLoader::GetInstance()->UnregisterBrowser(wef_id_);
  }
  RemoveNativeMouseMonitor();
  browser_view_ = nullptr;
}

bool WefWindowDelegate::CanClose(CefRefPtr<CefWindow> window) {
  CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();
  return browser ? browser->GetHost()->TryCloseBrowser() : true;
}

CefSize WefWindowDelegate::GetPreferredSize(CefRefPtr<CefView> view) {
  return CefSize(800, 600);
}

WefHandler::WefHandler() {
  g_handler = this;
}

WefHandler::~WefHandler() {
  g_handler = nullptr;
}

WefHandler* WefHandler::GetInstance() {
  return g_handler;
}

void WefHandler::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  browser_list_.push_back(browser);

  auto* loader = RuntimeLoader::GetInstance();
  if (!g_pending_wef_ids.empty()) {
    uint32_t wef_id = g_pending_wef_ids.front();
    g_pending_wef_ids.pop();
    loader->RegisterBrowser(wef_id, browser);
  }
}

bool WefHandler::DoClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();
  if (browser_list_.size() == 1) {
    is_closing_ = true;
  }
  return false;
}

void WefHandler::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  CEF_REQUIRE_UI_THREAD();

  auto* loader = RuntimeLoader::GetInstance();
  uint32_t wid = loader->GetWefIdForBrowser(browser);
  if (wid > 0) {
    loader->DispatchCloseRequestedEvent(wid);
  }

  for (auto it = browser_list_.begin(); it != browser_list_.end(); ++it) {
    if ((*it)->IsSame(browser)) {
      browser_list_.erase(it);
      break;
    }
  }
  if (browser_list_.empty()) {
    CefQuitMessageLoop();
  }
}

void WefHandler::OnTitleChange(CefRefPtr<CefBrowser> browser,
                               const CefString& title) {
  CEF_REQUIRE_UI_THREAD();
  if (auto browser_view = CefBrowserView::GetForBrowser(browser)) {
    if (auto window = browser_view->GetWindow()) {
      window->SetTitle(title);
    }
  }
}

namespace {

std::string CefKeyCodeToString(int windows_key_code, uint32_t character, bool is_char) {
  // If we have a character, use it for the "key" value
  if (character > 0 && character < 0x7F && isprint(static_cast<char>(character))) {
    return std::string(1, static_cast<char>(character));
  }
  // Map virtual key codes to W3C key values
  switch (windows_key_code) {
    case 8: return "Backspace";
    case 9: return "Tab";
    case 13: return "Enter";
    case 16: return "Shift";
    case 17: return "Control";
    case 18: return "Alt";
    case 19: return "Pause";
    case 20: return "CapsLock";
    case 27: return "Escape";
    case 32: return " ";
    case 33: return "PageUp";
    case 34: return "PageDown";
    case 35: return "End";
    case 36: return "Home";
    case 37: return "ArrowLeft";
    case 38: return "ArrowUp";
    case 39: return "ArrowRight";
    case 40: return "ArrowDown";
    case 45: return "Insert";
    case 46: return "Delete";
    case 91: case 93: return "Meta";
    case 112: return "F1";
    case 113: return "F2";
    case 114: return "F3";
    case 115: return "F4";
    case 116: return "F5";
    case 117: return "F6";
    case 118: return "F7";
    case 119: return "F8";
    case 120: return "F9";
    case 121: return "F10";
    case 122: return "F11";
    case 123: return "F12";
    case 144: return "NumLock";
    case 145: return "ScrollLock";
    default:
      if (windows_key_code >= 65 && windows_key_code <= 90) {
        return std::string(1, static_cast<char>(windows_key_code + 32)); // lowercase a-z
      }
      if (windows_key_code >= 48 && windows_key_code <= 57) {
        return std::string(1, static_cast<char>(windows_key_code)); // 0-9
      }
      return "Unidentified";
  }
}

std::string CefKeyCodeToCode(int windows_key_code) {
  switch (windows_key_code) {
    case 8: return "Backspace";
    case 9: return "Tab";
    case 13: return "Enter";
    case 16: return "ShiftLeft";
    case 17: return "ControlLeft";
    case 18: return "AltLeft";
    case 19: return "Pause";
    case 20: return "CapsLock";
    case 27: return "Escape";
    case 32: return "Space";
    case 33: return "PageUp";
    case 34: return "PageDown";
    case 35: return "End";
    case 36: return "Home";
    case 37: return "ArrowLeft";
    case 38: return "ArrowUp";
    case 39: return "ArrowRight";
    case 40: return "ArrowDown";
    case 45: return "Insert";
    case 46: return "Delete";
    case 91: return "MetaLeft";
    case 93: return "MetaRight";
    case 112: return "F1";
    case 113: return "F2";
    case 114: return "F3";
    case 115: return "F4";
    case 116: return "F5";
    case 117: return "F6";
    case 118: return "F7";
    case 119: return "F8";
    case 120: return "F9";
    case 121: return "F10";
    case 122: return "F11";
    case 123: return "F12";
    case 144: return "NumLock";
    case 145: return "ScrollLock";
    case 186: return "Semicolon";
    case 187: return "Equal";
    case 188: return "Comma";
    case 189: return "Minus";
    case 190: return "Period";
    case 191: return "Slash";
    case 192: return "Backquote";
    case 219: return "BracketLeft";
    case 220: return "Backslash";
    case 221: return "BracketRight";
    case 222: return "Quote";
    default:
      if (windows_key_code >= 65 && windows_key_code <= 90) {
        return "Key" + std::string(1, static_cast<char>(windows_key_code));
      }
      if (windows_key_code >= 48 && windows_key_code <= 57) {
        return "Digit" + std::string(1, static_cast<char>(windows_key_code));
      }
      return "Unidentified";
  }
}

} // namespace

bool WefHandler::OnKeyEvent(CefRefPtr<CefBrowser> browser,
                             const CefKeyEvent& event,
                             CefEventHandle os_event) {
  int state;
  if (event.type == KEYEVENT_RAWKEYDOWN || event.type == KEYEVENT_KEYDOWN) {
    state = WEF_KEY_PRESSED;
  } else if (event.type == KEYEVENT_KEYUP) {
    state = WEF_KEY_RELEASED;
  } else {
    return false;
  }

  uint32_t modifiers = 0;
  if (event.modifiers & EVENTFLAG_SHIFT_DOWN) modifiers |= WEF_MOD_SHIFT;
  if (event.modifiers & EVENTFLAG_CONTROL_DOWN) modifiers |= WEF_MOD_CONTROL;
  if (event.modifiers & EVENTFLAG_ALT_DOWN) modifiers |= WEF_MOD_ALT;
  if (event.modifiers & EVENTFLAG_COMMAND_DOWN) modifiers |= WEF_MOD_META;

  std::string key = CefKeyCodeToString(event.windows_key_code, event.character, event.type == KEYEVENT_CHAR);
  std::string code = CefKeyCodeToCode(event.windows_key_code);

  uint32_t wid = RuntimeLoader::GetInstance()->GetWefIdForBrowser(browser);
  RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
      wid, state, key.c_str(), code.c_str(), modifiers, false);

  return false; // Don't consume the event — let CEF handle it too
}

void WefHandler::CloseAllBrowsers(bool force_close) {
  if (!CefCurrentlyOn(TID_UI)) {
    CefPostTask(TID_UI, base::BindOnce(&WefHandler::CloseAllBrowsers, this, force_close));
    return;
  }
  for (const auto& browser : browser_list_) {
    browser->GetHost()->CloseBrowser(force_close);
  }
}

bool WefHandler::OnProcessMessageReceived(
    CefRefPtr<CefBrowser> browser,
    CefRefPtr<CefFrame> frame,
    CefProcessId source_process,
    CefRefPtr<CefProcessMessage> message) {
  CEF_REQUIRE_UI_THREAD();

  const std::string& name = message->GetName().ToString();

  if (name == "wef_call") {
    CefRefPtr<CefListValue> args = message->GetArgumentList();
    uint64_t call_id = static_cast<uint64_t>(args->GetInt(0));
    std::string method_path = args->GetString(1).ToString();
    CefRefPtr<CefListValue> callArgs = args->GetList(2);

    uint32_t wid = RuntimeLoader::GetInstance()->GetWefIdForBrowser(browser);
    RuntimeLoader::GetInstance()->OnJsCall(wid, call_id, method_path, callArgs);
    return true;
  }

  return false;
}

void WefApp::OnContextInitialized() {
  CEF_REQUIRE_UI_THREAD();

  // Create the handler and keep it alive for the lifetime of the app.
  // Backend_CreateWindow uses WefHandler::GetInstance() from the runtime thread,
  // so the handler must outlive this function scope.
  static CefRefPtr<WefHandler> handler(new WefHandler());

  if (!g_runtime_path.empty()) {
    if (!RuntimeLoader::GetInstance()->Load(g_runtime_path)) {
      std::cerr << "Failed to load runtime, exiting" << std::endl;
      CefQuitMessageLoop();
      return;
    }
    // Defer Start() to the next message loop iteration. OnContextInitialized
    // runs during CefInitialize(), before CefRunMessageLoop() has started.
    // The runtime thread's Backend_CreateWindow posts CefPostTasks to the UI
    // thread and blocks until they complete — this deadlocks if the message
    // loop isn't running yet.
    CefPostTask(TID_UI, base::BindOnce([]() {
      RuntimeLoader::GetInstance()->Start();
    }));
  } else {
    // No runtime: create a default window for demo
    uint32_t wef_id = RuntimeLoader::GetInstance()->AllocateWindowId();
    g_pending_wef_ids.push(wef_id);
    CefBrowserSettings browser_settings;
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(
        handler, "https://example.com", browser_settings, nullptr, nullptr, nullptr);
    CefWindow::CreateTopLevelWindow(new WefWindowDelegate(browser_view, wef_id));
  }
}
