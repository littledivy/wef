// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.
//
// Drop the extra keyDown Japanese IMEs post for the same physical key.

#ifndef LAUFEY_IME_KEY_ECHO_H_
#define LAUFEY_IME_KEY_ECHO_H_

#include <laufey.h>

#include <chrono>
#include <string>

namespace laufey_common {

struct ImeKeyEcho {
  std::string code;
  std::chrono::steady_clock::time_point time{};
  bool valid = false;
};

// `last` is the press that has not been released yet. An echo is an extra
// keydown of that same physical key with no keyup between.
inline bool ConsumeImeKeyEcho(ImeKeyEcho* last, int state, bool repeat,
                              const char* code) {
  if (!last) {
    return false;
  }
  std::string c = code ? code : "";
  if (state != LAUFEY_KEY_PRESSED) {
    if (last->valid && last->code == c) {
      last->valid = false;
    }
    return false;
  }
  // Windows' fastest auto-repeat is ~33ms, inside the 40ms window.
  if (repeat) {
    return false;
  }
  auto now = std::chrono::steady_clock::now();
  // Match `code` only. IME-consumed keys all report logical key "Process".
  if (last->valid &&
      (now - last->time) < std::chrono::milliseconds(40) && !c.empty() &&
      c != "Unidentified" && c == last->code) {
    return true;
  }
  last->code = std::move(c);
  last->time = now;
  last->valid = true;
  return false;
}

}  // namespace laufey_common

#endif  // LAUFEY_IME_KEY_ECHO_H_
