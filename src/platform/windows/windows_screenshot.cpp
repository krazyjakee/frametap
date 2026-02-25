#include "windows_backend.h"
#include "../../util/color.h"
#include "../../util/safe_alloc.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstring>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace frametap::internal {

// ---------------------------------------------------------------------------
// GDI-based screenshot (works for both monitors and windows)
// ---------------------------------------------------------------------------

static ImageData gdi_screenshot(HDC src_dc, int x, int y, int width,
                                 int height) {
  if (width <= 0 || height <= 0)
    return {};

  HDC mem_dc = CreateCompatibleDC(src_dc);
  HBITMAP bmp = CreateCompatibleBitmap(src_dc, width, height);
  HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem_dc, bmp));

  // M7: Check BitBlt return value
  if (!BitBlt(mem_dc, 0, 0, width, height, src_dc, x, y, SRCCOPY)) {
    SelectObject(mem_dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem_dc);
    return {};
  }

  BITMAPINFOHEADER bi{};
  bi.biSize = sizeof(bi);
  bi.biWidth = width;
  bi.biHeight = -height; // top-down
  bi.biPlanes = 1;
  bi.biBitCount = 32;
  bi.biCompression = BI_RGB;

  // H6: Overflow-checked allocation
  size_t pixel_bytes = checked_rgba_size(
      static_cast<size_t>(width), static_cast<size_t>(height));
  std::vector<uint8_t> pixels(pixel_bytes);

  GetDIBits(mem_dc, bmp, 0, height, pixels.data(),
            reinterpret_cast<BITMAPINFO *>(&bi), DIB_RGB_COLORS);

  SelectObject(mem_dc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(mem_dc);

  // BGRA -> RGBA
  size_t pixel_count = static_cast<size_t>(width) * height;
  bgra_to_rgba(pixels.data(), pixel_count);

  return ImageData{
      .data = std::move(pixels),
      .width = static_cast<size_t>(width),
      .height = static_cast<size_t>(height),
  };
}

// ---------------------------------------------------------------------------
// DXGI-based monitor screenshot (preferred path)
// ---------------------------------------------------------------------------

static ImageData dxgi_monitor_screenshot(int monitor_index, Rect region) {
  // Create DXGI factory
  IDXGIFactory1 *factory = nullptr;
  HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                   reinterpret_cast<void **>(&factory));
  if (FAILED(hr))
    return {};

  // Find the target adapter and output
  IDXGIAdapter1 *adapter = nullptr;
  IDXGIOutput *output = nullptr;
  int current_index = 0;

  for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
    for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
      if (current_index == monitor_index)
        goto found;
      ++current_index;
      output->Release();
      output = nullptr;
    }
    adapter->Release();
    adapter = nullptr;
  }

  factory->Release();
  return {}; // monitor not found

found:
  // Get output desc for dimensions
  DXGI_OUTPUT_DESC out_desc{};
  output->GetDesc(&out_desc);

  int desk_w = out_desc.DesktopCoordinates.right - out_desc.DesktopCoordinates.left;
  int desk_h = out_desc.DesktopCoordinates.bottom - out_desc.DesktopCoordinates.top;

  // Create D3D11 device on this adapter
  D3D_FEATURE_LEVEL feature_level;
  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;
  hr = D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                          nullptr, 0, D3D11_SDK_VERSION, &device,
                          &feature_level, &context);
  if (FAILED(hr)) {
    output->Release();
    adapter->Release();
    factory->Release();
    return {};
  }

  // Get DXGI Output1 for desktop duplication
  IDXGIOutput1 *output1 = nullptr;
  hr = output->QueryInterface(__uuidof(IDXGIOutput1),
                               reinterpret_cast<void **>(&output1));
  output->Release();

  if (FAILED(hr)) {
    context->Release();
    device->Release();
    adapter->Release();
    factory->Release();
    return {};
  }

  IDXGIOutputDuplication *duplication = nullptr;
  hr = output1->DuplicateOutput(device, &duplication);
  output1->Release();
  adapter->Release();
  factory->Release();

  if (FAILED(hr)) {
    context->Release();
    device->Release();
    return {};
  }

  // Acquire a frame
  DXGI_OUTDUPL_FRAME_INFO frame_info{};
  IDXGIResource *desktop_resource = nullptr;
  hr = duplication->AcquireNextFrame(500, &frame_info, &desktop_resource);

  if (FAILED(hr)) {
    duplication->Release();
    context->Release();
    device->Release();
    return {};
  }

  // Get the texture
  ID3D11Texture2D *desktop_tex = nullptr;
  hr = desktop_resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                         reinterpret_cast<void **>(&desktop_tex));
  desktop_resource->Release();

  if (FAILED(hr)) {
    duplication->ReleaseFrame();
    duplication->Release();
    context->Release();
    device->Release();
    return {};
  }

  // Create a CPU-readable staging texture
  D3D11_TEXTURE2D_DESC tex_desc{};
  desktop_tex->GetDesc(&tex_desc);
  tex_desc.Usage = D3D11_USAGE_STAGING;
  tex_desc.BindFlags = 0;
  tex_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  tex_desc.MiscFlags = 0;

  ID3D11Texture2D *staging = nullptr;
  hr = device->CreateTexture2D(&tex_desc, nullptr, &staging);
  if (FAILED(hr)) {
    desktop_tex->Release();
    duplication->ReleaseFrame();
    duplication->Release();
    context->Release();
    device->Release();
    return {};
  }

  context->CopyResource(staging, desktop_tex);
  desktop_tex->Release();

  // Map and read pixels
  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr = context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped);

  ImageData result;
  if (SUCCEEDED(hr)) {
    // M8: Determine and validate crop region
    int src_x = 0, src_y = 0;
    int out_w = desk_w, out_h = desk_h;
    if (region.width > 0 && region.height > 0) {
      src_x = static_cast<int>(region.x);
      src_y = static_cast<int>(region.y);
      out_w = static_cast<int>(region.width);
      out_h = static_cast<int>(region.height);

      // Clamp negatives
      if (src_x < 0) { out_w += src_x; src_x = 0; }
      if (src_y < 0) { out_h += src_y; src_y = 0; }

      // Clamp to desktop bounds
      if (src_x + out_w > desk_w) out_w = desk_w - src_x;
      if (src_y + out_h > desk_h) out_h = desk_h - src_y;
    }

    if (out_w > 0 && out_h > 0) {
      // H6: Overflow-checked allocation
      size_t pixel_bytes = checked_rgba_size(
          static_cast<size_t>(out_w), static_cast<size_t>(out_h));
      std::vector<uint8_t> pixels(pixel_bytes);

      auto *src = static_cast<const uint8_t *>(mapped.pData);
      for (int row = 0; row < out_h; ++row) {
        const uint8_t *src_row = src + (row + src_y) * mapped.RowPitch +
                                  src_x * 4;
        uint8_t *dst_row = pixels.data() + row * out_w * 4;
        bgra_to_rgba(src_row, dst_row, static_cast<size_t>(out_w));
      }

      result.data = std::move(pixels);
      result.width = static_cast<size_t>(out_w);
      result.height = static_cast<size_t>(out_h);
    }

    context->Unmap(staging, 0);
  }

  staging->Release();
  duplication->ReleaseFrame();
  duplication->Release();
  context->Release();
  device->Release();

  return result;
}

// ---------------------------------------------------------------------------
// Public screenshot functions used by WindowsBackend
// ---------------------------------------------------------------------------

ImageData windows_screenshot_monitor(int monitor_index, Rect region) {
  // Try DXGI first
  ImageData result = dxgi_monitor_screenshot(monitor_index, region);
  if (result.width > 0 && result.height > 0)
    return result;

  // GDI fallback
  HDC screen_dc = GetDC(nullptr);
  int x = static_cast<int>(region.x);
  int y = static_cast<int>(region.y);
  int w = static_cast<int>(region.width);
  int h = static_cast<int>(region.height);

  if (w <= 0 || h <= 0) {
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
    x = 0;
    y = 0;
  }

  result = gdi_screenshot(screen_dc, x, y, w, h);
  ReleaseDC(nullptr, screen_dc);
  return result;
}

ImageData windows_screenshot_window(HWND hwnd, Rect region) {
  RECT rect{};
  HRESULT hr = DwmGetWindowAttribute(
      hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect));
  if (FAILED(hr))
    GetWindowRect(hwnd, &rect);

  int win_w = rect.right - rect.left;
  int win_h = rect.bottom - rect.top;

  if (win_w <= 0 || win_h <= 0)
    return {};

  HDC win_dc = GetDC(hwnd);
  if (!win_dc)
    return {};

  HDC mem_dc = CreateCompatibleDC(win_dc);
  HBITMAP bmp = CreateCompatibleBitmap(win_dc, win_w, win_h);
  HBITMAP old_bmp = static_cast<HBITMAP>(SelectObject(mem_dc, bmp));

  // M7: Check PrintWindow return value
  if (!PrintWindow(hwnd, mem_dc, PW_RENDERFULLCONTENT)) {
    // Fall back to BitBlt
    if (!BitBlt(mem_dc, 0, 0, win_w, win_h, win_dc, 0, 0, SRCCOPY)) {
      SelectObject(mem_dc, old_bmp);
      DeleteObject(bmp);
      DeleteDC(mem_dc);
      ReleaseDC(hwnd, win_dc);
      return {};
    }
  }

  // M8: Determine and validate output region
  int src_x = 0, src_y = 0;
  int out_w = win_w, out_h = win_h;
  if (region.width > 0 && region.height > 0) {
    src_x = static_cast<int>(region.x);
    src_y = static_cast<int>(region.y);
    out_w = static_cast<int>(region.width);
    out_h = static_cast<int>(region.height);

    // Clamp negatives
    if (src_x < 0) { out_w += src_x; src_x = 0; }
    if (src_y < 0) { out_h += src_y; src_y = 0; }

    // Clamp to window bounds
    if (src_x + out_w > win_w) out_w = win_w - src_x;
    if (src_y + out_h > win_h) out_h = win_h - src_y;
  }

  if (out_w <= 0 || out_h <= 0) {
    SelectObject(mem_dc, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem_dc);
    ReleaseDC(hwnd, win_dc);
    return {};
  }

  BITMAPINFOHEADER bi{};
  bi.biSize = sizeof(bi);
  bi.biWidth = win_w;
  bi.biHeight = -win_h; // top-down
  bi.biPlanes = 1;
  bi.biBitCount = 32;
  bi.biCompression = BI_RGB;

  // H6: Overflow-checked allocation
  size_t full_bytes = checked_rgba_size(
      static_cast<size_t>(win_w), static_cast<size_t>(win_h));
  std::vector<uint8_t> full_pixels(full_bytes);

  GetDIBits(mem_dc, bmp, 0, win_h, full_pixels.data(),
            reinterpret_cast<BITMAPINFO *>(&bi), DIB_RGB_COLORS);

  SelectObject(mem_dc, old_bmp);
  DeleteObject(bmp);
  DeleteDC(mem_dc);
  ReleaseDC(hwnd, win_dc);

  // Crop + convert
  size_t pixel_bytes = checked_rgba_size(
      static_cast<size_t>(out_w), static_cast<size_t>(out_h));
  std::vector<uint8_t> pixels(pixel_bytes);

  for (int row = 0; row < out_h; ++row) {
    const uint8_t *src_row =
        full_pixels.data() + ((row + src_y) * win_w + src_x) * 4;
    uint8_t *dst_row = pixels.data() + row * out_w * 4;
    bgra_to_rgba(src_row, dst_row, static_cast<size_t>(out_w));
  }

  return ImageData{
      .data = std::move(pixels),
      .width = static_cast<size_t>(out_w),
      .height = static_cast<size_t>(out_h),
  };
}

} // namespace frametap::internal
