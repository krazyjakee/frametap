#import "macos_backend.h"
#import "../../util/color.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <cassert>
#include <cstring>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------
namespace frametap::internal {
class MacOSBackend;
}

// ---------------------------------------------------------------------------
// SCStream output delegate — replaces the old OutputProcessor @interface
// ---------------------------------------------------------------------------
@interface FTStreamOutput : NSObject <SCStreamOutput>
@property(assign) frametap::internal::MacOSBackend *backend;
@end

// ---------------------------------------------------------------------------
// MacOSBackend implementation
// ---------------------------------------------------------------------------
namespace frametap::internal {

class MacOSBackend : public Backend {
public:
  MacOSBackend() { display_id_ = CGMainDisplayID(); }

  explicit MacOSBackend(Rect region) : MacOSBackend() { region_ = region; }

  explicit MacOSBackend(Monitor monitor) : MacOSBackend() {
    display_id_ = static_cast<CGDirectDisplayID>(monitor.id);
  }

  explicit MacOSBackend(Window window) : MacOSBackend() {
    window_id_ = static_cast<CGWindowID>(window.id);
    capture_window_ = true;
  }

  ~MacOSBackend() override { stop(); }

  // -- Backend interface ----------------------------------------------------

  ImageData screenshot(Rect region) override {
    CGRect bounds;
    if (region.width > 0 && region.height > 0) {
      bounds = CGRectMake(region.x, region.y, region.width, region.height);
    } else if (region_.width > 0 && region_.height > 0) {
      bounds = CGRectMake(region_.x, region_.y, region_.width, region_.height);
    } else if (capture_window_) {
      // Window capture via CGWindowListCreateImage
      CGImageRef image = CGWindowListCreateImage(
          CGRectNull, kCGWindowListOptionIncludingWindow, window_id_,
          kCGWindowImageBoundsIgnoreFraming);
      if (!image)
        return {};
      return image_from_cgimage(image);
    } else {
      bounds = CGDisplayBounds(display_id_);
    }

    CGImageRef image = CGDisplayCreateImageForRect(display_id_, bounds);
    if (!image)
      return {};
    return image_from_cgimage(image);
  }

  void start(FrameCallback cb) override {
    callback_ = std::move(cb);
    if (!callback_)
      return;
    if (!setup_stream())
      return;
    start_stream();
  }

  void stop() override {
    if (!stream_)
      return;
    should_stop_ = true;
    dispatch_semaphore_t finished = dispatch_semaphore_create(0);
    [stream_ stopCaptureWithCompletionHandler:^(NSError *_Nullable) {
      dispatch_semaphore_signal(finished);
    }];
    dispatch_semaphore_wait(finished, DISPATCH_TIME_FOREVER);
    stream_ = nil;
    output_delegate_ = nil;
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

  // -- Called by FTStreamOutput ----------------------------------------------

  void on_sample_buffer(CMSampleBufferRef sampleBuffer) {
    {
      std::lock_guard lock(mutex_);
      if (paused_)
        return;
    }

    if (should_stop_)
      return;

    CVPixelBufferRef pixelBuffer =
        CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!pixelBuffer)
      return;

    CVReturn ok =
        CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    if (ok != kCVReturnSuccess)
      return;

    uint8_t *base =
        static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(pixelBuffer));
    size_t width = CVPixelBufferGetWidth(pixelBuffer);
    size_t height = CVPixelBufferGetHeight(pixelBuffer);

    // Crop to region if set
    size_t out_w = width, out_h = height;
    size_t src_x = 0, src_y = 0;
    if (region_.width > 0 && region_.height > 0) {
      src_x = static_cast<size_t>(region_.x);
      src_y = static_cast<size_t>(region_.y);
      out_w = static_cast<size_t>(region_.width);
      out_h = static_cast<size_t>(region_.height);
    }

    size_t buf_size = out_w * out_h * 4;
    std::vector<uint8_t> rgba(buf_size);

    for (size_t row = 0; row < out_h; ++row) {
      const uint8_t *src_row =
          base + ((row + src_y) * width + src_x) * 4;
      uint8_t *dst_row = rgba.data() + row * out_w * 4;
      bgra_to_rgba(src_row, dst_row, out_w);
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

    // Compute frame duration
    CMTime pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer);
    double duration_ms = 0;
    if (CMTimeCompare(last_pts_, kCMTimeZero) != 0) {
      CMTime delta = CMTimeSubtract(pts, last_pts_);
      duration_ms = CMTimeGetSeconds(delta) * 1000.0;
    }
    last_pts_ = pts;

    Frame frame{
        .image =
            {
                .data = std::move(rgba),
                .width = out_w,
                .height = out_h,
            },
        .duration_ms = duration_ms,
    };
    callback_(frame);
  }

private:
  // -- Streaming helpers ----------------------------------------------------

  bool setup_stream() {
    dispatch_semaphore_t ready = dispatch_semaphore_create(0);
    __block bool ok = true;
    __block SCShareableContent *content = nil;

    [SCShareableContent
        getShareableContentExcludingDesktopWindows:NO
                               onScreenWindowsOnly:YES
                                 completionHandler:^(
                                     SCShareableContent *c, NSError *err) {
                                   if (err) {
                                     ok = false;
                                   } else {
                                     content = c;
                                     [content retain];
                                   }
                                   dispatch_semaphore_signal(ready);
                                 }];
    dispatch_semaphore_wait(ready, DISPATCH_TIME_FOREVER);
    if (!ok || !content)
      throw CaptureError(
          "Screen recording permission denied. Go to System Settings > "
          "Privacy & Security > Screen Recording and enable your application.");

    // Find the target display
    SCDisplay *target_display = nil;
    for (SCDisplay *d in [content displays]) {
      if ([d displayID] == display_id_) {
        target_display = d;
        break;
      }
    }
    if (!target_display) {
      [content release];
      return false;
    }

    SCContentFilter *filter;
    if (capture_window_) {
      // Find the target window
      SCWindow *target_window = nil;
      for (SCWindow *w in [content windows]) {
        if ([w windowID] == window_id_) {
          target_window = w;
          break;
        }
      }
      if (!target_window) {
        [content release];
        return false;
      }
      filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:target_window];
    } else {
      NSArray<SCWindow *> *windows = [content windows];
      filter = [[SCContentFilter alloc] initWithDisplay:target_display
                                        includingWindows:windows];
    }

    SCStreamConfiguration *conf = [[SCStreamConfiguration alloc] init];
    [conf setPixelFormat:'BGRA'];
    [conf setCapturesAudio:NO];
    [conf setShowsCursor:YES];
    [conf setMinimumFrameInterval:CMTimeMake(1, 60)];

    stream_ = [[SCStream alloc] initWithFilter:filter
                                 configuration:conf
                                      delegate:nil];

    output_delegate_ = [[FTStreamOutput alloc] init];
    output_delegate_.backend = this;

    NSError *err = nil;
    bool added = [stream_ addStreamOutput:output_delegate_
                                     type:SCStreamOutputTypeScreen
                       sampleHandlerQueue:nil
                                    error:&err];
    [content release];
    return added;
  }

  void start_stream() {
    should_stop_ = false;
    last_pts_ = kCMTimeZero;
    dispatch_semaphore_t started = dispatch_semaphore_create(0);
    [stream_ startCaptureWithCompletionHandler:^(NSError *_Nullable) {
      dispatch_semaphore_signal(started);
    }];
    dispatch_semaphore_wait(started, DISPATCH_TIME_FOREVER);
  }

  // -- Screenshot helper ----------------------------------------------------

  ImageData image_from_cgimage(CGImageRef image) {
    size_t width = CGImageGetWidth(image);
    size_t height = CGImageGetHeight(image);
    size_t expected_bpr = width * 4;
    size_t buf_size = expected_bpr * height;

    std::vector<uint8_t> pixels(buf_size);
    CGContextRef ctx = CGBitmapContextCreate(
        pixels.data(), width, height, 8, expected_bpr,
        CGImageGetColorSpace(image), CGImageGetBitmapInfo(image));
    CGContextDrawImage(ctx, CGRectMake(0, 0, width, height), image);
    CGContextRelease(ctx);
    CGImageRelease(image);

    // BGRA → RGBA in-place
    bgra_to_rgba(pixels.data(), width * height);

    return ImageData{
        .data = std::move(pixels),
        .width = width,
        .height = height,
    };
  }

  // -- State ----------------------------------------------------------------
  CGDirectDisplayID display_id_ = 0;
  CGWindowID window_id_ = 0;
  bool capture_window_ = false;
  Rect region_{};
  FrameCallback callback_;

  SCStream *stream_ = nil;
  FTStreamOutput *output_delegate_ = nil;
  bool should_stop_ = false;
  CMTime last_pts_ = kCMTimeZero;

  mutable std::mutex mutex_;
  bool paused_ = false;
};

// ---------------------------------------------------------------------------
// Factory functions
// ---------------------------------------------------------------------------

std::unique_ptr<Backend> make_backend() {
  return std::make_unique<MacOSBackend>();
}

std::unique_ptr<Backend> make_backend(Rect region) {
  return std::make_unique<MacOSBackend>(region);
}

std::unique_ptr<Backend> make_backend(Monitor monitor) {
  return std::make_unique<MacOSBackend>(monitor);
}

std::unique_ptr<Backend> make_backend(Window window) {
  return std::make_unique<MacOSBackend>(window);
}

// ---------------------------------------------------------------------------
// Enumeration
// ---------------------------------------------------------------------------

std::vector<Monitor> enumerate_monitors() {
  std::vector<Monitor> result;

  uint32_t count = 0;
  CGGetActiveDisplayList(0, nullptr, &count);
  if (count == 0)
    return result;

  std::vector<CGDirectDisplayID> ids(count);
  CGGetActiveDisplayList(count, ids.data(), &count);

  for (uint32_t i = 0; i < count; ++i) {
    CGDirectDisplayID did = ids[i];
    CGRect bounds = CGDisplayBounds(did);

    // Try to get a display name from NSScreen
    std::string name = "Display " + std::to_string(i);
    for (NSScreen *screen in [NSScreen screens]) {
      NSDictionary *desc = [screen deviceDescription];
      NSNumber *screenNumber = desc[@"NSScreenNumber"];
      if (screenNumber && [screenNumber unsignedIntValue] == did) {
        name = [[screen localizedName] UTF8String];
        break;
      }
    }

    Monitor m{
        .id = static_cast<int>(did),
        .name = std::move(name),
        .x = static_cast<int>(bounds.origin.x),
        .y = static_cast<int>(bounds.origin.y),
        .width = static_cast<int>(bounds.size.width),
        .height = static_cast<int>(bounds.size.height),
        .scale = static_cast<float>(CGDisplayPixelsWide(did) /
                                    bounds.size.width),
    };
    result.push_back(std::move(m));
  }

  return result;
}

std::vector<Window> enumerate_windows() {
  std::vector<Window> result;

  CFArrayRef window_list = CGWindowListCopyWindowInfo(
      kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
      kCGNullWindowID);
  if (!window_list)
    return result;

  CFIndex count = CFArrayGetCount(window_list);
  for (CFIndex i = 0; i < count; ++i) {
    NSDictionary *info =
        (__bridge NSDictionary *)CFArrayGetValueAtIndex(window_list, i);

    // Skip windows with no name or layer != 0 (menu bars, etc.)
    NSString *name = info[(__bridge NSString *)kCGWindowName];
    NSNumber *layer = info[(__bridge NSString *)kCGWindowLayer];
    if (!name || [name length] == 0 || [layer intValue] != 0)
      continue;

    NSNumber *wid = info[(__bridge NSString *)kCGWindowNumber];
    NSDictionary *bounds_dict =
        info[(__bridge NSString *)kCGWindowBounds];

    CGRect bounds;
    CGRectMakeWithDictionaryRepresentation(
        (__bridge CFDictionaryRef)bounds_dict, &bounds);

    Window w{
        .id = [wid unsignedLongLongValue],
        .name = [name UTF8String],
        .x = static_cast<int>(bounds.origin.x),
        .y = static_cast<int>(bounds.origin.y),
        .width = static_cast<int>(bounds.size.width),
        .height = static_cast<int>(bounds.size.height),
    };
    result.push_back(std::move(w));
  }

  CFRelease(window_list);
  return result;
}

// ---------------------------------------------------------------------------
// Permission diagnostics
// ---------------------------------------------------------------------------

PermissionCheck check_platform_permissions() {
  PermissionCheck result;
  result.summary = "macOS";

  if (@available(macOS 12.3, *)) {
    // ScreenCaptureKit — check if we can list shareable content
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block bool has_permission = false;

    [SCShareableContent
        getShareableContentExcludingDesktopWindows:NO
                               onScreenWindowsOnly:YES
                                 completionHandler:^(SCShareableContent *c,
                                                     NSError *err) {
                                   has_permission = (err == nil && c != nil);
                                   dispatch_semaphore_signal(sem);
                                 }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);

    if (has_permission) {
      result.status = PermissionStatus::ok;
      result.summary = "macOS (ScreenCaptureKit)";
      result.details.push_back("Screen recording permission granted.");
    } else {
      result.status = PermissionStatus::error;
      result.summary = "macOS (ScreenCaptureKit) — no permission";
      result.details.push_back(
          "Screen recording permission is not granted.");
      result.details.push_back(
          "Go to System Settings > Privacy & Security > Screen Recording "
          "and enable your application.");
    }
  } else {
    // Older macOS — CGPreflightScreenCaptureAccess if available (10.15+)
    result.summary = "macOS (CoreGraphics)";
    if (CGPreflightScreenCaptureAccess()) {
      result.status = PermissionStatus::ok;
      result.details.push_back("Screen recording permission granted.");
    } else {
      // Try to request it
      CGRequestScreenCaptureAccess();
      result.status = PermissionStatus::error;
      result.details.push_back(
          "Screen recording permission is not granted.");
      result.details.push_back(
          "A permission dialog should have appeared. If not, go to "
          "System Preferences > Security & Privacy > Privacy > Screen "
          "Recording and enable your application.");
    }
  }

  return result;
}

} // namespace frametap::internal

// ---------------------------------------------------------------------------
// FTStreamOutput @implementation — must be outside the namespace
// ---------------------------------------------------------------------------
@implementation FTStreamOutput

- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
  if (type != SCStreamOutputTypeScreen)
    return;
  CFRetain(sampleBuffer);
  self.backend->on_sample_buffer(sampleBuffer);
  CFRelease(sampleBuffer);
}

@end
