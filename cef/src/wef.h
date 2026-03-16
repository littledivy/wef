// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#ifndef WEF_H
#define WEF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEF_API_VERSION 2

typedef struct wef_value wef_value_t;

typedef void (*wef_js_call_fn)(
    void* user_data,
    uint64_t call_id,
    const char* method_path,
    wef_value_t* args
);

// Keyboard event state
#define WEF_KEY_PRESSED  0
#define WEF_KEY_RELEASED 1

// Keyboard modifier flags (bitmask)
#define WEF_MOD_SHIFT   (1 << 0)
#define WEF_MOD_CONTROL (1 << 1)
#define WEF_MOD_ALT     (1 << 2)
#define WEF_MOD_META    (1 << 3)

// Callback for keyboard events.
typedef void (*wef_keyboard_event_fn)(
    void* user_data,
    int state,              // WEF_KEY_PRESSED or WEF_KEY_RELEASED
    const char* key,        // logical key (W3C UI Events key value)
    const char* code,       // physical key code (W3C UI Events code)
    uint32_t modifiers,     // bitmask of WEF_MOD_* flags
    bool repeat
);

typedef struct wef_backend_api {
    uint32_t version;
    void* backend_data;

    void (*navigate)(void* backend_data, const char* url);
    void (*set_title)(void* backend_data, const char* title);
    void (*execute_js)(void* backend_data, const char* script);
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

    void (*set_js_call_handler)(void* backend_data, wef_js_call_fn handler, void* user_data);
    void (*js_call_respond)(void* backend_data, uint64_t call_id, wef_value_t* result, wef_value_t* error);

    void (*invoke_js_callback)(void* backend_data, uint64_t callback_id, wef_value_t* args);
    void (*release_js_callback)(void* backend_data, uint64_t callback_id);

    void (*set_keyboard_event_handler)(
        void* backend_data,
        wef_keyboard_event_fn handler,
        void* user_data
    );

} wef_backend_api_t;

typedef int (*wef_runtime_init_fn)(const wef_backend_api_t* api);
#define WEF_RUNTIME_INIT_SYMBOL "wef_runtime_init"

typedef int (*wef_runtime_start_fn)(void);
#define WEF_RUNTIME_START_SYMBOL "wef_runtime_start"

typedef void (*wef_runtime_shutdown_fn)(void);
#define WEF_RUNTIME_SHUTDOWN_SYMBOL "wef_runtime_shutdown"

#ifdef __cplusplus
}
#endif

#endif
