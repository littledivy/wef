// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"
#include "wef_json.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <webkit2/webkit2.h>
#include <JavaScriptCore/JavaScript.h>

#include <iostream>
#include <map>
#include <mutex>

namespace keyboard {

std::string GdkKeyvalToKey(guint keyval) {
  switch (keyval) {
    case GDK_KEY_BackSpace: return "Backspace";
    case GDK_KEY_Tab: case GDK_KEY_ISO_Left_Tab: return "Tab";
    case GDK_KEY_Return: case GDK_KEY_KP_Enter: return "Enter";
    case GDK_KEY_Escape: return "Escape";
    case GDK_KEY_space: return " ";
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete: return "Delete";
    case GDK_KEY_Insert: case GDK_KEY_KP_Insert: return "Insert";
    case GDK_KEY_Home: case GDK_KEY_KP_Home: return "Home";
    case GDK_KEY_End: case GDK_KEY_KP_End: return "End";
    case GDK_KEY_Page_Up: case GDK_KEY_KP_Page_Up: return "PageUp";
    case GDK_KEY_Page_Down: case GDK_KEY_KP_Page_Down: return "PageDown";
    case GDK_KEY_Left: case GDK_KEY_KP_Left: return "ArrowLeft";
    case GDK_KEY_Right: case GDK_KEY_KP_Right: return "ArrowRight";
    case GDK_KEY_Up: case GDK_KEY_KP_Up: return "ArrowUp";
    case GDK_KEY_Down: case GDK_KEY_KP_Down: return "ArrowDown";
    case GDK_KEY_Shift_L: case GDK_KEY_Shift_R: return "Shift";
    case GDK_KEY_Control_L: case GDK_KEY_Control_R: return "Control";
    case GDK_KEY_Alt_L: case GDK_KEY_Alt_R: return "Alt";
    case GDK_KEY_Meta_L: case GDK_KEY_Meta_R:
    case GDK_KEY_Super_L: case GDK_KEY_Super_R: return "Meta";
    case GDK_KEY_Caps_Lock: return "CapsLock";
    case GDK_KEY_Num_Lock: return "NumLock";
    case GDK_KEY_Scroll_Lock: return "ScrollLock";
    case GDK_KEY_F1: return "F1";
    case GDK_KEY_F2: return "F2";
    case GDK_KEY_F3: return "F3";
    case GDK_KEY_F4: return "F4";
    case GDK_KEY_F5: return "F5";
    case GDK_KEY_F6: return "F6";
    case GDK_KEY_F7: return "F7";
    case GDK_KEY_F8: return "F8";
    case GDK_KEY_F9: return "F9";
    case GDK_KEY_F10: return "F10";
    case GDK_KEY_F11: return "F11";
    case GDK_KEY_F12: return "F12";
    case GDK_KEY_Pause: return "Pause";
    default: {
      guint32 uc = gdk_keyval_to_unicode(keyval);
      if (uc > 0 && g_unichar_isprint(uc)) {
        char buf[7];
        int len = g_unichar_to_utf8(uc, buf);
        buf[len] = '\0';
        return std::string(buf);
      }
      return "Unidentified";
    }
  }
}

std::string GdkKeycodeToCode(guint16 hardware_keycode) {
  switch (hardware_keycode) {
    case 9: return "Escape";
    case 10: return "Digit1";
    case 11: return "Digit2";
    case 12: return "Digit3";
    case 13: return "Digit4";
    case 14: return "Digit5";
    case 15: return "Digit6";
    case 16: return "Digit7";
    case 17: return "Digit8";
    case 18: return "Digit9";
    case 19: return "Digit0";
    case 20: return "Minus";
    case 21: return "Equal";
    case 22: return "Backspace";
    case 23: return "Tab";
    case 24: return "KeyQ";
    case 25: return "KeyW";
    case 26: return "KeyE";
    case 27: return "KeyR";
    case 28: return "KeyT";
    case 29: return "KeyY";
    case 30: return "KeyU";
    case 31: return "KeyI";
    case 32: return "KeyO";
    case 33: return "KeyP";
    case 34: return "BracketLeft";
    case 35: return "BracketRight";
    case 36: return "Enter";
    case 37: return "ControlLeft";
    case 38: return "KeyA";
    case 39: return "KeyS";
    case 40: return "KeyD";
    case 41: return "KeyF";
    case 42: return "KeyG";
    case 43: return "KeyH";
    case 44: return "KeyJ";
    case 45: return "KeyK";
    case 46: return "KeyL";
    case 47: return "Semicolon";
    case 48: return "Quote";
    case 49: return "Backquote";
    case 50: return "ShiftLeft";
    case 51: return "Backslash";
    case 52: return "KeyZ";
    case 53: return "KeyX";
    case 54: return "KeyC";
    case 55: return "KeyV";
    case 56: return "KeyB";
    case 57: return "KeyN";
    case 58: return "KeyM";
    case 59: return "Comma";
    case 60: return "Period";
    case 61: return "Slash";
    case 62: return "ShiftRight";
    case 64: return "AltLeft";
    case 65: return "Space";
    case 66: return "CapsLock";
    case 67: return "F1";
    case 68: return "F2";
    case 69: return "F3";
    case 70: return "F4";
    case 71: return "F5";
    case 72: return "F6";
    case 73: return "F7";
    case 74: return "F8";
    case 75: return "F9";
    case 76: return "F10";
    case 95: return "F11";
    case 96: return "F12";
    case 105: return "ControlRight";
    case 108: return "AltRight";
    case 110: return "Home";
    case 111: return "ArrowUp";
    case 112: return "PageUp";
    case 113: return "ArrowLeft";
    case 114: return "ArrowRight";
    case 115: return "End";
    case 116: return "ArrowDown";
    case 117: return "PageDown";
    case 118: return "Insert";
    case 119: return "Delete";
    case 133: return "MetaLeft";
    case 134: return "MetaRight";
    default: return "Unidentified";
  }
}

uint32_t GdkModifiersToWef(guint state) {
  uint32_t modifiers = 0;
  if (state & GDK_SHIFT_MASK) modifiers |= WEF_MOD_SHIFT;
  if (state & GDK_CONTROL_MASK) modifiers |= WEF_MOD_CONTROL;
  if (state & GDK_MOD1_MASK) modifiers |= WEF_MOD_ALT;
  if (state & GDK_MOD4_MASK) modifiers |= WEF_MOD_META;
  return modifiers;
}

} // namespace keyboard

// GtkWidget → wef_id mapping for event routing
static std::map<GtkWidget*, uint32_t> g_widget_to_wef_id;
static std::mutex g_widget_mutex;

static uint32_t WefIdForWidget(GtkWidget* widget) {
  if (!widget) return 0;
  // Walk up to find the toplevel window
  GtkWidget* toplevel = gtk_widget_get_toplevel(widget);
  std::lock_guard<std::mutex> lock(g_widget_mutex);
  auto it = g_widget_to_wef_id.find(toplevel);
  return it != g_widget_to_wef_id.end() ? it->second : 0;
}

static void RegisterWidget(GtkWidget* widget, uint32_t window_id) {
  std::lock_guard<std::mutex> lock(g_widget_mutex);
  g_widget_to_wef_id[widget] = window_id;
}

static void UnregisterWidget(GtkWidget* widget) {
  std::lock_guard<std::mutex> lock(g_widget_mutex);
  g_widget_to_wef_id.erase(widget);
}

// Per-window state
struct LinuxWindowState {
  uint32_t window_id;
  GtkWidget* window;
  WebKitWebView* webview;
  WebKitUserContentManager* content_manager;
};

// Track the click_count from press events for use in the corresponding release.
static int32_t g_last_click_count = 1;

static gboolean on_button_event(GtkWidget* widget, GdkEventButton* event, gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0) return FALSE;

  int state;
  int32_t click_count;

  switch (event->type) {
    case GDK_BUTTON_PRESS:
      state = WEF_MOUSE_PRESSED;
      click_count = 1;
      break;
    case GDK_2BUTTON_PRESS:
      state = WEF_MOUSE_PRESSED;
      click_count = 2;
      break;
    case GDK_3BUTTON_PRESS:
      state = WEF_MOUSE_PRESSED;
      click_count = 2;
      break;
    case GDK_BUTTON_RELEASE:
      state = WEF_MOUSE_RELEASED;
      click_count = g_last_click_count;
      break;
    default:
      return FALSE;
  }

  if (state == WEF_MOUSE_PRESSED) {
    g_last_click_count = click_count;
  }

  int button;
  switch (event->button) {
    case 1: button = WEF_MOUSE_BUTTON_LEFT; break;
    case 2: button = WEF_MOUSE_BUTTON_MIDDLE; break;
    case 3: button = WEF_MOUSE_BUTTON_RIGHT; break;
    case 8: button = WEF_MOUSE_BUTTON_BACK; break;
    case 9: button = WEF_MOUSE_BUTTON_FORWARD; break;
    default: button = static_cast<int>(event->button); break;
  }
  uint32_t modifiers = keyboard::GdkModifiersToWef(event->state);

  RuntimeLoader::GetInstance()->DispatchMouseClickEvent(
      wid, state, button, event->x, event->y, modifiers, click_count);

  return FALSE;
}

static gboolean on_motion_event(GtkWidget* widget, GdkEventMotion* event, gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0) return FALSE;
  uint32_t modifiers = keyboard::GdkModifiersToWef(event->state);
  RuntimeLoader::GetInstance()->DispatchMouseMoveEvent(wid, event->x, event->y, modifiers);
  return FALSE;
}

static gboolean on_scroll_event(GtkWidget* widget, GdkEventScroll* event, gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0) return FALSE;

  double delta_x = 0, delta_y = 0;
  int32_t delta_mode = WEF_WHEEL_DELTA_LINE;

  switch (event->direction) {
    case GDK_SCROLL_UP:    delta_y = -1.0; break;
    case GDK_SCROLL_DOWN:  delta_y = 1.0; break;
    case GDK_SCROLL_LEFT:  delta_x = -1.0; break;
    case GDK_SCROLL_RIGHT: delta_x = 1.0; break;
    case GDK_SCROLL_SMOOTH:
      gdk_event_get_scroll_deltas((GdkEvent*)event, &delta_x, &delta_y);
      delta_mode = WEF_WHEEL_DELTA_PIXEL;
      break;
  }

  uint32_t modifiers = keyboard::GdkModifiersToWef(event->state);
  RuntimeLoader::GetInstance()->DispatchWheelEvent(
      wid, delta_x, delta_y, event->x, event->y, modifiers, delta_mode);
  return FALSE;
}

static gboolean on_enter_notify_event(GtkWidget* widget, GdkEventCrossing* event, gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0) return FALSE;
  uint32_t modifiers = keyboard::GdkModifiersToWef(event->state);
  RuntimeLoader::GetInstance()->DispatchCursorEnterLeaveEvent(wid, 1, event->x, event->y, modifiers);
  return FALSE;
}

static gboolean on_leave_notify_event(GtkWidget* widget, GdkEventCrossing* event, gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0) return FALSE;
  uint32_t modifiers = keyboard::GdkModifiersToWef(event->state);
  RuntimeLoader::GetInstance()->DispatchCursorEnterLeaveEvent(wid, 0, event->x, event->y, modifiers);
  return FALSE;
}

static gboolean on_focus_in_event(GtkWidget* widget, GdkEventFocus* event, gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0) return FALSE;
  RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 1);
  return FALSE;
}

static gboolean on_focus_out_event(GtkWidget* widget, GdkEventFocus* event, gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0) return FALSE;
  RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 0);
  return FALSE;
}

static gboolean on_configure_event(GtkWidget* widget, GdkEventConfigure* event, gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0) return FALSE;
  RuntimeLoader::GetInstance()->DispatchResizeEvent(wid, event->width, event->height);
  RuntimeLoader::GetInstance()->DispatchMoveEvent(wid, event->x, event->y);
  return FALSE;
}

static gboolean on_key_event(GtkWidget* widget, GdkEventKey* event, gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid == 0) return FALSE;

  int state = (event->type == GDK_KEY_PRESS) ? WEF_KEY_PRESSED : WEF_KEY_RELEASED;
  std::string key = keyboard::GdkKeyvalToKey(event->keyval);
  std::string code = keyboard::GdkKeycodeToCode(event->hardware_keycode);
  uint32_t modifiers = keyboard::GdkModifiersToWef(event->state);

  RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
      wid, state, key.c_str(), code.c_str(), modifiers, false);

  return FALSE;
}

// ============================================================================
// WebKitGTK Backend
// ============================================================================

class WebKitGTKBackend;

// Forward declaration for message handler
static void on_script_message(WebKitUserContentManager* manager,
                              WebKitJavascriptResult* js_result,
                              gpointer user_data);

class WebKitGTKBackend : public WefBackend {
 public:
  WebKitGTKBackend();
  ~WebKitGTKBackend() override;

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

  void HandleJsMessage(uint32_t window_id, const char* json);

 private:
  LinuxWindowState* GetWindow(uint32_t window_id);

  std::map<uint32_t, LinuxWindowState> windows_;
  std::mutex windows_mutex_;
};

// Static instance pointer for GTK callbacks
static WebKitGTKBackend* g_gtk_backend = nullptr;

// GtkWidget → window_id mapping for script message routing
static std::map<WebKitUserContentManager*, uint32_t> g_content_manager_to_wef_id;

static const char* g_init_script = R"JS(
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

          window.webkit.messageHandlers.wef.postMessage(JSON.stringify({
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
)JS";

static void on_script_message(WebKitUserContentManager* manager,
                              WebKitJavascriptResult* js_result,
                              gpointer user_data) {
  auto it = g_content_manager_to_wef_id.find(manager);
  uint32_t wid = (it != g_content_manager_to_wef_id.end()) ? it->second : 0;

  JSCValue* value = webkit_javascript_result_get_js_value(js_result);
  if (jsc_value_is_string(value)) {
    gchar* str = jsc_value_to_string(value);
    if (g_gtk_backend) {
      g_gtk_backend->HandleJsMessage(wid, str);
    }
    g_free(str);
  }
}

static void on_window_destroy(GtkWidget* widget, gpointer user_data) {
  uint32_t wid = WefIdForWidget(widget);
  if (wid > 0) {
    RuntimeLoader::GetInstance()->DispatchCloseRequestedEvent(wid);
    UnregisterWidget(widget);
  }
  // If no more windows, quit
  {
    std::lock_guard<std::mutex> lock(g_widget_mutex);
    if (g_widget_to_wef_id.empty()) {
      gtk_main_quit();
    }
  }
}

WebKitGTKBackend::WebKitGTKBackend() {
  g_gtk_backend = this;
}

WebKitGTKBackend::~WebKitGTKBackend() {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  for (auto& [wid, state] : windows_) {
    webkit_user_content_manager_unregister_script_message_handler(state.content_manager, "wef");
    g_content_manager_to_wef_id.erase(state.content_manager);
    UnregisterWidget(state.window);
  }
  windows_.clear();
  g_gtk_backend = nullptr;
}

LinuxWindowState* WebKitGTKBackend::GetWindow(uint32_t window_id) {
  auto it = windows_.find(window_id);
  return it != windows_.end() ? &it->second : nullptr;
}

void WebKitGTKBackend::CreateWindow(uint32_t window_id, int width, int height) {
  GtkWidget* window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size(GTK_WINDOW(window), width, height);
  g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), nullptr);
  g_signal_connect(window, "key-press-event", G_CALLBACK(on_key_event), nullptr);
  g_signal_connect(window, "key-release-event", G_CALLBACK(on_key_event), nullptr);
  g_signal_connect(window, "button-press-event", G_CALLBACK(on_button_event), nullptr);
  g_signal_connect(window, "button-release-event", G_CALLBACK(on_button_event), nullptr);
  gtk_widget_add_events(window, GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK |
                                GDK_SMOOTH_SCROLL_MASK | GDK_ENTER_NOTIFY_MASK |
                                GDK_LEAVE_NOTIFY_MASK);
  g_signal_connect(window, "motion-notify-event", G_CALLBACK(on_motion_event), nullptr);
  g_signal_connect(window, "scroll-event", G_CALLBACK(on_scroll_event), nullptr);
  g_signal_connect(window, "enter-notify-event", G_CALLBACK(on_enter_notify_event), nullptr);
  g_signal_connect(window, "leave-notify-event", G_CALLBACK(on_leave_notify_event), nullptr);
  g_signal_connect(window, "focus-in-event", G_CALLBACK(on_focus_in_event), nullptr);
  g_signal_connect(window, "focus-out-event", G_CALLBACK(on_focus_out_event), nullptr);
  g_signal_connect(window, "configure-event", G_CALLBACK(on_configure_event), nullptr);

  RegisterWidget(window, window_id);

  WebKitUserContentManager* content_manager = webkit_user_content_manager_new();
  g_signal_connect(content_manager, "script-message-received::wef",
                   G_CALLBACK(on_script_message), nullptr);
  webkit_user_content_manager_register_script_message_handler(content_manager, "wef");
  g_content_manager_to_wef_id[content_manager] = window_id;

  WebKitWebView* webview = WEBKIT_WEB_VIEW(
      webkit_web_view_new_with_user_content_manager(content_manager));

  WebKitUserScript* script = webkit_user_script_new(
      g_init_script,
      WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
      nullptr, nullptr);
  webkit_user_content_manager_add_script(content_manager, script);
  webkit_user_script_unref(script);

  gtk_container_add(GTK_CONTAINER(window), GTK_WIDGET(webview));

  LinuxWindowState state;
  state.window_id = window_id;
  state.window = window;
  state.webview = webview;
  state.content_manager = content_manager;

  {
    std::lock_guard<std::mutex> lock(windows_mutex_);
    windows_[window_id] = state;
  }

  gtk_widget_show_all(window);
}

void WebKitGTKBackend::CloseWindow(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    webkit_user_content_manager_unregister_script_message_handler(state->content_manager, "wef");
    g_content_manager_to_wef_id.erase(state->content_manager);
    UnregisterWidget(state->window);
    gtk_widget_destroy(state->window);
    windows_.erase(window_id);
  }
}

void WebKitGTKBackend::Navigate(uint32_t window_id, const std::string& url) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    webkit_web_view_load_uri(state->webview, url.c_str());
  }
}

void WebKitGTKBackend::SetTitle(uint32_t window_id, const std::string& title) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_window_set_title(GTK_WINDOW(state->window), title.c_str());
  }
}

void WebKitGTKBackend::ExecuteJs(uint32_t window_id, const std::string& script) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    webkit_web_view_run_javascript(state->webview, script.c_str(),
                                    nullptr, nullptr, nullptr);
  }
}

void WebKitGTKBackend::Quit() {
  g_idle_add([](gpointer) -> gboolean {
    gtk_main_quit();
    return G_SOURCE_REMOVE;
  }, nullptr);
}

void WebKitGTKBackend::SetWindowSize(uint32_t window_id, int width, int height) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_window_resize(GTK_WINDOW(state->window), width, height);
  }
}

void WebKitGTKBackend::GetWindowSize(uint32_t window_id, int* width, int* height) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    int w = 0, h = 0;
    gtk_window_get_size(GTK_WINDOW(state->window), &w, &h);
    if (width) *width = w;
    if (height) *height = h;
  }
}

void WebKitGTKBackend::SetWindowPosition(uint32_t window_id, int x, int y) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_window_move(GTK_WINDOW(state->window), x, y);
  }
}

void WebKitGTKBackend::GetWindowPosition(uint32_t window_id, int* x, int* y) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    int wx = 0, wy = 0;
    gtk_window_get_position(GTK_WINDOW(state->window), &wx, &wy);
    if (x) *x = wx;
    if (y) *y = wy;
  }
}

void WebKitGTKBackend::SetResizable(uint32_t window_id, bool resizable) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_window_set_resizable(GTK_WINDOW(state->window), resizable);
  }
}

bool WebKitGTKBackend::IsResizable(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? gtk_window_get_resizable(GTK_WINDOW(state->window)) != FALSE : false;
}

void WebKitGTKBackend::SetAlwaysOnTop(uint32_t window_id, bool always_on_top) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_window_set_keep_above(GTK_WINDOW(state->window), always_on_top);
  }
}

bool WebKitGTKBackend::IsAlwaysOnTop(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    GdkWindow* gdk_window = gtk_widget_get_window(state->window);
    if (gdk_window) {
      GdkWindowState wstate = gdk_window_get_state(gdk_window);
      return (wstate & GDK_WINDOW_STATE_ABOVE) != 0;
    }
  }
  return false;
}

bool WebKitGTKBackend::IsVisible(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  return state ? gtk_widget_get_visible(state->window) != FALSE : false;
}

void WebKitGTKBackend::Show(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_widget_show_all(state->window);
  }
}

void WebKitGTKBackend::Hide(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_widget_hide(state->window);
  }
}

void WebKitGTKBackend::Focus(uint32_t window_id) {
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    gtk_widget_show(state->window);
    gtk_window_present(GTK_WINDOW(state->window));
  }
}

void WebKitGTKBackend::PostUiTask(void (*task)(void*), void* data) {
  struct TaskData { void (*task)(void*); void* data; };
  auto* td = new TaskData{task, data};
  g_idle_add([](gpointer data) -> gboolean {
    auto* td = static_cast<TaskData*>(data);
    td->task(td->data);
    delete td;
    return G_SOURCE_REMOVE;
  }, td);
}

void WebKitGTKBackend::InvokeJsCallback(uint32_t window_id, uint64_t callback_id, wef::ValuePtr args) {
  std::string argsJson = json::Serialize(args);
  std::string script = "window.__wefInvokeCallback(" +
                       std::to_string(callback_id) + ", " + argsJson + ");";
  std::lock_guard<std::mutex> lock(windows_mutex_);
  if (window_id == 0) {
    for (auto& [wid, state] : windows_) {
      webkit_web_view_run_javascript(state.webview, script.c_str(),
                                      nullptr, nullptr, nullptr);
    }
  } else {
    auto* state = GetWindow(window_id);
    if (state) {
      webkit_web_view_run_javascript(state->webview, script.c_str(),
                                      nullptr, nullptr, nullptr);
    }
  }
}

void WebKitGTKBackend::ReleaseJsCallback(uint32_t window_id, uint64_t callback_id) {
  std::string script = "window.__wefReleaseCallback(" +
                       std::to_string(callback_id) + ");";
  std::lock_guard<std::mutex> lock(windows_mutex_);
  if (window_id == 0) {
    for (auto& [wid, state] : windows_) {
      webkit_web_view_run_javascript(state.webview, script.c_str(),
                                      nullptr, nullptr, nullptr);
    }
  } else {
    auto* state = GetWindow(window_id);
    if (state) {
      webkit_web_view_run_javascript(state->webview, script.c_str(),
                                      nullptr, nullptr, nullptr);
    }
  }
}

void WebKitGTKBackend::RespondToJsCall(uint32_t window_id, uint64_t call_id,
                                        wef::ValuePtr result, wef::ValuePtr error) {
  std::string resultJson = json::Serialize(result);
  std::string errorJson = (error && !error->IsNull()) ? json::Serialize(error) : "null";
  std::string script;
  if (errorJson == "null") {
    script = "window.__wefRespond(" + std::to_string(call_id) + ", " +
             resultJson + ", null);";
  } else {
    script = "window.__wefRespond(" + std::to_string(call_id) + ", null, " +
             errorJson + ");";
  }
  std::lock_guard<std::mutex> lock(windows_mutex_);
  auto* state = GetWindow(window_id);
  if (state) {
    webkit_web_view_run_javascript(state->webview, script.c_str(),
                                    nullptr, nullptr, nullptr);
  }
}

void WebKitGTKBackend::Run() {
  gtk_main();
}

void WebKitGTKBackend::HandleJsMessage(uint32_t window_id, const char* jsonStr) {
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
  return new WebKitGTKBackend();
}
