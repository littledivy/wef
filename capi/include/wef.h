// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef WEF_H
#define WEF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEF_API_VERSION 8

// Window handle types for get_window_handle_type
#define WEF_WINDOW_HANDLE_UNKNOWN  0
#define WEF_WINDOW_HANDLE_APPKIT   1
#define WEF_WINDOW_HANDLE_WIN32    2
#define WEF_WINDOW_HANDLE_X11      3
#define WEF_WINDOW_HANDLE_WAYLAND  4

typedef struct wef_backend_api wef_backend_api_t;

typedef int (*wef_runtime_init_fn)(const wef_backend_api_t* api);
#define WEF_RUNTIME_INIT_SYMBOL "wef_runtime_init"

typedef int (*wef_runtime_start_fn)(void);
#define WEF_RUNTIME_START_SYMBOL "wef_runtime_start"

typedef void (*wef_runtime_shutdown_fn)(void);
#define WEF_RUNTIME_SHUTDOWN_SYMBOL "wef_runtime_shutdown"

typedef struct wef_value wef_value_t;

typedef void (*wef_js_call_fn)(
    void* user_data,
    uint64_t call_id,
    const char* method_path,
    wef_value_t* args
);

// Callback for execute_js results. Pass NULL to execute_js for fire-and-forget.
typedef void (*wef_js_result_fn)(
    wef_value_t* result,
    wef_value_t* error,
    void* user_data
);

typedef void (*wef_menu_click_fn)(
    void* user_data,
    const char* item_id
);

// Keyboard event state
#define WEF_KEY_PRESSED  0
#define WEF_KEY_RELEASED 1

// Keyboard modifier flags (bitmask)
#define WEF_MOD_SHIFT   (1 << 0)
#define WEF_MOD_CONTROL (1 << 1)
#define WEF_MOD_ALT     (1 << 2)
#define WEF_MOD_META    (1 << 3)

// Mouse button constants
#define WEF_MOUSE_BUTTON_LEFT    0
#define WEF_MOUSE_BUTTON_RIGHT   1
#define WEF_MOUSE_BUTTON_MIDDLE  2
#define WEF_MOUSE_BUTTON_BACK    3
#define WEF_MOUSE_BUTTON_FORWARD 4

// Mouse event state
#define WEF_MOUSE_PRESSED  0
#define WEF_MOUSE_RELEASED 1

// Callback for mouse click events.
typedef void (*wef_mouse_click_fn)(
    void* user_data,
    int state,          // WEF_MOUSE_PRESSED or WEF_MOUSE_RELEASED
    int button,         // WEF_MOUSE_BUTTON_*
    double x,           // x position in window coordinates
    double y,           // y position in window coordinates
    uint32_t modifiers, // bitmask of WEF_MOD_* flags
    int32_t click_count // 1 = single, 2 = double click
);

// Callback for mouse move events.
typedef void (*wef_mouse_move_fn)(
    void* user_data,
    double x,           // x position in window coordinates
    double y,           // y position in window coordinates
    uint32_t modifiers  // bitmask of WEF_MOD_* flags
);

// Wheel delta mode
#define WEF_WHEEL_DELTA_PIXEL 0
#define WEF_WHEEL_DELTA_LINE  1
#define WEF_WHEEL_DELTA_PAGE  2

// Callback for wheel (scroll) events.
typedef void (*wef_wheel_fn)(
    void* user_data,
    double delta_x,     // horizontal scroll amount
    double delta_y,     // vertical scroll amount
    double x,           // cursor x position in window coordinates
    double y,           // cursor y position in window coordinates
    uint32_t modifiers, // bitmask of WEF_MOD_* flags
    int32_t delta_mode  // WEF_WHEEL_DELTA_*
);

// Callback for cursor enter/leave events (mouseenter/mouseleave).
typedef void (*wef_cursor_enter_leave_fn)(
    void* user_data,
    int entered,        // 1 = cursor entered window, 0 = cursor left window
    double x,           // cursor x position in window coordinates
    double y,           // cursor y position in window coordinates
    uint32_t modifiers  // bitmask of WEF_MOD_* flags
);

// Callback for keyboard events.
typedef void (*wef_keyboard_event_fn)(
    void* user_data,
    int state,              // WEF_KEY_PRESSED or WEF_KEY_RELEASED
    const char* key,        // logical key (W3C UI Events key value, e.g. "a", "Enter", "Shift")
    const char* code,       // physical key code (W3C UI Events code, e.g. "KeyA", "Enter")
    uint32_t modifiers,     // bitmask of WEF_MOD_* flags
    bool repeat
);

struct wef_backend_api {
    uint32_t version;
    void* backend_data;

    void (*navigate)(void* backend_data, const char* url);
    void (*set_title)(void* backend_data, const char* title);
    void (*execute_js)(void* backend_data, const char* script,
                       wef_js_result_fn callback, void* callback_data);
    void (*quit)(void* backend_data);
    void (*set_window_size)(void* backend_data, int width, int height);
    void (*get_window_size)(void* backend_data, int* width, int* height);
    void (*set_window_position)(void* backend_data, int x, int y);
    void (*get_window_position)(void* backend_data, int* x, int* y);
    void (*set_resizable)(void* backend_data, bool resizable);
    bool (*is_resizable)(void* backend_data);
    void (*set_always_on_top)(void* backend_data, bool always_on_top);
    bool (*is_always_on_top)(void* backend_data);
    bool (*is_visible)(void* backend_data);
    void (*show)(void* backend_data);
    void (*hide)(void* backend_data);
    void (*focus)(void* backend_data);
    void (*post_ui_task)(void* backend_data, void (*task)(void* data), void* data);

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
    wef_value_t* (*value_binary)(void* backend_data, const void* data, size_t len);

    bool (*value_list_append)(wef_value_t* list, wef_value_t* val);
    bool (*value_list_set)(wef_value_t* list, size_t index, wef_value_t* val);

    bool (*value_dict_set)(wef_value_t* dict, const char* key, wef_value_t* val);

    void (*value_free)(wef_value_t* val);

    void (*set_js_call_handler)(
        void* backend_data,
        wef_js_call_fn handler,
        void* user_data
    );
    void (*js_call_respond)(
        void* backend_data,
        uint64_t call_id,
        wef_value_t* result,
        wef_value_t* error
    );
    void (*invoke_js_callback)(
        void* backend_data,
        uint64_t callback_id,
        wef_value_t* args
    );
    void (*release_js_callback)(
        void* backend_data,
        uint64_t callback_id
    );

    // Raw window/display handles for GPU surface creation.
    // Returns platform-specific handle:
    //   AppKit: NSView*, Win32: HWND, X11: Window (cast to void*), Wayland: wl_surface*
    void* (*get_window_handle)(void* backend_data);
    // Returns platform-specific display handle:
    //   AppKit: NULL, Win32: NULL (or HINSTANCE), X11: Display*, Wayland: wl_display*
    void* (*get_display_handle)(void* backend_data);
    // Returns WEF_WINDOW_HANDLE_* constant identifying the platform
    int (*get_window_handle_type)(void* backend_data);

    // Register a handler for keyboard input events.
    void (*set_keyboard_event_handler)(
        void* backend_data,
        wef_keyboard_event_fn handler,
        void* user_data
    );

    // Register a handler for mouse click events.
    void (*set_mouse_click_handler)(
        void* backend_data,
        wef_mouse_click_fn handler,
        void* user_data
    );

    // Register a handler for mouse move events.
    void (*set_mouse_move_handler)(
        void* backend_data,
        wef_mouse_move_fn handler,
        void* user_data
    );

    // Register a handler for wheel (scroll) events.
    void (*set_wheel_handler)(
        void* backend_data,
        wef_wheel_fn handler,
        void* user_data
    );

    // Register a handler for cursor enter/leave events.
    void (*set_cursor_enter_leave_handler)(
        void* backend_data,
        wef_cursor_enter_leave_fn handler,
        void* user_data
    );

    void (*poll_js_calls)(void* backend_data);

    void (*set_js_call_notify)(
        void* backend_data,
        void (*notify_fn)(void* notify_data),
        void* notify_data
    );

    // Application menu. menu_template is a wef_value_t list of menu items.
    // Each item is a dict with: label, submenu (list), role, type, id, accelerator.
    // When a custom item (with "id") is clicked, on_click is called with the id.
    void (*set_application_menu)(
        void* backend_data,
        wef_value_t* menu_template,
        wef_menu_click_fn on_click,
        void* on_click_data
    );
};

#ifdef __cplusplus
}
#endif

#endif // WEF_H
