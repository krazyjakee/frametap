#include "windows_backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <dxgi.h>
#include <dwmapi.h>

#include <string>
#include <vector>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

namespace frametap::internal {

// ---------------------------------------------------------------------------
// Monitor enumeration via DXGI
// ---------------------------------------------------------------------------

std::vector<Monitor> enumerate_monitors() {
  std::vector<Monitor> result;

  IDXGIFactory1 *factory = nullptr;
  HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                   reinterpret_cast<void **>(&factory));
  if (FAILED(hr) || !factory)
    return result;

  IDXGIAdapter1 *adapter = nullptr;
  for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
    IDXGIOutput *output = nullptr;
    for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
      DXGI_OUTPUT_DESC desc{};
      output->GetDesc(&desc);

      MONITORINFOEXW mi{};
      mi.cbSize = sizeof(mi);
      GetMonitorInfoW(desc.Monitor, &mi);

      // Get DPI scale
      HDC hdc = CreateDCW(L"DISPLAY", mi.szDevice, nullptr, nullptr);
      float scale = 1.0f;
      if (hdc) {
        int logical_w = GetDeviceCaps(hdc, HORZRES);
        int physical_w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        if (logical_w > 0)
          scale = static_cast<float>(physical_w) / static_cast<float>(logical_w);
        DeleteDC(hdc);
      }

      // Convert wide name to UTF-8
      std::string name;
      int len = WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1,
                                     nullptr, 0, nullptr, nullptr);
      if (len > 0) {
        name.resize(len - 1);
        WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1,
                            name.data(), len, nullptr, nullptr);
      }

      Monitor m{
          .id = static_cast<int>(result.size()),
          .name = std::move(name),
          .x = static_cast<int>(desc.DesktopCoordinates.left),
          .y = static_cast<int>(desc.DesktopCoordinates.top),
          .width = static_cast<int>(desc.DesktopCoordinates.right -
                                    desc.DesktopCoordinates.left),
          .height = static_cast<int>(desc.DesktopCoordinates.bottom -
                                     desc.DesktopCoordinates.top),
          .scale = scale,
      };
      result.push_back(std::move(m));

      output->Release();
    }
    adapter->Release();
  }

  factory->Release();
  return result;
}

// ---------------------------------------------------------------------------
// Window enumeration via EnumWindows
// ---------------------------------------------------------------------------

struct EnumState {
  std::vector<Window> *windows;
};

static BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lparam) {
  auto *state = reinterpret_cast<EnumState *>(lparam);

  if (!IsWindowVisible(hwnd))
    return TRUE;

  // Skip windows with no title
  int title_len = GetWindowTextLengthW(hwnd);
  if (title_len <= 0)
    return TRUE;

  // Skip tool windows, cloaked windows (UWP hidden), etc.
  LONG ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
  if (ex_style & WS_EX_TOOLWINDOW)
    return TRUE;

  // Skip DWM-cloaked windows (hidden UWP apps, virtual desktop windows)
  BOOL cloaked = FALSE;
  DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
  if (cloaked)
    return TRUE;

  // Get window title as UTF-8
  std::wstring wtitle(title_len + 1, L'\0');
  GetWindowTextW(hwnd, wtitle.data(), title_len + 1);

  std::string name;
  int len = WideCharToMultiByte(CP_UTF8, 0, wtitle.c_str(), -1,
                                 nullptr, 0, nullptr, nullptr);
  if (len > 0) {
    name.resize(len - 1);
    WideCharToMultiByte(CP_UTF8, 0, wtitle.c_str(), -1,
                        name.data(), len, nullptr, nullptr);
  }

  // Get window bounds — prefer DWM extended frame bounds for accuracy
  RECT rect{};
  HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                      &rect, sizeof(rect));
  if (FAILED(hr))
    GetWindowRect(hwnd, &rect);

  Window w{
      .id = reinterpret_cast<uint64_t>(hwnd),
      .name = std::move(name),
      .x = static_cast<int>(rect.left),
      .y = static_cast<int>(rect.top),
      .width = static_cast<int>(rect.right - rect.left),
      .height = static_cast<int>(rect.bottom - rect.top),
  };
  state->windows->push_back(std::move(w));

  return TRUE;
}

std::vector<Window> enumerate_windows() {
  std::vector<Window> result;
  EnumState state{&result};
  EnumWindows(enum_windows_proc, reinterpret_cast<LPARAM>(&state));
  return result;
}

// ---------------------------------------------------------------------------
// Permission diagnostics
// ---------------------------------------------------------------------------

PermissionCheck check_platform_permissions() {
  PermissionCheck result;
  result.summary = "Windows";

  // Check DXGI availability by trying to create a factory
  IDXGIFactory1 *factory = nullptr;
  HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                   reinterpret_cast<void **>(&factory));
  if (SUCCEEDED(hr) && factory) {
    // Check if Desktop Duplication is available (needs an output)
    IDXGIAdapter1 *adapter = nullptr;
    IDXGIOutput *output = nullptr;
    bool has_output = false;

    for (UINT ai = 0;
         factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
      if (adapter->EnumOutputs(0, &output) != DXGI_ERROR_NOT_FOUND) {
        has_output = true;
        output->Release();
        adapter->Release();
        break;
      }
      adapter->Release();
    }
    factory->Release();

    if (has_output) {
      result.status = PermissionStatus::ok;
      result.details.push_back("DXGI Desktop Duplication available.");
    } else {
      result.status = PermissionStatus::warning;
      result.details.push_back(
          "No DXGI outputs found. This may happen in RDP sessions "
          "or headless environments. GDI fallback will be used.");
    }
  } else {
    result.status = PermissionStatus::warning;
    result.details.push_back(
        "DXGI unavailable. GDI fallback will be used for capture.");
  }

  // Check if running in a Remote Desktop session
  if (GetSystemMetrics(SM_REMOTESESSION)) {
    if (result.status == PermissionStatus::ok)
      result.status = PermissionStatus::warning;
    result.details.push_back(
        "Remote Desktop session detected. DXGI Desktop Duplication "
        "may not work; GDI fallback will be used.");
  }

  if (result.status == PermissionStatus::ok)
    result.summary = "Windows (DXGI)";
  else
    result.summary = "Windows (GDI fallback)";

  return result;
}

} // namespace frametap::internal
