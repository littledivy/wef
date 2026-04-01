// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"
#include "app.h"

#ifndef _WIN32
#include <dlfcn.h>
#else
#include <windows.h>
#endif

#include <iostream>
#include <cstring>
#include <vector>
#include <mutex>
#include <condition_variable>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

RuntimeLoader* RuntimeLoader::instance_ = nullptr;

// Helper to run a callback synchronously on the CEF UI thread.
// If already on the UI thread, runs immediately.
template <typename F>
static void cef_invoke_sync(F&& fn) {
  if (CefCurrentlyOn(TID_UI)) {
    fn();
    return;
  }
  std::mutex mtx;
  std::condition_variable cv;
  bool done = false;
  CefPostTask(TID_UI, base::BindOnce(
      [](F* fn, std::mutex* mtx, std::condition_variable* cv, bool* done) {
        (*fn)();
        {
          std::lock_guard<std::mutex> lock(*mtx);
          *done = true;
        }
        cv->notify_one();
      },
      &fn, &mtx, &cv, &done));
  std::unique_lock<std::mutex> lock(mtx);
  cv.wait(lock, [&done] { return done; });
}

// --- Backend API functions (cross-platform, using CEF Views) ---

static void Backend_Navigate(void* data, uint32_t window_id, const char* url) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser && url) {
    std::string url_str(url);
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b, std::string u) {
          b->GetMainFrame()->LoadURL(u);
        },
        browser, url_str));
  }
}

static void Backend_SetTitle(void* data, uint32_t window_id, const char* title) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser && title) {
    std::string title_str(title);
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b, std::string t) {
          auto browser_view = CefBrowserView::GetForBrowser(b);
          if (browser_view) {
            auto window = browser_view->GetWindow();
            if (window) {
              window->SetTitle(t);
            }
          }
        },
        browser, title_str));
  }
}

static void Backend_ExecuteJs(void* data, uint32_t window_id, const char* script,
                              wef_js_result_fn callback, void* callback_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser && script) {
    std::string script_str(script);
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b, std::string s) {
          b->GetMainFrame()->ExecuteJavaScript(s, "", 0);
        },
        browser, script_str));
    if (callback) {
      callback(nullptr, nullptr, callback_data);
    }
  }
}

static void Backend_Quit(void* data) {
  CefPostTask(TID_UI, base::BindOnce([]() {
    CefQuitMessageLoop();
  }));
}

static void Backend_SetWindowSize(void* data, uint32_t window_id, int width, int height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b, int w, int h) {
          auto browser_view = CefBrowserView::GetForBrowser(b);
          if (browser_view) {
            auto window = browser_view->GetWindow();
            if (window) {
              window->SetSize(CefSize(w, h));
            }
          }
        },
        browser, width, height));
  }
}

static void Backend_GetWindowSize(void* data, uint32_t window_id, int* width, int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  int w = 0, h = 0;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (browser_view) {
        auto window = browser_view->GetWindow();
        if (window) {
          CefSize size = window->GetSize();
          w = size.width;
          h = size.height;
        }
      }
    });
  }
  if (width) *width = w;
  if (height) *height = h;
}

static void Backend_SetWindowPosition(void* data, uint32_t window_id, int x, int y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b, int px, int py) {
          auto browser_view = CefBrowserView::GetForBrowser(b);
          if (browser_view) {
            auto window = browser_view->GetWindow();
            if (window) {
              window->SetPosition(CefPoint(px, py));
            }
          }
        },
        browser, x, y));
  }
}

static void Backend_GetWindowPosition(void* data, uint32_t window_id, int* x, int* y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  int px = 0, py = 0;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (browser_view) {
        auto window = browser_view->GetWindow();
        if (window) {
          CefPoint pos = window->GetPosition();
          px = pos.x;
          py = pos.y;
        }
      }
    });
  }
  if (x) *x = px;
  if (y) *y = py;
}

static void Backend_SetResizable(void* data, uint32_t window_id, bool resizable) {
  // CEF Views framework does not expose a direct resizable toggle
  (void)data;
  (void)window_id;
  (void)resizable;
}

static bool Backend_IsResizable(void* data, uint32_t window_id) {
  // CEF Views framework does not expose a direct resizable query
  (void)data;
  (void)window_id;
  return true;
}

static void Backend_SetAlwaysOnTop(void* data, uint32_t window_id, bool always_on_top) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b, bool on_top) {
          auto browser_view = CefBrowserView::GetForBrowser(b);
          if (browser_view) {
            auto window = browser_view->GetWindow();
            if (window) {
              window->SetAlwaysOnTop(on_top);
            }
          }
        },
        browser, always_on_top));
  }
}

static bool Backend_IsAlwaysOnTop(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  bool result = false;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (browser_view) {
        auto window = browser_view->GetWindow();
        if (window) {
          result = window->IsAlwaysOnTop();
        }
      }
    });
  }
  return result;
}

static bool Backend_IsVisible(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  bool result = false;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (browser_view) {
        auto window = browser_view->GetWindow();
        if (window) {
          result = window->IsVisible();
        }
      }
    });
  }
  return result;
}

static void Backend_Show(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b) {
          auto browser_view = CefBrowserView::GetForBrowser(b);
          if (browser_view) {
            auto window = browser_view->GetWindow();
            if (window) {
              window->Show();
            }
          }
        },
        browser));
  }
}

static void Backend_Hide(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b) {
          auto browser_view = CefBrowserView::GetForBrowser(b);
          if (browser_view) {
            auto window = browser_view->GetWindow();
            if (window) {
              window->Hide();
            }
          }
        },
        browser));
  }
}

static void Backend_Focus(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b) {
          auto browser_view = CefBrowserView::GetForBrowser(b);
          if (browser_view) {
            auto window = browser_view->GetWindow();
            if (window) {
              window->Show();
              window->Activate();
            }
          }
        },
        browser));
  }
}

static void Backend_PostUiTask(void* data, void (*task)(void*), void* task_data) {
  if (task) {
    CefPostTask(TID_UI, base::BindOnce(
        [](void (*t)(void*), void* d) {
          t(d);
        },
        task, task_data));
  }
}

// --- Value accessors ---

static bool Backend_ValueIsNull(wef_value_t* val) {
  if (!val || !val->value) return true;
  return val->value->GetType() == VTYPE_NULL;
}

static bool Backend_ValueIsBool(wef_value_t* val) {
  if (!val || !val->value) return false;
  return val->value->GetType() == VTYPE_BOOL;
}

static bool Backend_ValueIsInt(wef_value_t* val) {
  if (!val || !val->value) return false;
  return val->value->GetType() == VTYPE_INT;
}

static bool Backend_ValueIsDouble(wef_value_t* val) {
  if (!val || !val->value) return false;
  return val->value->GetType() == VTYPE_DOUBLE;
}

static bool Backend_ValueIsString(wef_value_t* val) {
  if (!val || !val->value) return false;
  return val->value->GetType() == VTYPE_STRING;
}

static bool Backend_ValueIsList(wef_value_t* val) {
  if (!val || !val->value) return false;
  return val->value->GetType() == VTYPE_LIST;
}

static bool Backend_ValueIsDict(wef_value_t* val) {
  if (!val || !val->value) return false;
  return val->value->GetType() == VTYPE_DICTIONARY;
}

static bool Backend_ValueIsBinary(wef_value_t* val) {
  if (!val || !val->value) return false;
  return val->value->GetType() == VTYPE_BINARY;
}

static bool Backend_ValueIsCallback(wef_value_t* val) {
  if (!val) return false;
  return val->is_callback;
}

static bool Backend_ValueGetBool(wef_value_t* val) {
  if (!val || !val->value) return false;
  return val->value->GetBool();
}

static int Backend_ValueGetInt(wef_value_t* val) {
  if (!val || !val->value) return 0;
  return val->value->GetInt();
}

static double Backend_ValueGetDouble(wef_value_t* val) {
  if (!val || !val->value) return 0.0;
  return val->value->GetDouble();
}

static char* Backend_ValueGetString(wef_value_t* val, size_t* len_out) {
  if (!val || !val->value) {
    if (len_out) *len_out = 0;
    return nullptr;
  }
  std::string str = val->value->GetString().ToString();
  if (len_out) *len_out = str.size();
  char* result = static_cast<char*>(malloc(str.size() + 1));
  if (result) {
    memcpy(result, str.c_str(), str.size() + 1);
  }
  return result;
}

static void Backend_ValueFreeString(char* str) {
  free(str);
}

static size_t Backend_ValueListSize(wef_value_t* val) {
  if (!val || !val->value || val->value->GetType() != VTYPE_LIST) return 0;
  return val->value->GetList()->GetSize();
}

static wef_value_t* Backend_ValueListGet(wef_value_t* val, size_t index) {
  if (!val || !val->value || val->value->GetType() != VTYPE_LIST) return nullptr;
  CefRefPtr<CefListValue> list = val->value->GetList();
  if (index >= list->GetSize()) return nullptr;
  return new wef_value(list->GetValue(index));
}

static wef_value_t* Backend_ValueDictGet(wef_value_t* dict, const char* key) {
  if (!dict || !dict->value || dict->value->GetType() != VTYPE_DICTIONARY || !key)
    return nullptr;
  CefRefPtr<CefDictionaryValue> d = dict->value->GetDictionary();
  if (!d->HasKey(key)) return nullptr;
  return new wef_value(d->GetValue(key));
}

static bool Backend_ValueDictHas(wef_value_t* dict, const char* key) {
  if (!dict || !dict->value || dict->value->GetType() != VTYPE_DICTIONARY || !key)
    return false;
  return dict->value->GetDictionary()->HasKey(key);
}

static size_t Backend_ValueDictSize(wef_value_t* dict) {
  if (!dict || !dict->value || dict->value->GetType() != VTYPE_DICTIONARY)
    return 0;
  return dict->value->GetDictionary()->GetSize();
}

static char** Backend_ValueDictKeys(wef_value_t* dict, size_t* count_out) {
  if (!dict || !dict->value || dict->value->GetType() != VTYPE_DICTIONARY) {
    if (count_out) *count_out = 0;
    return nullptr;
  }
  CefRefPtr<CefDictionaryValue> d = dict->value->GetDictionary();
  CefDictionaryValue::KeyList keys;
  d->GetKeys(keys);

  if (count_out) *count_out = keys.size();
  if (keys.empty()) return nullptr;

  char** result = static_cast<char**>(malloc(sizeof(char*) * keys.size()));
  for (size_t i = 0; i < keys.size(); ++i) {
    std::string key = keys[i].ToString();
    result[i] = static_cast<char*>(malloc(key.size() + 1));
    memcpy(result[i], key.c_str(), key.size() + 1);
  }
  return result;
}

static void Backend_ValueFreeKeys(char** keys, size_t count) {
  if (!keys) return;
  for (size_t i = 0; i < count; ++i) {
    free(keys[i]);
  }
  free(keys);
}

static const void* Backend_ValueGetBinary(wef_value_t* val, size_t* len_out) {
  if (!val || !val->value || val->value->GetType() != VTYPE_BINARY) {
    if (len_out) *len_out = 0;
    return nullptr;
  }
  CefRefPtr<CefBinaryValue> binary = val->value->GetBinary();
  if (len_out) *len_out = binary->GetSize();
  return binary->GetRawData();
}

static uint64_t Backend_ValueGetCallbackId(wef_value_t* val) {
  if (!val || !val->is_callback) return 0;
  return val->callback_id;
}

static wef_value_t* Backend_ValueNull(void*) {
  CefRefPtr<CefValue> val = CefValue::Create();
  val->SetNull();
  return new wef_value(val);
}

static wef_value_t* Backend_ValueBool(void*, bool v) {
  CefRefPtr<CefValue> val = CefValue::Create();
  val->SetBool(v);
  return new wef_value(val);
}

static wef_value_t* Backend_ValueInt(void*, int v) {
  CefRefPtr<CefValue> val = CefValue::Create();
  val->SetInt(v);
  return new wef_value(val);
}

static wef_value_t* Backend_ValueDouble(void*, double v) {
  CefRefPtr<CefValue> val = CefValue::Create();
  val->SetDouble(v);
  return new wef_value(val);
}

static wef_value_t* Backend_ValueString(void*, const char* v) {
  CefRefPtr<CefValue> val = CefValue::Create();
  val->SetString(v ? v : "");
  return new wef_value(val);
}

static wef_value_t* Backend_ValueList(void*) {
  CefRefPtr<CefValue> val = CefValue::Create();
  val->SetList(CefListValue::Create());
  return new wef_value(val);
}

static wef_value_t* Backend_ValueDict(void*) {
  CefRefPtr<CefValue> val = CefValue::Create();
  val->SetDictionary(CefDictionaryValue::Create());
  return new wef_value(val);
}

static wef_value_t* Backend_ValueBinary(void*, const void* data, size_t len) {
  CefRefPtr<CefValue> val = CefValue::Create();
  CefRefPtr<CefBinaryValue> binary = CefBinaryValue::Create(data, len);
  val->SetBinary(binary);
  return new wef_value(val);
}

static bool Backend_ValueListAppend(wef_value_t* list, wef_value_t* val) {
  if (!list || !list->value || list->value->GetType() != VTYPE_LIST)
    return false;
  if (!val || !val->value) return false;
  CefRefPtr<CefListValue> l = list->value->GetList();
  size_t index = l->GetSize();
  return l->SetValue(index, val->value);
}

static bool Backend_ValueListSet(wef_value_t* list, size_t index, wef_value_t* val) {
  if (!list || !list->value || list->value->GetType() != VTYPE_LIST)
    return false;
  if (!val || !val->value) return false;
  return list->value->GetList()->SetValue(index, val->value);
}

static bool Backend_ValueDictSet(wef_value_t* dict, const char* key, wef_value_t* val) {
  if (!dict || !dict->value || dict->value->GetType() != VTYPE_DICTIONARY)
    return false;
  if (!key || !val || !val->value) return false;
  return dict->value->GetDictionary()->SetValue(key, val->value);
}

static void Backend_ValueFree(wef_value_t* val) {
  delete val;
}

// --- JS call/callback handling ---

static void Backend_SetJsCallHandler(void* data, wef_js_call_fn handler, void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetJsCallHandler(handler, user_data);
}

static void Backend_JsCallRespond(void* data, uint64_t call_id,
                                  wef_value_t* result, wef_value_t* error) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  uint32_t window_id = loader->ConsumeCallWindow(call_id);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser) return;

  CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("wef_response");
  CefRefPtr<CefListValue> args = msg->GetArgumentList();
  args->SetInt(0, static_cast<int>(call_id));

  if (result && result->value) {
    args->SetValue(1, result->value);
  } else {
    CefRefPtr<CefValue> null_val = CefValue::Create();
    null_val->SetNull();
    args->SetValue(1, null_val);
  }

  if (error && error->value) {
    args->SetValue(2, error->value);
  } else {
    CefRefPtr<CefValue> null_val = CefValue::Create();
    null_val->SetNull();
    args->SetValue(2, null_val);
  }

  CefPostTask(TID_UI, base::BindOnce(
      [](CefRefPtr<CefBrowser> b, CefRefPtr<CefProcessMessage> m) {
        b->GetMainFrame()->SendProcessMessage(PID_RENDERER, m);
      },
      browser, msg));
}

static void Backend_InvokeJsCallback(void* data, uint64_t callback_id, wef_value_t* args) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);

  CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("wef_callback");
  CefRefPtr<CefListValue> msgArgs = msg->GetArgumentList();
  msgArgs->SetInt(0, static_cast<int>(callback_id));

  if (args && args->value && args->value->GetType() == VTYPE_LIST) {
    msgArgs->SetList(1, args->value->GetList());
  } else {
    msgArgs->SetList(1, CefListValue::Create());
  }

  loader->ForEachBrowser([&msg](CefRefPtr<CefBrowser> browser) {
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b, CefRefPtr<CefProcessMessage> m) {
          b->GetMainFrame()->SendProcessMessage(PID_RENDERER, m);
        },
        browser, msg));
  });
}

static void Backend_SetKeyboardEventHandler(void* data,
                                             wef_keyboard_event_fn handler,
                                             void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetKeyboardEventHandler(handler, user_data);
}

static void Backend_SetMouseClickHandler(void* data,
                                          wef_mouse_click_fn handler,
                                          void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMouseClickHandler(handler, user_data);
}

static void Backend_SetMouseMoveHandler(void* data,
                                         wef_mouse_move_fn handler,
                                         void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMouseMoveHandler(handler, user_data);
}

static void Backend_SetWheelHandler(void* data,
                                     wef_wheel_fn handler,
                                     void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetWheelHandler(handler, user_data);
}

static void Backend_SetCursorEnterLeaveHandler(void* data,
                                                wef_cursor_enter_leave_fn handler,
                                                void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetCursorEnterLeaveHandler(handler, user_data);
}

static void Backend_SetFocusedHandler(void* data,
                                       wef_focused_fn handler,
                                       void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetFocusedHandler(handler, user_data);
}

static void Backend_SetResizeHandler(void* data,
                                      wef_resize_fn handler,
                                      void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetResizeHandler(handler, user_data);
}

static void Backend_SetMoveHandler(void* data,
                                    wef_move_fn handler,
                                    void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMoveHandler(handler, user_data);
}

static void Backend_ReleaseJsCallback(void* data, uint64_t callback_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);

  CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("wef_release_callback");
  CefRefPtr<CefListValue> msgArgs = msg->GetArgumentList();
  msgArgs->SetInt(0, static_cast<int>(callback_id));

  loader->ForEachBrowser([&msg](CefRefPtr<CefBrowser> browser) {
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b, CefRefPtr<CefProcessMessage> m) {
          b->GetMainFrame()->SendProcessMessage(PID_RENDERER, m);
        },
        browser, msg));
  });
}

// --- Platform-specific menu (stub on Windows, implemented in runtime_loader.mm on macOS) ---

#if defined(_WIN32)
#include <win32_menu.h>

static void Backend_SetApplicationMenu(void* data, uint32_t window_id,
                                       wef_value_t* menu_template,
                                       wef_menu_click_fn on_click,
                                       void* on_click_data) {
  if (!menu_template) return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();

  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b, uint32_t wid, wef_value_t* tmpl, const wef_backend_api_t* a,
           wef_menu_click_fn fn, void* d) {
          HWND hwnd = b->GetHost()->GetWindowHandle();
          if (hwnd) {
            win32_menu::SetApplicationMenu(hwnd, tmpl, a, fn, d, wid);
          }
        },
        browser, window_id, menu_template, api, on_click, on_click_data));
  }
}

static void Backend_ShowContextMenu(void* data, uint32_t window_id,
                                    int x, int y,
                                    wef_value_t* menu_template,
                                    wef_menu_click_fn on_click,
                                    void* on_click_data) {
  if (!menu_template) return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();

  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b, uint32_t wid, int cx, int cy,
           wef_value_t* tmpl, const wef_backend_api_t* a,
           wef_menu_click_fn fn, void* d) {
          HWND hwnd = b->GetHost()->GetWindowHandle();
          if (hwnd) {
            win32_menu::ShowContextMenu(hwnd, cx, cy, tmpl, a, fn, d, wid);
          }
        },
        browser, window_id, x, y, menu_template, api, on_click, on_click_data));
  }
}
#elif defined(__APPLE__)
// Defined in runtime_loader_mac.mm
extern void Backend_SetApplicationMenu_Mac(void* data, uint32_t window_id,
                                           wef_value_t* menu_template,
                                           wef_menu_click_fn on_click,
                                           void* on_click_data);
extern void Backend_ShowContextMenu_Mac(void* data, uint32_t window_id,
                                        int x, int y,
                                        wef_value_t* menu_template,
                                        wef_menu_click_fn on_click,
                                        void* on_click_data);
#endif

static void Backend_OpenDevTools(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b) {
          CefWindowInfo windowInfo;
#if defined(_WIN32)
          windowInfo.SetAsPopup(nullptr, "DevTools");
#endif
          b->GetHost()->ShowDevTools(windowInfo, nullptr, CefBrowserSettings(), CefPoint());
        },
        browser));
  }
}

static void Backend_SetJsNamespace(void* data, const char* name) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  if (name) {
    loader->SetJsNamespace(name);
  }
}

// --- InitializeBackendApi ---

static uint32_t Backend_CreateWindow(void* data) {
  auto* loader = RuntimeLoader::GetInstance();
  uint32_t window_id = loader->AllocateWindowId();

  CefPostTask(TID_UI, base::BindOnce([](uint32_t wid) {
    auto* handler = WefHandler::GetInstance();
    if (!handler) return;

    // Push wef_id before creating the browser so OnAfterCreated can pop it.
    // Both run on the UI thread so no race.
    g_pending_wef_ids.push(wid);

    CefBrowserSettings browser_settings;
    CefRefPtr<CefDictionaryValue> extra_info = CefDictionaryValue::Create();
    extra_info->SetString("wef_js_namespace",
        RuntimeLoader::GetInstance()->GetJsNamespace());
    CefRefPtr<CefBrowserView> browser_view = CefBrowserView::CreateBrowserView(
        handler, "about:blank", browser_settings, extra_info, nullptr, nullptr);
    CefWindow::CreateTopLevelWindow(new WefWindowDelegate(browser_view, wid));
  }, window_id));

  // Block until the browser is registered by OnAfterCreated, so that
  // subsequent calls (navigate, set_title, etc.) can find it.
  loader->WaitForBrowser(window_id);

  return window_id;
}

static void Backend_CloseWindow(void* data, uint32_t window_id) {
  auto* loader = RuntimeLoader::GetInstance();
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b) {
          b->GetHost()->CloseBrowser(true);
        },
        browser));
  }
}

static void Backend_SetCloseRequestedHandler(void* data,
                                              wef_close_requested_fn handler,
                                              void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetCloseRequestedHandler(handler, user_data);
}

static void Backend_ShowDialog(void* data, uint32_t window_id, int dialog_type,
                               const char* title, const char* message,
                               const char* default_value,
                               wef_dialog_result_fn callback, void* callback_data) {
  std::string title_str = title ? title : "";
  std::string message_str = message ? message : "";
  std::string default_str = default_value ? default_value : "";

#ifdef __APPLE__
  ShowNativeDialog_Mac(dialog_type, title_str.c_str(), message_str.c_str(),
                       default_str.c_str(), callback, callback_data);
#elif defined(__linux__)
  // Use zenity for dialogs on Linux
  std::string cmd;
  if (dialog_type == WEF_DIALOG_ALERT) {
    cmd = "zenity --info --title=\"" + title_str + "\" --text=\"" + message_str + "\" 2>/dev/null";
    int ret = system(cmd.c_str());
    if (callback) callback(callback_data, (ret == 0) ? 1 : 0, nullptr);
  } else if (dialog_type == WEF_DIALOG_CONFIRM) {
    cmd = "zenity --question --title=\"" + title_str + "\" --text=\"" + message_str + "\" 2>/dev/null";
    int ret = system(cmd.c_str());
    if (callback) callback(callback_data, (ret == 0) ? 1 : 0, nullptr);
  } else if (dialog_type == WEF_DIALOG_PROMPT) {
    cmd = "zenity --entry --title=\"" + title_str + "\" --text=\"" + message_str +
          "\" --entry-text=\"" + default_str + "\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (fp) {
      char buf[4096] = {};
      if (fgets(buf, sizeof(buf), fp)) {
        // Remove trailing newline
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n') buf[len - 1] = '\0';
      }
      int ret = pclose(fp);
      if (callback) callback(callback_data, (ret == 0) ? 1 : 0, (ret == 0) ? buf : nullptr);
    } else {
      if (callback) callback(callback_data, 0, nullptr);
    }
  }
#elif defined(_WIN32)
  CefPostTask(TID_UI, base::BindOnce(
      [](int dtype, std::string t, std::string m, std::string d,
         wef_dialog_result_fn cb, void* cb_data) {
        if (dtype == WEF_DIALOG_ALERT) {
          MessageBoxA(nullptr, m.c_str(), t.c_str(), MB_OK | MB_ICONINFORMATION);
          if (cb) cb(cb_data, 1, nullptr);
        } else if (dtype == WEF_DIALOG_CONFIRM) {
          int ret = MessageBoxA(nullptr, m.c_str(), t.c_str(), MB_OKCANCEL | MB_ICONQUESTION);
          if (cb) cb(cb_data, (ret == IDOK) ? 1 : 0, nullptr);
        } else if (dtype == WEF_DIALOG_PROMPT) {
          // Use default value as result for now (Windows prompt requires custom dialog)
          int ret = MessageBoxA(nullptr, m.c_str(), t.c_str(), MB_OKCANCEL | MB_ICONQUESTION);
          if (cb) cb(cb_data, (ret == IDOK) ? 1 : 0, (ret == IDOK) ? d.c_str() : nullptr);
        }
      },
      dialog_type, title_str, message_str, default_str, callback, callback_data));
#endif
}

void RuntimeLoader::InitializeBackendApi() {
  memset(&backend_api_, 0, sizeof(backend_api_));
  backend_api_.version = WEF_API_VERSION;
  backend_api_.backend_data = this;

  backend_api_.create_window = Backend_CreateWindow;
  backend_api_.close_window = Backend_CloseWindow;

  backend_api_.navigate = Backend_Navigate;
  backend_api_.set_title = Backend_SetTitle;
  backend_api_.execute_js = Backend_ExecuteJs;
  backend_api_.quit = Backend_Quit;
  backend_api_.set_window_size = Backend_SetWindowSize;
  backend_api_.get_window_size = Backend_GetWindowSize;
  backend_api_.set_window_position = Backend_SetWindowPosition;
  backend_api_.get_window_position = Backend_GetWindowPosition;
  backend_api_.set_resizable = Backend_SetResizable;
  backend_api_.is_resizable = Backend_IsResizable;
  backend_api_.set_always_on_top = Backend_SetAlwaysOnTop;
  backend_api_.is_always_on_top = Backend_IsAlwaysOnTop;
  backend_api_.is_visible = Backend_IsVisible;
  backend_api_.show = Backend_Show;
  backend_api_.hide = Backend_Hide;
  backend_api_.focus = Backend_Focus;
  backend_api_.post_ui_task = Backend_PostUiTask;

  backend_api_.value_is_null = Backend_ValueIsNull;
  backend_api_.value_is_bool = Backend_ValueIsBool;
  backend_api_.value_is_int = Backend_ValueIsInt;
  backend_api_.value_is_double = Backend_ValueIsDouble;
  backend_api_.value_is_string = Backend_ValueIsString;
  backend_api_.value_is_list = Backend_ValueIsList;
  backend_api_.value_is_dict = Backend_ValueIsDict;
  backend_api_.value_is_binary = Backend_ValueIsBinary;
  backend_api_.value_is_callback = Backend_ValueIsCallback;

  backend_api_.value_get_bool = Backend_ValueGetBool;
  backend_api_.value_get_int = Backend_ValueGetInt;
  backend_api_.value_get_double = Backend_ValueGetDouble;
  backend_api_.value_get_string = Backend_ValueGetString;
  backend_api_.value_free_string = Backend_ValueFreeString;
  backend_api_.value_list_size = Backend_ValueListSize;
  backend_api_.value_list_get = Backend_ValueListGet;
  backend_api_.value_dict_get = Backend_ValueDictGet;
  backend_api_.value_dict_has = Backend_ValueDictHas;
  backend_api_.value_dict_size = Backend_ValueDictSize;
  backend_api_.value_dict_keys = Backend_ValueDictKeys;
  backend_api_.value_free_keys = Backend_ValueFreeKeys;
  backend_api_.value_get_binary = Backend_ValueGetBinary;
  backend_api_.value_get_callback_id = Backend_ValueGetCallbackId;

  backend_api_.value_null = Backend_ValueNull;
  backend_api_.value_bool = Backend_ValueBool;
  backend_api_.value_int = Backend_ValueInt;
  backend_api_.value_double = Backend_ValueDouble;
  backend_api_.value_string = Backend_ValueString;
  backend_api_.value_list = Backend_ValueList;
  backend_api_.value_dict = Backend_ValueDict;
  backend_api_.value_binary = Backend_ValueBinary;

  backend_api_.value_list_append = Backend_ValueListAppend;
  backend_api_.value_list_set = Backend_ValueListSet;
  backend_api_.value_dict_set = Backend_ValueDictSet;
  backend_api_.value_free = Backend_ValueFree;

  backend_api_.set_js_call_handler = Backend_SetJsCallHandler;
  backend_api_.js_call_respond = Backend_JsCallRespond;

  backend_api_.invoke_js_callback = Backend_InvokeJsCallback;
  backend_api_.release_js_callback = Backend_ReleaseJsCallback;

  backend_api_.get_window_handle = [](void*, uint32_t) -> void* { return nullptr; };
  backend_api_.get_display_handle = [](void*, uint32_t) -> void* { return nullptr; };
#if defined(_WIN32)
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int { return WEF_WINDOW_HANDLE_WIN32; };
#elif defined(__APPLE__)
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int { return WEF_WINDOW_HANDLE_APPKIT; };
#else
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int { return WEF_WINDOW_HANDLE_X11; };
#endif

  backend_api_.set_keyboard_event_handler = Backend_SetKeyboardEventHandler;
  backend_api_.set_mouse_click_handler = Backend_SetMouseClickHandler;
  backend_api_.set_mouse_move_handler = Backend_SetMouseMoveHandler;
  backend_api_.set_wheel_handler = Backend_SetWheelHandler;
  backend_api_.set_cursor_enter_leave_handler = Backend_SetCursorEnterLeaveHandler;
  backend_api_.set_focused_handler = Backend_SetFocusedHandler;
  backend_api_.set_resize_handler = Backend_SetResizeHandler;
  backend_api_.set_move_handler = Backend_SetMoveHandler;
  backend_api_.set_close_requested_handler = Backend_SetCloseRequestedHandler;

  backend_api_.poll_js_calls = [](void* data) {
    RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
    loader->PollPendingJsCalls();
  };

  backend_api_.set_js_call_notify = [](void* data, void (*notify_fn)(void*), void* notify_data) {
    RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
    loader->SetJsCallNotify(notify_fn, notify_data);
  };

#if defined(_WIN32)
  backend_api_.set_application_menu = Backend_SetApplicationMenu;
  backend_api_.show_context_menu = Backend_ShowContextMenu;
#elif defined(__APPLE__)
  backend_api_.set_application_menu = Backend_SetApplicationMenu_Mac;
  backend_api_.show_context_menu = Backend_ShowContextMenu_Mac;
#else
  // Linux: CEF Views creates raw X11 windows (not GtkWindows), so there is
  // no GTK container to attach a GtkMenuBar to. Application menus would
  // require D-Bus menu protocol or drawing a custom menu bar, which is
  // future work (see issue #3 — Wayland/Phase 4).
  backend_api_.set_application_menu = [](void*, uint32_t, wef_value_t*, wef_menu_click_fn, void*) {};
  backend_api_.show_context_menu = [](void*, uint32_t, int, int, wef_value_t*, wef_menu_click_fn, void*) {};
#endif

  backend_api_.open_devtools = Backend_OpenDevTools;
  backend_api_.set_js_namespace = Backend_SetJsNamespace;
  backend_api_.show_dialog = Backend_ShowDialog;
}

// --- RuntimeLoader lifecycle ---

RuntimeLoader::RuntimeLoader() {
  instance_ = this;
  InitializeBackendApi();
}

RuntimeLoader::~RuntimeLoader() {
  Shutdown();
  if (library_handle_) {
#ifndef _WIN32
    dlclose(library_handle_);
#else
    FreeLibrary(static_cast<HMODULE>(library_handle_));
#endif
  }
  instance_ = nullptr;
}

RuntimeLoader* RuntimeLoader::GetInstance() {
  if (!instance_) {
    instance_ = new RuntimeLoader();
  }
  return instance_;
}

bool RuntimeLoader::Load(const std::string& path) {
#ifndef _WIN32
  library_handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!library_handle_) {
    std::cerr << "Failed to load runtime: " << dlerror() << std::endl;
    return false;
  }

  init_fn_ = reinterpret_cast<wef_runtime_init_fn>(
      dlsym(library_handle_, WEF_RUNTIME_INIT_SYMBOL));
  if (!init_fn_) {
    std::cerr << "Failed to find " << WEF_RUNTIME_INIT_SYMBOL << ": " << dlerror() << std::endl;
    return false;
  }

  start_fn_ = reinterpret_cast<wef_runtime_start_fn>(
      dlsym(library_handle_, WEF_RUNTIME_START_SYMBOL));
  if (!start_fn_) {
    std::cerr << "Failed to find " << WEF_RUNTIME_START_SYMBOL << ": " << dlerror() << std::endl;
    return false;
  }

  shutdown_fn_ = reinterpret_cast<wef_runtime_shutdown_fn>(
      dlsym(library_handle_, WEF_RUNTIME_SHUTDOWN_SYMBOL));
  if (!shutdown_fn_) {
    std::cerr << "Failed to find " << WEF_RUNTIME_SHUTDOWN_SYMBOL << ": " << dlerror() << std::endl;
    return false;
  }
#else
  library_handle_ = LoadLibraryA(path.c_str());
  if (!library_handle_) {
    std::cerr << "Failed to load runtime: error " << GetLastError() << std::endl;
    return false;
  }

  init_fn_ = reinterpret_cast<wef_runtime_init_fn>(
      GetProcAddress(static_cast<HMODULE>(library_handle_), WEF_RUNTIME_INIT_SYMBOL));
  if (!init_fn_) {
    std::cerr << "Failed to find " << WEF_RUNTIME_INIT_SYMBOL << std::endl;
    return false;
  }

  start_fn_ = reinterpret_cast<wef_runtime_start_fn>(
      GetProcAddress(static_cast<HMODULE>(library_handle_), WEF_RUNTIME_START_SYMBOL));
  if (!start_fn_) {
    std::cerr << "Failed to find " << WEF_RUNTIME_START_SYMBOL << std::endl;
    return false;
  }

  shutdown_fn_ = reinterpret_cast<wef_runtime_shutdown_fn>(
      GetProcAddress(static_cast<HMODULE>(library_handle_), WEF_RUNTIME_SHUTDOWN_SYMBOL));
  if (!shutdown_fn_) {
    std::cerr << "Failed to find " << WEF_RUNTIME_SHUTDOWN_SYMBOL << std::endl;
    return false;
  }
#endif

  std::cout << "Runtime loaded successfully from: " << path << std::endl;
  return true;
}

bool RuntimeLoader::Start() {
  if (running_) {
    return true;
  }

  if (!init_fn_ || !start_fn_) {
    std::cerr << "Runtime not loaded" << std::endl;
    return false;
  }

  int result = init_fn_(&backend_api_);
  if (result != 0) {
    std::cerr << "Runtime init failed with code: " << result << std::endl;
    return false;
  }

  running_ = true;
  runtime_thread_ = std::thread(&RuntimeLoader::RuntimeThread, this);

  std::cout << "Runtime started" << std::endl;
  return true;
}

void RuntimeLoader::RuntimeThread() {
  int result = start_fn_();
  if (result != 0) {
    std::cerr << "Runtime start returned error: " << result << std::endl;
  }
  running_ = false;
}

void RuntimeLoader::Shutdown() {
  if (shutdown_fn_) {
    shutdown_fn_();
  }

  if (runtime_thread_.joinable()) {
    runtime_thread_.join();
  }
}

void RuntimeLoader::OnJsCall(uint32_t window_id, uint64_t call_id, const std::string& method_path,
                             CefRefPtr<CefListValue> args) {
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_js_calls_.push({window_id, call_id, method_path, args});
  }
  StoreCallWindow(call_id, window_id);

  std::lock_guard<std::mutex> lock(notify_mutex_);
  if (js_call_notify_fn_) {
    js_call_notify_fn_(js_call_notify_data_);
  }
}

void RuntimeLoader::PollPendingJsCalls() {
  std::vector<PendingJsCall> calls;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    while (!pending_js_calls_.empty()) {
      calls.push_back(std::move(pending_js_calls_.front()));
      pending_js_calls_.pop();
    }
  }

  if (calls.empty()) return;

  wef_js_call_fn handler;
  void* user_data;
  {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    handler = js_call_handler_;
    user_data = js_call_user_data_;
  }

  for (auto& call : calls) {
    if (handler) {
      CefRefPtr<CefValue> argsValue = CefValue::Create();
      argsValue->SetList(call.args);
      wef_value_t* argsWrapper = new wef_value(argsValue);
      handler(user_data, call.window_id, call.call_id, call.method_path.c_str(), argsWrapper);
    } else {
      CefRefPtr<CefValue> errVal = CefValue::Create();
      errVal->SetString("No JS call handler registered");
      wef_value_t errWrapper(errVal);
      Backend_JsCallRespond(this, call.call_id, nullptr, &errWrapper);
    }
  }
}
