// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
// macOS-specific RuntimeLoader parts: NSEvent monitors, NSMenu, NSWindow helpers.

#include "runtime_loader.h"

#include <string>
#include <vector>

#import <Cocoa/Cocoa.h>

// --- NSWindow Helpers (called from app.cc to avoid Obj-C in cross-platform code) ---

void RegisterNSWindowForCefHandle(void* cef_handle, uint32_t window_id) {
  NSView* view = (__bridge NSView*)cef_handle;
  NSWindow* nswindow = [view window];
  if (nswindow) {
    RuntimeLoader::GetInstance()->RegisterNSWindow((__bridge void*)nswindow, window_id);
  }
}

void UnregisterNSWindowForCefHandle(void* cef_handle) {
  NSView* view = (__bridge NSView*)cef_handle;
  NSWindow* nswindow = [view window];
  if (nswindow) {
    RuntimeLoader::GetInstance()->UnregisterNSWindow((__bridge void*)nswindow);
  }
}

// --- Native Mouse Monitor (macOS) ---

static id g_mouse_monitor = nil;
static id g_mouse_move_monitor = nil;
static id g_scroll_monitor = nil;
static id g_focus_observer = nil;
static id g_blur_observer = nil;
static id g_resize_observer = nil;
static id g_move_observer = nil;

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

static uint32_t WefIdForNSWindow(NSWindow* win) {
  if (!win) return 0;
  return RuntimeLoader::GetInstance()->GetWefIdForNSWindow((__bridge void*)win);
}

// Per-window menu storage (must be declared before focus observer uses them)
static std::map<uint32_t, NSMenu*> g_window_menus;
static std::mutex g_window_menus_mutex;

void InstallNativeMouseMonitor() {
  if (g_mouse_monitor) return;

  NSEventMask mask = NSEventMaskLeftMouseDown | NSEventMaskLeftMouseUp
                   | NSEventMaskRightMouseDown | NSEventMaskRightMouseUp
                   | NSEventMaskOtherMouseDown | NSEventMaskOtherMouseUp;

  g_mouse_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:mask
      handler:^NSEvent*(NSEvent* event) {
        NSWindow* win = [event window];
        uint32_t wid = WefIdForNSWindow(win);
        if (wid == 0) return event;

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
        double x = loc.x;
        double y = 0;
        if (win) {
          y = [win contentLayoutRect].size.height - loc.y;
        }

        RuntimeLoader::GetInstance()->DispatchMouseClickEvent(
            wid, state, button, x, y, modifiers, click_count);

        return event;
      }];

  g_mouse_move_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:
      (NSEventMaskMouseMoved | NSEventMaskLeftMouseDragged |
       NSEventMaskRightMouseDragged | NSEventMaskOtherMouseDragged)
      handler:^NSEvent*(NSEvent* event) {
        NSWindow* win = [event window];
        uint32_t wid = WefIdForNSWindow(win);
        if (wid == 0) return event;

        uint32_t modifiers = NSModifierFlagsToWef([event modifierFlags]);
        NSPoint loc = [event locationInWindow];
        double x = loc.x;
        double y = 0;
        if (win) {
          y = [win contentLayoutRect].size.height - loc.y;
        }

        RuntimeLoader::GetInstance()->DispatchMouseMoveEvent(wid, x, y, modifiers);
        return event;
      }];

  g_scroll_monitor = [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskScrollWheel
      handler:^NSEvent*(NSEvent* event) {
        NSWindow* win = [event window];
        uint32_t wid = WefIdForNSWindow(win);
        if (wid == 0) return event;

        double delta_x = [event scrollingDeltaX];
        double delta_y = [event scrollingDeltaY];
        uint32_t modifiers = NSModifierFlagsToWef([event modifierFlags]);

        int32_t delta_mode = [event hasPreciseScrollingDeltas]
            ? WEF_WHEEL_DELTA_PIXEL : WEF_WHEEL_DELTA_LINE;

        NSPoint loc = [event locationInWindow];
        double x = loc.x;
        double y = 0;
        if (win) {
          y = [win contentLayoutRect].size.height - loc.y;
        }

        RuntimeLoader::GetInstance()->DispatchWheelEvent(
            wid, delta_x, delta_y, x, y, modifiers, delta_mode);
        return event;
      }];

  g_focus_observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:NSWindowDidBecomeKeyNotification
      object:nil
      queue:nil
      usingBlock:^(NSNotification* note) {
        uint32_t wid = WefIdForNSWindow([note object]);
        if (wid > 0) {
          RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 1);
          // Swap to this window's menu
          std::lock_guard<std::mutex> lock(g_window_menus_mutex);
          auto it = g_window_menus.find(wid);
          if (it != g_window_menus.end() && it->second) {
            [NSApp setMainMenu:it->second];
          }
        }
      }];

  g_blur_observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:NSWindowDidResignKeyNotification
      object:nil
      queue:nil
      usingBlock:^(NSNotification* note) {
        uint32_t wid = WefIdForNSWindow([note object]);
        if (wid > 0) {
          RuntimeLoader::GetInstance()->DispatchFocusedEvent(wid, 0);
        }
      }];

  g_resize_observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:NSWindowDidResizeNotification
      object:nil
      queue:nil
      usingBlock:^(NSNotification* note) {
        NSWindow* win = [note object];
        uint32_t wid = WefIdForNSWindow(win);
        if (wid > 0 && win) {
          NSRect frame = [[win contentView] frame];
          RuntimeLoader::GetInstance()->DispatchResizeEvent(
              wid, (int)frame.size.width, (int)frame.size.height);
        }
      }];

  g_move_observer = [[NSNotificationCenter defaultCenter]
      addObserverForName:NSWindowDidMoveNotification
      object:nil
      queue:nil
      usingBlock:^(NSNotification* note) {
        NSWindow* win = [note object];
        uint32_t wid = WefIdForNSWindow(win);
        if (wid > 0 && win) {
          NSRect frame = [win frame];
          RuntimeLoader::GetInstance()->DispatchMoveEvent(
              wid, (int)frame.origin.x, (int)frame.origin.y);
        }
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
  if (g_focus_observer) {
    [[NSNotificationCenter defaultCenter] removeObserver:g_focus_observer];
    g_focus_observer = nil;
  }
  if (g_blur_observer) {
    [[NSNotificationCenter defaultCenter] removeObserver:g_blur_observer];
    g_blur_observer = nil;
  }
  if (g_resize_observer) {
    [[NSNotificationCenter defaultCenter] removeObserver:g_resize_observer];
    g_resize_observer = nil;
  }
  if (g_move_observer) {
    [[NSNotificationCenter defaultCenter] removeObserver:g_move_observer];
    g_move_observer = nil;
  }
}

// --- Native Dialog (macOS) ---

void ShowNativeDialog_Mac(int dialog_type, const char* title, const char* message,
                          const char* default_value, wef_dialog_result_fn callback,
                          void* callback_data) {
  NSString* nsTitle = title ? [NSString stringWithUTF8String:title] : @"";
  NSString* nsMessage = message ? [NSString stringWithUTF8String:message] : @"";
  NSString* nsDefault = default_value ? [NSString stringWithUTF8String:default_value] : @"";

  dispatch_async(dispatch_get_main_queue(), ^{
    NSAlert* alert = [[NSAlert alloc] init];
    [alert setMessageText:nsTitle];
    [alert setInformativeText:nsMessage];

    NSTextField* inputField = nil;

    if (dialog_type == WEF_DIALOG_ALERT) {
      [alert addButtonWithTitle:@"OK"];
    } else if (dialog_type == WEF_DIALOG_CONFIRM) {
      [alert addButtonWithTitle:@"OK"];
      [alert addButtonWithTitle:@"Cancel"];
    } else if (dialog_type == WEF_DIALOG_PROMPT) {
      [alert addButtonWithTitle:@"OK"];
      [alert addButtonWithTitle:@"Cancel"];
      inputField = [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 300, 24)];
      [inputField setStringValue:nsDefault];
      [alert setAccessoryView:inputField];
      // Make the text field first responder
      [alert layout];
      [[alert window] makeFirstResponder:inputField];
    }

    NSModalResponse response = [alert runModal];
    bool confirmed = (response == NSAlertFirstButtonReturn);

    if (callback) {
      if (dialog_type == WEF_DIALOG_PROMPT && confirmed && inputField) {
        const char* text = [[inputField stringValue] UTF8String];
        callback(callback_data, 1, text);
      } else {
        callback(callback_data, confirmed ? 1 : 0, nullptr);
      }
    }
  });
}

// --- Application Menu (macOS) ---

static wef_menu_click_fn g_menu_click_fn = nullptr;
static void* g_menu_click_data = nullptr;

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
    uint32_t wid = WefIdForNSWindow([NSApp keyWindow]);
    g_menu_click_fn(g_menu_click_data, wid, [itemId UTF8String]);
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

  if (role == "quit") { title = @"Quit"; action = @selector(terminate:); keyEquiv = @"q"; }
  else if (role == "copy") { title = @"Copy"; action = @selector(copy:); keyEquiv = @"c"; }
  else if (role == "paste") { title = @"Paste"; action = @selector(paste:); keyEquiv = @"v"; }
  else if (role == "cut") { title = @"Cut"; action = @selector(cut:); keyEquiv = @"x"; }
  else if (role == "selectall" || role == "selectAll") { title = @"Select All"; action = @selector(selectAll:); keyEquiv = @"a"; }
  else if (role == "undo") { title = @"Undo"; action = @selector(undo:); keyEquiv = @"z"; }
  else if (role == "redo") { title = @"Redo"; action = @selector(redo:); keyEquiv = @"Z"; mask = NSEventModifierFlagCommand | NSEventModifierFlagShift; }
  else if (role == "minimize") { title = @"Minimize"; action = @selector(performMiniaturize:); keyEquiv = @"m"; }
  else if (role == "zoom") { title = @"Zoom"; action = @selector(performZoom:); }
  else if (role == "close") { title = @"Close"; action = @selector(performClose:); keyEquiv = @"w"; }
  else if (role == "about") { title = @"About"; action = @selector(orderFrontStandardAboutPanel:); }
  else if (role == "hide") { title = @"Hide"; action = @selector(hide:); keyEquiv = @"h"; }
  else if (role == "hideothers" || role == "hideOthers") { title = @"Hide Others"; action = @selector(hideOtherApplications:); keyEquiv = @"h"; mask = NSEventModifierFlagCommand | NSEventModifierFlagOption; }
  else if (role == "unhide") { title = @"Show All"; action = @selector(unhideAllApplications:); }
  else if (role == "front") { title = @"Bring All to Front"; action = @selector(arrangeInFront:); }
  else if (role == "togglefullscreen" || role == "toggleFullScreen") { title = @"Toggle Full Screen"; action = @selector(toggleFullScreen:); keyEquiv = @"f"; mask = NSEventModifierFlagCommand | NSEventModifierFlagControl; }
  else { return nil; }

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
    wef_value_t* labelVal = api->value_dict_get(itemVal, "label");
    if (!labelVal || !api->value_is_string(labelVal)) continue;
    size_t labelLen = 0;
    char* labelStr = api->value_get_string(labelVal, &labelLen);
    if (!labelStr) continue;
    NSString* label = [NSString stringWithUTF8String:labelStr];
    api->value_free_string(labelStr);
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

// Exported function called from runtime_loader.cc on macOS
void Backend_ShowContextMenu_Mac(void* data, uint32_t window_id,
                                 int x, int y,
                                 wef_value_t* menu_template,
                                 wef_menu_click_fn on_click,
                                 void* on_click_data) {
  if (!menu_template) return;
  g_menu_click_fn = on_click;
  g_menu_click_data = on_click_data;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();

  CefRefPtr<CefBrowser> browser = loader->GetBrowserForWindow(window_id);
  if (!browser) return;

  void* handle = browser->GetHost()->GetWindowHandle();
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menu = BuildMenuFromValue(menu_template, api);
    if (!menu) return;

    NSView* view = (__bridge NSView*)handle;
    NSWindow* win = [view window];
    if (!win) return;

    NSView* contentView = [win contentView];
    // Convert from top-left origin (wef coordinates) to bottom-left origin (NSView)
    NSPoint loc = NSMakePoint(x, [contentView frame].size.height - y);
    [menu popUpMenuPositioningItem:nil atLocation:loc inView:contentView];
  });
}

void Backend_SetApplicationMenu_Mac(void* data, uint32_t window_id,
                                    wef_value_t* menu_template,
                                    wef_menu_click_fn on_click,
                                    void* on_click_data) {
  if (!menu_template) return;
  g_menu_click_fn = on_click;
  g_menu_click_data = on_click_data;
  RuntimeLoader* loader = static_cast<RuntimeLoader*>(data);
  const wef_backend_api_t* api = &loader->GetBackendApi();
  dispatch_async(dispatch_get_main_queue(), ^{
    NSMenu* menubar = BuildMenuFromValue(menu_template, api);
    if (menubar) {
      // Store per-window
      {
        std::lock_guard<std::mutex> lock(g_window_menus_mutex);
        g_window_menus[window_id] = menubar;
      }
      // If this window is currently key, apply immediately
      uint32_t keyWid = WefIdForNSWindow([NSApp keyWindow]);
      if (keyWid == window_id) {
        [NSApp setMainMenu:menubar];
      }
    }
  });
}
