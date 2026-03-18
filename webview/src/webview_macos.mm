// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#import <Cocoa/Cocoa.h>
#import <WebKit/WebKit.h>

#include "runtime_loader.h"
#include "wef_json.h"

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

  void SetApplicationMenu(wef_value_t* menu_template,
                          const wef_backend_api_t* api,
                          wef_menu_click_fn on_click,
                          void* on_click_data) override;

  void HandleJsMessage(uint64_t call_id, const std::string& method, wef::ValuePtr args);

 private:
  NSWindow* window_;
  WKWebView* webview_;
  WefScriptMessageHandler* message_handler_;
  WefWindowDelegate* window_delegate_;
  id keyboard_monitor_;
  id mouse_monitor_;
  id mouse_move_monitor_;
  id scroll_monitor_;

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

int NSButtonToWef(NSInteger buttonNumber) {
  switch (buttonNumber) {
    case 0: return WEF_MOUSE_BUTTON_LEFT;
    case 1: return WEF_MOUSE_BUTTON_RIGHT;
    case 2: return WEF_MOUSE_BUTTON_MIDDLE;
    case 3: return WEF_MOUSE_BUTTON_BACK;
    case 4: return WEF_MOUSE_BUTTON_FORWARD;
    default: return WEF_MOUSE_BUTTON_LEFT;
  }
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

    mouse_monitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:
        (NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp |
         NSEventMaskRightMouseDown | NSEventMaskRightMouseUp |
         NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp)
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

          NSPoint loc = [event locationInWindow];
          NSWindow* win = [event window];
          double x = loc.x;
          double y = 0;
          if (win) {
            y = [win contentLayoutRect].size.height - loc.y;
          }

          RuntimeLoader::GetInstance()->DispatchMouseClickEvent(
              state, button, x, y, modifiers, click_count);

          return event; // Don't consume the event
        }];

    mouse_move_monitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:
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

    scroll_monitor_ = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
        handler:^NSEvent*(NSEvent* event) {
          double delta_x = [event scrollingDeltaX];
          double delta_y = [event scrollingDeltaY];
          uint32_t modifiers = NSModifierFlagsToWef([event modifierFlags]);

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
}

WKWebViewBackend::~WKWebViewBackend() {
  @autoreleasepool {
    if (keyboard_monitor_) {
      [NSEvent removeMonitor:keyboard_monitor_];
      keyboard_monitor_ = nil;
    }
    if (mouse_monitor_) {
      [NSEvent removeMonitor:mouse_monitor_];
      mouse_monitor_ = nil;
    }
    if (mouse_move_monitor_) {
      [NSEvent removeMonitor:mouse_move_monitor_];
      mouse_move_monitor_ = nil;
    }
    if (scroll_monitor_) {
      [NSEvent removeMonitor:scroll_monitor_];
      scroll_monitor_ = nil;
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
  std::string titleCopy = title;
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      [window_ setTitle:[NSString stringWithUTF8String:titleCopy.c_str()]];
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

// --- Application Menu ---

static wef_menu_click_fn g_webview_menu_click_fn = nullptr;
static void* g_webview_menu_click_data = nullptr;

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
  if (itemId && g_webview_menu_click_fn) {
    g_webview_menu_click_fn(g_webview_menu_click_data, [itemId UTF8String]);
  }
}
@end

static void ParseAccelerator(const std::string& accel, NSString** outKey,
                             NSEventModifierFlags* outMask) {
  *outKey = @"";
  *outMask = 0;

  std::string lower = accel;
  for (auto& c : lower) c = tolower(c);

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
      *outKey = [NSString stringWithUTF8String:part.c_str()];
    }
  }
}

static NSMenuItem* CreateRoleMenuItem(const std::string& role) {
  NSString* title = @"";
  SEL action = nil;
  NSString* keyEquiv = @"";
  NSEventModifierFlags mask = NSEventModifierFlagCommand;

  if (role == "quit") {
    title = @"Quit"; action = @selector(terminate:); keyEquiv = @"q";
  } else if (role == "copy") {
    title = @"Copy"; action = @selector(copy:); keyEquiv = @"c";
  } else if (role == "paste") {
    title = @"Paste"; action = @selector(paste:); keyEquiv = @"v";
  } else if (role == "cut") {
    title = @"Cut"; action = @selector(cut:); keyEquiv = @"x";
  } else if (role == "selectall" || role == "selectAll") {
    title = @"Select All"; action = @selector(selectAll:); keyEquiv = @"a";
  } else if (role == "undo") {
    title = @"Undo"; action = @selector(undo:); keyEquiv = @"z";
  } else if (role == "redo") {
    title = @"Redo"; action = @selector(redo:); keyEquiv = @"Z";
    mask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
  } else if (role == "minimize") {
    title = @"Minimize"; action = @selector(performMiniaturize:); keyEquiv = @"m";
  } else if (role == "zoom") {
    title = @"Zoom"; action = @selector(performZoom:);
  } else if (role == "close") {
    title = @"Close"; action = @selector(performClose:); keyEquiv = @"w";
  } else if (role == "about") {
    title = @"About"; action = @selector(orderFrontStandardAboutPanel:);
  } else if (role == "hide") {
    title = @"Hide"; action = @selector(hide:); keyEquiv = @"h";
  } else if (role == "hideothers" || role == "hideOthers") {
    title = @"Hide Others"; action = @selector(hideOtherApplications:);
    keyEquiv = @"h"; mask = NSEventModifierFlagCommand | NSEventModifierFlagOption;
  } else if (role == "unhide") {
    title = @"Show All"; action = @selector(unhideAllApplications:);
  } else if (role == "front") {
    title = @"Bring All to Front"; action = @selector(arrangeInFront:);
  } else if (role == "togglefullscreen" || role == "toggleFullScreen") {
    title = @"Toggle Full Screen"; action = @selector(toggleFullScreen:);
    keyEquiv = @"f"; mask = NSEventModifierFlagCommand | NSEventModifierFlagControl;
  } else {
    return nil;
  }

  NSMenuItem* item = [[NSMenuItem alloc]
      initWithTitle:title action:action keyEquivalent:keyEquiv];
  [item setKeyEquivalentModifierMask:mask];
  return item;
}

static NSMenu* BuildMenuFromValue(wef_value_t* val, const wef_backend_api_t* api) {
  if (!val || !api->value_is_list(val)) return nil;

  NSMenu* menu = [[NSMenu alloc] init];
  [menu setAutoenablesItems:NO];

  size_t count = api->value_list_size(val);
  for (size_t i = 0; i < count; ++i) {
    wef_value_t* itemVal = api->value_list_get(val, i);
    if (!itemVal || !api->value_is_dict(itemVal)) continue;

    // Separator
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

    // Role
    wef_value_t* roleVal = api->value_dict_get(itemVal, "role");
    if (roleVal && api->value_is_string(roleVal)) {
      size_t len = 0;
      char* roleStr = api->value_get_string(roleVal, &len);
      if (roleStr) {
        NSMenuItem* roleItem = CreateRoleMenuItem(roleStr);
        if (roleItem) [menu addItem:roleItem];
        api->value_free_string(roleStr);
        continue;
      }
    }

    // Label
    wef_value_t* labelVal = api->value_dict_get(itemVal, "label");
    if (!labelVal || !api->value_is_string(labelVal)) continue;
    size_t labelLen = 0;
    char* labelStr = api->value_get_string(labelVal, &labelLen);
    if (!labelStr) continue;
    NSString* label = [NSString stringWithUTF8String:labelStr];
    api->value_free_string(labelStr);

    // Submenu
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
        initWithTitle:label action:@selector(menuItemClicked:) keyEquivalent:keyEquiv];
    [nsItem setKeyEquivalentModifierMask:modMask];
    [nsItem setTarget:[WefMenuTarget shared]];

    wef_value_t* idVal = api->value_dict_get(itemVal, "id");
    if (idVal && api->value_is_string(idVal)) {
      size_t idLen = 0;
      char* idStr = api->value_get_string(idVal, &idLen);
      if (idStr) {
        [nsItem setRepresentedObject:[NSString stringWithUTF8String:idStr]];
        api->value_free_string(idStr);
      }
    }

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

void WKWebViewBackend::SetApplicationMenu(wef_value_t* menu_template,
                                           const wef_backend_api_t* api,
                                           wef_menu_click_fn on_click,
                                           void* on_click_data) {
  g_webview_menu_click_fn = on_click;
  g_webview_menu_click_data = on_click_data;

  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menubar = BuildMenuFromValue(menu_template, api);
    if (menubar) {
      [NSApp setMainMenu:menubar];
    }
  });
}

WefBackend* CreateWefBackend(int width, int height, const std::string& title) {
  return new WKWebViewBackend(width, height, title);
}
