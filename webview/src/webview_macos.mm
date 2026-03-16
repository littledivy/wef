// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include "runtime_loader.h"
#include "webview_value.h"

#include <atomic>
#include <map>
#include <mutex>
#include <sstream>

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

std::string Serialize(const wef::ValuePtr& value);

std::string SerializeList(const wef::ValueList& list) {
  std::ostringstream ss;
  ss << "[";
  for (size_t i = 0; i < list.size(); ++i) {
    if (i > 0) ss << ",";
    ss << Serialize(list[i]);
  }
  ss << "]";
  return ss.str();
}

std::string SerializeDict(const wef::ValueDict& dict) {
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

std::string Serialize(const wef::ValuePtr& value) {
  if (!value) return "null";
  switch (value->type) {
    case wef::ValueType::Null:
      return "null";
    case wef::ValueType::Bool:
      return value->GetBool() ? "true" : "false";
    case wef::ValueType::Int:
      return std::to_string(value->GetInt());
    case wef::ValueType::Double: {
      char buf[64];
      snprintf(buf, sizeof(buf), "%.17g", value->GetDouble());
      return buf;
    }
    case wef::ValueType::String:
      return "\"" + Escape(value->GetString()) + "\"";
    case wef::ValueType::Binary: {
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
    case wef::ValueType::List:
      return SerializeList(value->GetList());
    case wef::ValueType::Dict:
      return SerializeDict(value->GetDict());
    case wef::ValueType::Callback:
      return "{\"__callback__\":\"" + std::to_string(value->GetCallbackId()) + "\"}";
  }
  return "null";
}

wef::ValuePtr Parse(const char*& p);

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

wef::ValuePtr ParseArray(const char*& p) {
  auto list = wef::Value::List();
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

wef::ValuePtr ParseObject(const char*& p) {
  auto dict = wef::Value::Dict();
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
    return wef::Value::Callback(id);
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
    return wef::Value::Binary(data.data(), data.size());
  }

  return dict;
}

wef::ValuePtr Parse(const char*& p) {
  SkipWhitespace(p);
  if (!*p) return wef::Value::Null();

  if (*p == 'n' && strncmp(p, "null", 4) == 0) {
    p += 4;
    return wef::Value::Null();
  }
  if (*p == 't' && strncmp(p, "true", 4) == 0) {
    p += 4;
    return wef::Value::Bool(true);
  }
  if (*p == 'f' && strncmp(p, "false", 5) == 0) {
    p += 5;
    return wef::Value::Bool(false);
  }
  if (*p == '"') {
    return wef::Value::String(ParseString(p));
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
      return wef::Value::Int((int)d);
    }
    return wef::Value::Double(d);
  }

  return wef::Value::Null();
}

wef::ValuePtr ParseJson(const std::string& json) {
  const char* p = json.c_str();
  return Parse(p);
}

}

@class WefScriptMessageHandler;
@class WefWindowDelegate;

class WKWebViewBackend : public WefBackend {
 public:
  WKWebViewBackend(int width, int height, const std::string& title);
  ~WKWebViewBackend() override;

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

  void InvokeJsCallback(uint64_t callback_id, wef::ValuePtr args) override;
  void ReleaseJsCallback(uint64_t callback_id) override;
  void RespondToJsCall(uint64_t call_id, wef::ValuePtr result, wef::ValuePtr error) override;

  void Run() override;

  void HandleJsMessage(uint64_t call_id, const std::string& method, wef::ValuePtr args);

 private:
  NSWindow* window_;
  WKWebView* webview_;
  WefScriptMessageHandler* message_handler_;
  WefWindowDelegate* window_delegate_;
  id keyboard_monitor_;

  std::atomic<uint64_t> next_callback_id_{1};
  std::map<uint64_t, bool> stored_callbacks_;
  std::mutex callbacks_mutex_;
};

@interface WefScriptMessageHandler : NSObject <WKScriptMessageHandler>
@property (nonatomic, assign) WKWebViewBackend* backend;
@end

@implementation WefScriptMessageHandler

- (void)userContentController:(WKUserContentController *)userContentController
      didReceiveScriptMessage:(WKScriptMessage *)message {
  if (![message.name isEqualToString:@"wef"]) return;

  if (![message.body isKindOfClass:[NSDictionary class]]) return;

  NSDictionary* body = (NSDictionary*)message.body;
  NSNumber* callIdNum = body[@"callId"];
  NSString* method = body[@"method"];
  id argsJson = body[@"args"];

  if (!callIdNum || !method) return;

  uint64_t call_id = [callIdNum unsignedLongLongValue];
  std::string methodStr = [method UTF8String];

  wef::ValuePtr args = wef::Value::List();
  if ([argsJson isKindOfClass:[NSArray class]]) {
    NSArray* argsArray = (NSArray*)argsJson;
    NSError* error = nil;
    NSData* jsonData = [NSJSONSerialization dataWithJSONObject:argsArray options:0 error:&error];
    if (jsonData) {
      NSString* jsonStr = [[NSString alloc] initWithData:jsonData encoding:NSUTF8StringEncoding];
      args = json::ParseJson([jsonStr UTF8String]);
    }
  }

  if (self.backend) {
    self.backend->HandleJsMessage(call_id, methodStr, args);
  }
}

@end

@interface WefWindowDelegate : NSObject <NSWindowDelegate>
@property (nonatomic, assign) WKWebViewBackend* backend;
@end

@implementation WefWindowDelegate

- (BOOL)windowShouldClose:(NSWindow *)sender {
  [NSApp stop:nil];
  NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                      location:NSMakePoint(0, 0)
                                 modifierFlags:0
                                     timestamp:0
                                  windowNumber:0
                                       context:nil
                                       subtype:0
                                         data1:0
                                         data2:0];
  [NSApp postEvent:event atStart:YES];
  return YES;
}

@end

namespace {

std::string NSEventKeyToString(NSEvent* event) {
  NSString* chars = [event characters];
  if (chars && [chars length] > 0) {
    unichar c = [chars characterAtIndex:0];
    // Map special characters to W3C key values
    switch (c) {
      case NSUpArrowFunctionKey: return "ArrowUp";
      case NSDownArrowFunctionKey: return "ArrowDown";
      case NSLeftArrowFunctionKey: return "ArrowLeft";
      case NSRightArrowFunctionKey: return "ArrowRight";
      case NSHomeFunctionKey: return "Home";
      case NSEndFunctionKey: return "End";
      case NSPageUpFunctionKey: return "PageUp";
      case NSPageDownFunctionKey: return "PageDown";
      case NSDeleteFunctionKey: return "Delete";
      case NSInsertFunctionKey: return "Insert";
      case NSF1FunctionKey: return "F1";
      case NSF2FunctionKey: return "F2";
      case NSF3FunctionKey: return "F3";
      case NSF4FunctionKey: return "F4";
      case NSF5FunctionKey: return "F5";
      case NSF6FunctionKey: return "F6";
      case NSF7FunctionKey: return "F7";
      case NSF8FunctionKey: return "F8";
      case NSF9FunctionKey: return "F9";
      case NSF10FunctionKey: return "F10";
      case NSF11FunctionKey: return "F11";
      case NSF12FunctionKey: return "F12";
      case 27: return "Escape";
      case 13: case 3: return "Enter";
      case 9: return "Tab";
      case 127: return "Backspace";
      case 32: return " ";
      default:
        if (c >= 0x20 && c < 0x7F) {
          return std::string(1, static_cast<char>(c));
        }
        return [chars UTF8String] ?: "Unidentified";
    }
  }
  return "Unidentified";
}

std::string NSEventKeyCodeToCode(unsigned short keyCode) {
  switch (keyCode) {
    case 0: return "KeyA";
    case 1: return "KeyS";
    case 2: return "KeyD";
    case 3: return "KeyF";
    case 4: return "KeyH";
    case 5: return "KeyG";
    case 6: return "KeyZ";
    case 7: return "KeyX";
    case 8: return "KeyC";
    case 9: return "KeyV";
    case 11: return "KeyB";
    case 12: return "KeyQ";
    case 13: return "KeyW";
    case 14: return "KeyE";
    case 15: return "KeyR";
    case 16: return "KeyY";
    case 17: return "KeyT";
    case 18: return "Digit1";
    case 19: return "Digit2";
    case 20: return "Digit3";
    case 21: return "Digit4";
    case 22: return "Digit6";
    case 23: return "Digit5";
    case 24: return "Equal";
    case 25: return "Digit9";
    case 26: return "Digit7";
    case 27: return "Minus";
    case 28: return "Digit8";
    case 29: return "Digit0";
    case 30: return "BracketRight";
    case 31: return "KeyO";
    case 32: return "KeyU";
    case 33: return "BracketLeft";
    case 34: return "KeyI";
    case 35: return "KeyP";
    case 36: return "Enter";
    case 37: return "KeyL";
    case 38: return "KeyJ";
    case 39: return "Quote";
    case 40: return "KeyK";
    case 41: return "Semicolon";
    case 42: return "Backslash";
    case 43: return "Comma";
    case 44: return "Slash";
    case 45: return "KeyN";
    case 46: return "KeyM";
    case 47: return "Period";
    case 48: return "Tab";
    case 49: return "Space";
    case 50: return "Backquote";
    case 51: return "Backspace";
    case 53: return "Escape";
    case 55: return "MetaLeft";
    case 56: return "ShiftLeft";
    case 57: return "CapsLock";
    case 58: return "AltLeft";
    case 59: return "ControlLeft";
    case 60: return "ShiftRight";
    case 61: return "AltRight";
    case 62: return "ControlRight";
    case 96: return "F5";
    case 97: return "F6";
    case 98: return "F7";
    case 99: return "F3";
    case 100: return "F8";
    case 101: return "F9";
    case 109: return "F10";
    case 103: return "F11";
    case 111: return "F12";
    case 118: return "F4";
    case 120: return "F2";
    case 122: return "F1";
    case 123: return "ArrowLeft";
    case 124: return "ArrowRight";
    case 125: return "ArrowDown";
    case 126: return "ArrowUp";
    case 117: return "Delete";
    case 114: return "Insert";
    case 115: return "Home";
    case 119: return "End";
    case 116: return "PageUp";
    case 121: return "PageDown";
    default: return "Unidentified";
  }
}

uint32_t NSModifierFlagsToWef(NSEventModifierFlags flags) {
  uint32_t modifiers = 0;
  if (flags & NSEventModifierFlagShift) modifiers |= WEF_MOD_SHIFT;
  if (flags & NSEventModifierFlagControl) modifiers |= WEF_MOD_CONTROL;
  if (flags & NSEventModifierFlagOption) modifiers |= WEF_MOD_ALT;
  if (flags & NSEventModifierFlagCommand) modifiers |= WEF_MOD_META;
  return modifiers;
}

} // namespace

WKWebViewBackend::WKWebViewBackend(int width, int height, const std::string& title) {
  @autoreleasepool {
    NSRect frame = NSMakeRect(0, 0, width, height);
    NSWindowStyleMask style = NSWindowStyleMaskTitled |
                              NSWindowStyleMaskClosable |
                              NSWindowStyleMaskMiniaturizable |
                              NSWindowStyleMaskResizable;
    window_ = [[NSWindow alloc] initWithContentRect:frame
                                          styleMask:style
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
    [window_ setTitle:[NSString stringWithUTF8String:title.c_str()]];
    [window_ center];

    window_delegate_ = [[WefWindowDelegate alloc] init];
    window_delegate_.backend = this;
    [window_ setDelegate:window_delegate_];

    message_handler_ = [[WefScriptMessageHandler alloc] init];
    message_handler_.backend = this;

    WKWebViewConfiguration* config = [[WKWebViewConfiguration alloc] init];
    [config.userContentController addScriptMessageHandler:message_handler_ name:@"wef"];

    NSString* initScript = @R"JS(
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

          window.webkit.messageHandlers.wef.postMessage({
            callId: callId,
            method: path.join('.'),
            args: processedArgs
          });
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

    WKUserScript* script = [[WKUserScript alloc]
        initWithSource:initScript
        injectionTime:WKUserScriptInjectionTimeAtDocumentStart
        forMainFrameOnly:YES];
    [config.userContentController addUserScript:script];

    webview_ = [[WKWebView alloc] initWithFrame:frame configuration:config];
    [window_ setContentView:webview_];

    [window_ makeKeyAndOrderFront:nil];

    keyboard_monitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:
        (NSEventMaskKeyDown | NSEventMaskKeyUp)
        handler:^NSEvent*(NSEvent* event) {
          int state = ([event type] == NSEventTypeKeyDown)
              ? WEF_KEY_PRESSED : WEF_KEY_RELEASED;
          std::string key = NSEventKeyToString(event);
          std::string code = NSEventKeyCodeToCode([event keyCode]);
          uint32_t modifiers = NSModifierFlagsToWef([event modifierFlags]);
          bool repeat = [event isARepeat];

          RuntimeLoader::GetInstance()->DispatchKeyboardEvent(
              state, key.c_str(), code.c_str(), modifiers, repeat);

          return event; // Don't consume the event
        }];
  }
}

WKWebViewBackend::~WKWebViewBackend() {
  @autoreleasepool {
    if (keyboard_monitor_) {
      [NSEvent removeMonitor:keyboard_monitor_];
      keyboard_monitor_ = nil;
    }
    if (webview_) {
      [webview_.configuration.userContentController removeScriptMessageHandlerForName:@"wef"];
    }
  }
}

void WKWebViewBackend::Navigate(const std::string& url) {
  std::string urlCopy = url;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      if (urlCopy.find("data:text/html,") == 0) {
        NSString* html = [NSString stringWithUTF8String:urlCopy.c_str() + 15];
        html = [html stringByRemovingPercentEncoding];
        [webview_ loadHTMLString:html baseURL:nil];
        return;
      }

      NSURL* nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:urlCopy.c_str()]];
      if (nsurl && nsurl.scheme && nsurl.scheme.length > 0) {
        NSURLRequest* request = [NSURLRequest requestWithURL:nsurl];
        [webview_ loadRequest:request];
      } else {
        NSString* path = [NSString stringWithUTF8String:urlCopy.c_str()];
        NSURL* fileURL = [NSURL fileURLWithPath:path];
        if (fileURL) {
          [webview_ loadFileURL:fileURL allowingReadAccessToURL:fileURL];
        }
      }
    }
  });
}

void WKWebViewBackend::SetTitle(const std::string& title) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      [window_ setTitle:[NSString stringWithUTF8String:title.c_str()]];
    }
  });
}

void WKWebViewBackend::ExecuteJs(const std::string& script) {
  std::string scriptCopy = script;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      [webview_ evaluateJavaScript:[NSString stringWithUTF8String:scriptCopy.c_str()]
                 completionHandler:nil];
    }
  });
}

void WKWebViewBackend::Quit() {
  dispatch_async(dispatch_get_main_queue(), ^{
    [NSApp stop:nil];
      NSEvent* event = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                        location:NSMakePoint(0, 0)
                                   modifierFlags:0
                                       timestamp:0
                                    windowNumber:0
                                         context:nil
                                         subtype:0
                                           data1:0
                                           data2:0];
    [NSApp postEvent:event atStart:YES];
  });
}

void WKWebViewBackend::SetWindowSize(int width, int height) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSRect frame = [window_ frame];
      frame.size = NSMakeSize(width, height);
      [window_ setFrame:frame display:YES];
    }
  });
}

void WKWebViewBackend::GetWindowSize(int* width, int* height) {
  NSRect frame = [window_ frame];
  if (width) *width = static_cast<int>(frame.size.width);
  if (height) *height = static_cast<int>(frame.size.height);
}

void WKWebViewBackend::SetWindowPosition(int x, int y) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSRect frame = [window_ frame];
      NSRect screenFrame = [[window_ screen] frame];
      // Convert from top-left origin to macOS bottom-left origin
      CGFloat flippedY = screenFrame.size.height - y - frame.size.height;
      [window_ setFrameOrigin:NSMakePoint(x, flippedY)];
    }
  });
}

void WKWebViewBackend::GetWindowPosition(int* x, int* y) {
  NSRect frame = [window_ frame];
  NSRect screenFrame = [[window_ screen] frame];
  if (x) *x = static_cast<int>(frame.origin.x);
  // Convert from macOS bottom-left origin to top-left origin
  if (y) *y = static_cast<int>(screenFrame.size.height - frame.origin.y - frame.size.height);
}

void WKWebViewBackend::SetResizable(bool resizable) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSWindowStyleMask mask = [window_ styleMask];
      if (resizable) {
        mask |= NSWindowStyleMaskResizable;
      } else {
        mask &= ~NSWindowStyleMaskResizable;
      }
      [window_ setStyleMask:mask];
    }
  });
}

bool WKWebViewBackend::IsResizable() {
  return ([window_ styleMask] & NSWindowStyleMaskResizable) != 0;
}

void WKWebViewBackend::SetAlwaysOnTop(bool always_on_top) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      [window_ setLevel:always_on_top ? NSFloatingWindowLevel : NSNormalWindowLevel];
    }
  });
}

bool WKWebViewBackend::IsAlwaysOnTop() {
  return [window_ level] >= NSFloatingWindowLevel;
}

bool WKWebViewBackend::IsVisible() {
  return [window_ isVisible];
}

void WKWebViewBackend::Show() {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      [window_ makeKeyAndOrderFront:nil];
    }
  });
}

void WKWebViewBackend::Hide() {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      [window_ orderOut:nil];
    }
  });
}

void WKWebViewBackend::Focus() {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      [NSApp activateIgnoringOtherApps:YES];
      [window_ makeKeyAndOrderFront:nil];
    }
  });
}

void WKWebViewBackend::PostUiTask(void (*task)(void*), void* data) {
  dispatch_async(dispatch_get_main_queue(), ^{
    task(data);
  });
}

void WKWebViewBackend::InvokeJsCallback(uint64_t callback_id, wef::ValuePtr args) {
  std::string argsJson = json::Serialize(args);
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSString* script = [NSString stringWithFormat:
          @"window.__wefInvokeCallback(%llu, %s);",
          callback_id,
          argsJson.c_str()];
      [webview_ evaluateJavaScript:script completionHandler:nil];
    }
  });
}

void WKWebViewBackend::ReleaseJsCallback(uint64_t callback_id) {
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSString* script = [NSString stringWithFormat:
          @"window.__wefReleaseCallback(%llu);",
          callback_id];
      [webview_ evaluateJavaScript:script completionHandler:nil];
    }
  });
}

void WKWebViewBackend::RespondToJsCall(uint64_t call_id, wef::ValuePtr result, wef::ValuePtr error) {
  std::string resultJson = json::Serialize(result);
  std::string errorJson = error ? json::Serialize(error) : "null";
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSString* script;
      if (error) {
        script = [NSString stringWithFormat:
            @"window.__wefRespond(%llu, null, %s);",
            call_id,
            errorJson.c_str()];
      } else {
        script = [NSString stringWithFormat:
            @"window.__wefRespond(%llu, %s, null);",
            call_id,
            resultJson.c_str()];
      }
      [webview_ evaluateJavaScript:script completionHandler:nil];
    }
  });
}

void WKWebViewBackend::Run() {
  @autoreleasepool {
    [window_ makeKeyAndOrderFront:nil];
    [NSApp run];
  }
}

void WKWebViewBackend::HandleJsMessage(uint64_t call_id, const std::string& method,
                                        wef::ValuePtr args) {
  RuntimeLoader::GetInstance()->OnJsCall(call_id, method, args);
}

WefBackend* CreateWefBackend(int width, int height, const std::string& title) {
  return new WKWebViewBackend(width, height, title);
}
