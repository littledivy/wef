// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "runtime_loader.h"
#include "wef_json.h"

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <JavaScriptCore/JavaScript.h>

#include <iostream>

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
  void PostUiTask(void (*task)(void*), void* data) override;

  void InvokeJsCallback(uint64_t callback_id, wef::ValuePtr args) override;
  void ReleaseJsCallback(uint64_t callback_id) override;
  void RespondToJsCall(uint64_t call_id, wef::ValuePtr result, const char* error) override;

  void Run() override;

  void HandleJsMessage(const char* json);

 private:
  GtkWidget* window_;
  WebKitWebView* webview_;
  WebKitUserContentManager* content_manager_;

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

void WebKitGTKBackend::InvokeJsCallback(uint64_t callback_id, wef::ValuePtr args) {
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

void WebKitGTKBackend::RespondToJsCall(uint64_t call_id, wef::ValuePtr result, const char* error) {
  std::string resultJson = json::Serialize(result);
  std::string errorStr = error ? json::Escape(error) : "";
  struct RespData { WebKitGTKBackend* backend; uint64_t id; std::string result; std::string error; };
  auto* respData = new RespData{this, call_id, resultJson, errorStr};
  g_idle_add([](gpointer data) -> gboolean {
    auto* rd = static_cast<RespData*>(data);
    std::string script;
    if (rd->error.empty()) {
      script = "window.__wefRespond(" + std::to_string(rd->id) + ", " +
               rd->result + ", null);";
    } else {
      script = "window.__wefRespond(" + std::to_string(rd->id) + ", null, \"" +
               rd->error + "\");";
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

  RuntimeLoader::GetInstance()->OnJsCall(call_id, method, args);
}

// ============================================================================
// Factory Function
// ============================================================================

WebviewBackend* CreateWebviewBackend(int width, int height, const std::string& title) {
  return new WebKitGTKBackend(width, height, title);
}
