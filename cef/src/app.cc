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

namespace {
WefHandler* g_handler = nullptr;

class WefWindowDelegate : public CefWindowDelegate {
 public:
  explicit WefWindowDelegate(CefRefPtr<CefBrowserView> browser_view)
      : browser_view_(browser_view) {}

  void OnWindowCreated(CefRefPtr<CefWindow> window) override {
    window->AddChildView(browser_view_);
    window->Show();
  }

  void OnWindowDestroyed(CefRefPtr<CefWindow> window) override {
    browser_view_ = nullptr;
  }

  bool CanClose(CefRefPtr<CefWindow> window) override {
    CefRefPtr<CefBrowser> browser = browser_view_->GetBrowser();
    return browser ? browser->GetHost()->TryCloseBrowser() : true;
  }

  CefSize GetPreferredSize(CefRefPtr<CefView> view) override {
    return CefSize(800, 600);
  }

 private:
  CefRefPtr<CefBrowserView> browser_view_;
  IMPLEMENT_REFCOUNTING(WefWindowDelegate);
};

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

  RuntimeLoader::GetInstance()->SetBrowser(browser);

  if (!g_runtime_path.empty()) {
    RuntimeLoader::GetInstance()->Start();
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

    RuntimeLoader::GetInstance()->OnJsCall(call_id, method_path, callArgs);
    return true;
  }

  return false;
}

void WefApp::OnContextInitialized() {
  CEF_REQUIRE_UI_THREAD();

  if (!g_runtime_path.empty()) {
    if (!RuntimeLoader::GetInstance()->Load(g_runtime_path)) {
      std::cerr << "Failed to load runtime, exiting" << std::endl;
      CefQuitMessageLoop();
      return;
    }
  }

  CefRefPtr<WefHandler> handler(new WefHandler());
  CefBrowserSettings browser_settings;

  std::string initial_url = g_runtime_path.empty() ? "https://example.com" : "about:blank";

  CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(
      handler, initial_url, browser_settings, nullptr, nullptr, nullptr);

  CefWindow::CreateTopLevelWindow(new WefWindowDelegate(browser_view));
}
