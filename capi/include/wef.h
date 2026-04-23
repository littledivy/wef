// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef WEF_H
#define WEF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEF_API_VERSION 21

// Window handle types for get_window_handle_type
#define WEF_WINDOW_HANDLE_UNKNOWN 0
#define WEF_WINDOW_HANDLE_APPKIT 1
#define WEF_WINDOW_HANDLE_WIN32 2
#define WEF_WINDOW_HANDLE_X11 3
#define WEF_WINDOW_HANDLE_WAYLAND 4

typedef struct wef_backend_api wef_backend_api_t;

typedef int (*wef_runtime_init_fn)(const wef_backend_api_t* api);
#define WEF_RUNTIME_INIT_SYMBOL "wef_runtime_init"

typedef int (*wef_runtime_start_fn)(void);
#define WEF_RUNTIME_START_SYMBOL "wef_runtime_start"

typedef void (*wef_runtime_shutdown_fn)(void);
#define WEF_RUNTIME_SHUTDOWN_SYMBOL "wef_runtime_shutdown"

typedef struct wef_value wef_value_t;

typedef void (*wef_js_call_fn)(void* user_data, uint32_t window_id,
                               uint64_t call_id, const char* method_path,
                               wef_value_t* args);

// Callback for execute_js results. Pass NULL to execute_js for fire-and-forget.
typedef void (*wef_js_result_fn)(wef_value_t* result, wef_value_t* error,
                                 void* user_data);

typedef void (*wef_menu_click_fn)(void* user_data, uint32_t window_id,
                                  const char* item_id);

// Keyboard event state
#define WEF_KEY_PRESSED 0
#define WEF_KEY_RELEASED 1

// Keyboard modifier flags (bitmask)
#define WEF_MOD_SHIFT (1 << 0)
#define WEF_MOD_CONTROL (1 << 1)
#define WEF_MOD_ALT (1 << 2)
#define WEF_MOD_META (1 << 3)

// Mouse button constants
#define WEF_MOUSE_BUTTON_LEFT 0
#define WEF_MOUSE_BUTTON_RIGHT 1
#define WEF_MOUSE_BUTTON_MIDDLE 2
#define WEF_MOUSE_BUTTON_BACK 3
#define WEF_MOUSE_BUTTON_FORWARD 4

// Mouse event state
#define WEF_MOUSE_PRESSED 0
#define WEF_MOUSE_RELEASED 1

// Dialog types
#define WEF_DIALOG_ALERT 0
#define WEF_DIALOG_CONFIRM 1
#define WEF_DIALOG_PROMPT 2

// Dock / taskbar bounce / flash types. Values match macOS
// NSRequestUserAttentionType so the ABI passes through unchanged.
#define WEF_DOCK_BOUNCE_INFORMATIONAL 10
#define WEF_DOCK_BOUNCE_CRITICAL 0

// Callback fired when the user clicks the dock / taskbar icon for an app that
// has no visible windows (macOS only; Windows/Linux have no equivalent event).
// has_visible_windows is true if any app window is currently on-screen.
typedef void (*wef_dock_reopen_fn)(void* user_data, bool has_visible_windows);

// Callback fired when the user left-clicks a tray / status-bar icon.
// (Right-click is reserved for the tray's menu.)
typedef void (*wef_tray_click_fn)(void* user_data, uint32_t tray_id);

// Callback for dialog results.
typedef void (*wef_dialog_result_fn)(
    void* user_data,
    int confirmed,  // 1 = OK/Yes, 0 = Cancel/No
    const char*
        input_value  // For prompt: user input text. NULL for alert/confirm.
);

// Callback for mouse click events.
typedef void (*wef_mouse_click_fn)(
    void* user_data, uint32_t window_id,
    int state,           // WEF_MOUSE_PRESSED or WEF_MOUSE_RELEASED
    int button,          // WEF_MOUSE_BUTTON_*
    double x,            // x position in window coordinates
    double y,            // y position in window coordinates
    uint32_t modifiers,  // bitmask of WEF_MOD_* flags
    int32_t click_count  // 1 = single, 2 = double click
);

// Callback for mouse move events.
typedef void (*wef_mouse_move_fn)(
    void* user_data, uint32_t window_id,
    double x,           // x position in window coordinates
    double y,           // y position in window coordinates
    uint32_t modifiers  // bitmask of WEF_MOD_* flags
);

// Wheel delta mode
#define WEF_WHEEL_DELTA_PIXEL 0
#define WEF_WHEEL_DELTA_LINE 1
#define WEF_WHEEL_DELTA_PAGE 2

// Callback for wheel (scroll) events.
typedef void (*wef_wheel_fn)(
    void* user_data, uint32_t window_id,
    double delta_x,      // horizontal scroll amount
    double delta_y,      // vertical scroll amount
    double x,            // cursor x position in window coordinates
    double y,            // cursor y position in window coordinates
    uint32_t modifiers,  // bitmask of WEF_MOD_* flags
    int32_t delta_mode   // WEF_WHEEL_DELTA_*
);

// Callback for cursor enter/leave events (mouseenter/mouseleave).
typedef void (*wef_cursor_enter_leave_fn)(
    void* user_data, uint32_t window_id,
    int entered,        // 1 = cursor entered window, 0 = cursor left window
    double x,           // cursor x position in window coordinates
    double y,           // cursor y position in window coordinates
    uint32_t modifiers  // bitmask of WEF_MOD_* flags
);

// Callback for window move events.
typedef void (*wef_move_fn)(void* user_data, uint32_t window_id,
                            int x,  // new x position
                            int y   // new y position
);

// Callback for window resize events.
typedef void (*wef_resize_fn)(void* user_data, uint32_t window_id,
                              int width,  // new width in pixels
                              int height  // new height in pixels
);

// Callback for window focus/blur events.
typedef void (*wef_focused_fn)(
    void* user_data, uint32_t window_id,
    int focused  // 1 = window gained focus, 0 = window lost focus
);

// Callback for keyboard events.
typedef void (*wef_keyboard_event_fn)(
    void* user_data, uint32_t window_id,
    int state,        // WEF_KEY_PRESSED or WEF_KEY_RELEASED
    const char* key,  // logical key (W3C UI Events key value, e.g. "a",
                      // "Enter", "Shift")
    const char*
        code,  // physical key code (W3C UI Events code, e.g. "KeyA", "Enter")
    uint32_t modifiers,  // bitmask of WEF_MOD_* flags
    bool repeat);

// Callback for window close requested events.
typedef void (*wef_close_requested_fn)(void* user_data, uint32_t window_id);

struct wef_backend_api {
  uint32_t version;
  void* backend_data;

  // Window lifecycle
  uint32_t (*create_window)(void* backend_data);
  void (*close_window)(void* backend_data, uint32_t window_id);

  void (*navigate)(void* backend_data, uint32_t window_id, const char* url);
  void (*set_title)(void* backend_data, uint32_t window_id, const char* title);
  void (*execute_js)(void* backend_data, uint32_t window_id, const char* script,
                     wef_js_result_fn callback, void* callback_data);
  void (*quit)(void* backend_data);
  void (*set_window_size)(void* backend_data, uint32_t window_id, int width,
                          int height);
  void (*get_window_size)(void* backend_data, uint32_t window_id, int* width,
                          int* height);
  void (*set_window_position)(void* backend_data, uint32_t window_id, int x,
                              int y);
  void (*get_window_position)(void* backend_data, uint32_t window_id, int* x,
                              int* y);
  void (*set_resizable)(void* backend_data, uint32_t window_id, bool resizable);
  bool (*is_resizable)(void* backend_data, uint32_t window_id);
  void (*set_always_on_top)(void* backend_data, uint32_t window_id,
                            bool always_on_top);
  bool (*is_always_on_top)(void* backend_data, uint32_t window_id);
  bool (*is_visible)(void* backend_data, uint32_t window_id);
  void (*show)(void* backend_data, uint32_t window_id);
  void (*hide)(void* backend_data, uint32_t window_id);
  void (*focus)(void* backend_data, uint32_t window_id);
  void (*post_ui_task)(void* backend_data, void (*task)(void* data),
                       void* data);

  bool (*value_is_null)(wef_value_t* val);
  bool (*value_is_bool)(wef_value_t* val);
  bool (*value_is_int)(wef_value_t* val);
  bool (*value_is_double)(wef_value_t* val);
  bool (*value_is_string)(wef_value_t* val);
  bool (*value_is_list)(wef_value_t* val);
  bool (*value_is_dict)(wef_value_t* val);
  bool (*value_is_binary)(wef_value_t* val);
  bool (*value_is_callback)(wef_value_t* val);

  bool (*value_get_bool)(wef_value_t* val);
  int (*value_get_int)(wef_value_t* val);
  double (*value_get_double)(wef_value_t* val);

  char* (*value_get_string)(wef_value_t* val, size_t* len_out);
  void (*value_free_string)(char* str);

  size_t (*value_list_size)(wef_value_t* val);
  wef_value_t* (*value_list_get)(wef_value_t* val, size_t index);

  wef_value_t* (*value_dict_get)(wef_value_t* dict, const char* key);
  bool (*value_dict_has)(wef_value_t* dict, const char* key);
  size_t (*value_dict_size)(wef_value_t* dict);

  char** (*value_dict_keys)(wef_value_t* dict, size_t* count_out);
  void (*value_free_keys)(char** keys, size_t count);

  const void* (*value_get_binary)(wef_value_t* val, size_t* len_out);

  uint64_t (*value_get_callback_id)(wef_value_t* val);

  wef_value_t* (*value_null)(void* backend_data);
  wef_value_t* (*value_bool)(void* backend_data, bool val);
  wef_value_t* (*value_int)(void* backend_data, int val);
  wef_value_t* (*value_double)(void* backend_data, double val);
  wef_value_t* (*value_string)(void* backend_data, const char* val);
  wef_value_t* (*value_list)(void* backend_data);
  wef_value_t* (*value_dict)(void* backend_data);
  wef_value_t* (*value_binary)(void* backend_data, const void* data,
                               size_t len);

  bool (*value_list_append)(wef_value_t* list, wef_value_t* val);
  bool (*value_list_set)(wef_value_t* list, size_t index, wef_value_t* val);

  bool (*value_dict_set)(wef_value_t* dict, const char* key, wef_value_t* val);

  void (*value_free)(wef_value_t* val);

  void (*set_js_call_handler)(void* backend_data, wef_js_call_fn handler,
                              void* user_data);
  void (*js_call_respond)(void* backend_data, uint64_t call_id,
                          wef_value_t* result, wef_value_t* error);
  void (*invoke_js_callback)(void* backend_data, uint64_t callback_id,
                             wef_value_t* args);
  void (*release_js_callback)(void* backend_data, uint64_t callback_id);

  // Raw window/display handles for GPU surface creation.
  // Returns platform-specific handle:
  //   AppKit: NSView*, Win32: HWND, X11: Window (cast to void*), Wayland:
  //   wl_surface*
  void* (*get_window_handle)(void* backend_data, uint32_t window_id);
  // Returns platform-specific display handle:
  //   AppKit: NULL, Win32: NULL (or HINSTANCE), X11: Display*, Wayland:
  //   wl_display*
  void* (*get_display_handle)(void* backend_data, uint32_t window_id);
  // Returns WEF_WINDOW_HANDLE_* constant identifying the platform
  int (*get_window_handle_type)(void* backend_data, uint32_t window_id);

  // Register a handler for keyboard input events (global, receives window_id in
  // callback).
  void (*set_keyboard_event_handler)(void* backend_data,
                                     wef_keyboard_event_fn handler,
                                     void* user_data);

  // Register a handler for mouse click events.
  void (*set_mouse_click_handler)(void* backend_data,
                                  wef_mouse_click_fn handler, void* user_data);

  // Register a handler for mouse move events.
  void (*set_mouse_move_handler)(void* backend_data, wef_mouse_move_fn handler,
                                 void* user_data);

  // Register a handler for wheel (scroll) events.
  void (*set_wheel_handler)(void* backend_data, wef_wheel_fn handler,
                            void* user_data);

  // Register a handler for cursor enter/leave events.
  void (*set_cursor_enter_leave_handler)(void* backend_data,
                                         wef_cursor_enter_leave_fn handler,
                                         void* user_data);

  // Register a handler for window focus/blur events.
  void (*set_focused_handler)(void* backend_data, wef_focused_fn handler,
                              void* user_data);

  // Register a handler for window resize events.
  void (*set_resize_handler)(void* backend_data, wef_resize_fn handler,
                             void* user_data);

  // Register a handler for window move events.
  void (*set_move_handler)(void* backend_data, wef_move_fn handler,
                           void* user_data);

  // Register a handler for window close requested events.
  void (*set_close_requested_handler)(void* backend_data,
                                      wef_close_requested_fn handler,
                                      void* user_data);

  void (*poll_js_calls)(void* backend_data);

  void (*set_js_call_notify)(void* backend_data,
                             void (*notify_fn)(void* notify_data),
                             void* notify_data);

  // Application menu. menu_template is a wef_value_t list of menu items.
  // Each item is a dict with: label, submenu (list), role, type, id,
  // accelerator. When a custom item (with "id") is clicked, on_click is called
  // with the id. On macOS the menu is applied to the global menu bar and
  // swapped on window focus. On Windows/Linux the menu is attached to the
  // specific window.
  void (*set_application_menu)(void* backend_data, uint32_t window_id,
                               wef_value_t* menu_template,
                               wef_menu_click_fn on_click, void* on_click_data);

  // Show a context menu at the given position (in window coordinates).
  // menu_template uses the same format as set_application_menu (list of menu
  // item dicts). on_click is called with the id of the clicked item.
  void (*show_context_menu)(void* backend_data, uint32_t window_id, int x,
                            int y, wef_value_t* menu_template,
                            wef_menu_click_fn on_click, void* on_click_data);

  // Open the DevTools inspector for the given window.
  void (*open_devtools)(void* backend_data, uint32_t window_id);

  // Set the global JS namespace name for bindings (default: "Wef").
  // Must be called before creating any windows.
  void (*set_js_namespace)(void* backend_data, const char* name);

  // Show a native dialog (alert, confirm, or prompt).
  // The callback is invoked with the result after the user dismisses the
  // dialog.
  void (*show_dialog)(
      void* backend_data, uint32_t window_id,
      int dialog_type,  // WEF_DIALOG_*
      const char* title, const char* message,
      const char* default_value,  // For prompt: default input text. NULL for
                                  // alert/confirm.
      wef_dialog_result_fn callback, void* callback_data);

  // --- Dock / taskbar ---
  //
  // Semantics are app-scoped on macOS (all operate on the process's Dock
  // tile) and focused-window-scoped on Windows/Linux (taskbar button for the
  // currently-focused WEF window). Backends that don't support an operation
  // on a given platform leave the function pointer NULL.

  // Set or clear a short text badge on the app's dock / taskbar icon.
  // Pass NULL or "" to clear. macOS: NSDockTile badgeLabel. Windows: renders
  // text to a small overlay icon via GDI+ + ITaskbarList3::SetOverlayIcon.
  // Linux: prepends "(text) " to the focused window's title.
  void (*set_dock_badge)(void* backend_data, const char* badge_or_null);

  // Request the user's attention by bouncing the dock icon (macOS) or
  // flashing the focused window's taskbar button (Windows) or setting the
  // urgency hint (Linux). `type` is WEF_DOCK_BOUNCE_*.
  void (*bounce_dock)(void* backend_data, int type);

  // Set a custom menu for the app's dock icon (macOS only).
  // menu_template uses the same format as set_application_menu. on_click is
  // called with the id of the clicked item. window_id in the callback will
  // be 0 since the menu is app-scoped. Windows/Linux: leave NULL.
  void (*set_dock_menu)(void* backend_data, wef_value_t* menu_template,
                        wef_menu_click_fn on_click, void* on_click_data);

  // Show or hide the app from the dock / task switcher (macOS activation
  // policy). Windows/Linux: leave NULL (no app-level equivalent).
  void (*set_dock_visible)(void* backend_data, bool visible);

  // Register a callback invoked when the user clicks the dock icon for an
  // app that has no visible windows (macOS). The backend always swallows
  // the default "show hidden window" behavior — the user callback is
  // informational. Windows/Linux: leave NULL.
  void (*set_dock_reopen_handler)(void* backend_data, wef_dock_reopen_fn fn,
                                  void* user_data);

  // --- Tray / status-bar icon ---
  //
  // A tray icon is an explicitly-created, persistent icon in the OS status
  // area (macOS menu bar extras, Windows system tray, Linux AppIndicator).
  // Each call to create_tray_icon returns a new id; destroy removes it.
  // Backends that don't support tray icons leave these NULL.

  // Create a new empty tray icon. Returns a tray_id > 0, or 0 on failure.
  uint32_t (*create_tray_icon)(void* backend_data);

  // Destroy a tray icon created via create_tray_icon.
  void (*destroy_tray_icon)(void* backend_data, uint32_t tray_id);

  // Set the icon image (PNG-encoded bytes). Required before the icon is
  // visible on most platforms.
  void (*set_tray_icon)(void* backend_data, uint32_t tray_id,
                        const void* png_bytes, size_t len);

  // Set or clear the tooltip shown on hover. Pass NULL or "" to clear.
  void (*set_tray_tooltip)(void* backend_data, uint32_t tray_id,
                           const char* tooltip_or_null);

  // Set the context (right-click) menu. menu_template uses the same format
  // as set_application_menu. on_click is called with the id of the clicked
  // item; the window_id argument of the callback is 0 (tray menus are
  // app-scoped, not window-scoped). Pass NULL menu_template to clear.
  void (*set_tray_menu)(void* backend_data, uint32_t tray_id,
                        wef_value_t* menu_template,
                        wef_menu_click_fn on_click, void* on_click_data);

  // Register a handler for left-click on the tray icon.
  void (*set_tray_click_handler)(void* backend_data, uint32_t tray_id,
                                 wef_tray_click_fn handler, void* user_data);

  // Register a handler for left-double-click. Fires after a quick second
  // click; the single-click handler (if any) still fires for the first
  // click. No-op on Linux (AppIndicator has no click events).
  void (*set_tray_double_click_handler)(void* backend_data, uint32_t tray_id,
                                        wef_tray_click_fn handler,
                                        void* user_data);

  // Set the icon used when the OS is in dark mode. When set, the backend
  // swaps between the primary (light) icon from set_tray_icon and this
  // one based on the current system appearance. Pass NULL/zero len to
  // clear the dark variant (then the primary icon is used in both modes).
  void (*set_tray_icon_dark)(void* backend_data, uint32_t tray_id,
                             const void* png_bytes, size_t len);
};

#ifdef __cplusplus
}
#endif

#endif  // WEF_H
