// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef WEF_APP_H_
#define WEF_APP_H_

#include <list>
#include <string>

#include "include/cef_app.h"
#include "include/cef_client.h"

extern std::string g_runtime_path;

class WefHandler : public CefClient,
                   public CefLifeSpanHandler,
                   public CefDisplayHandler {
 public:
  WefHandler();
  ~WefHandler() override;

  static WefHandler* GetInstance();

  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  bool DoClose(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  void OnTitleChange(CefRefPtr<CefBrowser> browser,
                     const CefString& title) override;

  bool OnProcessMessageReceived(CefRefPtr<CefBrowser> browser,
                                CefRefPtr<CefFrame> frame,
                                CefProcessId source_process,
                                CefRefPtr<CefProcessMessage> message) override;

  void CloseAllBrowsers(bool force_close);
  bool IsClosing() const { return is_closing_; }

 private:
  std::list<CefRefPtr<CefBrowser>> browser_list_;
  bool is_closing_ = false;

  IMPLEMENT_REFCOUNTING(WefHandler);
};

class WefApp : public CefApp, public CefBrowserProcessHandler {
 public:
  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
    command_line->AppendSwitch("use-mock-keychain");
  }

  void OnContextInitialized() override;

 private:
  IMPLEMENT_REFCOUNTING(WefApp);
};

#endif
