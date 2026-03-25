// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include <iostream>
#include <string>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <map>

#include "include/cef_app.h"
#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/views/cef_browser_view.h"
#include "include/views/cef_window.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "app.h"
#include "renderer_app.h"
#include "runtime_loader.h"

// --- Native event monitors (Linux / X11) ---
// Uses XI2 (X Input Extension 2) on a dedicated X11 connection to monitor
// mouse, scroll, cursor enter/leave, and focus events for CEF Views windows.
// Window resize/move events use StructureNotifyMask on the same connection.
//
// A separate X11 connection is used so that event selection doesn't interfere
// with CEF's or GDK's own event handling. The connection's FD is integrated
// into the GLib main loop via g_io_add_watch.

#include <gdk/gdk.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#include <X11/Xlib.h>
#include <X11/extensions/XInput2.h>

static Display* g_monitor_display = nullptr;
static int g_xi2_opcode = 0;
static GIOChannel* g_io_channel = nullptr;
static guint g_io_source_id = 0;
static bool g_monitor_installed = false;

// Cache: WM frame XID -> (wef_id, CEF content XID).
// With a reparenting WM, the top-level child of root is the WM frame,
// not the CEF window. This cache maps frame XIDs to the wef IDs and
// the actual content window for coordinate translation.
struct CachedWindow {
  uint32_t wef_id;
  Window content_xid;
};
static std::map<Window, CachedWindow> g_frame_cache;

static uint32_t XI2ModsToWef(XIModifierState* mods) {
  uint32_t wef = 0;
  if (mods->effective & ShiftMask) wef |= WEF_MOD_SHIFT;
  if (mods->effective & ControlMask) wef |= WEF_MOD_CONTROL;
  if (mods->effective & Mod1Mask) wef |= WEF_MOD_ALT;
  if (mods->effective & Mod4Mask) wef |= WEF_MOD_META;
  return wef;
}

static int XI2ButtonToWef(int detail) {
  switch (detail) {
    case 1: return WEF_MOUSE_BUTTON_LEFT;
    case 2: return WEF_MOUSE_BUTTON_MIDDLE;
    case 3: return WEF_MOUSE_BUTTON_RIGHT;
    case 8: return WEF_MOUSE_BUTTON_BACK;
    case 9: return WEF_MOUSE_BUTTON_FORWARD;
    default: return WEF_MOUSE_BUTTON_LEFT;
  }
}

// Resolve an XI2 device event to a wef window ID and content-relative coords.
static bool ResolveWindow(XIDeviceEvent* dev, uint32_t* out_wid,
                          double* out_x, double* out_y) {
  if (!dev->child) return false;

  RuntimeLoader* loader = RuntimeLoader::GetInstance();

  // Try the child (top-level under root) directly — works when no
  // reparenting WM is running or the CEF window IS the top-level.
  uint32_t wid = loader->GetWefIdForNativeHandle(
      (void*)(uintptr_t)dev->child);
  Window content_xid = dev->child;

  if (wid == 0) {
    // Check cached frame mapping (reparenting WM case).
    auto it = g_frame_cache.find(dev->child);
    if (it != g_frame_cache.end()) {
      wid = it->second.wef_id;
      content_xid = it->second.content_xid;
    }
  }

  if (wid == 0) return false;

  // Translate root-relative coordinates to content-window-relative.
  int wx, wy;
  Window child_ret;
  if (XTranslateCoordinates(g_monitor_display,
          DefaultRootWindow(g_monitor_display), content_xid,
          (int)dev->root_x, (int)dev->root_y,
          &wx, &wy, &child_ret)) {
    *out_wid = wid;
    *out_x = static_cast<double>(wx);
    *out_y = static_cast<double>(wy);
    return true;
  }

  return false;
}

// Resolve a window XID from an XI2 enter/focus event to a wef ID.
static uint32_t ResolveWefId(Window xid) {
  if (!xid) return 0;
  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  uint32_t wid = loader->GetWefIdForNativeHandle((void*)(uintptr_t)xid);
  if (wid == 0) {
    auto it = g_frame_cache.find(xid);
    if (it != g_frame_cache.end()) wid = it->second.wef_id;
  }
  return wid;
}

static void ProcessXI2Event(XEvent* xev) {
  if (!XGetEventData(xev->xcookie.display, &xev->xcookie)) return;

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  int evtype = xev->xcookie.evtype;

  if (evtype == XI_ButtonPress || evtype == XI_ButtonRelease ||
      evtype == XI_Motion || evtype == XI_Enter || evtype == XI_Leave) {
    XIDeviceEvent* dev = static_cast<XIDeviceEvent*>(xev->xcookie.data);
    uint32_t wid;
    double x, y;

    if (ResolveWindow(dev, &wid, &x, &y)) {
      uint32_t modifiers = XI2ModsToWef(&dev->mods);

      switch (evtype) {
        case XI_ButtonPress: {
          int detail = dev->detail;
          if (detail >= 4 && detail <= 7) {
            // X11 scroll wheel: buttons 4-7
            double dx = 0, dy = 0;
            if (detail == 4) dy = -1.0;
            else if (detail == 5) dy = 1.0;
            else if (detail == 6) dx = -1.0;
            else if (detail == 7) dx = 1.0;
            loader->DispatchWheelEvent(
                wid, dx, dy, x, y, modifiers, WEF_WHEEL_DELTA_LINE);
          } else {
            loader->DispatchMouseClickEvent(
                wid, WEF_MOUSE_PRESSED, XI2ButtonToWef(detail),
                x, y, modifiers, 1);
          }
          break;
        }
        case XI_ButtonRelease: {
          if (dev->detail >= 4 && dev->detail <= 7) break;
          loader->DispatchMouseClickEvent(
              wid, WEF_MOUSE_RELEASED, XI2ButtonToWef(dev->detail),
              x, y, modifiers, 1);
          break;
        }
        case XI_Motion:
          loader->DispatchMouseMoveEvent(wid, x, y, modifiers);
          break;
        case XI_Enter:
          loader->DispatchCursorEnterLeaveEvent(wid, 1, x, y, modifiers);
          break;
        case XI_Leave:
          loader->DispatchCursorEnterLeaveEvent(wid, 0, x, y, modifiers);
          break;
      }
    }
  } else if (evtype == XI_FocusIn || evtype == XI_FocusOut) {
    XIEnterEvent* enter = static_cast<XIEnterEvent*>(xev->xcookie.data);
    Window target = enter->child ? enter->child : enter->event;
    uint32_t wid = ResolveWefId(target);
    if (wid > 0) {
      loader->DispatchFocusedEvent(wid, evtype == XI_FocusIn ? 1 : 0);
    }
  }

  XFreeEventData(xev->xcookie.display, &xev->xcookie);
}

static void ProcessStructureEvent(XEvent* xev) {
  if (xev->type != ConfigureNotify) return;

  RuntimeLoader* loader = RuntimeLoader::GetInstance();
  XConfigureEvent* config = &xev->xconfigure;

  uint32_t wid = loader->GetWefIdForNativeHandle(
      (void*)(uintptr_t)config->window);
  if (wid > 0) {
    loader->DispatchResizeEvent(wid, config->width, config->height);
    loader->DispatchMoveEvent(wid, config->x, config->y);
  }
}

static gboolean X11IoCallback(GIOChannel* source, GIOCondition condition,
                               gpointer data) {
  if (!g_monitor_display) return FALSE;

  while (XPending(g_monitor_display)) {
    XEvent xev;
    XNextEvent(g_monitor_display, &xev);

    if (xev.type == GenericEvent && xev.xcookie.extension == g_xi2_opcode) {
      ProcessXI2Event(&xev);
    } else {
      ProcessStructureEvent(&xev);
    }
  }

  return TRUE;
}

#endif // GDK_WINDOWING_X11

void InstallNativeMouseMonitor() {
#ifdef GDK_WINDOWING_X11
  if (g_monitor_installed) return;

  GdkDisplay* gdk_display = gdk_display_get_default();
  if (!gdk_display || !GDK_IS_X11_DISPLAY(gdk_display)) return;

  // Open a dedicated X11 connection for event monitoring.
  g_monitor_display = XOpenDisplay(nullptr);
  if (!g_monitor_display) return;

  // Check for XI2 support.
  int xi2_event, xi2_error;
  if (!XQueryExtension(g_monitor_display, "XInputExtension",
                       &g_xi2_opcode, &xi2_event, &xi2_error)) {
    XCloseDisplay(g_monitor_display);
    g_monitor_display = nullptr;
    return;
  }

  int major = 2, minor = 0;
  if (XIQueryVersion(g_monitor_display, &major, &minor) != Success) {
    XCloseDisplay(g_monitor_display);
    g_monitor_display = nullptr;
    return;
  }

  // Select XI2 events on the root window for all master devices.
  Window root = DefaultRootWindow(g_monitor_display);
  unsigned char mask_bits[XIMaskLen(XI_LASTEVENT)] = {};
  XISetMask(mask_bits, XI_ButtonPress);
  XISetMask(mask_bits, XI_ButtonRelease);
  XISetMask(mask_bits, XI_Motion);
  XISetMask(mask_bits, XI_Enter);
  XISetMask(mask_bits, XI_Leave);
  XISetMask(mask_bits, XI_FocusIn);
  XISetMask(mask_bits, XI_FocusOut);

  XIEventMask mask;
  mask.deviceid = XIAllMasterDevices;
  mask.mask_len = sizeof(mask_bits);
  mask.mask = mask_bits;
  XISelectEvents(g_monitor_display, root, &mask, 1);
  XFlush(g_monitor_display);

  // Integrate the monitoring connection into the GLib main loop.
  int fd = ConnectionNumber(g_monitor_display);
  g_io_channel = g_io_channel_unix_new(fd);
  g_io_source_id = g_io_add_watch(
      g_io_channel,
      static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR),
      X11IoCallback, nullptr);

  g_monitor_installed = true;
#endif
}

void RemoveNativeMouseMonitor() {
#ifdef GDK_WINDOWING_X11
  if (!g_monitor_installed) return;

  if (g_io_source_id) {
    g_source_remove(g_io_source_id);
    g_io_source_id = 0;
  }
  if (g_io_channel) {
    g_io_channel_unref(g_io_channel);
    g_io_channel = nullptr;
  }
  if (g_monitor_display) {
    XCloseDisplay(g_monitor_display);
    g_monitor_display = nullptr;
  }

  g_frame_cache.clear();
  g_monitor_installed = false;
#endif
}

void MonitorLinuxWindowEvents(unsigned long xid) {
#ifdef GDK_WINDOWING_X11
  if (!g_monitor_display) return;

  // Select StructureNotifyMask on the CEF window to get ConfigureNotify
  // (resize/move) events on our monitoring connection.
  XSelectInput(g_monitor_display, xid, StructureNotifyMask);

  // Walk up the window tree to find the WM frame (direct child of root).
  // With a reparenting WM, the CEF window is reparented inside the frame.
  // XI2 events on root report the frame as `child`, so we cache the
  // frame → (wef_id, content_xid) mapping for fast lookup.
  Window root_ret, parent;
  Window* children;
  unsigned int nchildren;
  Window current = xid;

  while (XQueryTree(g_monitor_display, current, &root_ret, &parent,
                    &children, &nchildren)) {
    if (children) XFree(children);
    if (parent == root_ret || parent == 0) {
      // current is the top-level frame (or the window itself if no WM).
      if (current != xid) {
        uint32_t wid = RuntimeLoader::GetInstance()->GetWefIdForNativeHandle(
            (void*)(uintptr_t)xid);
        if (wid > 0) {
          g_frame_cache[current] = {wid, xid};
        }
      }
      break;
    }
    current = parent;
  }

  XFlush(g_monitor_display);
#else
  (void)xid;
#endif
}

// --- Headless / forked worker support ---

static int run_headless(const std::string& runtimePath) {
  RuntimeLoader* loader = RuntimeLoader::GetInstance();

  if (runtimePath.empty()) {
    std::cerr << "No runtime library found for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Load(runtimePath)) {
    std::cerr << "Failed to load runtime for headless worker." << std::endl;
    return 1;
  }

  if (!loader->Start()) {
    std::cerr << "Failed to start headless worker runtime." << std::endl;
    return 1;
  }

  loader->Shutdown();
  return 0;
}

static bool is_forked_worker() {
  return getenv("NODE_CHANNEL_FD") != nullptr
      || getenv("NEXT_PRIVATE_WORKER") != nullptr;
}

static bool is_cli_worker_command(int argc, char* argv[]) {
  if (argc < 3 || strcmp(argv[1], "run") != 0) {
    return false;
  }
  for (int i = 2; i < argc; ++i) {
    if (argv[i][0] == '-') {
      continue;
    }
    return true;
  }
  return false;
}

// Combined app that handles both browser and renderer processes (single-exe model)
class WefCombinedApp : public CefApp,
                       public CefBrowserProcessHandler {
 public:
  WefCombinedApp() : renderer_app_(new WefRendererApp()) {}

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {
    return renderer_app_->GetRenderProcessHandler();
  }

  void OnBeforeCommandLineProcessing(
      const CefString& process_type,
      CefRefPtr<CefCommandLine> command_line) override {
  }

  void OnContextInitialized() override {
    CEF_REQUIRE_UI_THREAD();

    // Keep the handler alive for the lifetime of the app.
    // Backend_CreateWindow uses WefHandler::GetInstance() from the runtime
    // thread, so the handler must outlive this function scope.
    static CefRefPtr<WefHandler> handler(new WefHandler());

    if (!g_runtime_path.empty()) {
      if (!RuntimeLoader::GetInstance()->Load(g_runtime_path)) {
        std::cerr << "Failed to load runtime, exiting" << std::endl;
        CefQuitMessageLoop();
        return;
      }
      // Defer Start() to the next message loop iteration.
      // OnContextInitialized runs during CefInitialize(), before
      // CefRunMessageLoop() has started. The runtime thread's
      // Backend_CreateWindow posts CefPostTasks to the UI thread and blocks
      // until they complete — this deadlocks if the loop isn't running yet.
      CefPostTask(TID_UI, base::BindOnce([]() {
        RuntimeLoader::GetInstance()->Start();
      }));
    } else {
      // No runtime: create a default window for demo
      uint32_t wef_id = RuntimeLoader::GetInstance()->AllocateWindowId();
      g_pending_wef_ids.push(wef_id);
      CefBrowserSettings browser_settings;
      CefRefPtr<CefBrowserView> browser_view =
          CefBrowserView::CreateBrowserView(
              handler, "https://example.com", browser_settings,
              nullptr, nullptr, nullptr);
      CefWindow::CreateTopLevelWindow(
          new WefWindowDelegate(browser_view, wef_id));
    }
  }

 private:
  CefRefPtr<WefRendererApp> renderer_app_;
  IMPLEMENT_REFCOUNTING(WefCombinedApp);
};

int main(int argc, char* argv[]) {
  CefMainArgs main_args(argc, argv);

  // Single-exe model: check if we are a subprocess first
  CefRefPtr<WefCombinedApp> app(new WefCombinedApp());
  int exit_code = CefExecuteProcess(main_args, app, nullptr);
  if (exit_code >= 0) {
    return exit_code;
  }

  // Parse --runtime argument
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--runtime") == 0 && i + 1 < argc) {
      g_runtime_path = argv[++i];
    } else if (strncmp(argv[i], "--runtime=", 10) == 0) {
      g_runtime_path = argv[i] + 10;
    }
  }

  if (g_runtime_path.empty()) {
    const char* envPath = getenv("WEF_RUNTIME_PATH");
    if (envPath) {
      g_runtime_path = envPath;
    }
  }

  // Check for headless / forked worker mode (skip CEF entirely)
  if (is_forked_worker() || is_cli_worker_command(argc, argv)) {
    return run_headless(g_runtime_path);
  }

  CefSettings settings;
  settings.no_sandbox = true;

  // Set cache path
  std::string cache_path = "/tmp/wef_cef_" + std::to_string(getpid());
  CefString(&settings.root_cache_path) = cache_path;

  if (!CefInitialize(main_args, settings, app.get(), nullptr)) {
    return 1;
  }

  CefRunMessageLoop();

  RuntimeLoader::GetInstance()->Shutdown();

  CefShutdown();

  return 0;
}
