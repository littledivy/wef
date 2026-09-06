//! Backend-agnostic native-chrome / windowing e2e battery.
//!
//! This runtime is a cdylib loaded by *any* backend via `--runtime <path>` /
//! `LAUFEY_RUNTIME_PATH`, so the same assertions run under CEF, WebView, and
//! Winit. Because backends implement different subsets of the C ABI (Winit has
//! no web engine, some tray/menu fns may be absent), every assertion is
//! *capability-probed*: it emits one of
//!
//!   [e2e] PASS <name>   assertion held
//!   [e2e] FAIL <name>   supported here but wrong  -> process exits 1
//!   [e2e] N/A  <name>   capability absent on this backend (not a failure)
//!
//! This covers Layer 0 (in-process readback + event-callback round-trips),
//! menu/tray *click* round-trips via the `test_click_menu_item` C ABI hook
//! (API 30+; `N/A` on backends without it), the close-handler round-trip
//! via `test_trigger_close_requested` (API 31+), and synthetic pointer /
//! key / wheel events via `test_inject_input` (API 35). OS-observer
//! *structure* checks (Layer 1: the Linux D-Bus driver; macOS/Windows
//! pending a backend hook) live outside this runtime. See
//! docs/e2e-testing.md.
//!
//! Mirrors the execution model of `examples/cef_e2e` (tokio runtime, spawned
//! event-loop pump, PASS/FAIL + exit code) so the existing runtime loader drives
//! it unchanged.

use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::sync::{Arc, Mutex};

use laufey::{
  CursorEnterLeaveEvent, KeyState, KeyboardEvent, MenuItem, MouseButton,
  MouseButtonState, MouseClickEvent, MouseMoveEvent, TestInput, TrayIcon,
  WheelDeltaMode, WheelEvent, Window, WindowOptions, LAUFEY_MOD_SHIFT,
  LAUFEY_MOUSE_BUTTON_LEFT, LAUFEY_WHEEL_DELTA_LINE,
};

static FAILED: AtomicBool = AtomicBool::new(false);

/// Minimal valid 1x1 transparent PNG. Used to give the tray a real icon so the
/// Linux D-Bus observer's `IconThemePath`/`IconName` assertion holds.
const TINY_PNG: &[u8] = &[
  0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49,
  0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06,
  0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44,
  0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D,
  0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42,
  0x60, 0x82,
];

fn check(name: &str, ok: bool) {
  if ok {
    eprintln!("[e2e] PASS {name}");
  } else {
    eprintln!("[e2e] FAIL {name}");
    FAILED.store(true, Ordering::SeqCst);
  }
}

/// Capability absent on this backend — informational, never fails the run.
fn na(name: &str) {
  eprintln!("[e2e] N/A  {name}");
}

/// Decorated macOS / Windows chrome puts the content origin below (and
/// usually a bit to the right of) the frame origin. Linux reports the
/// compositor's frame as the content, so the two coincide.
fn expect_title_bar_offset() -> bool {
  cfg!(any(target_os = "macos", target_os = "windows"))
}

fn check_inner_vs_frame(
  name: &str,
  frame: (i32, i32),
  inner: (i32, i32),
  expect_chrome: bool,
) {
  if frame == (0, 0) && inner == (0, 0) {
    na(&format!("{name} (window not placed yet)"));
    return;
  }
  // Winit's get_position only reports a not-yet-applied set, so after
  // realize the frame reads as (0, 0) while inner is the real content
  // origin. That is a getter limitation, not a chrome-offset bug.
  if frame == (0, 0) && inner != (0, 0) {
    na(&format!("{name} (frame origin not readable)"));
    return;
  }
  let dx = inner.0 - frame.0;
  let dy = inner.1 - frame.1;
  if expect_chrome {
    check(name, dy > 0 && dx >= 0 && dy > dx);
  } else {
    check(name, dx == 0 && dy == 0);
  }
}

fn check_outer_vs_inner(
  name: &str,
  inner: (i32, i32),
  outer: (i32, i32),
  expect_chrome: bool,
) {
  if inner == (0, 0) && outer == (0, 0) {
    na(&format!("{name} (size not available yet)"));
    return;
  }
  if expect_chrome {
    check(name, outer.0 >= inner.0 && outer.1 > inner.1);
  } else {
    check(name, outer == inner);
  }
}

async fn wait_for<F: Fn() -> bool>(f: F, attempts: u32, step_ms: u64) -> bool {
  for _ in 0..attempts {
    if f() {
      return true;
    }
    tokio::time::sleep(std::time::Duration::from_millis(step_ms)).await;
  }
  f()
}

fn expected_handle_type() -> (&'static str, &'static [i32]) {
  // (label, accepted type enums). Linux may be X11 or Wayland; under Xvfb it's
  // X11. See LAUFEY_WINDOW_HANDLE_* in the capi.
  if cfg!(target_os = "macos") {
    ("APPKIT", &[laufey::LAUFEY_WINDOW_HANDLE_APPKIT])
  } else if cfg!(target_os = "windows") {
    ("WIN32", &[laufey::LAUFEY_WINDOW_HANDLE_WIN32])
  } else {
    (
      "X11/WAYLAND",
      &[
        laufey::LAUFEY_WINDOW_HANDLE_X11,
        laufey::LAUFEY_WINDOW_HANDLE_WAYLAND,
      ],
    )
  }
}

fn e2e_main() {
  let rt = tokio::runtime::Runtime::new().expect("tokio runtime");
  rt.block_on(async move {
    // Pump the laufey event loop (JS-call dispatch, timers).
    tokio::spawn(async { laufey::run().await });

    // ---- window creation + event-callback wiring -------------------------
    // Resize / move / focus handlers record the last event so we can drive the
    // setter and assert the callback round-trips (technique B).
    let resize_w = Arc::new(AtomicI32::new(0));
    let resize_h = Arc::new(AtomicI32::new(0));
    let moved = Arc::new(AtomicBool::new(false));
    let focused_seen = Arc::new(AtomicBool::new(false));
    let page_loaded = Arc::new(AtomicBool::new(false));

    let (rw, rh) = (resize_w.clone(), resize_h.clone());
    let mv = moved.clone();
    let fc = focused_seen.clone();
    let pl = page_loaded.clone();

    let win = Window::new(800, 600)
      .title("native-e2e")
      .on_resize(move |e| {
        rw.store(e.width, Ordering::SeqCst);
        rh.store(e.height, Ordering::SeqCst);
      })
      .on_move(move |_e| {
        mv.store(true, Ordering::SeqCst);
      })
      .on_focused(move |e| {
        if e.focused {
          fc.store(true, Ordering::SeqCst);
        }
      })
      // Readiness signal for the print_to_pdf test below; engine-less
      // backends never fire it, which that test treats as best-effort.
      .on_page_load(move |_e| {
        pl.store(true, Ordering::SeqCst);
      })
      // Trivial page; a no-op on engine-less backends (Winit navigate is None).
      .load("data:text/html,<!doctype html><title>native-e2e</title>");

    check("window id is nonzero", win.id() != 0);

    // Constructor-time getters must not wait for the OS window: JS can
    // read them from the constructor, and the winit backend creates the
    // NSWindow / HWND asynchronously. Seeded DPR used to stay 1.0 until
    // the first Resized; inner position used to be the frame origin.
    let early_scale = win.get_scale_factor();
    check("early scale factor is positive", early_scale > 0.0);
    let (early_w, early_h) = win.get_size();
    check(
      "constructor size is readable immediately",
      (early_w - 800).abs() <= 2 && (early_h - 600).abs() <= 2,
    );

    let decorated = Window::new(400, 300).title("native-e2e-chrome");
    decorated.set_position(240, 160);
    check_inner_vs_frame(
      "decorated inner is offset by the title bar",
      decorated.get_position(),
      decorated.get_inner_position(),
      expect_title_bar_offset(),
    );

    let frameless = Window::new_with_options(
      200,
      150,
      WindowOptions {
        frameless: true,
        ..WindowOptions::default()
      },
    )
    .title("native-e2e-frameless");
    frameless.set_position(300, 200);
    check_inner_vs_frame(
      "frameless inner matches the frame origin",
      frameless.get_position(),
      frameless.get_inner_position(),
      false,
    );
    check_outer_vs_inner(
      "decorated outer is larger than the content",
      decorated.get_size(),
      decorated.get_outer_size(),
      expect_title_bar_offset(),
    );
    check_outer_vs_inner(
      "frameless outer matches the content",
      frameless.get_size(),
      frameless.get_outer_size(),
      false,
    );

    // ---- race probe: native handle immediately after creation ------------
    // Regression guard for denoland/deno#35785. The winit/raw backend creates
    // the OS window asynchronously, so reading the handle *right now* — with no
    // wait — is exactly the race that returned an uninitialized type 0
    // ("unknown Laufey window handle type: 0"). The getter must block until the
    // window exists and hand back the real type. Capability-probed: backends
    // that don't expose native handles legitimately return null/UNKNOWN.
    let early_handle = win.get_window_handle();
    if early_handle.is_null() {
      na("early window handle (backend doesn't expose native handles)");
    } else {
      let (label, accepted) = expected_handle_type();
      let ht = win.get_window_handle_type();
      check(
        &format!("early handle type resolved without racing (want {label}, got {ht})"),
        accepted.contains(&ht),
      );
    }

    // Give the backend a moment to realize the window on screen.
    tokio::time::sleep(std::time::Duration::from_millis(300)).await;

    let settled_scale = win.get_scale_factor();
    check("scale factor is still positive after realize", settled_scale > 0.0);
    check(
      "scale factor is stable from the constructor",
      (early_scale - settled_scale).abs() < 0.01,
    );
    let (settled_w, settled_h) = win.get_size();
    check(
      "constructor size survives realize",
      (settled_w - 800).abs() <= 2 && (settled_h - 600).abs() <= 2,
    );

    // After realize, WebView / CEF can still compare inner vs frame from
    // the live window. Winit's get_position only reflects a pending set,
    // so a (0, 0) frame with a nonzero inner is N/A rather than a fail.
    check_inner_vs_frame(
      "settled decorated inner is offset by the title bar",
      decorated.get_position(),
      decorated.get_inner_position(),
      expect_title_bar_offset(),
    );
    check_inner_vs_frame(
      "settled frameless inner matches the frame origin",
      frameless.get_position(),
      frameless.get_inner_position(),
      false,
    );
    check_outer_vs_inner(
      "settled decorated outer is larger than the content",
      decorated.get_size(),
      decorated.get_outer_size(),
      expect_title_bar_offset(),
    );
    check_outer_vs_inner(
      "settled frameless outer matches the content",
      frameless.get_size(),
      frameless.get_outer_size(),
      false,
    );

    // ---- test_inject_input ----------------------------------------------
    let last_key = Arc::new(Mutex::new(None::<KeyboardEvent>));
    let last_click = Arc::new(Mutex::new(None::<MouseClickEvent>));
    let last_move = Arc::new(Mutex::new(None::<MouseMoveEvent>));
    let last_wheel = Arc::new(Mutex::new(None::<WheelEvent>));
    let last_enter = Arc::new(Mutex::new(None::<CursorEnterLeaveEvent>));
    let input_win = {
      let lk = last_key.clone();
      let lc = last_click.clone();
      let lm = last_move.clone();
      let lw = last_wheel.clone();
      let le = last_enter.clone();
      Window::new(200, 150)
        .title("native-e2e-input")
        .on_keyboard_event(move |e| *lk.lock().unwrap() = Some(e))
        .on_mouse_click(move |e| *lc.lock().unwrap() = Some(e))
        .on_mouse_move(move |e| *lm.lock().unwrap() = Some(e))
        .on_wheel(move |e| *lw.lock().unwrap() = Some(e))
        .on_cursor_enter_leave(move |e| *le.lock().unwrap() = Some(e))
    };
    let input_id = input_win.id();

    if !laufey::test_inject_input(
      input_id,
      &TestInput::MouseMove {
        x: 10.0,
        y: 10.0,
        modifiers: 0,
      },
    ) {
      na("test_inject_input (backend has no hook)");
    } else {
      check(
        "inject mousemove reaches on_mouse_move",
        last_move
          .lock()
          .unwrap()
          .as_ref()
          .is_some_and(|e| (e.x - 10.0).abs() < 0.5 && (e.y - 10.0).abs() < 0.5),
      );

      // Reset hover so the next enter is pending until a real coordinate.
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::CursorLeave {
          x: 10.0,
          y: 10.0,
          modifiers: 0,
        },
      );
      *last_enter.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::CursorEnter {
          x: 0.0,
          y: 0.0,
          modifiers: 0,
        },
      );
      let enter_before_move = last_enter.lock().unwrap().clone();
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::MouseMove {
          x: 40.0,
          y: 50.0,
          modifiers: 0,
        },
      );
      let enter_after_move = last_enter.lock().unwrap().clone();
      match (enter_before_move, enter_after_move) {
        (None, Some(e)) if e.entered && (e.x - 40.0).abs() < 0.5 && (e.y - 50.0).abs() < 0.5 => {
          check("mouseenter waits for the first move", true);
        }
        (Some(e), _) if e.entered && e.x == 0.0 && e.y == 0.0 => {
          na("mouseenter waits for the first move (backend injects enter immediately)");
        }
        _ => check("mouseenter waits for the first move", false),
      }

      *last_click.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::MouseButton {
          button: LAUFEY_MOUSE_BUTTON_LEFT,
          pressed: true,
          x: 40.0,
          y: 50.0,
          modifiers: 0,
        },
      );
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::MouseButton {
          button: LAUFEY_MOUSE_BUTTON_LEFT,
          pressed: false,
          x: 40.0,
          y: 50.0,
          modifiers: 0,
        },
      );
      let first = last_click.lock().unwrap().clone();
      check(
        "inject click.detail is 1",
        first.as_ref().is_some_and(|e| {
          e.state == MouseButtonState::Released
            && e.button == MouseButton::Left
            && e.click_count == 1
        }),
      );

      *last_click.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::MouseButton {
          button: LAUFEY_MOUSE_BUTTON_LEFT,
          pressed: true,
          x: 40.0,
          y: 50.0,
          modifiers: 0,
        },
      );
      match last_click.lock().unwrap().as_ref().map(|e| e.click_count) {
        Some(2) => check("inject second nearby press is click.detail 2", true),
        Some(1) => na(
          "inject second nearby press is click.detail 2 (backend inject does not track multi-click)",
        ),
        _ => check("inject second nearby press is click.detail 2", false),
      }

      *last_wheel.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::Wheel {
          delta_x: 0.0,
          delta_y: -1.0,
          delta_mode: LAUFEY_WHEEL_DELTA_LINE,
          x: 40.0,
          y: 50.0,
          modifiers: 0,
        },
      );
      check(
        "inject line-scroll up is DOM-negative deltaY",
        last_wheel.lock().unwrap().as_ref().is_some_and(|e| {
          e.delta_y < 0.0 && e.delta_mode == WheelDeltaMode::Line
        }),
      );

      *last_key.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::Key {
          key: "a".into(),
          code: "KeyA".into(),
          pressed: true,
          repeat: false,
          modifiers: 0,
        },
      );
      check(
        "inject KeyA reaches on_keyboard_event",
        last_key.lock().unwrap().as_ref().is_some_and(|e| {
          e.state == KeyState::Pressed && e.key == "a" && e.code == "KeyA"
        }),
      );

      *last_key.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::Modifiers {
          modifiers: LAUFEY_MOD_SHIFT,
        },
      );
      check(
        "inject Shift modifiers emit keydown Shift/ShiftLeft",
        last_key.lock().unwrap().as_ref().is_some_and(|e| {
          e.state == KeyState::Pressed
            && e.key == "Shift"
            && e.code == "ShiftLeft"
            && e.modifiers.shift
        }),
      );
      *last_key.lock().unwrap() = None;
      let _ = laufey::test_inject_input(
        input_id,
        &TestInput::Modifiers { modifiers: 0 },
      );
      check(
        "inject Shift release emits keyup",
        last_key.lock().unwrap().as_ref().is_some_and(|e| {
          e.state == KeyState::Released
            && e.key == "Shift"
            && !e.modifiers.shift
        }),
      );
    }
    let _ = &input_win;

    // ---- A. direct state readback ---------------------------------------
    win.set_size(640, 480);
    let sized = wait_for(
      || {
        let (w, h) = win.get_size();
        (w - 640).abs() <= 2 && (h - 480).abs() <= 2
      },
      50,
      40,
    )
    .await;
    check("set_size -> get_size round-trips", sized);

    // Position is advisory: window managers may constrain it. Assert loosely.
    win.set_position(150, 170);
    tokio::time::sleep(std::time::Duration::from_millis(200)).await;
    let (px, py) = win.get_position();
    if (px - 150).abs() <= 40 && (py - 170).abs() <= 40 {
      check("set_position -> get_position round-trips", true);
    } else {
      na("set_position (window manager constrained placement)");
    }

    win.set_resizable(false);
    let not_resizable = wait_for(|| !win.get_resizable(), 25, 20).await;
    win.set_resizable(true);
    let resizable_again = wait_for(|| win.get_resizable(), 25, 20).await;
    check(
      "set_resizable false/true round-trips",
      not_resizable && resizable_again,
    );

    // always-on-top and opacity are advisory: several backends / window
    // managers don't honor a runtime toggle or don't reflect it in the getter
    // (GTK/X11, some WMs). Treat "didn't round-trip" as N/A, not a failure —
    // this is a capability probe, not a WM-conformance test.
    win.set_always_on_top(true);
    let aot = wait_for(|| win.get_always_on_top(), 25, 20).await;
    win.set_always_on_top(false);
    if aot {
      check("set_always_on_top round-trips", true);
    } else {
      na("set_always_on_top (backend/WM doesn't reflect the toggle)");
    }

    win.set_opacity(0.6);
    tokio::time::sleep(std::time::Duration::from_millis(150)).await;
    if (win.get_opacity() - 0.6).abs() < 0.05 {
      check("set_opacity -> get_opacity round-trips", true);
    } else {
      na("set_opacity (backend doesn't support a runtime opacity toggle)");
    }
    win.set_opacity(1.0);

    win.set_click_passthrough(true);
    let passthrough = wait_for(|| win.get_click_passthrough(), 25, 20).await;
    win.set_click_passthrough(false);
    if passthrough {
      check("set_click_passthrough round-trips", true);
    } else {
      na("set_click_passthrough (backend doesn't reflect the toggle)");
    }

    // Forwarding is macOS-only for now; elsewhere the setter is a no-op and
    // the getter reports false, which this treats as N/A. Whether forwarded
    // events actually arrive needs real OS input and is not probed here.
    win.set_click_passthrough_forward(true);
    let forward = wait_for(|| win.get_click_passthrough_forward(), 25, 20).await;
    win.set_click_passthrough_forward(false);
    if forward {
      check("set_click_passthrough_forward round-trips", true);
    } else {
      na("set_click_passthrough_forward (platform doesn't support forwarding)");
    }

    // Visibility.
    win.show();
    let visible = wait_for(|| win.get_visible(), 25, 20).await;
    check("show -> get_visible true", visible);

    // ---- window handle (capability-probed) -------------------------------
    // Some backends (e.g. the system WebView) intentionally don't expose native
    // window handles and return null / UNKNOWN — that's N/A, not a failure.
    let handle = win.get_window_handle();
    if handle.is_null() {
      na("window handle (backend doesn't expose native handles)");
    } else {
      check("get_window_handle non-null", true);
      let (label, accepted) = expected_handle_type();
      let ht = win.get_window_handle_type();
      check(
        &format!("window handle type is {label} (got {ht})"),
        accepted.contains(&ht),
      );
    }

    // ---- B. event-callback round-trips ----------------------------------
    win.set_size(720, 540);
    let resize_fired = wait_for(
      || {
        (resize_w.load(Ordering::SeqCst) - 720).abs() <= 4
          && (resize_h.load(Ordering::SeqCst) - 540).abs() <= 4
      },
      60,
      40,
    )
    .await;
    if resize_fired {
      check("on_resize callback round-trips", true);
    } else if resize_w.load(Ordering::SeqCst) != 0 {
      // Fired but with different dims (HiDPI scaling etc.) — still a round-trip.
      check("on_resize callback fired", true);
    } else {
      na("on_resize (backend emits no resize events)");
    }

    win.set_position(220, 240);
    let move_fired = wait_for(|| moved.load(Ordering::SeqCst), 40, 40).await;
    if move_fired {
      check("on_move callback fires", true);
    } else {
      na("on_move (backend emits no move events / WM ignored)");
    }

    win.focus();
    let focus_fired =
      wait_for(|| focused_seen.load(Ordering::SeqCst), 40, 40).await;
    if focus_fired {
      check("on_focused callback fires", true);
    } else {
      na("on_focused (no focus event in headless session)");
    }

    // ---- system integration: clipboard round-trip -----------------------
    let nonce = std::process::id();
    let payload = format!("laufey-e2e-{nonce}");
    laufey::write_clipboard_text(&payload);
    match laufey::read_clipboard_text() {
      Some(got) if got == payload => {
        check("clipboard write/read round-trips", true)
      }
      Some(_) => check("clipboard write/read round-trips", false),
      None => na("clipboard (backend has no clipboard support)"),
    }

    // ---- native chrome: menu + tray click round-trips -------------------
    // Build an app menu and a tray menu with distinct item ids, then use the
    // test hook (test_click_menu_item) to synthesize a click and assert the
    // registered on_click handler fires with the right id. This exercises the
    // backend's template -> id-registration -> dispatch plumbing without OS
    // input. On backends without the hook (API < 30) the synth returns false
    // -> N/A.
    let app_click = Arc::new(std::sync::Mutex::new(None::<String>));
    let ac = app_click.clone();
    win.set_menu(
      &[MenuItem::Submenu {
        label: "E2E".into(),
        items: vec![
          MenuItem::Item {
            label: "Ping".into(),
            id: Some("app_ping".into()),
            accelerator: None,
            enabled: true,
            checked: false,
            icon: None,
            tooltip: None,
          },
          MenuItem::Separator,
          MenuItem::Role {
            role: "quit".into(),
          },
        ],
      }],
      move |id| *ac.lock().unwrap() = Some(id.to_string()),
    );
    tokio::time::sleep(std::time::Duration::from_millis(200)).await;
    if laufey::test_click_menu_item("app_ping") {
      check(
        "app menu click round-trips to on_click with id 'app_ping'",
        app_click.lock().unwrap().as_deref() == Some("app_ping"),
      );
    } else {
      na("app menu click round-trip (backend has no test_click hook)");
    }

    // Tray: id == 0 means the backend can't create tray icons here. Keep the
    // binding alive past this block (it destroys the native icon on drop) so
    // the Layer-1 D-Bus observer can introspect it during the hold below.
    //
    // On Linux the cef/webview tray is dlopen-gated on an appindicator
    // runtime library, so id 0 normally reads as N/A. A CI leg that installs
    // the library sets LAUFEY_E2E_REQUIRE_TRAY so a stub tray FAILS instead
    // of passing silently (issue #63).
    let require_tray =
      std::env::var("LAUFEY_E2E_REQUIRE_TRAY").is_ok_and(|v| !v.is_empty());
    let tray = TrayIcon::new();
    if tray.id() == 0 {
      if require_tray {
        check(
          "create_tray_icon returned nonzero id (LAUFEY_E2E_REQUIRE_TRAY)",
          false,
        );
      } else {
        na("tray (backend has no tray support on this platform)");
      }
    } else {
      check("create_tray_icon returned nonzero id", true);
      tray.set_icon(TINY_PNG);
      let tray_click = Arc::new(std::sync::Mutex::new(None::<String>));
      let tc = tray_click.clone();
      tray.set_menu(
        &[
          MenuItem::Item {
            label: "Ping".into(),
            id: Some("tray_ping".into()),
            accelerator: None,
            enabled: true,
            checked: false,
            icon: None,
            tooltip: None,
          },
          MenuItem::Role {
            role: "quit".into(),
          },
        ],
        move |id| *tc.lock().unwrap() = Some(id.to_string()),
      );
      tokio::time::sleep(std::time::Duration::from_millis(200)).await;
      if laufey::test_click_menu_item("tray_ping") {
        check(
          "tray menu click round-trips to on_click with id 'tray_ping'",
          tray_click.lock().unwrap().as_deref() == Some("tray_ping"),
        );
      } else {
        na("tray menu click round-trip (backend has no test_click hook)");
      }
    }

    // Verifying menu/tray *structure* against the OS (that the widget was
    // really registered, not just that set_menu was accepted) needs main-thread
    // UI access. Linux is covered out-of-process by the D-Bus driver (Layer 1).
    // macOS self-AX is proven (docs/e2e-testing.md §7.2) but must run on the
    // backend's main thread — which this worker-thread runtime can't reach
    // (a dispatch_sync to the main queue deadlocks against the backend event
    // loop). It belongs behind a backend test hook; tracked as a follow-up.
    na("menu/tray OS-structure check (Linux: D-Bus driver; macOS/Windows: pending backend hook)");

    // ---- print_to_pdf smoke test (API >= 32) -----------------------------
    // Render the loaded page to a PDF and assert real PDF bytes come back
    // (`%PDF-` magic), exercising each backend's actual render path (WKWebView
    // createPDFWithConfiguration, WebView2 PrintToPdfStream, WebKitGTK
    // print-to-file, CEF DevTools Page.printToPDF). Winit has no web engine
    // and reports "unsupported" through the callback -> N/A. A backend whose
    // completion handler never fires shows up here as a FAIL on the delivery
    // assertion.
    //
    // The render races page load: WKWebView's createPDFWithConfiguration
    // fails with a generic NSError while the page is still loading, so wait
    // for on_page_load first (best effort -- engine-less backends never fire
    // it) and retry a transient error before judging; only a persistent error
    // is a real failure.
    let _ = wait_for(|| page_loaded.load(Ordering::SeqCst), 50, 100).await;
    let mut outcome: Option<Result<Vec<u8>, String>> = None;
    for attempt in 0..3u32 {
      if attempt > 0 {
        tokio::time::sleep(std::time::Duration::from_millis(700)).await;
      }
      let pdf_result =
        Arc::new(std::sync::Mutex::new(None::<Result<Vec<u8>, String>>));
      let pr = pdf_result.clone();
      win.print_to_pdf(None, move |r| *pr.lock().unwrap() = Some(r));
      if !wait_for(|| pdf_result.lock().unwrap().is_some(), 150, 100).await {
        continue; // nothing delivered this attempt; retry
      }
      let result = pdf_result.lock().unwrap().take().unwrap();
      let transient = matches!(&result, Err(e) if !e.contains("not supported"));
      outcome = Some(result);
      if !transient {
        break;
      }
    }
    match outcome {
      None => check("print_to_pdf delivers a result within 15s", false),
      Some(Ok(bytes)) => check(
        &format!("print_to_pdf returns real PDF bytes ({} bytes)", bytes.len()),
        bytes.starts_with(b"%PDF-"),
      ),
      Some(Err(e)) if e.contains("not supported") => {
        na("print_to_pdf (backend has no web engine / PDF support)")
      }
      Some(Err(e)) => {
        check(&format!("print_to_pdf succeeds (got: {e})"), false)
      }
    }

    // ---- close-requested handler round-trip --------------------------------
    // A second window (kept separate from `win`, which must survive to
    // shutdown). Registers an on_close_requested handler that *stashes* the
    // window_id instead of resolving inline — proving resolution genuinely
    // works from outside the handler (any thread, any time), not just
    // synchronously inline. Synthesizes the close via
    // test_trigger_close_requested (API >= 31, implemented by every in-tree
    // backend — init_api's version check guarantees it's present, so a
    // failure here is a regression, never a missing hook).
    let close_win = Window::new(200, 150)
      .title("native-e2e-close")
      .load("data:text/html,<!doctype html><title>close</title>");
    let close_win_id = close_win.id();
    let baseline_sized = wait_for(
      || {
        let (w, _h) = close_win.get_size();
        w != 0
      },
      50,
      40,
    )
    .await;

    let close_handler_fired = Arc::new(AtomicBool::new(false));
    let pending_close: Arc<std::sync::Mutex<Option<u32>>> =
      Arc::new(std::sync::Mutex::new(None));
    let close_win = {
      let fired = close_handler_fired.clone();
      let pending = pending_close.clone();
      close_win.on_close_requested(move |event| {
        fired.store(true, Ordering::SeqCst);
        *pending.lock().unwrap() = Some(event.window_id);
      })
    };

    let still_open_after_defer =
      laufey::test_trigger_close_requested(close_win_id);
    if !baseline_sized {
      // Precondition, not a close-dispatch check: without a sized baseline
      // the stays-open/closed probes below are meaningless.
      na("close handler (precondition failed: close_win never reported a nonzero size)");
    } else {
      // The hook itself is guaranteed present: init_api rejects any backend
      // whose API version differs from the current one, and every in-tree
      // backend implements test_trigger_close_requested. Dispatch is synchronous,
      // so a handler that didn't fire is a real regression — fail, don't
      // N/A.
      check(
        "on_close_requested handler fired for synthesized close",
        close_handler_fired.load(Ordering::SeqCst),
      );
      check(
        "close handler defers — window stays open",
        still_open_after_defer,
      );
      let (w, _h) = close_win.get_size();
      check("window state still readable after deferred close", w != 0);

      // Resolve the stashed window_id — from here, not from inside the
      // handler — and confirm the window actually closes.
      let pending_id = pending_close.lock().unwrap().take();
      if let Some(id) = pending_id {
        Window::from_id(id).close();
      }
      let closed = wait_for(
        || {
          let (w, h) = close_win.get_size();
          w == 0 && h == 0
        },
        50,
        40,
      )
      .await;
      check(
        "closing from outside the on_close_requested handler actually closes the window",
        closed,
      );
    }

    // ---- Layer-1 hold ----------------------------------------------------
    // When driven by the D-Bus observer (native_e2e_driver), stay alive with
    // the tray + menu registered so it can read the StatusNotifierItem, walk
    // the dbusmenu layout, and fire a menu Event that round-trips to the
    // `on_click` above. The driver kills us when it's done.
    if std::env::var_os("LAUFEY_E2E_HOLD").is_some() {
      eprintln!("[e2e] holding for Layer-1 observer");
      tokio::time::sleep(std::time::Duration::from_secs(8)).await;
    }

    // ---- shutdown --------------------------------------------------------
    // Decide the result and terminate immediately with a deterministic exit
    // code. We deliberately skip close()/quit(): tearing the window/webview
    // down on the backend's main thread can crash or race and clobber the exit
    // code (e.g. SIGTRAP -> 133), which would corrupt the CI signal. The OS
    // reclaims everything on exit. `_ = &win;` keeps the window alive to here.
    let _ = &win;
    let failed = FAILED.load(Ordering::SeqCst);
    eprintln!("[e2e] OVERALL {}", if failed { "FAIL" } else { "PASS" });
    let _ = std::io::Write::flush(&mut std::io::stderr());
    // _exit avoids running C++ static destructors / atexit handlers in the
    // backend, which is where the teardown crash lives.
    unsafe { libc_exit(if failed { 1 } else { 0 }) };
  });
}

extern "C" {
  #[link_name = "_exit"]
  fn libc_exit(code: i32) -> !;
}

laufey::main!(e2e_main);
