// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Test-only click registry shared by the menu builders. Maps a menu/tray item
// id to its on_click handler so automated e2e tests can synthesize a click
// (via the `test_click_menu_item` C ABI hook) without OS-level input injection.
// See docs/e2e-testing.md.

#include "laufey_backend_common.h"

#include <map>
#include <mutex>
#include <string>

namespace laufey_common {

namespace {

struct MenuClickEntry {
  laufey_menu_click_fn fn;
  void* data;
  uint32_t window_id;
};

std::mutex& MenuClickMutex() {
  static std::mutex m;
  return m;
}

std::map<std::string, MenuClickEntry>& MenuClickMap() {
  static std::map<std::string, MenuClickEntry> m;
  return m;
}

}  // namespace

void RegisterMenuClick(const std::string& id, laufey_menu_click_fn fn,
                       void* data, uint32_t window_id) {
  if (id.empty() || !fn)
    return;
  std::lock_guard<std::mutex> lock(MenuClickMutex());
  MenuClickMap()[id] = MenuClickEntry{fn, data, window_id};
}

bool TestClickMenuItem(const char* item_id) {
  if (!item_id)
    return false;
  std::string id(item_id);
  MenuClickEntry entry;
  {
    std::lock_guard<std::mutex> lock(MenuClickMutex());
    auto it = MenuClickMap().find(id);
    if (it == MenuClickMap().end())
      return false;
    entry = it->second;
  }
  // Invoke outside the lock: the handler may re-enter the menu system.
  entry.fn(entry.data, entry.window_id, id.c_str());
  return true;
}

namespace {

std::mutex& InjectMutex() {
  static std::mutex m;
  return m;
}

std::map<uint32_t, uint32_t>& InjectModifiers() {
  static std::map<uint32_t, uint32_t> m;
  return m;
}

void EmitModifierEdges(uint32_t window_id, uint32_t prev, uint32_t next,
                       const TestInjectSink& sink) {
  auto edge = [&](uint32_t bit, const char* key, const char* code) {
    bool was = (prev & bit) != 0;
    bool now = (next & bit) != 0;
    if (was == now || !sink.key)
      return;
    sink.key(sink.ctx, window_id,
             now ? LAUFEY_KEY_PRESSED : LAUFEY_KEY_RELEASED, key, code, next,
             false);
  };
  edge(LAUFEY_MOD_SHIFT, "Shift", "ShiftLeft");
  edge(LAUFEY_MOD_CONTROL, "Control", "ControlLeft");
  edge(LAUFEY_MOD_ALT, "Alt", "AltLeft");
  edge(LAUFEY_MOD_META, "Meta", "MetaLeft");
}

}  // namespace

bool TestInjectInput(uint32_t window_id, const laufey_test_input_t* event,
                     const TestInjectSink& sink) {
  if (!event)
    return false;
  switch (event->kind) {
    case LAUFEY_TEST_INPUT_KEY:
      if (!sink.key)
        return false;
      sink.key(sink.ctx, window_id,
               event->pressed ? LAUFEY_KEY_PRESSED : LAUFEY_KEY_RELEASED,
               event->key ? event->key : "", event->code ? event->code : "",
               event->modifiers, event->repeat);
      return true;
    case LAUFEY_TEST_INPUT_MOUSE_MOVE:
      if (!sink.move)
        return false;
      sink.move(sink.ctx, window_id, event->x, event->y, event->modifiers);
      return true;
    case LAUFEY_TEST_INPUT_MOUSE_BUTTON:
      if (!sink.click)
        return false;
      sink.click(sink.ctx, window_id,
                 event->pressed ? LAUFEY_MOUSE_PRESSED : LAUFEY_MOUSE_RELEASED,
                 event->button, event->x, event->y, event->modifiers, 1);
      return true;
    case LAUFEY_TEST_INPUT_WHEEL:
      if (!sink.wheel)
        return false;
      sink.wheel(sink.ctx, window_id, event->delta_x, event->delta_y, event->x,
                 event->y, event->modifiers, event->delta_mode);
      return true;
    case LAUFEY_TEST_INPUT_CURSOR_ENTER:
      if (!sink.enter_leave)
        return false;
      sink.enter_leave(sink.ctx, window_id, 1, event->x, event->y,
                       event->modifiers);
      return true;
    case LAUFEY_TEST_INPUT_CURSOR_LEAVE:
      if (!sink.enter_leave)
        return false;
      sink.enter_leave(sink.ctx, window_id, 0, event->x, event->y,
                       event->modifiers);
      return true;
    case LAUFEY_TEST_INPUT_MODIFIERS: {
      uint32_t prev = 0;
      {
        std::lock_guard<std::mutex> lock(InjectMutex());
        auto& slot = InjectModifiers()[window_id];
        prev = slot;
        slot = event->modifiers;
      }
      EmitModifierEdges(window_id, prev, event->modifiers, sink);
      return true;
    }
    default:
      return false;
  }
}

}  // namespace laufey_common
