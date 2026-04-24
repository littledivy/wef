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
                          [](F* fn, std::mutex* mtx,
                             std::condition_variable* cv, bool* done) {
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

static void Backend_SetTitle(void* data, uint32_t window_id,
                             const char* title) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser && title) {
    std::string title_str(title);
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, std::string t) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
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

static void Backend_ExecuteJs(void* data, uint32_t window_id,
                              const char* script, wef_js_result_fn callback,
                              void* callback_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser || !script) {
    if (callback)
      callback(nullptr, nullptr, callback_data);
    return;
  }

  if (!callback) {
    // Fire-and-forget: use the simple path
    std::string script_str(script);
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, std::string s) {
                              b->GetMainFrame()->ExecuteJavaScript(s, "", 0);
                            },
                            browser, script_str));
    return;
  }

  // With callback: send IPC to renderer for eval with result
  uint64_t eval_id = loader->StoreEvalCallback(callback, callback_data);
  std::string script_str(script);
  CefPostTask(TID_UI,
              base::BindOnce(
                  [](CefRefPtr<CefBrowser> b, uint64_t id, std::string s) {
                    CefRefPtr<CefProcessMessage> msg =
                        CefProcessMessage::Create("wef_eval");
                    CefRefPtr<CefListValue> args = msg->GetArgumentList();
                    args->SetInt(0, static_cast<int>(id));
                    args->SetString(1, s);
                    b->GetMainFrame()->SendProcessMessage(PID_RENDERER, msg);
                  },
                  browser, eval_id, script_str));
}

static void Backend_Quit(void* data) {
  CefPostTask(TID_UI, base::BindOnce([]() { CefQuitMessageLoop(); }));
}

static void Backend_SetWindowSize(void* data, uint32_t window_id, int width,
                                  int height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, int w, int h) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
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

static void Backend_GetWindowSize(void* data, uint32_t window_id, int* width,
                                  int* height) {
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
  if (width)
    *width = w;
  if (height)
    *height = h;
}

static void Backend_SetWindowPosition(void* data, uint32_t window_id, int x,
                                      int y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, int px, int py) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
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

static void Backend_GetWindowPosition(void* data, uint32_t window_id, int* x,
                                      int* y) {
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
  if (x)
    *x = px;
  if (y)
    *y = py;
}

static void Backend_SetResizable(void* data, uint32_t window_id,
                                 bool resizable) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, bool r) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
                              if (!browser_view)
                                return;
                              auto window = browser_view->GetWindow();
                              if (!window)
                                return;
#ifdef _WIN32
                              HWND hwnd = window->GetWindowHandle();
                              LONG style = GetWindowLong(hwnd, GWL_STYLE);
                              if (r) {
                                style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
                              } else {
                                style &= ~(WS_THICKFRAME | WS_MAXIMIZEBOX);
                              }
                              SetWindowLong(hwnd, GWL_STYLE, style);
#elif defined(__APPLE__)
                              SetNSWindowResizable(window->GetWindowHandle(),
                                                   r);
#elif defined(__linux__)
                              SetLinuxWindowResizable(window->GetWindowHandle(),
                                                      r);
#endif
                            },
                            browser, resizable));
  }
}

static bool Backend_IsResizable(void* data, uint32_t window_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  bool result = true;
  if (browser) {
    cef_invoke_sync([&] {
      auto browser_view = CefBrowserView::GetForBrowser(browser);
      if (!browser_view)
        return;
      auto window = browser_view->GetWindow();
      if (!window)
        return;
#ifdef _WIN32
      HWND hwnd = window->GetWindowHandle();
      LONG style = GetWindowLong(hwnd, GWL_STYLE);
      result = (style & WS_THICKFRAME) != 0;
#elif defined(__APPLE__)
      result = IsNSWindowResizable(window->GetWindowHandle());
#elif defined(__linux__)
      result = IsLinuxWindowResizable(window->GetWindowHandle());
#endif
    });
  }
  return result;
}

static void Backend_SetAlwaysOnTop(void* data, uint32_t window_id,
                                   bool always_on_top) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, bool on_top) {
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
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
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
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
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
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
                              auto browser_view =
                                  CefBrowserView::GetForBrowser(b);
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

static void Backend_PostUiTask(void* data, void (*task)(void*),
                               void* task_data) {
  if (task) {
    CefPostTask(TID_UI, base::BindOnce([](void (*t)(void*), void* d) { t(d); },
                                       task, task_data));
  }
}

// --- Value accessors ---

static bool Backend_ValueIsNull(wef_value_t* val) {
  if (!val || !val->value)
    return true;
  return val->value->GetType() == VTYPE_NULL;
}

static bool Backend_ValueIsBool(wef_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->GetType() == VTYPE_BOOL;
}

static bool Backend_ValueIsInt(wef_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->GetType() == VTYPE_INT;
}

static bool Backend_ValueIsDouble(wef_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->GetType() == VTYPE_DOUBLE;
}

static bool Backend_ValueIsString(wef_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->GetType() == VTYPE_STRING;
}

static bool Backend_ValueIsList(wef_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->GetType() == VTYPE_LIST;
}

static bool Backend_ValueIsDict(wef_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->GetType() == VTYPE_DICTIONARY;
}

static bool Backend_ValueIsBinary(wef_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->GetType() == VTYPE_BINARY;
}

static bool Backend_ValueIsCallback(wef_value_t* val) {
  if (!val)
    return false;
  return val->is_callback;
}

static bool Backend_ValueGetBool(wef_value_t* val) {
  if (!val || !val->value)
    return false;
  return val->value->GetBool();
}

static int Backend_ValueGetInt(wef_value_t* val) {
  if (!val || !val->value)
    return 0;
  return val->value->GetInt();
}

static double Backend_ValueGetDouble(wef_value_t* val) {
  if (!val || !val->value)
    return 0.0;
  return val->value->GetDouble();
}

static char* Backend_ValueGetString(wef_value_t* val, size_t* len_out) {
  if (!val || !val->value) {
    if (len_out)
      *len_out = 0;
    return nullptr;
  }
  std::string str = val->value->GetString().ToString();
  if (len_out)
    *len_out = str.size();
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
  if (!val || !val->value || val->value->GetType() != VTYPE_LIST)
    return 0;
  return val->value->GetList()->GetSize();
}

static wef_value_t* Backend_ValueListGet(wef_value_t* val, size_t index) {
  if (!val || !val->value || val->value->GetType() != VTYPE_LIST)
    return nullptr;
  CefRefPtr<CefListValue> list = val->value->GetList();
  if (index >= list->GetSize())
    return nullptr;
  return new wef_value(list->GetValue(index));
}

static wef_value_t* Backend_ValueDictGet(wef_value_t* dict, const char* key) {
  if (!dict || !dict->value || dict->value->GetType() != VTYPE_DICTIONARY ||
      !key)
    return nullptr;
  CefRefPtr<CefDictionaryValue> d = dict->value->GetDictionary();
  if (!d->HasKey(key))
    return nullptr;
  return new wef_value(d->GetValue(key));
}

static bool Backend_ValueDictHas(wef_value_t* dict, const char* key) {
  if (!dict || !dict->value || dict->value->GetType() != VTYPE_DICTIONARY ||
      !key)
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
    if (count_out)
      *count_out = 0;
    return nullptr;
  }
  CefRefPtr<CefDictionaryValue> d = dict->value->GetDictionary();
  CefDictionaryValue::KeyList keys;
  d->GetKeys(keys);

  if (count_out)
    *count_out = keys.size();
  if (keys.empty())
    return nullptr;

  char** result = static_cast<char**>(malloc(sizeof(char*) * keys.size()));
  for (size_t i = 0; i < keys.size(); ++i) {
    std::string key = keys[i].ToString();
    result[i] = static_cast<char*>(malloc(key.size() + 1));
    memcpy(result[i], key.c_str(), key.size() + 1);
  }
  return result;
}

static void Backend_ValueFreeKeys(char** keys, size_t count) {
  if (!keys)
    return;
  for (size_t i = 0; i < count; ++i) {
    free(keys[i]);
  }
  free(keys);
}

static const void* Backend_ValueGetBinary(wef_value_t* val, size_t* len_out) {
  if (!val || !val->value || val->value->GetType() != VTYPE_BINARY) {
    if (len_out)
      *len_out = 0;
    return nullptr;
  }
  CefRefPtr<CefBinaryValue> binary = val->value->GetBinary();
  if (len_out)
    *len_out = binary->GetSize();
  return binary->GetRawData();
}

static uint64_t Backend_ValueGetCallbackId(wef_value_t* val) {
  if (!val || !val->is_callback)
    return 0;
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
  if (!val || !val->value)
    return false;
  CefRefPtr<CefListValue> l = list->value->GetList();
  size_t index = l->GetSize();
  return l->SetValue(index, val->value);
}

static bool Backend_ValueListSet(wef_value_t* list, size_t index,
                                 wef_value_t* val) {
  if (!list || !list->value || list->value->GetType() != VTYPE_LIST)
    return false;
  if (!val || !val->value)
    return false;
  return list->value->GetList()->SetValue(index, val->value);
}

static bool Backend_ValueDictSet(wef_value_t* dict, const char* key,
                                 wef_value_t* val) {
  if (!dict || !dict->value || dict->value->GetType() != VTYPE_DICTIONARY)
    return false;
  if (!key || !val || !val->value)
    return false;
  return dict->value->GetDictionary()->SetValue(key, val->value);
}

static void Backend_ValueFree(wef_value_t* val) {
  delete val;
}

// --- JS call/callback handling ---

static void Backend_SetJsCallHandler(void* data, wef_js_call_fn handler,
                                     void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetJsCallHandler(handler, user_data);
}

static void Backend_JsCallRespond(void* data, uint64_t call_id,
                                  wef_value_t* result, wef_value_t* error) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  uint32_t window_id = loader->ConsumeCallWindow(call_id);
  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser)
    return;

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

  CefPostTask(TID_UI,
              base::BindOnce(
                  [](CefRefPtr<CefBrowser> b, CefRefPtr<CefProcessMessage> m) {
                    b->GetMainFrame()->SendProcessMessage(PID_RENDERER, m);
                  },
                  browser, msg));
}

static void Backend_InvokeJsCallback(void* data, uint64_t callback_id,
                                     wef_value_t* args) {
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
    CefPostTask(
        TID_UI,
        base::BindOnce(
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

static void Backend_SetMouseClickHandler(void* data, wef_mouse_click_fn handler,
                                         void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMouseClickHandler(handler, user_data);
}

static void Backend_SetMouseMoveHandler(void* data, wef_mouse_move_fn handler,
                                        void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMouseMoveHandler(handler, user_data);
}

static void Backend_SetWheelHandler(void* data, wef_wheel_fn handler,
                                    void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetWheelHandler(handler, user_data);
}

static void Backend_SetCursorEnterLeaveHandler(
    void* data, wef_cursor_enter_leave_fn handler, void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetCursorEnterLeaveHandler(handler, user_data);
}

static void Backend_SetFocusedHandler(void* data, wef_focused_fn handler,
                                      void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetFocusedHandler(handler, user_data);
}

static void Backend_SetResizeHandler(void* data, wef_resize_fn handler,
                                     void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetResizeHandler(handler, user_data);
}

static void Backend_SetMoveHandler(void* data, wef_move_fn handler,
                                   void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetMoveHandler(handler, user_data);
}

static void Backend_ReleaseJsCallback(void* data, uint64_t callback_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);

  CefRefPtr<CefProcessMessage> msg =
      CefProcessMessage::Create("wef_release_callback");
  CefRefPtr<CefListValue> msgArgs = msg->GetArgumentList();
  msgArgs->SetInt(0, static_cast<int>(callback_id));

  loader->ForEachBrowser([&msg](CefRefPtr<CefBrowser> browser) {
    CefPostTask(
        TID_UI,
        base::BindOnce(
            [](CefRefPtr<CefBrowser> b, CefRefPtr<CefProcessMessage> m) {
              b->GetMainFrame()->SendProcessMessage(PID_RENDERER, m);
            },
            browser, msg));
  });
}

// --- Platform-specific menu (stub on Windows, implemented in runtime_loader.mm
// on macOS) ---

#if defined(_WIN32)
#include <win32_menu.h>

static void Backend_SetApplicationMenu(void* data, uint32_t window_id,
                                       wef_value_t* menu_template,
                                       wef_menu_click_fn on_click,
                                       void* on_click_data) {
  if (!menu_template)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();

  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(
        TID_UI,
        base::BindOnce(
            [](CefRefPtr<CefBrowser> b, uint32_t wid, wef_value_t* tmpl,
               const wef_backend_api_t* a, wef_menu_click_fn fn, void* d) {
              HWND hwnd = b->GetHost()->GetWindowHandle();
              if (hwnd) {
                win32_menu::SetApplicationMenu(hwnd, tmpl, a, fn, d, wid);
              }
            },
            browser, window_id, menu_template, api, on_click, on_click_data));
  }
}

static void Backend_ShowContextMenu(void* data, uint32_t window_id, int x,
                                    int y, wef_value_t* menu_template,
                                    wef_menu_click_fn on_click,
                                    void* on_click_data) {
  if (!menu_template)
    return;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();

  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (browser) {
    CefPostTask(TID_UI,
                base::BindOnce(
                    [](CefRefPtr<CefBrowser> b, uint32_t wid, int cx, int cy,
                       wef_value_t* tmpl, const wef_backend_api_t* a,
                       wef_menu_click_fn fn, void* d) {
                      HWND hwnd = b->GetHost()->GetWindowHandle();
                      if (hwnd) {
                        win32_menu::ShowContextMenu(hwnd, cx, cy, tmpl, a, fn,
                                                    d, wid);
                      }
                    },
                    browser, window_id, x, y, menu_template, api, on_click,
                    on_click_data));
  }
}
#elif defined(__APPLE__)
// Defined in runtime_loader_mac.mm
extern void Backend_SetApplicationMenu_Mac(void* data, uint32_t window_id,
                                           wef_value_t* menu_template,
                                           wef_menu_click_fn on_click,
                                           void* on_click_data);
extern void Backend_ShowContextMenu_Mac(void* data, uint32_t window_id, int x,
                                        int y, wef_value_t* menu_template,
                                        wef_menu_click_fn on_click,
                                        void* on_click_data);
extern void Backend_SetDockBadge_Mac(void* data, const char* badge_or_null);
extern void Backend_BounceDock_Mac(void* data, int type);
extern void Backend_SetDockMenu_Mac(void* data, wef_value_t* menu_template,
                                    wef_menu_click_fn on_click,
                                    void* on_click_data);
extern void Backend_SetDockVisible_Mac(void* data, bool visible);
extern void Backend_SetDockReopenHandler_Mac(void* data,
                                             wef_dock_reopen_fn handler,
                                             void* user_data);

extern uint32_t Backend_CreateTrayIcon_Mac(void* data);
extern void Backend_DestroyTrayIcon_Mac(void* data, uint32_t tray_id);
extern void Backend_SetTrayIcon_Mac(void* data, uint32_t tray_id,
                                    const void* png_bytes, size_t len);
extern void Backend_SetTrayTooltip_Mac(void* data, uint32_t tray_id,
                                       const char* tooltip_or_null);
extern void Backend_SetTrayMenu_Mac(void* data, uint32_t tray_id,
                                    wef_value_t* menu_template,
                                    wef_menu_click_fn on_click,
                                    void* on_click_data);
extern void Backend_SetTrayClickHandler_Mac(void* data, uint32_t tray_id,
                                            wef_tray_click_fn handler,
                                            void* user_data);
extern void Backend_SetTrayDoubleClickHandler_Mac(void* data, uint32_t tray_id,
                                                  wef_tray_click_fn handler,
                                                  void* user_data);
extern void Backend_SetTrayIconDark_Mac(void* data, uint32_t tray_id,
                                        const void* png_bytes, size_t len);
#elif defined(__linux__)
// Defined in runtime_loader_linux.cc
extern void Backend_ShowContextMenu_Linux(void* data, uint32_t window_id, int x,
                                          int y, wef_value_t* menu_template,
                                          wef_menu_click_fn on_click,
                                          void* on_click_data);
extern uint32_t Backend_CreateTrayIcon_Linux(void* data);
extern void Backend_DestroyTrayIcon_Linux(void* data, uint32_t tray_id);
extern void Backend_SetTrayIcon_Linux(void* data, uint32_t tray_id,
                                      const void* png_bytes, size_t len);
extern void Backend_SetTrayTooltip_Linux(void* data, uint32_t tray_id,
                                         const char* tooltip_or_null);
extern void Backend_SetTrayMenu_Linux(void* data, uint32_t tray_id,
                                      wef_value_t* menu_template,
                                      wef_menu_click_fn on_click,
                                      void* on_click_data);
extern void Backend_SetTrayClickHandler_Linux(void* data, uint32_t tray_id,
                                              wef_tray_click_fn handler,
                                              void* user_data);
extern void Backend_SetTrayDoubleClickHandler_Linux(void* data,
                                                    uint32_t tray_id,
                                                    wef_tray_click_fn handler,
                                                    void* user_data);
extern void Backend_SetTrayIconDark_Linux(void* data, uint32_t tray_id,
                                          const void* png_bytes, size_t len);
#endif

// --- Dock / taskbar (Windows + Linux) ---
//
// The dock is a macOS concept; on Windows the analog is the taskbar button
// (per-window), and on Linux it's the WM urgency hint. Bounce maps cleanly:
//   - Windows: FlashWindowEx on every WEF window's HWND.
//   - Linux:   X11 UrgencyHint on every WEF window's X11 Window.
// Badge is implemented as a `"(N) " prefix on each window's title — the
// convention used by Slack/Discord/Telegram. If user code updates the title
// while a badge is active, the badge falls off (best-effort v1; proper
// Windows overlay icons and Linux libunity are future work). Menu, visible,
// and reopen have no clean analog.

#if !defined(__APPLE__)
#include <map>
#include <mutex>
#include <string>

#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"

// Badge is implemented as a title prefix. Per window we remember the
// "original" title at the moment a badge is applied, so clearing can
// restore it. If user code updates the window title while a badge is
// active, the badge is visually lost — that's a best-effort v1
// limitation; calling set_dock_badge again re-applies it on top of the
// (now-stale) saved original.
static std::mutex g_cef_badge_mutex;
static std::map<uint32_t, std::string> g_cef_saved_titles;

static void Backend_SetDockBadge_TitlePrefix(void* data,
                                             const char* badge_or_null) {
  std::string badge =
      (badge_or_null && *badge_or_null) ? std::string(badge_or_null) : "";
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);

  loader->ForEachBrowserWithId([&badge](uint32_t wid,
                                        CefRefPtr<CefBrowser> browser) {
    CefPostTask(
        TID_UI,
        base::BindOnce(
            [](uint32_t wid, CefRefPtr<CefBrowser> b, std::string bg) {
              auto bv = CefBrowserView::GetForBrowser(b);
              if (!bv)
                return;
              auto win = bv->GetWindow();
              if (!win)
                return;
              std::lock_guard<std::mutex> lock(g_cef_badge_mutex);
              if (!bg.empty()) {
                if (g_cef_saved_titles.find(wid) == g_cef_saved_titles.end()) {
                  g_cef_saved_titles[wid] = win->GetTitle().ToString();
                }
                win->SetTitle("(" + bg + ") " + g_cef_saved_titles[wid]);
              } else {
                auto it = g_cef_saved_titles.find(wid);
                if (it != g_cef_saved_titles.end()) {
                  win->SetTitle(it->second);
                  g_cef_saved_titles.erase(it);
                }
              }
            },
            wid, browser, badge));
  });
}
#endif  // !__APPLE__

#if defined(_WIN32)
#include <shellapi.h>
#include <windows.h>
#include <windowsx.h>
#include <wincodec.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "windowscodecs.lib")

static void Backend_BounceDock_Win(void* data, int type) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->ForEachBrowser([type](CefRefPtr<CefBrowser> browser) {
    CefPostTask(TID_UI, base::BindOnce(
                            [](CefRefPtr<CefBrowser> b, int t) {
                              HWND hwnd = b->GetHost()->GetWindowHandle();
                              if (!hwnd)
                                return;
                              FLASHWINFO fi = {sizeof(FLASHWINFO), hwnd, 0, 0,
                                               0};
                              if (t == WEF_DOCK_BOUNCE_CRITICAL) {
                                fi.dwFlags = FLASHW_ALL | FLASHW_TIMER;
                                fi.uCount = 0;
                              } else {
                                fi.dwFlags = FLASHW_TIMERNOFG;
                                fi.uCount = 3;
                              }
                              FlashWindowEx(&fi);
                            },
                            browser, type));
  });
}

// --- Tray (Windows) ---
//
// Shell_NotifyIcon + a hidden message-only window that receives
// WM_TRAYICON (one per process). PNG → HICON via WIC.

#define WM_WEF_TRAYICON (WM_APP + 1)

struct WinTrayEntry {
  UINT uid;
  HICON hicon_light;
  HICON hicon_dark;
  HMENU hmenu;  // tray context menu (right-click)
  // Shared mapping from menu item command id -> (menu_click callback, item id
  // string)
  std::map<UINT, std::string> cmd_to_id;
  wef_menu_click_fn menu_click_fn;
  void* menu_click_data;
  wef_tray_click_fn click_fn;
  void* click_data;
  wef_tray_click_fn dblclick_fn;
  void* dblclick_data;
};

static std::mutex& TrayMutexWin() {
  static std::mutex m;
  return m;
}
static std::map<uint32_t, WinTrayEntry>& TrayMapWin() {
  static std::map<uint32_t, WinTrayEntry> map;
  return map;
}
static std::atomic<uint32_t> g_next_tray_id_win{1};
static std::atomic<UINT> g_next_cmd_id{1000};
static HWND g_tray_msg_hwnd = nullptr;

// Forward declared so TrayWndProc can call it on WM_SETTINGCHANGE.
static bool WinIsDarkMode();
static void ReapplyAllWinTrayIcons();

static LRESULT CALLBACK TrayWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_SETTINGCHANGE) {
    if (lp && wcscmp((LPCWSTR)lp, L"ImmersiveColorSet") == 0) {
      ReapplyAllWinTrayIcons();
    }
    return 0;
  }
  if (msg == WM_WEF_TRAYICON) {
    uint32_t tray_id = (uint32_t)wp;
    UINT event = LOWORD(lp);
    if (event == WM_LBUTTONDBLCLK) {
      wef_tray_click_fn fn = nullptr;
      void* data = nullptr;
      {
        std::lock_guard<std::mutex> lock(TrayMutexWin());
        auto it = TrayMapWin().find(tray_id);
        if (it != TrayMapWin().end()) {
          fn = it->second.dblclick_fn;
          data = it->second.dblclick_data;
        }
      }
      if (fn)
        fn(data, tray_id);
      return 0;
    }
    if (event == WM_LBUTTONUP) {
      wef_tray_click_fn fn = nullptr;
      void* data = nullptr;
      {
        std::lock_guard<std::mutex> lock(TrayMutexWin());
        auto it = TrayMapWin().find(tray_id);
        if (it != TrayMapWin().end()) {
          fn = it->second.click_fn;
          data = it->second.click_data;
        }
      }
      if (fn)
        fn(data, tray_id);
    } else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) {
      HMENU menu = nullptr;
      {
        std::lock_guard<std::mutex> lock(TrayMutexWin());
        auto it = TrayMapWin().find(tray_id);
        if (it != TrayMapWin().end())
          menu = it->second.hmenu;
      }
      if (menu) {
        POINT pt;
        GetCursorPos(&pt);
        SetForegroundWindow(hwnd);
        UINT cmd =
            TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                           pt.x, pt.y, 0, hwnd, nullptr);
        if (cmd) {
          wef_menu_click_fn fn = nullptr;
          void* data = nullptr;
          std::string item_id;
          {
            std::lock_guard<std::mutex> lock(TrayMutexWin());
            auto it = TrayMapWin().find(tray_id);
            if (it != TrayMapWin().end()) {
              auto cit = it->second.cmd_to_id.find(cmd);
              if (cit != it->second.cmd_to_id.end())
                item_id = cit->second;
              fn = it->second.menu_click_fn;
              data = it->second.menu_click_data;
            }
          }
          if (fn && !item_id.empty())
            fn(data, tray_id, item_id.c_str());
        }
      }
    }
    return 0;
  }
  return DefWindowProc(hwnd, msg, wp, lp);
}

static HWND EnsureTrayMessageWindow() {
  if (g_tray_msg_hwnd)
    return g_tray_msg_hwnd;
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = TrayWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"WefTrayMessageWindow";
  RegisterClassExW(&wc);
  g_tray_msg_hwnd =
      CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE,
                      nullptr, wc.hInstance, nullptr);
  return g_tray_msg_hwnd;
}

static HICON DecodePngToHicon(const void* bytes, size_t len, int desired) {
  if (!bytes || len == 0)
    return nullptr;
  IWICImagingFactory* factory = nullptr;
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    // Already initialized on this thread is fine.
  }
  if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
    return nullptr;
  }
  IWICStream* stream = nullptr;
  factory->CreateStream(&stream);
  stream->InitializeFromMemory((BYTE*)bytes, (DWORD)len);
  IWICBitmapDecoder* decoder = nullptr;
  factory->CreateDecoderFromStream(stream, nullptr,
                                   WICDecodeMetadataCacheOnLoad, &decoder);
  IWICBitmapFrameDecode* frame = nullptr;
  if (decoder)
    decoder->GetFrame(0, &frame);
  IWICFormatConverter* conv = nullptr;
  factory->CreateFormatConverter(&conv);
  if (frame && conv) {
    conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                     WICBitmapDitherTypeNone, nullptr, 0.0,
                     WICBitmapPaletteTypeCustom);
  }
  IWICBitmapScaler* scaler = nullptr;
  factory->CreateBitmapScaler(&scaler);
  UINT w = desired, h = desired;
  if (conv)
    scaler->Initialize(conv, w, h, WICBitmapInterpolationModeHighQualityCubic);
  std::vector<BYTE> pixels(w * h * 4);
  if (scaler)
    scaler->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
  HICON hicon = nullptr;
  if (scaler) {
    ICONINFO ii = {};
    ii.fIcon = TRUE;
    ii.xHotspot = 0;
    ii.yHotspot = 0;
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = w;
    bi.bV5Height = -(LONG)h;  // top-down
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;
    HDC hdc = GetDC(nullptr);
    void* bits = nullptr;
    ii.hbmColor = CreateDIBSection(hdc, (BITMAPINFO*)&bi, DIB_RGB_COLORS, &bits,
                                   nullptr, 0);
    ReleaseDC(nullptr, hdc);
    if (ii.hbmColor && bits)
      memcpy(bits, pixels.data(), pixels.size());
    ii.hbmMask = CreateBitmap(w, h, 1, 1, nullptr);
    if (ii.hbmColor && ii.hbmMask)
      hicon = CreateIconIndirect(&ii);
    if (ii.hbmColor)
      DeleteObject(ii.hbmColor);
    if (ii.hbmMask)
      DeleteObject(ii.hbmMask);
  }
  if (scaler)
    scaler->Release();
  if (conv)
    conv->Release();
  if (frame)
    frame->Release();
  if (decoder)
    decoder->Release();
  if (stream)
    stream->Release();
  if (factory)
    factory->Release();
  return hicon;
}

// Parse the wef_value_t template and build an HMENU, populating cmd_to_id.
static HMENU BuildWinMenuFromValue(wef_value_t* val,
                                   const wef_backend_api_t* api,
                                   std::map<UINT, std::string>& cmd_to_id) {
  if (!val || !api->value_is_list(val))
    return nullptr;
  HMENU menu = CreatePopupMenu();
  size_t count = api->value_list_size(val);
  for (size_t i = 0; i < count; ++i) {
    wef_value_t* itemVal = api->value_list_get(val, i);
    if (!itemVal || !api->value_is_dict(itemVal))
      continue;

    // Separator
    wef_value_t* typeVal = api->value_dict_get(itemVal, "type");
    if (typeVal && api->value_is_string(typeVal)) {
      size_t len = 0;
      char* typeStr = api->value_get_string(typeVal, &len);
      if (typeStr && std::string(typeStr) == "separator") {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        api->value_free_string(typeStr);
        continue;
      }
      if (typeStr)
        api->value_free_string(typeStr);
    }

    wef_value_t* labelVal = api->value_dict_get(itemVal, "label");
    std::wstring wlabel;
    if (labelVal && api->value_is_string(labelVal)) {
      size_t len = 0;
      char* s = api->value_get_string(labelVal, &len);
      if (s) {
        int n = MultiByteToWideChar(CP_UTF8, 0, s, (int)len, nullptr, 0);
        wlabel.resize(n);
        MultiByteToWideChar(CP_UTF8, 0, s, (int)len, wlabel.data(), n);
        api->value_free_string(s);
      }
    }

    wef_value_t* submenuVal = api->value_dict_get(itemVal, "submenu");
    if (submenuVal && api->value_is_list(submenuVal)) {
      HMENU sub = BuildWinMenuFromValue(submenuVal, api, cmd_to_id);
      AppendMenuW(menu, MF_POPUP | MF_STRING, (UINT_PTR)sub, wlabel.c_str());
      continue;
    }

    wef_value_t* idVal = api->value_dict_get(itemVal, "id");
    std::string item_id;
    if (idVal && api->value_is_string(idVal)) {
      size_t len = 0;
      char* s = api->value_get_string(idVal, &len);
      if (s) {
        item_id = std::string(s, len);
        api->value_free_string(s);
      }
    }

    UINT cmd = g_next_cmd_id.fetch_add(1, std::memory_order_relaxed);
    UINT flags = MF_STRING;
    wef_value_t* enabledVal = api->value_dict_get(itemVal, "enabled");
    if (enabledVal && api->value_is_bool(enabledVal) &&
        !api->value_get_bool(enabledVal)) {
      flags |= MF_GRAYED;
    }
    AppendMenuW(menu, flags, cmd, wlabel.c_str());
    if (!item_id.empty())
      cmd_to_id[cmd] = item_id;
  }
  return menu;
}

uint32_t Backend_CreateTrayIcon_Win(void* /*data*/) {
  uint32_t tray_id = g_next_tray_id_win.fetch_add(1, std::memory_order_relaxed);
  // Run on UI thread to create the icon.
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid) {
                            HWND hwnd = EnsureTrayMessageWindow();
                            if (!hwnd)
                              return;
                            NOTIFYICONDATAW nid = {};
                            nid.cbSize = sizeof(nid);
                            nid.hWnd = hwnd;
                            nid.uID = tid;
                            nid.uFlags = NIF_MESSAGE;
                            nid.uCallbackMessage = WM_WEF_TRAYICON;
                            Shell_NotifyIconW(NIM_ADD, &nid);
                            WinTrayEntry entry = {};
                            entry.uid = tid;
                            std::lock_guard<std::mutex> lock(TrayMutexWin());
                            TrayMapWin()[tid] = std::move(entry);
                          },
                          tray_id));
  return tray_id;
}

void Backend_DestroyTrayIcon_Win(void* /*data*/, uint32_t tray_id) {
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid) {
                            HWND hwnd = g_tray_msg_hwnd;
                            if (!hwnd)
                              return;
                            NOTIFYICONDATAW nid = {};
                            nid.cbSize = sizeof(nid);
                            nid.hWnd = hwnd;
                            nid.uID = tid;
                            Shell_NotifyIconW(NIM_DELETE, &nid);
                            std::lock_guard<std::mutex> lock(TrayMutexWin());
                            auto it = TrayMapWin().find(tid);
                            if (it != TrayMapWin().end()) {
                              if (it->second.hicon_light)
                                DestroyIcon(it->second.hicon_light);
                              if (it->second.hicon_dark)
                                DestroyIcon(it->second.hicon_dark);
                              if (it->second.hmenu)
                                DestroyMenu(it->second.hmenu);
                              TrayMapWin().erase(it);
                            }
                          },
                          tray_id));
}

static bool WinIsDarkMode() {
  DWORD data = 1, size = sizeof(data), kind = 0;
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER,
                    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\"
                    L"Personalize",
                    0, KEY_READ, &key) != 0) {
    return false;
  }
  LONG rc = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &kind,
                             (LPBYTE)&data, &size);
  RegCloseKey(key);
  return (rc == 0 && kind == REG_DWORD && data == 0);
}

static void ApplyActiveIconWin(uint32_t tray_id) {
  HWND hwnd = g_tray_msg_hwnd;
  if (!hwnd)
    return;
  std::lock_guard<std::mutex> lock(TrayMutexWin());
  auto it = TrayMapWin().find(tray_id);
  if (it == TrayMapWin().end())
    return;
  HICON chosen = (WinIsDarkMode() && it->second.hicon_dark)
                     ? it->second.hicon_dark
                     : it->second.hicon_light;
  if (!chosen)
    return;
  NOTIFYICONDATAW nid = {};
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd;
  nid.uID = tray_id;
  nid.uFlags = NIF_ICON;
  nid.hIcon = chosen;
  Shell_NotifyIconW(NIM_MODIFY, &nid);
}

static void ReapplyAllWinTrayIcons() {
  std::vector<uint32_t> ids;
  {
    std::lock_guard<std::mutex> lock(TrayMutexWin());
    for (auto& [tid, e] : TrayMapWin())
      ids.push_back(tid);
  }
  for (uint32_t id : ids)
    ApplyActiveIconWin(id);
}

void Backend_SetTrayIcon_Win(void* /*data*/, uint32_t tray_id,
                             const void* png_bytes, size_t len) {
  if (!png_bytes || len == 0)
    return;
  std::vector<BYTE> copy((const BYTE*)png_bytes, (const BYTE*)png_bytes + len);
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, std::vector<BYTE> bytes) {
                            HICON hicon =
                                DecodePngToHicon(bytes.data(), bytes.size(),
                                                 GetSystemMetrics(SM_CXSMICON));
                            if (!hicon)
                              return;
                            {
                              std::lock_guard<std::mutex> lock(TrayMutexWin());
                              auto it = TrayMapWin().find(tid);
                              if (it == TrayMapWin().end()) {
                                DestroyIcon(hicon);
                                return;
                              }
                              if (it->second.hicon_light)
                                DestroyIcon(it->second.hicon_light);
                              it->second.hicon_light = hicon;
                            }
                            ApplyActiveIconWin(tid);
                          },
                          tray_id, std::move(copy)));
}

void Backend_SetTrayIconDark_Win(void* /*data*/, uint32_t tray_id,
                                 const void* png_bytes, size_t len) {
  if (!png_bytes || len == 0) {
    CefPostTask(TID_UI,
                base::BindOnce(
                    [](uint32_t tid) {
                      {
                        std::lock_guard<std::mutex> lock(TrayMutexWin());
                        auto it = TrayMapWin().find(tid);
                        if (it != TrayMapWin().end() && it->second.hicon_dark) {
                          DestroyIcon(it->second.hicon_dark);
                          it->second.hicon_dark = nullptr;
                        }
                      }
                      ApplyActiveIconWin(tid);
                    },
                    tray_id));
    return;
  }
  std::vector<BYTE> copy((const BYTE*)png_bytes, (const BYTE*)png_bytes + len);
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, std::vector<BYTE> bytes) {
                            HICON hicon =
                                DecodePngToHicon(bytes.data(), bytes.size(),
                                                 GetSystemMetrics(SM_CXSMICON));
                            if (!hicon)
                              return;
                            {
                              std::lock_guard<std::mutex> lock(TrayMutexWin());
                              auto it = TrayMapWin().find(tid);
                              if (it == TrayMapWin().end()) {
                                DestroyIcon(hicon);
                                return;
                              }
                              if (it->second.hicon_dark)
                                DestroyIcon(it->second.hicon_dark);
                              it->second.hicon_dark = hicon;
                            }
                            ApplyActiveIconWin(tid);
                          },
                          tray_id, std::move(copy)));
}

void Backend_SetTrayDoubleClickHandler_Win(void* /*data*/, uint32_t tray_id,
                                           wef_tray_click_fn handler,
                                           void* user_data) {
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, wef_tray_click_fn h, void* d) {
                            std::lock_guard<std::mutex> lock(TrayMutexWin());
                            auto it = TrayMapWin().find(tid);
                            if (it != TrayMapWin().end()) {
                              it->second.dblclick_fn = h;
                              it->second.dblclick_data = d;
                            }
                          },
                          tray_id, handler, user_data));
}

void Backend_SetTrayTooltip_Win(void* /*data*/, uint32_t tray_id,
                                const char* tooltip_or_null) {
  std::wstring wtip;
  if (tooltip_or_null && *tooltip_or_null) {
    int n = MultiByteToWideChar(CP_UTF8, 0, tooltip_or_null, -1, nullptr, 0);
    wtip.resize(n > 0 ? n - 1 : 0);
    if (n > 0)
      MultiByteToWideChar(CP_UTF8, 0, tooltip_or_null, -1, wtip.data(), n);
  }
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, std::wstring t) {
                            HWND hwnd = g_tray_msg_hwnd;
                            if (!hwnd)
                              return;
                            NOTIFYICONDATAW nid = {};
                            nid.cbSize = sizeof(nid);
                            nid.hWnd = hwnd;
                            nid.uID = tid;
                            nid.uFlags = NIF_TIP;
                            wcsncpy_s(nid.szTip, t.c_str(), _TRUNCATE);
                            Shell_NotifyIconW(NIM_MODIFY, &nid);
                          },
                          tray_id, std::move(wtip)));
}

void Backend_SetTrayMenu_Win(void* data, uint32_t tray_id,
                             wef_value_t* menu_template,
                             wef_menu_click_fn on_click, void* on_click_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();
  CefPostTask(
      TID_UI,
      base::BindOnce(
          [](uint32_t tid, wef_value_t* tmpl, const wef_backend_api_t* a,
             wef_menu_click_fn cb, void* cb_data) {
            std::map<UINT, std::string> cmd_to_id;
            HMENU menu =
                tmpl ? BuildWinMenuFromValue(tmpl, a, cmd_to_id) : nullptr;
            if (tmpl)
              a->value_free(tmpl);
            std::lock_guard<std::mutex> lock(TrayMutexWin());
            auto it = TrayMapWin().find(tid);
            if (it == TrayMapWin().end()) {
              if (menu)
                DestroyMenu(menu);
              return;
            }
            if (it->second.hmenu)
              DestroyMenu(it->second.hmenu);
            it->second.hmenu = menu;
            it->second.cmd_to_id = std::move(cmd_to_id);
            it->second.menu_click_fn = cb;
            it->second.menu_click_data = cb_data;
          },
          tray_id, menu_template, api, on_click, on_click_data));
}

void Backend_SetTrayClickHandler_Win(void* /*data*/, uint32_t tray_id,
                                     wef_tray_click_fn handler,
                                     void* user_data) {
  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t tid, wef_tray_click_fn h, void* d) {
                            std::lock_guard<std::mutex> lock(TrayMutexWin());
                            auto it = TrayMapWin().find(tid);
                            if (it != TrayMapWin().end()) {
                              it->second.click_fn = h;
                              it->second.click_data = d;
                            }
                          },
                          tray_id, handler, user_data));
}

#elif defined(__linux__)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

static void Backend_BounceDock_Linux(void* data, int /*type*/) {
  // X11 urgency hint is binary — there's no informational vs critical. Set
  // it on every WEF window; WMs will surface this (taskbar flash, workspace
  // indicator, etc.).
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->ForEachBrowser([](CefRefPtr<CefBrowser> browser) {
    CefPostTask(TID_UI,
                base::BindOnce(
                    [](CefRefPtr<CefBrowser> b) {
#ifdef GDK_WINDOWING_X11
                      GdkDisplay* gdk_display = gdk_display_get_default();
                      if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display))
                        return;
                      Display* display = GDK_DISPLAY_XDISPLAY(gdk_display);
                      ::Window win = (::Window)b->GetHost()->GetWindowHandle();
                      if (!win)
                        return;
                      XWMHints* hints = XGetWMHints(display, win);
                      if (!hints)
                        hints = XAllocWMHints();
                      if (hints) {
                        hints->flags |= XUrgencyHint;
                        XSetWMHints(display, win, hints);
                        XFree(hints);
                        XFlush(display);
                      }
#else
                      (void)b;
#endif
                    },
                    browser));
  });
}
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
                              b->GetHost()->ShowDevTools(windowInfo, nullptr,
                                                         CefBrowserSettings(),
                                                         CefPoint());
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

  CefPostTask(TID_UI, base::BindOnce(
                          [](uint32_t wid) {
                            auto* handler = WefHandler::GetInstance();
                            if (!handler)
                              return;

                            // Push wef_id before creating the browser so
                            // OnAfterCreated can pop it. Both run on the UI
                            // thread so no race.
                            g_pending_wef_ids.push(wid);

                            CefBrowserSettings browser_settings;
                            CefRefPtr<CefDictionaryValue> extra_info =
                                CefDictionaryValue::Create();
                            extra_info->SetString(
                                "wef_js_namespace",
                                RuntimeLoader::GetInstance()->GetJsNamespace());
                            CefRefPtr<CefBrowserView> browser_view =
                                CefBrowserView::CreateBrowserView(
                                    handler, "about:blank", browser_settings,
                                    extra_info, nullptr, nullptr);
                            CefWindow::CreateTopLevelWindow(
                                new WefWindowDelegate(browser_view, wid));
                          },
                          window_id));

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
                               wef_dialog_result_fn callback,
                               void* callback_data) {
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
    cmd = "zenity --info --title=\"" + title_str + "\" --text=\"" +
          message_str + "\" 2>/dev/null";
    int ret = system(cmd.c_str());
    if (callback)
      callback(callback_data, (ret == 0) ? 1 : 0, nullptr);
  } else if (dialog_type == WEF_DIALOG_CONFIRM) {
    cmd = "zenity --question --title=\"" + title_str + "\" --text=\"" +
          message_str + "\" 2>/dev/null";
    int ret = system(cmd.c_str());
    if (callback)
      callback(callback_data, (ret == 0) ? 1 : 0, nullptr);
  } else if (dialog_type == WEF_DIALOG_PROMPT) {
    cmd = "zenity --entry --title=\"" + title_str + "\" --text=\"" +
          message_str + "\" --entry-text=\"" + default_str + "\" 2>/dev/null";
    FILE* fp = popen(cmd.c_str(), "r");
    if (fp) {
      char buf[4096] = {};
      if (fgets(buf, sizeof(buf), fp)) {
        // Remove trailing newline
        size_t len = strlen(buf);
        if (len > 0 && buf[len - 1] == '\n')
          buf[len - 1] = '\0';
      }
      int ret = pclose(fp);
      if (callback)
        callback(callback_data, (ret == 0) ? 1 : 0, (ret == 0) ? buf : nullptr);
    } else {
      if (callback)
        callback(callback_data, 0, nullptr);
    }
  }
#elif defined(_WIN32)
  CefPostTask(TID_UI,
              base::BindOnce(
                  [](int dtype, std::string t, std::string m, std::string d,
                     wef_dialog_result_fn cb, void* cb_data) {
                    if (dtype == WEF_DIALOG_ALERT) {
                      MessageBoxA(nullptr, m.c_str(), t.c_str(),
                                  MB_OK | MB_ICONINFORMATION);
                      if (cb)
                        cb(cb_data, 1, nullptr);
                    } else if (dtype == WEF_DIALOG_CONFIRM) {
                      int ret = MessageBoxA(nullptr, m.c_str(), t.c_str(),
                                            MB_OKCANCEL | MB_ICONQUESTION);
                      if (cb)
                        cb(cb_data, (ret == IDOK) ? 1 : 0, nullptr);
                    } else if (dtype == WEF_DIALOG_PROMPT) {
                      // Use default value as result for now (Windows prompt
                      // requires custom dialog)
                      int ret = MessageBoxA(nullptr, m.c_str(), t.c_str(),
                                            MB_OKCANCEL | MB_ICONQUESTION);
                      if (cb)
                        cb(cb_data, (ret == IDOK) ? 1 : 0,
                           (ret == IDOK) ? d.c_str() : nullptr);
                    }
                  },
                  dialog_type, title_str, message_str, default_str, callback,
                  callback_data));
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

  backend_api_.get_window_handle = [](void*, uint32_t) -> void* {
    return nullptr;
  };
  backend_api_.get_display_handle = [](void*, uint32_t) -> void* {
    return nullptr;
  };
#if defined(_WIN32)
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int {
    return WEF_WINDOW_HANDLE_WIN32;
  };
#elif defined(__APPLE__)
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int {
    return WEF_WINDOW_HANDLE_APPKIT;
  };
#else
  backend_api_.get_window_handle_type = [](void*, uint32_t) -> int {
    return WEF_WINDOW_HANDLE_X11;
  };
#endif

  backend_api_.set_keyboard_event_handler = Backend_SetKeyboardEventHandler;
  backend_api_.set_mouse_click_handler = Backend_SetMouseClickHandler;
  backend_api_.set_mouse_move_handler = Backend_SetMouseMoveHandler;
  backend_api_.set_wheel_handler = Backend_SetWheelHandler;
  backend_api_.set_cursor_enter_leave_handler =
      Backend_SetCursorEnterLeaveHandler;
  backend_api_.set_focused_handler = Backend_SetFocusedHandler;
  backend_api_.set_resize_handler = Backend_SetResizeHandler;
  backend_api_.set_move_handler = Backend_SetMoveHandler;
  backend_api_.set_close_requested_handler = Backend_SetCloseRequestedHandler;

  backend_api_.poll_js_calls = [](void* data) {
    RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
    loader->PollPendingJsCalls();
  };

  backend_api_.set_js_call_notify = [](void* data, void (*notify_fn)(void*),
                                       void* notify_data) {
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
  // Linux: in-window menu bar (set_application_menu) requires packing a
  // GtkMenuBar above the browser, which means the top-level window must be
  // a GtkWindow we own. CEF Views creates the native window itself, so an
  // embedded menubar isn't reachable without reparenting CEF into a GTK
  // host — and that path breaks on XWayland (cross-client X11 child
  // windows aren't supported in Wayland-native ways). Context menus and
  // tray menus still work because GtkMenu popups don't need a GtkWindow.
  backend_api_.set_application_menu = [](void*, uint32_t, wef_value_t*,
                                         wef_menu_click_fn, void*) {};
  backend_api_.show_context_menu = Backend_ShowContextMenu_Linux;
#endif

  backend_api_.open_devtools = Backend_OpenDevTools;
  backend_api_.set_js_namespace = Backend_SetJsNamespace;
  backend_api_.show_dialog = Backend_ShowDialog;

  // --- Dock / taskbar ---
#if defined(__APPLE__)
  backend_api_.set_dock_badge = Backend_SetDockBadge_Mac;
  backend_api_.bounce_dock = Backend_BounceDock_Mac;
  backend_api_.set_dock_menu = Backend_SetDockMenu_Mac;
  backend_api_.set_dock_visible = Backend_SetDockVisible_Mac;
  backend_api_.set_dock_reopen_handler = Backend_SetDockReopenHandler_Mac;
#elif defined(_WIN32)
  backend_api_.bounce_dock = Backend_BounceDock_Win;
  backend_api_.set_dock_badge = Backend_SetDockBadge_TitlePrefix;
  // Menu/visible/reopen have no Windows analog — left nullptr; the
  // runtime-side Rust wrapper silently no-ops when the pointer is missing.
#elif defined(__linux__)
  backend_api_.bounce_dock = Backend_BounceDock_Linux;
  backend_api_.set_dock_badge = Backend_SetDockBadge_TitlePrefix;
  // Menu/visible/reopen: left nullptr (no clean Linux analog).
#endif

  // --- Tray / status bar ---
#if defined(__APPLE__)
  backend_api_.create_tray_icon = Backend_CreateTrayIcon_Mac;
  backend_api_.destroy_tray_icon = Backend_DestroyTrayIcon_Mac;
  backend_api_.set_tray_icon = Backend_SetTrayIcon_Mac;
  backend_api_.set_tray_tooltip = Backend_SetTrayTooltip_Mac;
  backend_api_.set_tray_menu = Backend_SetTrayMenu_Mac;
  backend_api_.set_tray_click_handler = Backend_SetTrayClickHandler_Mac;
  backend_api_.set_tray_double_click_handler =
      Backend_SetTrayDoubleClickHandler_Mac;
  backend_api_.set_tray_icon_dark = Backend_SetTrayIconDark_Mac;
#elif defined(_WIN32)
  backend_api_.create_tray_icon = Backend_CreateTrayIcon_Win;
  backend_api_.destroy_tray_icon = Backend_DestroyTrayIcon_Win;
  backend_api_.set_tray_icon = Backend_SetTrayIcon_Win;
  backend_api_.set_tray_tooltip = Backend_SetTrayTooltip_Win;
  backend_api_.set_tray_menu = Backend_SetTrayMenu_Win;
  backend_api_.set_tray_click_handler = Backend_SetTrayClickHandler_Win;
  backend_api_.set_tray_double_click_handler =
      Backend_SetTrayDoubleClickHandler_Win;
  backend_api_.set_tray_icon_dark = Backend_SetTrayIconDark_Win;
#elif defined(__linux__)
  backend_api_.create_tray_icon = Backend_CreateTrayIcon_Linux;
  backend_api_.destroy_tray_icon = Backend_DestroyTrayIcon_Linux;
  backend_api_.set_tray_icon = Backend_SetTrayIcon_Linux;
  backend_api_.set_tray_tooltip = Backend_SetTrayTooltip_Linux;
  backend_api_.set_tray_menu = Backend_SetTrayMenu_Linux;
  backend_api_.set_tray_click_handler = Backend_SetTrayClickHandler_Linux;
  backend_api_.set_tray_double_click_handler =
      Backend_SetTrayDoubleClickHandler_Linux;
  backend_api_.set_tray_icon_dark = Backend_SetTrayIconDark_Linux;
#endif
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
    std::cerr << "Failed to find " << WEF_RUNTIME_INIT_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }

  start_fn_ = reinterpret_cast<wef_runtime_start_fn>(
      dlsym(library_handle_, WEF_RUNTIME_START_SYMBOL));
  if (!start_fn_) {
    std::cerr << "Failed to find " << WEF_RUNTIME_START_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }

  shutdown_fn_ = reinterpret_cast<wef_runtime_shutdown_fn>(
      dlsym(library_handle_, WEF_RUNTIME_SHUTDOWN_SYMBOL));
  if (!shutdown_fn_) {
    std::cerr << "Failed to find " << WEF_RUNTIME_SHUTDOWN_SYMBOL << ": "
              << dlerror() << std::endl;
    return false;
  }
#else
  library_handle_ = LoadLibraryA(path.c_str());
  if (!library_handle_) {
    std::cerr << "Failed to load runtime: error " << GetLastError()
              << std::endl;
    return false;
  }

  init_fn_ = reinterpret_cast<wef_runtime_init_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), WEF_RUNTIME_INIT_SYMBOL));
  if (!init_fn_) {
    std::cerr << "Failed to find " << WEF_RUNTIME_INIT_SYMBOL << std::endl;
    return false;
  }

  start_fn_ = reinterpret_cast<wef_runtime_start_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), WEF_RUNTIME_START_SYMBOL));
  if (!start_fn_) {
    std::cerr << "Failed to find " << WEF_RUNTIME_START_SYMBOL << std::endl;
    return false;
  }

  shutdown_fn_ = reinterpret_cast<wef_runtime_shutdown_fn>(GetProcAddress(
      static_cast<HMODULE>(library_handle_), WEF_RUNTIME_SHUTDOWN_SYMBOL));
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

void RuntimeLoader::HandleEvalResult(uint64_t eval_id,
                                     CefRefPtr<CefValue> result,
                                     const std::string& error) {
  PendingEval eval;
  {
    std::lock_guard<std::mutex> lock(eval_mutex_);
    auto it = pending_evals_.find(eval_id);
    if (it == pending_evals_.end())
      return;
    eval = it->second;
    pending_evals_.erase(it);
  }

  if (!error.empty()) {
    CefRefPtr<CefValue> errValue = CefValue::Create();
    errValue->SetString(error);
    wef_value errWef(errValue);
    eval.callback(nullptr, &errWef, eval.callback_data);
  } else if (result && result->GetType() != VTYPE_NULL) {
    wef_value resultWef(result);
    eval.callback(&resultWef, nullptr, eval.callback_data);
  } else {
    eval.callback(nullptr, nullptr, eval.callback_data);
  }
}

void RuntimeLoader::OnJsCall(uint32_t window_id, uint64_t call_id,
                             const std::string& method_path,
                             CefRefPtr<CefListValue> args) {
  // The CefListValue passed in is owned by the CefProcessMessage and
  // becomes invalid once OnProcessMessageReceived returns. Copy it so the
  // queued entry survives until PollPendingJsCalls runs.
  CefRefPtr<CefListValue> owned_args =
      args ? args->Copy() : CefListValue::Create();
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_js_calls_.push({window_id, call_id, method_path, owned_args});
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

  if (calls.empty())
    return;

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
      handler(user_data, call.window_id, call.call_id, call.method_path.c_str(),
              argsWrapper);
    } else {
      CefRefPtr<CefValue> errVal = CefValue::Create();
      errVal->SetString("No JS call handler registered");
      wef_value_t errWrapper(errVal);
      Backend_JsCallRespond(this, call.call_id, nullptr, &errWrapper);
    }
  }
}
