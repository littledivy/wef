// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"
#include "webview_value.h"

#include <gtk/gtk.h>
#include <gdk/gdkkeysyms.h>
#include <webkit2/webkit2.h>
#include <JavaScriptCore/JavaScript.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>

// JSON serialization (same as macOS - could be shared)
namespace json {

std::string Escape(const std::string& s) {
  std::string result;
  for (char c : s) {
    switch (c) {
      case '"': result += "\\\""; break;
      case '\\': result += "\\\\"; break;
      case '\b': result += "\\b"; break;
      case '\f': result += "\\f"; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          snprintf(buf, sizeof(buf), "\\u%04x", c);
          result += buf;
        } else {
          result += c;
        }
    }
  }
  return result;
}

std::string Serialize(const webview::ValuePtr& value);

std::string SerializeList(const webview::ValueList& list) {
  std::ostringstream ss;
  ss << "[";
  for (size_t i = 0; i < list.size(); ++i) {
    if (i > 0) ss << ",";
    ss << Serialize(list[i]);
  }
  ss << "]";
  return ss.str();
}

std::string SerializeDict(const webview::ValueDict& dict) {
  std::ostringstream ss;
  ss << "{";
  bool first = true;
  for (const auto& pair : dict) {
    if (!first) ss << ",";
    first = false;
    ss << "\"" << Escape(pair.first) << "\":" << Serialize(pair.second);
  }
  ss << "}";
  return ss.str();
}

std::string Serialize(const webview::ValuePtr& value) {
  if (!value) return "null";
  switch (value->type) {
    case webview::ValueType::Null:
      return "null";
    case webview::ValueType::Bool:
      return value->GetBool() ? "true" : "false";
    case webview::ValueType::Int:
      return std::to_string(value->GetInt());
    case webview::ValueType::Double: {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.17g", value->GetDouble());
      return buf;
    }
    case webview::ValueType::String:
      return "\"" + Escape(value->GetString()) + "\"";
    case webview::ValueType::Binary: {
      const auto& binary = value->GetBinary();
      std::string base64;
      static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      size_t i = 0;
      const uint8_t* data = binary.data.data();
      size_t len = binary.data.size();
      while (i < len) {
        uint32_t n = (data[i] << 16);
        if (i + 1 < len) n |= (data[i + 1] << 8);
        if (i + 2 < len) n |= data[i + 2];
        base64 += chars[(n >> 18) & 0x3F];
        base64 += chars[(n >> 12) & 0x3F];
        base64 += (i + 1 < len) ? chars[(n >> 6) & 0x3F] : '=';
        base64 += (i + 2 < len) ? chars[n & 0x3F] : '=';
        i += 3;
      }
      return "{\"__binary__\":\"" + base64 + "\"}";
    }
    case webview::ValueType::List:
      return SerializeList(value->GetList());
    case webview::ValueType::Dict:
      return SerializeDict(value->GetDict());
    case webview::ValueType::Callback:
      return "{\"__callback__\":\"" + std::to_string(value->GetCallbackId()) + "\"}";
  }
  return "null";
}

// Parse JSON to Value
webview::ValuePtr Parse(const char*& p);

void SkipWhitespace(const char*& p) {
  while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
}

std::string ParseString(const char*& p) {
  std::string result;
  ++p;
  while (*p && *p != '"') {
    if (*p == '\\' && *(p + 1)) {
      ++p;
      switch (*p) {
        case '"': result += '"'; break;
        case '\\': result += '\\'; break;
        case 'b': result += '\b'; break;
        case 'f': result += '\f'; break;
        case 'n': result += '\n'; break;
        case 'r': result += '\r'; break;
        case 't': result += '\t'; break;
        case 'u': {
          if (p[1] && p[2] && p[3] && p[4]) {
            char hex[5] = {p[1], p[2], p[3], p[4], 0};
            int codepoint = (int)strtol(hex, nullptr, 16);
            if (codepoint < 0x80) {
              result += (char)codepoint;
            } else if (codepoint < 0x800) {
              result += (char)(0xC0 | (codepoint >> 6));
              result += (char)(0x80 | (codepoint & 0x3F));
            } else {
              result += (char)(0xE0 | (codepoint >> 12));
              result += (char)(0x80 | ((codepoint >> 6) & 0x3F));
              result += (char)(0x80 | (codepoint & 0x3F));
            }
            p += 4;
          }
          break;
        }
        default: result += *p;
      }
    } else {
      result += *p;
    }
    ++p;
  }
  if (*p == '"') ++p;
  return result;
}

webview::ValuePtr ParseArray(const char*& p) {
  auto list = webview::Value::List();
  ++p;
  SkipWhitespace(p);
  while (*p && *p != ']') {
    list->GetList().push_back(Parse(p));
    SkipWhitespace(p);
    if (*p == ',') ++p;
    SkipWhitespace(p);
  }
  if (*p == ']') ++p;
  return list;
}

webview::ValuePtr ParseObject(const char*& p) {
  auto dict = webview::Value::Dict();
  ++p;
  SkipWhitespace(p);
  while (*p && *p != '}') {
    SkipWhitespace(p);
    if (*p != '"') break;
    std::string key = ParseString(p);
    SkipWhitespace(p);
    if (*p == ':') ++p;
    SkipWhitespace(p);
    dict->GetDict()[key] = Parse(p);
    SkipWhitespace(p);
    if (*p == ',') ++p;
    SkipWhitespace(p);
  }
  if (*p == '}') ++p;

  const auto& d = dict->GetDict();
  auto it = d.find("__callback__");
  if (it != d.end() && it->second->IsString()) {
    uint64_t id = std::stoull(it->second->GetString());
    return webview::Value::Callback(id);
  }
  it = d.find("__binary__");
  if (it != d.end() && it->second->IsString()) {
    const std::string& base64 = it->second->GetString();
    std::vector<uint8_t> data;
    static const int decode[256] = {
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
      52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
      -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
      15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
      -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
      41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
      -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    int val = 0, bits = -8;
    for (char c : base64) {
      if (c == '=') break;
      int d = decode[(unsigned char)c];
      if (d < 0) continue;
      val = (val << 6) | d;
      bits += 6;
      if (bits >= 0) {
        data.push_back((val >> bits) & 0xFF);
        bits -= 8;
      }
    }
    return webview::Value::Binary(data.data(), data.size());
  }

  return dict;
}

webview::ValuePtr Parse(const char*& p) {
  SkipWhitespace(p);
  if (!*p) return webview::Value::Null();

  if (*p == 'n' && strncmp(p, "null", 4) == 0) {
    p += 4;
    return webview::Value::Null();
  }
  if (*p == 't' && strncmp(p, "true", 4) == 0) {
    p += 4;
    return webview::Value::Bool(true);
  }
  if (*p == 'f' && strncmp(p, "false", 5) == 0) {
    p += 5;
    return webview::Value::Bool(false);
  }
  if (*p == '"') {
    return webview::Value::String(ParseString(p));
  }
  if (*p == '[') {
    return ParseArray(p);
  }
  if (*p == '{') {
    return ParseObject(p);
  }
  if (*p == '-' || (*p >= '0' && *p <= '9')) {
    char* end;
    double d = strtod(p, &end);
    bool isInt = true;
    for (const char* c = p; c < end; ++c) {
      if (*c == '.' || *c == 'e' || *c == 'E') {
        isInt = false;
        break;
      }
    }
    p = end;
    if (isInt && d >= INT_MIN && d <= INT_MAX) {
      return webview::Value::Int((int)d);
    }
    return webview::Value::Double(d);
  }

  return webview::Value::Null();
}

webview::ValuePtr ParseJson(const std::string& json) {
  const char* p = json.c_str();
  return Parse(p);
}

} // namespace json

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
  // Linux evdev keycodes (hardware_keycode - 8 = evdev code)
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

static gboolean on_key_event(GtkWidget* widget, GdkEventKey* event, gpointer user_data) {
  int state = (event->type == GDK_KEY_PRESS) ? WEF_KEY_PRESSED : WEF_KEY_RELEASED;
  std::string key = keyboard::GdkKeyvalToKey(event->keyval);
  std::string code = keyboard::GdkKeycodeToCode(event->hardware_keycode);
  uint32_t modifiers = keyboard::GdkModifiersToWef(event->state);

  RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
      state, key.c_str(), code.c_str(), modifiers, false);

  return FALSE; // Don't consume the event
}

// ============================================================================
// WebKitGTK Backend
// ============================================================================

class WebKitGTKBackend : public WebviewBackend {
 public:
  WebKitGTKBackend(int width, int height, const std::string& title);
  ~WebKitGTKBackend() override;

  void Navigate(const std::string& url) override;
  void SetTitle(const std::string& title) override;
  void ExecuteJs(const std::string& script) override;
  void Quit() override;
  void SetWindowSize(int width, int height) override;
  void GetWindowSize(int* width, int* height) override;
  void SetWindowPosition(int x, int y) override;
  void GetWindowPosition(int* x, int* y) override;
  void SetResizable(bool resizable) override;
  bool IsResizable() override;
  void SetAlwaysOnTop(bool always_on_top) override;
  bool IsAlwaysOnTop() override;
  bool IsVisible() override;
  void Show() override;
  void Hide() override;
  void Focus() override;
  void PostUiTask(void (*task)(void*), void* data) override;

  void InvokeJsCallback(uint64_t callback_id, webview::ValuePtr args) override;
  void ReleaseJsCallback(uint64_t callback_id) override;
  void RespondToJsCall(uint64_t call_id, webview::ValuePtr result, webview::ValuePtr error) override;

  void Run() override;

  void HandleJsMessage(const char* json);

 private:
  GtkWidget* window_;
  WebKitWebView* webview_;
  WebKitUserContentManager* content_manager_;

  std::atomic<uint64_t> next_callback_id_{1};
  std::map<uint64_t, bool> stored_callbacks_;
  std::mutex callbacks_mutex_;
};

// Callback for script messages from JavaScript
static void on_script_message(WebKitUserContentManager* manager,
                              WebKitJavascriptResult* js_result,
                              gpointer user_data) {
  WebKitGTKBackend* backend = static_cast<WebKitGTKBackend*>(user_data);

  JSCValue* value = webkit_javascript_result_get_js_value(js_result);
  if (jsc_value_is_string(value)) {
    gchar* str = jsc_value_to_string(value);
    backend->HandleJsMessage(str);
    g_free(str);
  }
}

// Callback for window destroy
static void on_window_destroy(GtkWidget* widget, gpointer user_data) {
  gtk_main_quit();
}

WebKitGTKBackend::WebKitGTKBackend(int width, int height, const std::string& title) {
  // Create window
  window_ = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window_), title.c_str());
  gtk_window_set_default_size(GTK_WINDOW(window_), width, height);
  g_signal_connect(window_, "destroy", G_CALLBACK(on_window_destroy), this);
  g_signal_connect(window_, "key-press-event", G_CALLBACK(on_key_event), this);
  g_signal_connect(window_, "key-release-event", G_CALLBACK(on_key_event), this);

  // Create user content manager for message handling
  content_manager_ = webkit_user_content_manager_new();
  g_signal_connect(content_manager_, "script-message-received::wef",
                   G_CALLBACK(on_script_message), this);
  webkit_user_content_manager_register_script_message_handler(content_manager_, "wef");

  // Create webview with content manager
  webview_ = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(content_manager_));

  // Inject initialization script
  const char* initScript = R"JS(
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

  WebKitUserScript* script = webkit_user_script_new(
      initScript,
      WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
      nullptr, nullptr);
  webkit_user_content_manager_add_script(content_manager_, script);
  webkit_user_script_unref(script);

  // Add webview to window
  gtk_container_add(GTK_CONTAINER(window_), GTK_WIDGET(webview_));
}

WebKitGTKBackend::~WebKitGTKBackend() {
  webkit_user_content_manager_unregister_script_message_handler(content_manager_, "wef");
}

void WebKitGTKBackend::Navigate(const std::string& url) {
  std::string urlCopy = url;
  g_idle_add([](gpointer data) -> gboolean {
    auto* pair = static_cast<std::pair<WebKitGTKBackend*, std::string>*>(data);
    webkit_web_view_load_uri(pair->first->webview_, pair->second.c_str());
    delete pair;
    return G_SOURCE_REMOVE;
  }, new std::pair<WebKitGTKBackend*, std::string>(this, urlCopy));
}

void WebKitGTKBackend::SetTitle(const std::string& title) {
  std::string titleCopy = title;
  g_idle_add([](gpointer data) -> gboolean {
    auto* pair = static_cast<std::pair<WebKitGTKBackend*, std::string>*>(data);
    gtk_window_set_title(GTK_WINDOW(pair->first->window_), pair->second.c_str());
    delete pair;
    return G_SOURCE_REMOVE;
  }, new std::pair<WebKitGTKBackend*, std::string>(this, titleCopy));
}

void WebKitGTKBackend::ExecuteJs(const std::string& script) {
  std::string scriptCopy = script;
  g_idle_add([](gpointer data) -> gboolean {
    auto* pair = static_cast<std::pair<WebKitGTKBackend*, std::string>*>(data);
    webkit_web_view_run_javascript(pair->first->webview_, pair->second.c_str(),
                                    nullptr, nullptr, nullptr);
    delete pair;
    return G_SOURCE_REMOVE;
  }, new std::pair<WebKitGTKBackend*, std::string>(this, scriptCopy));
}

void WebKitGTKBackend::Quit() {
  g_idle_add([](gpointer data) -> gboolean {
    gtk_main_quit();
    return G_SOURCE_REMOVE;
  }, nullptr);
}

void WebKitGTKBackend::SetWindowSize(int width, int height) {
  struct SizeData { WebKitGTKBackend* backend; int w; int h; };
  auto* sizeData = new SizeData{this, width, height};
  g_idle_add([](gpointer data) -> gboolean {
    auto* sd = static_cast<SizeData*>(data);
    gtk_window_resize(GTK_WINDOW(sd->backend->window_), sd->w, sd->h);
    delete sd;
    return G_SOURCE_REMOVE;
  }, sizeData);
}

void WebKitGTKBackend::GetWindowSize(int* width, int* height) {
  int w = 0, h = 0;
  gtk_window_get_size(GTK_WINDOW(window_), &w, &h);
  if (width) *width = w;
  if (height) *height = h;
}

void WebKitGTKBackend::SetWindowPosition(int x, int y) {
  struct PosData { WebKitGTKBackend* backend; int x; int y; };
  auto* posData = new PosData{this, x, y};
  g_idle_add([](gpointer data) -> gboolean {
    auto* pd = static_cast<PosData*>(data);
    gtk_window_move(GTK_WINDOW(pd->backend->window_), pd->x, pd->y);
    delete pd;
    return G_SOURCE_REMOVE;
  }, posData);
}

void WebKitGTKBackend::GetWindowPosition(int* x, int* y) {
  int wx = 0, wy = 0;
  gtk_window_get_position(GTK_WINDOW(window_), &wx, &wy);
  if (x) *x = wx;
  if (y) *y = wy;
}

void WebKitGTKBackend::SetResizable(bool resizable) {
  struct ResData { WebKitGTKBackend* backend; bool resizable; };
  auto* resData = new ResData{this, resizable};
  g_idle_add([](gpointer data) -> gboolean {
    auto* rd = static_cast<ResData*>(data);
    gtk_window_set_resizable(GTK_WINDOW(rd->backend->window_), rd->resizable);
    delete rd;
    return G_SOURCE_REMOVE;
  }, resData);
}

bool WebKitGTKBackend::IsResizable() {
  return gtk_window_get_resizable(GTK_WINDOW(window_)) != FALSE;
}

void WebKitGTKBackend::SetAlwaysOnTop(bool always_on_top) {
  struct TopData { WebKitGTKBackend* backend; bool on_top; };
  auto* topData = new TopData{this, always_on_top};
  g_idle_add([](gpointer data) -> gboolean {
    auto* td = static_cast<TopData*>(data);
    gtk_window_set_keep_above(GTK_WINDOW(td->backend->window_), td->on_top);
    delete td;
    return G_SOURCE_REMOVE;
  }, topData);
}

bool WebKitGTKBackend::IsAlwaysOnTop() {
  // GTK does not provide a direct getter for keep-above state;
  // GDK window state must be checked.
  GdkWindow* gdk_window = gtk_widget_get_window(window_);
  if (gdk_window) {
    GdkWindowState state = gdk_window_get_state(gdk_window);
    return (state & GDK_WINDOW_STATE_ABOVE) != 0;
  }
  return false;
}

bool WebKitGTKBackend::IsVisible() {
  return gtk_widget_get_visible(window_) != FALSE;
}

void WebKitGTKBackend::Show() {
  g_idle_add([](gpointer data) -> gboolean {
    gtk_widget_show(static_cast<GtkWidget*>(data));
    return G_SOURCE_REMOVE;
  }, window_);
}

void WebKitGTKBackend::Hide() {
  g_idle_add([](gpointer data) -> gboolean {
    gtk_widget_hide(static_cast<GtkWidget*>(data));
    return G_SOURCE_REMOVE;
  }, window_);
}

void WebKitGTKBackend::Focus() {
  struct FocusData { GtkWidget* window; };
  auto* fd = new FocusData{window_};
  g_idle_add([](gpointer data) -> gboolean {
    auto* fd = static_cast<FocusData*>(data);
    gtk_widget_show(fd->window);
    gtk_window_present(GTK_WINDOW(fd->window));
    delete fd;
    return G_SOURCE_REMOVE;
  }, fd);
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

void WebKitGTKBackend::InvokeJsCallback(uint64_t callback_id, webview::ValuePtr args) {
  std::string argsJson = json::Serialize(args);
  struct CbData { WebKitGTKBackend* backend; uint64_t id; std::string args; };
  auto* cbData = new CbData{this, callback_id, argsJson};
  g_idle_add([](gpointer data) -> gboolean {
    auto* cbd = static_cast<CbData*>(data);
    std::string script = "window.__wefInvokeCallback(" +
                         std::to_string(cbd->id) + ", " + cbd->args + ");";
    webkit_web_view_run_javascript(cbd->backend->webview_, script.c_str(),
                                    nullptr, nullptr, nullptr);
    delete cbd;
    return G_SOURCE_REMOVE;
  }, cbData);
}

void WebKitGTKBackend::ReleaseJsCallback(uint64_t callback_id) {
  struct CbData { WebKitGTKBackend* backend; uint64_t id; };
  auto* cbData = new CbData{this, callback_id};
  g_idle_add([](gpointer data) -> gboolean {
    auto* cbd = static_cast<CbData*>(data);
    std::string script = "window.__wefReleaseCallback(" +
                         std::to_string(cbd->id) + ");";
    webkit_web_view_run_javascript(cbd->backend->webview_, script.c_str(),
                                    nullptr, nullptr, nullptr);
    delete cbd;
    return G_SOURCE_REMOVE;
  }, cbData);
}

void WebKitGTKBackend::RespondToJsCall(uint64_t call_id, webview::ValuePtr result, webview::ValuePtr error) {
  std::string resultJson = json::Serialize(result);
  std::string errorJson = error ? json::Serialize(error) : "null";
  struct RespData { WebKitGTKBackend* backend; uint64_t id; std::string result; std::string error; };
  auto* respData = new RespData{this, call_id, resultJson, errorJson};
  g_idle_add([](gpointer data) -> gboolean {
    auto* rd = static_cast<RespData*>(data);
    std::string script;
    if (rd->error == "null") {
      script = "window.__wefRespond(" + std::to_string(rd->id) + ", " +
               rd->result + ", null);";
    } else {
      script = "window.__wefRespond(" + std::to_string(rd->id) + ", null, " +
               rd->error + ");";
    }
    webkit_web_view_run_javascript(rd->backend->webview_, script.c_str(),
                                    nullptr, nullptr, nullptr);
    delete rd;
    return G_SOURCE_REMOVE;
  }, respData);
}

void WebKitGTKBackend::Run() {
  gtk_widget_show_all(window_);
  gtk_main();
}

void WebKitGTKBackend::HandleJsMessage(const char* jsonStr) {
  webview::ValuePtr msg = json::ParseJson(jsonStr);
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
  webview::ValuePtr args = (argsIt != dict.end()) ? argsIt->second : webview::Value::List();

  RuntimeLoader::GetInstance()->OnJsCall(call_id, method, args);
}

// ============================================================================
// Factory Function
// ============================================================================

WebviewBackend* CreateWebviewBackend(int width, int height, const std::string& title) {
  return new WebKitGTKBackend(width, height, title);
}
