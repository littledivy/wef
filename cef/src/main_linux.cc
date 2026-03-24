// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

#include "include/cef_app.h"
#include "include/wrapper/cef_helpers.h"

#include "app.h"
#include "renderer_app.h"
#include "runtime_loader.h"

// --- Native event monitors (Linux / X11) ---
// CEF Views on Linux uses X11 internally. The keyboard events are already
// captured through CEF's OnKeyEvent handler. Mouse event interception
// would require XRecord or similar — stubbed for now.

void InstallNativeMouseMonitor() {
  // TODO: implement via XRecord or GDK event filter
}

void RemoveNativeMouseMonitor() {
  // TODO: cleanup
}

// --- Headless / forked worker support ---

static int run_headless(const std::string& runtimePath) {
  RuntimeLoader* loader = RuntimeLoader::GetInstance();

  if (runtimePath.empty()) {
    std::cerr << "No runtime library found for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Load(runtimePath)) {
    std::cerr << "Failed to load runtime for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Start()) {
    std::cerr << "Failed to start headless worker runtime." << std::endl;
    return 1;
  }

  loader->Shutdown();
  return 0;
}

static bool is_forked_worker() {
  return getenv("NODE_CHANNEL_FD") != nullptr
      || getenv("NEXT_PRIVATE_WORKER") != nullptr;
}

static bool is_cli_worker_command(int argc, char* argv[]) {
  if (argc < 3 || strcmp(argv[1], "run") != 0) {
    return false;
  }
  for (int i = 2; i < argc; ++i) {
    if (argv[i][0] == '-') {
      continue;
    }
    return true;
  }
  return false;
}

// Combined app that handles both browser and renderer processes (single-exe model)
class WefCombinedApp : public CefApp,
                       public CefBrowserProcessHandler {
 public:
  WefCombinedApp() : renderer_app_(new WefRendererApp()) {}

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return renderer_app_->GetRenderProcessHandler();
  }

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
  }

  void OnContextInitialized() override {
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

    uint32_t wef_id = RuntimeLoader::GetInstance()->AllocateWindowId();
    g_pending_wef_ids.push(wef_id);
    CefWindow::CreateTopLevelWindow(
        new WefWindowDelegate(browser_view, wef_id));
  }

 private:
  CefRefPtr<WefRendererApp> renderer_app_;
  IMPLEMENT_REFCOUNTING(WefCombinedApp);
};

int main(int argc, char* argv[]) {
  CefMainArgs main_args(argc, argv);

  // Single-exe model: check if we are a subprocess first
  CefRefPtr<WefCombinedApp> app(new WefCombinedApp());
  int exit_code = CefExecuteProcess(main_args, app, nullptr);
  if (exit_code >= 0) {
    return exit_code;
  }

  // Parse --runtime argument
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc) {
      g_runtime_path = argv[++i];
    } else if (strncmp(argv[i], "--runtime=", 10) == 0) {
      g_runtime_path = argv[i] + 10;
    }
  }

  if (g_runtime_path.empty()) {
    const char* envPath = getenv("WEF_RUNTIME_PATH");
    if (envPath) {
      g_runtime_path = envPath;
    }
  }

  // Check for headless / forked worker mode (skip CEF entirely)
  if (is_forked_worker() || is_cli_worker_command(argc, argv)) {
    return run_headless(g_runtime_path);
  }

  CefSettings settings;
  settings.no_sandbox = true;

  // Set cache path
  std::string cache_path = "/tmp/wef_cef_" + std::to_string(getpid());
  CefString(&settings.root_cache_path) = cache_path;

  if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
    return 1;
  }

  CefRunMessageLoop();

  RuntimeLoader::GetInstance()->Shutdown();

  CefShutdown();

  return 0;
}
