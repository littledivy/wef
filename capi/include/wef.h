// Copyright 2023-2026 Divy Srivastava <dj.srivastava23@gmail.com>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef WEF_H
#define WEF_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DD_API_VERSION 1

typedef struct dd_backend_api dd_backend_api_t;

typedef int (*dd_runtime_init_fn)(const dd_backend_api_t* api);
#define DD_RUNTIME_INIT_SYMBOL "dd_runtime_init"

typedef int (*dd_runtime_start_fn)(void);
#define DD_RUNTIME_START_SYMBOL "dd_runtime_start"

typedef void (*dd_runtime_shutdown_fn)(void);
#define DD_RUNTIME_SHUTDOWN_SYMBOL "dd_runtime_shutdown"

typedef struct dd_value dd_value_t;

typedef void (*dd_js_call_fn)(
    void* user_data,
    uint64_t call_id,
    const char* method_path,
    dd_value_t* args
);

struct dd_backend_api {
    uint32_t version;
    void* backend_data;

    void (*navigate)(void* backend_data, const char* url);
    void (*set_title)(void* backend_data, const char* title);
    void (*execute_js)(void* backend_data, const char* script);
    void (*quit)(void* backend_data);
    void (*set_window_size)(void* backend_data, int width, int height);
    void (*post_ui_task)(void* backend_data, void (*task)(void* data), void* data);

    bool (*value_is_null)(dd_value_t* val);
    bool (*value_is_bool)(dd_value_t* val);
    bool (*value_is_int)(dd_value_t* val);
    bool (*value_is_double)(dd_value_t* val);
    bool (*value_is_string)(dd_value_t* val);
    bool (*value_is_list)(dd_value_t* val);
    bool (*value_is_dict)(dd_value_t* val);
    bool (*value_is_binary)(dd_value_t* val);
    bool (*value_is_callback)(dd_value_t* val);

    bool (*value_get_bool)(dd_value_t* val);
    int (*value_get_int)(dd_value_t* val);
    double (*value_get_double)(dd_value_t* val);

    char* (*value_get_string)(dd_value_t* val, size_t* len_out);
    void (*value_free_string)(char* str);

    size_t (*value_list_size)(dd_value_t* val);
    dd_value_t* (*value_list_get)(dd_value_t* val, size_t index);

    dd_value_t* (*value_dict_get)(dd_value_t* dict, const char* key);
    bool (*value_dict_has)(dd_value_t* dict, const char* key);
    size_t (*value_dict_size)(dd_value_t* dict);

    char** (*value_dict_keys)(dd_value_t* dict, size_t* count_out);
    void (*value_free_keys)(char** keys, size_t count);

    const void* (*value_get_binary)(dd_value_t* val, size_t* len_out);

    uint64_t (*value_get_callback_id)(dd_value_t* val);

    dd_value_t* (*value_null)(void* backend_data);
    dd_value_t* (*value_bool)(void* backend_data, bool val);
    dd_value_t* (*value_int)(void* backend_data, int val);
    dd_value_t* (*value_double)(void* backend_data, double val);
    dd_value_t* (*value_string)(void* backend_data, const char* val);
    dd_value_t* (*value_list)(void* backend_data);
    dd_value_t* (*value_dict)(void* backend_data);
    dd_value_t* (*value_binary)(void* backend_data, const void* data, size_t len);

    bool (*value_list_append)(dd_value_t* list, dd_value_t* val);
    bool (*value_list_set)(dd_value_t* list, size_t index, dd_value_t* val);

    bool (*value_dict_set)(dd_value_t* dict, const char* key, dd_value_t* val);

    void (*value_free)(dd_value_t* val);

    void (*set_js_call_handler)(
        void* backend_data,
        dd_js_call_fn handler,
        void* user_data
    );
    void (*js_call_respond)(
        void* backend_data,
        uint64_t call_id,
        dd_value_t* result,
        const char* error
    );
    void (*invoke_js_callback)(
        void* backend_data,
        uint64_t callback_id,
        dd_value_t* args
    );
    void (*release_js_callback)(
        void* backend_data,
        uint64_t callback_id
    );

};

#ifdef __cplusplus
}
#endif

#endif // WEF_H
