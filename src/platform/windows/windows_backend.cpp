#include "windows_backend.h"
#include "../../util/color.h"
#include "../../util/safe_alloc.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dwmapi.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dwmapi.lib")

namespace frametap::internal {

// Declared in windows_screenshot.cpp
ImageData windows_screenshot_monitor(int monitor_index, Rect region);
ImageData windows_screenshot_window(HWND hwnd, Rect region);

// ---------------------------------------------------------------------------
// DXGI resources — held for the lifetime of a streaming session
// ---------------------------------------------------------------------------

struct DxgiState {
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  IDXGIOutputDuplication *duplication = nullptr;
  ID3D11Texture2D *staging = nullptr;
  int width = 0;
  int height = 0;

  bool init(int monitor_index) {
    IDXGIFactory1 *factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                     reinterpret_cast<void **>(&factory));
    if (FAILED(hr))
      return false;

    IDXGIAdapter1 *adapter = nullptr;
    IDXGIOutput *output = nullptr;
    int current = 0;

    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
      for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
        if (current == monitor_index)
          goto found;
        ++current;
        output->Release();
        output = nullptr;
      }
      adapter->Release();
      adapter = nullptr;
    }
    factory->Release();
    return false;

  found:
    DXGI_OUTPUT_DESC out_desc{};
    output->GetDesc(&out_desc);
    width = out_desc.DesktopCoordinates.right - out_desc.DesktopCoordinates.left;
    height = out_desc.DesktopCoordinates.bottom - out_desc.DesktopCoordinates.top;

    D3D_FEATURE_LEVEL fl;
    hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                            nullptr, 0, D3D11_SDK_VERSION, &device, &fl,
                            &context);
    adapter->Release();
    factory->Release();

    if (FAILED(hr)) {
      output->Release();
      return false;
    }

    IDXGIOutput1 *output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1),
                                 reinterpret_cast<void **>(&output1));
    output->Release();
    if (FAILED(hr))
      return false;

    hr = output1->DuplicateOutput(device, &duplication);
    output1->Release();
    if (FAILED(hr))
      return false;

    // Create reusable staging texture
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    hr = device->CreateTexture2D(&td, nullptr, &staging);
    return SUCCEEDED(hr);
  }

  void release() {
    if (staging) { staging->Release(); staging = nullptr; }
    if (duplication) { duplication->Release(); duplication = nullptr; }
    if (context) { context->Release(); context = nullptr; }
    if (device) { device->Release(); device = nullptr; }
  }

  ~DxgiState() { release(); }
};

// ---------------------------------------------------------------------------
// WindowsBackend
// ---------------------------------------------------------------------------

class WindowsBackend : public Backend {
public:
  WindowsBackend() = default;

  explicit WindowsBackend(Rect region) : region_(region) {}

  explicit WindowsBackend(Monitor monitor)
      : monitor_index_(monitor.id) {}

  explicit WindowsBackend(Window window)
      : window_handle_(reinterpret_cast<HWND>(window.id)),
        capture_window_(true) {}

  ~WindowsBackend() override { stop(); }

  // -- Backend interface ----------------------------------------------------

  ImageData screenshot(Rect region) override {
    Rect r = (region.width > 0 && region.height > 0) ? region : region_;
    if (capture_window_)
      return windows_screenshot_window(window_handle_, r);
    return windows_screenshot_monitor(monitor_index_, r);
  }

  void start(FrameCallback cb) override {
    callback_ = std::move(cb);
    if (!callback_)
      return;

    capture_thread_ = std::jthread([this](std::stop_token token) {
      if (capture_window_)
        gdi_capture_loop(token);
      else
        dxgi_capture_loop(token);
    });
  }

  void stop() override {
    if (capture_thread_.joinable()) {
      capture_thread_.request_stop();
      capture_thread_.join();
    }
  }

  void pause() override {
    std::lock_guard lock(mutex_);
    paused_ = true;
  }

  void resume() override {
    std::lock_guard lock(mutex_);
    paused_ = false;
  }

  bool is_paused() const override {
    std::lock_guard lock(mutex_);
    return paused_;
  }

  void set_region(Rect region) override { region_ = region; }

private:
  // -- DXGI streaming (monitor capture) ------------------------------------

  void dxgi_capture_loop(std::stop_token token) {
    DxgiState dxgi;
    if (!dxgi.init(monitor_index_)) {
      // Fall back to GDI polling for monitors
      gdi_monitor_capture_loop(token);
      return;
    }

    auto last_time = std::chrono::steady_clock::now();

    while (!token.stop_requested()) {
      {
        std::lock_guard lock(mutex_);
        if (paused_) {
          std::this_thread::sleep_for(std::chrono::milliseconds(16));
          continue;
        }
      }

      DXGI_OUTDUPL_FRAME_INFO frame_info{};
      IDXGIResource *resource = nullptr;
      HRESULT hr = dxgi.duplication->AcquireNextFrame(100, &frame_info,
                                                       &resource);

      if (hr == DXGI_ERROR_WAIT_TIMEOUT)
        continue;

      if (hr == DXGI_ERROR_ACCESS_LOST) {
        // Desktop switch (e.g. UAC, lock screen, RDP)
        dxgi.release();
        if (!dxgi.init(monitor_index_))
          return;
        continue;
      }

      if (FAILED(hr))
        continue;

      // Only process if there are new pixels
      if (frame_info.LastPresentTime.QuadPart == 0) {
        resource->Release();
        dxgi.duplication->ReleaseFrame();
        continue;
      }

      ID3D11Texture2D *tex = nullptr;
      hr = resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                     reinterpret_cast<void **>(&tex));
      resource->Release();

      if (FAILED(hr)) {
        dxgi.duplication->ReleaseFrame();
        continue;
      }

      dxgi.context->CopyResource(dxgi.staging, tex);
      tex->Release();

      D3D11_MAPPED_SUBRESOURCE mapped{};
      hr = dxgi.context->Map(dxgi.staging, 0, D3D11_MAP_READ, 0, &mapped);

      if (SUCCEEDED(hr)) {
        // M8: Crop to region with bounds validation
        int src_x = 0, src_y = 0;
        int out_w = dxgi.width, out_h = dxgi.height;
        if (region_.width > 0 && region_.height > 0) {
          src_x = static_cast<int>(region_.x);
          src_y = static_cast<int>(region_.y);
          out_w = static_cast<int>(region_.width);
          out_h = static_cast<int>(region_.height);

          // Clamp negatives
          if (src_x < 0) { out_w += src_x; src_x = 0; }
          if (src_y < 0) { out_h += src_y; src_y = 0; }

          // Clamp to desktop bounds
          if (src_x + out_w > dxgi.width) out_w = dxgi.width - src_x;
          if (src_y + out_h > dxgi.height) out_h = dxgi.height - src_y;
        }

        if (out_w <= 0 || out_h <= 0) {
          dxgi.context->Unmap(dxgi.staging, 0);
          dxgi.duplication->ReleaseFrame();
          continue;
        }

        // H6: Overflow-checked allocation
        std::vector<uint8_t> rgba(checked_rgba_size(
            static_cast<size_t>(out_w), static_cast<size_t>(out_h)));

        auto *src = static_cast<const uint8_t *>(mapped.pData);
        for (int row = 0; row < out_h; ++row) {
          const uint8_t *src_row =
              src + (row + src_y) * mapped.RowPitch + src_x * 4;
          uint8_t *dst_row = rgba.data() + row * out_w * 4;
          bgra_to_rgba(src_row, dst_row, static_cast<size_t>(out_w));
        }

        dxgi.context->Unmap(dxgi.staging, 0);

        auto now = std::chrono::steady_clock::now();
        double duration_ms =
            std::chrono::duration<double, std::milli>(now - last_time).count();
        last_time = now;

        Frame frame{
            .image =
                {
                    .data = std::move(rgba),
                    .width = static_cast<size_t>(out_w),
                    .height = static_cast<size_t>(out_h),
                },
            .duration_ms = duration_ms,
        };
        callback_(frame);
      }

      dxgi.duplication->ReleaseFrame();
    }
  }

  // -- GDI polling (window capture) ----------------------------------------

  void gdi_capture_loop(std::stop_token token) {
    auto last_time = std::chrono::steady_clock::now();

    while (!token.stop_requested()) {
      {
        std::lock_guard lock(mutex_);
        if (paused_) {
          std::this_thread::sleep_for(std::chrono::milliseconds(16));
          continue;
        }
      }

      ImageData img = windows_screenshot_window(window_handle_, region_);
      if (img.width == 0 || img.height == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        continue;
      }

      auto now = std::chrono::steady_clock::now();
      double duration_ms =
          std::chrono::duration<double, std::milli>(now - last_time).count();
      last_time = now;

      Frame frame{
          .image = std::move(img),
          .duration_ms = duration_ms,
      };
      callback_(frame);

      // ~60 fps target
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  }

  // -- GDI polling fallback (monitor capture when DXGI fails) --------------

  void gdi_monitor_capture_loop(std::stop_token token) {
    auto last_time = std::chrono::steady_clock::now();

    while (!token.stop_requested()) {
      {
        std::lock_guard lock(mutex_);
        if (paused_) {
          std::this_thread::sleep_for(std::chrono::milliseconds(16));
          continue;
        }
      }

      ImageData img = windows_screenshot_monitor(monitor_index_, region_);
      if (img.width == 0 || img.height == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        continue;
      }

      auto now = std::chrono::steady_clock::now();
      double duration_ms =
          std::chrono::duration<double, std::milli>(now - last_time).count();
      last_time = now;

      Frame frame{
          .image = std::move(img),
          .duration_ms = duration_ms,
      };
      callback_(frame);

      std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
  }

  // -- State ----------------------------------------------------------------
  int monitor_index_ = 0;
  HWND window_handle_ = nullptr;
  bool capture_window_ = false;
  Rect region_{};
  FrameCallback callback_;

  std::jthread capture_thread_;
  mutable std::mutex mutex_;
  bool paused_ = false;
};

// ---------------------------------------------------------------------------
// Factory functions
// ---------------------------------------------------------------------------

std::unique_ptr<Backend> make_backend() {
  return std::make_unique<WindowsBackend>();
}

std::unique_ptr<Backend> make_backend(Rect region) {
  return std::make_unique<WindowsBackend>(region);
}

std::unique_ptr<Backend> make_backend(Monitor monitor) {
  return std::make_unique<WindowsBackend>(monitor);
}

std::unique_ptr<Backend> make_backend(Window window) {
  return std::make_unique<WindowsBackend>(window);
}

} // namespace frametap::internal
