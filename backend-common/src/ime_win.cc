// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

#include "laufey_backend_common.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imm.h>

#include <map>
#include <mutex>
#include <string>

#pragma comment(lib, "imm32.lib")

namespace laufey_common {
namespace {

struct SubclassState {
  WNDPROC old_proc = nullptr;
  uint32_t window_id = 0;
  ImeNoteFn note = nullptr;
};

std::mutex g_mu;
std::map<HWND, SubclassState> g_subclasses;

std::string ImmString(HIMC imc, DWORD idx) {
  LONG bytes = ImmGetCompositionStringW(imc, idx, nullptr, 0);
  if (bytes <= 0) {
    return {};
  }
  std::wstring w(static_cast<size_t>(bytes) / sizeof(wchar_t), L'\0');
  ImmGetCompositionStringW(imc, idx, w.data(), bytes);
  while (!w.empty() && w.back() == L'\0') {
    w.pop_back();
  }
  return WideToUtf8(w);
}

void HandleImeOnHwnd(HWND hwnd, UINT msg, LPARAM lParam, uint32_t window_id,
                     ImeNoteFn note) {
  if (!note) {
    return;
  }
  switch (msg) {
    case WM_IME_STARTCOMPOSITION:
      note(window_id, true, {});
      break;
    case WM_IME_COMPOSITION: {
      HIMC imc = ImmGetContext(hwnd);
      if (!imc) {
        break;
      }
      if (lParam & GCS_RESULTSTR) {
        note(window_id, false, ImmString(imc, GCS_RESULTSTR));
      } else if (lParam & GCS_COMPSTR) {
        note(window_id, true, ImmString(imc, GCS_COMPSTR));
      }
      ImmReleaseContext(hwnd, imc);
      break;
    }
    case WM_IME_ENDCOMPOSITION:
      note(window_id, false, {});
      break;
    default:
      break;
  }
}

bool IsImeMessage(UINT msg) {
  return msg == WM_IME_STARTCOMPOSITION || msg == WM_IME_COMPOSITION ||
         msg == WM_IME_ENDCOMPOSITION;
}

LRESULT CALLBACK ImeSubclassProc(HWND hwnd, UINT msg, WPARAM wParam,
                                 LPARAM lParam) {
  SubclassState state;
  {
    std::lock_guard<std::mutex> lock(g_mu);
    auto it = g_subclasses.find(hwnd);
    if (it == g_subclasses.end()) {
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    state = it->second;
  }
  if (IsImeMessage(msg)) {
    HandleImeOnHwnd(hwnd, msg, lParam, state.window_id, state.note);
  }
  if (msg == WM_NCDESTROY) {
    WNDPROC old = state.old_proc;
    {
      std::lock_guard<std::mutex> lock(g_mu);
      g_subclasses.erase(hwnd);
    }
    return CallWindowProcW(old, hwnd, msg, wParam, lParam);
  }
  return CallWindowProcW(state.old_proc, hwnd, msg, wParam, lParam);
}

void SubclassHwnd(HWND hwnd, uint32_t window_id, ImeNoteFn note) {
  if (!hwnd || !note) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  if (g_subclasses.count(hwnd)) {
    g_subclasses[hwnd].window_id = window_id;
    g_subclasses[hwnd].note = note;
    return;
  }
  SubclassState state;
  state.window_id = window_id;
  state.note = note;
  state.old_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
      hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(ImeSubclassProc)));
  if (state.old_proc) {
    g_subclasses[hwnd] = state;
  }
}

BOOL CALLBACK EnumChromeChild(HWND hwnd, LPARAM lParam) {
  wchar_t cls[64] = {};
  GetClassNameW(hwnd, cls, 64);
  if (wcsstr(cls, L"Chrome_WidgetWin") ||
      wcsstr(cls, L"Chrome_RenderWidgetHostHWND") ||
      wcsstr(cls, L"Intermediate D3D Window")) {
    auto* ctx = reinterpret_cast<std::pair<uint32_t, ImeNoteFn>*>(lParam);
    SubclassHwnd(hwnd, ctx->first, ctx->second);
  }
  return TRUE;
}

}  // namespace

void HandleWinImeMessage(void* hwnd, unsigned msg, unsigned long long,
                         long long lparam, uint32_t window_id, ImeNoteFn note) {
  HandleImeOnHwnd(static_cast<HWND>(hwnd), static_cast<UINT>(msg),
                  static_cast<LPARAM>(lparam), window_id, note);
}

void InstallWinImeObserver(void* hwnd_void, uint32_t window_id,
                           ImeNoteFn note) {
  HWND hwnd = static_cast<HWND>(hwnd_void);
  if (!hwnd || !note) {
    return;
  }
  SubclassHwnd(hwnd, window_id, note);
  std::pair<uint32_t, ImeNoteFn> ctx{window_id, note};
  EnumChildWindows(hwnd, EnumChromeChild, reinterpret_cast<LPARAM>(&ctx));
}

void UninstallWinImeObserver(void* hwnd_void) {
  HWND hwnd = static_cast<HWND>(hwnd_void);
  if (!hwnd) {
    return;
  }
  std::lock_guard<std::mutex> lock(g_mu);
  auto it = g_subclasses.find(hwnd);
  if (it != g_subclasses.end()) {
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                      reinterpret_cast<LONG_PTR>(it->second.old_proc));
    g_subclasses.erase(it);
  }
}

}  // namespace laufey_common
