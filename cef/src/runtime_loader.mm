// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"

#include <dlfcn.h>
#include <iostream>
#include <cstring>

#import <Cocoa/Cocoa.h>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

RuntimeLoader* RuntimeLoader::instance_ = nullptr;

// --- Native Mouse Monitor (macOS) ---

static id g_mouse_monitor = nil;
static id g_mouse_move_monitor = nil;
static id g_scroll_monitor = nil;

static uint32_t NSModifierFlagsToWef(NSEventModifierFlags flags) {
  uint32_t mods = 0;
  if (flags & NSEventModifierFlagShift)   mods |= WEF_MOD_SHIFT;
  if (flags & NSEventModifierFlagControl) mods |= WEF_MOD_CONTROL;
  if (flags & NSEventModifierFlagOption)  mods |= WEF_MOD_ALT;
  if (flags & NSEventModifierFlagCommand) mods |= WEF_MOD_META;
  return mods;
}

static int NSButtonToWef(NSInteger buttonNumber) {
  switch (buttonNumber) {
    case 0: return WEF_MOUSE_BUTTON_LEFT;
    case 1: return WEF_MOUSE_BUTTON_RIGHT;
    case 2: return WEF_MOUSE_BUTTON_MIDDLE;
    case 3: return WEF_MOUSE_BUTTON_BACK;
    case 4: return WEF_MOUSE_BUTTON_FORWARD;
    default: return WEF_MOUSE_BUTTON_LEFT;
  }
}

void InstallNativeMouseMonitor() {
  if (g_mouse_monitor) return;

  NSEventMask mask = NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp
                   | NSEventMaskRightMouseDown | NSEventMaskRightMouseUp
                   | NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp;

  g_mouse_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
      handler:^NSEvent*(NSEvent* event) {
        int state;
        switch ([event type]) {
          case NSEventTypeLeftMouseDown:
          case NSEventTypeRightMouseDown:
          case NSEventTypeOtherMouseDown:
            state = WEF_MOUSE_PRESSED;
            break;
          default:
            state = WEF_MOUSE_RELEASED;
            break;
        }

        int button = NSButtonToWef([event buttonNumber]);
        uint32_t modifiers = NSModifierFlagsToWef([event modifierFlags]);
        int32_t click_count = (int32_t)[event clickCount];

        // Get position in window coordinates
        NSPoint loc = [event locationInWindow];
        NSWindow* win = [event window];
        double x = loc.x;
        double y = 0;
        if (win) {
          // Flip Y: NSWindow uses bottom-left origin, we want top-left
          y = [win contentLayoutRect].size.height - loc.y;
        }

        RuntimeLoader::GetInstance()->DispatchMouseClickEvent(
            state, button, x, y, modifiers, click_count);

        return event; // Don't consume
      }];

  g_mouse_move_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:
      (NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
       NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged)
      handler:^NSEvent*(NSEvent* event) {
        uint32_t modifiers = NSModifierFlagsToWef([event modifierFlags]);
        NSPoint loc = [event locationInWindow];
        NSWindow* win = [event window];
        double x = loc.x;
        double y = 0;
        if (win) {
          y = [win contentLayoutRect].size.height - loc.y;
        }

        RuntimeLoader::GetInstance()->DispatchMouseMoveEvent(x, y, modifiers);
        return event;
      }];

  g_scroll_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
      handler:^NSEvent*(NSEvent* event) {
        double delta_x = [event scrollingDeltaX];
        double delta_y = [event scrollingDeltaY];
        uint32_t modifiers = NSModifierFlagsToWef([event modifierFlags]);

        // Determine delta mode: continuous (trackpad) = pixel, line-based (mouse wheel) = line
        int32_t delta_mode = [event hasPreciseScrollingDeltas]
            ? WEF_WHEEL_DELTA_PIXEL : WEF_WHEEL_DELTA_LINE;

        NSPoint loc = [event locationInWindow];
        NSWindow* win = [event window];
        double x = loc.x;
        double y = 0;
        if (win) {
          y = [win contentLayoutRect].size.height - loc.y;
        }

        RuntimeLoader::GetInstance()->DispatchWheelEvent(
            delta_x, delta_y, x, y, modifiers, delta_mode);
        return event;
      }];
}

void RemoveNativeMouseMonitor() {
  if (g_mouse_monitor) {
    [NSEvent removeMonitor:g_mouse_monitor];
    g_mouse_monitor = nil;
  }
  if (g_mouse_move_monitor) {
    [NSEvent removeMonitor:g_mouse_move_monitor];
    g_mouse_move_monitor = nil;
  }
  if (g_scroll_monitor) {
    [NSEvent removeMonitor:g_scroll_monitor];
    g_scroll_monitor = nil;
  }
}

static void Backend_Navigate(void* data, const char* url) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
  if (browser && url) {
    std::string url_str(url);
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b, std::string u) {
          b->GetMainFrame()->LoadURL(u);
        },
        browser, url_str));
  }
}

static void Backend_SetTitle(void* data, const char* title) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
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

static void Backend_ExecuteJs(void* data, const char* script,
                              wef_js_result_fn callback, void* callback_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
  if (browser && script) {
    std::string script_str(script);
    CefPostTask(TID_UI, base::BindOnce(
        [](CefRefPtr<CefBrowser> b, std::string s) {
          b->GetMainFrame()->ExecuteJavaScript(s, "", 0);
        },
        browser, script_str));
    // TODO: CEF's ExecuteJavaScript is fire-and-forget, no result callback support
    // For now, if a callback is provided, call it with null result immediately
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

static void Backend_SetWindowSize(void* data, int width, int height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
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

static void Backend_GetWindowSize(void* data, int* width, int* height) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
  if (browser) {
    auto browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view) {
      auto window = browser_view->GetWindow();
      if (window) {
        CefSize size = window->GetSize();
        if (width) *width = size.width;
        if (height) *height = size.height;
        return;
      }
    }
  }
  if (width) *width = 0;
  if (height) *height = 0;
}

static void Backend_SetWindowPosition(void* data, int x, int y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
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

static void Backend_GetWindowPosition(void* data, int* x, int* y) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
  if (browser) {
    auto browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view) {
      auto window = browser_view->GetWindow();
      if (window) {
        CefPoint pos = window->GetPosition();
        if (x) *x = pos.x;
        if (y) *y = pos.y;
        return;
      }
    }
  }
  if (x) *x = 0;
  if (y) *y = 0;
}

static void Backend_SetResizable(void* data, bool resizable) {
  // CEF Views framework does not expose a direct resizable toggle
  (void)data;
  (void)resizable;
}

static bool Backend_IsResizable(void* data) {
  // CEF Views framework does not expose a direct resizable query
  (void)data;
  return true;
}

static void Backend_SetAlwaysOnTop(void* data, bool always_on_top) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
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

static bool Backend_IsAlwaysOnTop(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
  if (browser) {
    auto browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view) {
      auto window = browser_view->GetWindow();
      if (window) {
        return window->IsAlwaysOnTop();
      }
    }
  }
  return false;
}

static bool Backend_IsVisible(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
  if (browser) {
    auto browser_view = CefBrowserView::GetForBrowser(browser);
    if (browser_view) {
      auto window = browser_view->GetWindow();
      if (window) {
        return window->IsVisible();
      }
    }
  }
  return false;
}

static void Backend_Show(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
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

static void Backend_Hide(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
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

static void Backend_Focus(void* data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
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

static void Backend_SetJsCallHandler(void* data, wef_js_call_fn handler, void* user_data) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  loader->SetJsCallHandler(handler, user_data);
}

static void Backend_JsCallRespond(void* data, uint64_t call_id,
                                  wef_value_t* result, wef_value_t* error) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
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
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
  if (!browser) return;

  CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("wef_callback");
  CefRefPtr<CefListValue> msgArgs = msg->GetArgumentList();
  msgArgs->SetInt(0, static_cast<int>(callback_id));

  if (args && args->value && args->value->GetType() == VTYPE_LIST) {
    msgArgs->SetList(1, args->value->GetList());
  } else {
    msgArgs->SetList(1, CefListValue::Create());
  }

  CefPostTask(TID_UI, base::BindOnce(
      [](CefRefPtr<CefBrowser> b, CefRefPtr<CefProcessMessage> m) {
        b->GetMainFrame()->SendProcessMessage(PID_RENDERER, m);
      },
      browser, msg));
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

static void Backend_ReleaseJsCallback(void* data, uint64_t callback_id) {
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  CefRefPtr<CefBrowser> browser = loader->GetBrowser();
  if (!browser) return;

  CefRefPtr<CefProcessMessage> msg = CefProcessMessage::Create("wef_release_callback");
  CefRefPtr<CefListValue> msgArgs = msg->GetArgumentList();
  msgArgs->SetInt(0, static_cast<int>(callback_id));

  CefPostTask(TID_UI, base::BindOnce(
      [](CefRefPtr<CefBrowser> b, CefRefPtr<CefProcessMessage> m) {
        b->GetMainFrame()->SendProcessMessage(PID_RENDERER, m);
      },
      browser, msg));
}

// --- Application Menu ---

// Store the menu click handler globally so NSMenu target-action can invoke it.
static wef_menu_click_fn g_menu_click_fn = nullptr;
static void* g_menu_click_data = nullptr;

// Objective-C class that handles menu item clicks for custom items.
#ifdef __OBJC__
@interface WefMenuTarget : NSObject
+ (instancetype)shared;
- (void)menuItemClicked:(id)sender;
@end

@implementation WefMenuTarget

+ (instancetype)shared {
  static WefMenuTarget* instance = nil;
  static dispatch_once_t onceToken;
  dispatch_once(&onceToken, ^{
    instance = [[WefMenuTarget alloc] init];
  });
  return instance;
}

- (void)menuItemClicked:(id)sender {
  NSMenuItem* item = (NSMenuItem*)sender;
  NSString* itemId = [item representedObject];
  if (itemId && g_menu_click_fn) {
    g_menu_click_fn(g_menu_click_data, [itemId UTF8String]);
  }
}

@end
#endif

// Parse accelerator string like "cmd+shift+n" into key equivalent + modifier mask.
static void ParseAccelerator(const std::string& accel, NSString** outKey,
                             NSEventModifierFlags* outMask) {
  *outKey = @"";
  *outMask = 0;

  std::string lower = accel;
  for (auto& c : lower) c = tolower(c);

  // Split by +
  size_t pos = 0;
  std::vector<std::string> parts;
  std::string remaining = lower;
  while ((pos = remaining.find('+')) != std::string::npos) {
    parts.push_back(remaining.substr(0, pos));
    remaining = remaining.substr(pos + 1);
  }
  if (!remaining.empty()) parts.push_back(remaining);

  for (const auto& part : parts) {
    if (part == "cmd" || part == "command" || part == "cmdorctrl" ||
        part == "commandorcontrol") {
      *outMask |= NSEventModifierFlagCommand;
    } else if (part == "shift") {
      *outMask |= NSEventModifierFlagShift;
    } else if (part == "alt" || part == "option") {
      *outMask |= NSEventModifierFlagOption;
    } else if (part == "ctrl" || part == "control") {
      *outMask |= NSEventModifierFlagControl;
    } else {
      // This is the key
      *outKey = [NSString stringWithUTF8String:part.c_str()];
    }
  }
}

// Map a role string to an NSMenuItem with the standard action.
static NSMenuItem* CreateRoleMenuItem(const std::string& role) {
  NSString* title = @"";
  SEL action = nil;
  NSString* keyEquiv = @"";
  NSEventModifierFlags mask = NSEventModifierFlagCommand;

  if (role == "quit") {
    title = @"Quit";
    action = @selector(terminate:);
    keyEquiv = @"q";
  } else if (role == "copy") {
    title = @"Copy";
    action = @selector(copy:);
    keyEquiv = @"c";
  } else if (role == "paste") {
    title = @"Paste";
    action = @selector(paste:);
    keyEquiv = @"v";
  } else if (role == "cut") {
    title = @"Cut";
    action = @selector(cut:);
    keyEquiv = @"x";
  } else if (role == "selectall" || role == "selectAll") {
    title = @"Select All";
    action = @selector(selectAll:);
    keyEquiv = @"a";
  } else if (role == "undo") {
    title = @"Undo";
    action = @selector(undo:);
    keyEquiv = @"z";
  } else if (role == "redo") {
    title = @"Redo";
    action = @selector(redo:);
    keyEquiv = @"Z";
    mask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
  } else if (role == "minimize") {
    title = @"Minimize";
    action = @selector(performMiniaturize:);
    keyEquiv = @"m";
  } else if (role == "zoom") {
    title = @"Zoom";
    action = @selector(performZoom:);
  } else if (role == "close") {
    title = @"Close";
    action = @selector(performClose:);
    keyEquiv = @"w";
  } else if (role == "about") {
    title = @"About";
    action = @selector(orderFrontStandardAboutPanel:);
  } else if (role == "hide") {
    title = @"Hide";
    action = @selector(hide:);
    keyEquiv = @"h";
  } else if (role == "hideothers" || role == "hideOthers") {
    title = @"Hide Others";
    action = @selector(hideOtherApplications:);
    keyEquiv = @"h";
    mask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
  } else if (role == "unhide") {
    title = @"Show All";
    action = @selector(unhideAllApplications:);
  } else if (role == "front") {
    title = @"Bring All to Front";
    action = @selector(arrangeInFront:);
  } else if (role == "togglefullscreen" || role == "toggleFullScreen") {
    title = @"Toggle Full Screen";
    action = @selector(toggleFullScreen:);
    keyEquiv = @"f";
    mask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
  } else {
    return nil;
  }

  NSMenuItem* item = [[NSMenuItem alloc]
      initWithTitle:title
             action:action
      keyEquivalent:keyEquiv];
  [item setKeyEquivalentModifierMask:mask];
  return item;
}

// Recursively build an NSMenu from a wef_value_t list.
static NSMenu* BuildMenuFromValue(wef_value_t* val,
                                  const wef_backend_api_t* api) {
  if (!val || !api->value_is_list(val)) return nil;

  NSMenu* menu = [[NSMenu alloc] init];
  [menu setAutoenablesItems:NO];

  size_t count = api->value_list_size(val);
  for (size_t i = 0; i < count; ++i) {
    wef_value_t* itemVal = api->value_list_get(val, i);
    if (!itemVal || !api->value_is_dict(itemVal)) continue;

    // Check for separator
    wef_value_t* typeVal = api->value_dict_get(itemVal, "type");
    if (typeVal && api->value_is_string(typeVal)) {
      size_t len = 0;
      char* typeStr = api->value_get_string(typeVal, &len);
      if (typeStr && std::string(typeStr) == "separator") {
        [menu addItem:[NSMenuItem separatorItem]];
        api->value_free_string(typeStr);
        continue;
      }
      if (typeStr) api->value_free_string(typeStr);
    }

    // Check for role
    wef_value_t* roleVal = api->value_dict_get(itemVal, "role");
    if (roleVal && api->value_is_string(roleVal)) {
      size_t len = 0;
      char* roleStr = api->value_get_string(roleVal, &len);
      if (roleStr) {
        NSMenuItem* roleItem = CreateRoleMenuItem(roleStr);
        if (roleItem) {
          [menu addItem:roleItem];
        }
        api->value_free_string(roleStr);
        continue;
      }
    }

    // Regular item or submenu
    wef_value_t* labelVal = api->value_dict_get(itemVal, "label");
    if (!labelVal || !api->value_is_string(labelVal)) continue;

    size_t labelLen = 0;
    char* labelStr = api->value_get_string(labelVal, &labelLen);
    if (!labelStr) continue;
    NSString* label = [NSString stringWithUTF8String:labelStr];
    api->value_free_string(labelStr);

    // Check for submenu
    wef_value_t* submenuVal = api->value_dict_get(itemVal, "submenu");
    if (submenuVal && api->value_is_list(submenuVal)) {
      NSMenuItem* submenuItem = [[NSMenuItem alloc] init];
      [submenuItem setTitle:label];
      NSMenu* submenu = BuildMenuFromValue(submenuVal, api);
      [submenu setTitle:label];
      [submenuItem setSubmenu:submenu];
      [menu addItem:submenuItem];
      continue;
    }

    // Regular clickable item
    NSString* keyEquiv = @"";
    NSEventModifierFlags modMask = NSEventModifierFlagCommand;

    wef_value_t* accelVal = api->value_dict_get(itemVal, "accelerator");
    if (accelVal && api->value_is_string(accelVal)) {
      size_t accelLen = 0;
      char* accelStr = api->value_get_string(accelVal, &accelLen);
      if (accelStr) {
        ParseAccelerator(accelStr, &keyEquiv, &modMask);
        api->value_free_string(accelStr);
      }
    }

    NSMenuItem* nsItem = [[NSMenuItem alloc]
        initWithTitle:label
               action:@selector(menuItemClicked:)
        keyEquivalent:keyEquiv];
    [nsItem setKeyEquivalentModifierMask:modMask];
    [nsItem setTarget:[WefMenuTarget shared]];

    // Store the item id
    wef_value_t* idVal = api->value_dict_get(itemVal, "id");
    if (idVal && api->value_is_string(idVal)) {
      size_t idLen = 0;
      char* idStr = api->value_get_string(idVal, &idLen);
      if (idStr) {
        [nsItem setRepresentedObject:[NSString stringWithUTF8String:idStr]];
        api->value_free_string(idStr);
      }
    }

    // Check enabled state
    wef_value_t* enabledVal = api->value_dict_get(itemVal, "enabled");
    if (enabledVal && api->value_is_bool(enabledVal)) {
      [nsItem setEnabled:api->value_get_bool(enabledVal)];
    } else {
      [nsItem setEnabled:YES];
    }

    [menu addItem:nsItem];
  }

  return menu;
}

static void Backend_SetApplicationMenu(void* data, wef_value_t* menu_template,
                                       wef_menu_click_fn on_click,
                                       void* on_click_data) {
  if (!menu_template) return;

  // Store callback
  g_menu_click_fn = on_click;
  g_menu_click_data = on_click_data;

  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  // Need a reference to the API for value accessors
  const wef_backend_api_t* api = &loader->GetBackendApi();

  // Build the menu on the UI thread
  // We need to copy the template since it may be freed after this call returns.
  // For simplicity, we'll build the menu structure here and post to UI thread.
  // Since CefPostTask requires the work to happen on the UI thread for NSMenu,
  // we do the parsing here (safe from any thread) and post the NSMenu creation.

  // Parse on current thread, create NSMenu on UI thread
  // Actually, NSMenu creation must happen on the main thread.
  // Let's just post the whole thing to the UI thread.

  // We need to capture the value. Since wef_value_t is backend-owned,
  // we need to build the NSMenu directly. Use dispatch_async for macOS.
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menubar = BuildMenuFromValue(menu_template, api);
    if (menubar) {
      // The top-level menu is a list of submenus, each becoming a menubar item.
      // BuildMenuFromValue already creates the structure correctly.
      [NSApp setMainMenu:menubar];
    }
  });
}

void RuntimeLoader::InitializeBackendApi() {
  backend_api_.version = WEF_API_VERSION;
  backend_api_.backend_data = this;

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

  backend_api_.get_window_handle = [](void*) -> void* { return nullptr; };
  backend_api_.get_display_handle = [](void*) -> void* { return nullptr; };
  backend_api_.get_window_handle_type = [](void*) -> int { return WEF_WINDOW_HANDLE_APPKIT; };

  backend_api_.set_keyboard_event_handler = Backend_SetKeyboardEventHandler;
  backend_api_.set_mouse_click_handler = Backend_SetMouseClickHandler;
  backend_api_.set_mouse_move_handler = Backend_SetMouseMoveHandler;
  backend_api_.set_wheel_handler = Backend_SetWheelHandler;

  backend_api_.poll_js_calls = [](void* data) {
    RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
    loader->PollPendingJsCalls();
  };

  backend_api_.set_js_call_notify = [](void* data, void (*notify_fn)(void*), void* notify_data) {
    RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
    loader->SetJsCallNotify(notify_fn, notify_data);
  };

  backend_api_.set_application_menu = Backend_SetApplicationMenu;
}

RuntimeLoader::RuntimeLoader() {
  instance_ = this;
  InitializeBackendApi();
}

RuntimeLoader::~RuntimeLoader() {
  Shutdown();
  if (library_handle_) {
    dlclose(library_handle_);
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

void RuntimeLoader::SetBrowser(CefRefPtr<CefBrowser> browser) {
  browser_ = browser;
}

CefRefPtr<CefBrowser> RuntimeLoader::GetBrowser() {
  return browser_;
}

void RuntimeLoader::OnJsCall(uint64_t call_id, const std::string& method_path,
                             CefRefPtr<CefListValue> args) {
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_js_calls_.push({call_id, method_path, args});
  }

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
      handler(user_data, call.call_id, call.method_path.c_str(), argsWrapper);
    } else {
      CefRefPtr<CefValue> errVal = CefValue::Create();
      errVal->SetString("No JS call handler registered");
      wef_value_t errWrapper(errVal);
      Backend_JsCallRespond(this, call.call_id, nullptr, &errWrapper);
    }
  }
}
