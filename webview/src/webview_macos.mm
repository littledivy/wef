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
  void PostUiTask(void (*task)(void*), void* data) override;

  void InvokeJsCallback(uint64_t callback_id, wef::ValuePtr args) override;
  void ReleaseJsCallback(uint64_t callback_id) override;
  void RespondToJsCall(uint64_t call_id, wef::ValuePtr result, const char* error) override;

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
  }
}

WKWebViewBackend::~WKWebViewBackend() {
  @autoreleasepool {
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

void WKWebViewBackend::RespondToJsCall(uint64_t call_id, wef::ValuePtr result, const char* error) {
  std::string resultJson = json::Serialize(result);
  std::string errorStr = error ? error : "";
  dispatch_async(dispatch_get_main_queue(), ^{
    @autoreleasepool {
      NSString* script;
      if (errorStr.empty()) {
        script = [NSString stringWithFormat:
            @"window.__wefRespond(%llu, %s, null);",
            call_id,
            resultJson.c_str()];
      } else {
        script = [NSString stringWithFormat:
            @"window.__wefRespond(%llu, null, \"%s\");",
            call_id,
            json::Escape(errorStr).c_str()];
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
