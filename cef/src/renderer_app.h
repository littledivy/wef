// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef WEF_RENDERER_APP_H_
#define WEF_RENDERER_APP_H_

#include "include/cef_app.h"
#include "render_process_handler.h"

class WefRendererApp : public CefApp {
 public:
  WefRendererApp();

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return render_handler_;
  }

 private:
  CefRefPtr<WefRenderProcessHandler> render_handler_;

  IMPLEMENT_REFCOUNTING(WefRendererApp);
};

#endif
