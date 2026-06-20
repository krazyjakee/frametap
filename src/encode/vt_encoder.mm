#include "encode/vt_encoder.h"

#import <Coremedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace frametap::enc {

namespace {

// Append `nal` (without start code) to `out` as an Annex-B unit.
void append_annexb(std::vector<uint8_t> &out, const uint8_t *nal, size_t len) {
  static const uint8_t kStart[4] = {0, 0, 0, 1};
  out.insert(out.end(), kStart, kStart + 4);
  out.insert(out.end(), nal, nal + len);
}

std::string fourcc_err(const char *what, OSStatus st) {
  return std::string("VtEncoder: ") + what + " failed (status " +
         std::to_string(static_cast<long>(st)) + ")";
}

} // namespace

struct VtEncoder::Impl {
  EncParams params;
  PacketSink sink;
  bool hevc = false;

  VTCompressionSessionRef session = nullptr;
  CVPixelBufferPoolRef pool = nullptr;

  // Set by the output callback (which runs while the encode thread is parked in
  // VTCompressionSessionCompleteFrames), surfaced by encode()/flush().
  bool callback_failed = false;
  std::string callback_error;

  // Scratch reused across frames to avoid per-frame heap churn.
  std::vector<uint8_t> au;

  ~Impl() { close(); }

  void close() {
    if (session) {
      VTCompressionSessionInvalidate(session);
      CFRelease(session);
      session = nullptr;
    }
    if (pool) {
      CFRelease(pool);
      pool = nullptr;
    }
  }

  // Pull SPS/PPS(/VPS) out of the sample's format description and emit them as
  // Annex-B. Done on every keyframe so the elementary stream is self-contained
  // (the hand-rolled muxers strip/repackage these as needed).
  void append_parameter_sets(CMFormatDescriptionRef fmt) {
    size_t count = 0;
    int nal_len = 4;
    OSStatus st;
    if (hevc) {
      st = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
          fmt, 0, nullptr, nullptr, &count, &nal_len);
    } else {
      st = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
          fmt, 0, nullptr, nullptr, &count, &nal_len);
    }
    if (st != noErr)
      return;
    for (size_t i = 0; i < count; ++i) {
      const uint8_t *ptr = nullptr;
      size_t size = 0;
      if (hevc)
        st = CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(
            fmt, i, &ptr, &size, nullptr, nullptr);
      else
        st = CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            fmt, i, &ptr, &size, nullptr, nullptr);
      if (st == noErr && ptr && size)
        append_annexb(au, ptr, size);
    }
  }

  void on_encoded(OSStatus status, CMSampleBufferRef sample) {
    if (status != noErr || !sample) {
      callback_failed = true;
      callback_error = fourcc_err("encode callback", status);
      return;
    }
    if (!CMSampleBufferDataIsReady(sample))
      return;

    // Keyframe (sync sample) unless the first attachment marks NotSync.
    bool keyframe = true;
    CFArrayRef attachments =
        CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
      CFDictionaryRef d =
          (CFDictionaryRef)CFArrayGetValueAtIndex(attachments, 0);
      CFBooleanRef not_sync = nullptr;
      if (CFDictionaryGetValueIfPresent(
              d, kCMSampleAttachmentKey_NotSync, (const void **)&not_sync) &&
          not_sync && CFBooleanGetValue(not_sync))
        keyframe = false;
    }

    au.clear();
    if (keyframe)
      append_parameter_sets(CMSampleBufferGetFormatDescription(sample));

    // The slice data is length-prefixed (AVCC/HVCC); rewrite each NAL to a
    // start-coded Annex-B unit.
    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    if (!block)
      return;
    size_t total = 0;
    char *bytes = nullptr;
    if (CMBlockBufferGetDataPointer(block, 0, nullptr, &total, &bytes) !=
            kCMBlockBufferNoErr ||
        !bytes)
      return;

    const auto *p = reinterpret_cast<const uint8_t *>(bytes);
    size_t off = 0;
    while (off + 4 <= total) {
      const uint32_t nlen = (uint32_t(p[off]) << 24) | (uint32_t(p[off + 1])
                            << 16) | (uint32_t(p[off + 2]) << 8) | p[off + 3];
      off += 4;
      if (nlen == 0 || off + nlen > total)
        break;
      append_annexb(au, p + off, nlen);
      off += nlen;
    }

    if (!au.empty() && sink)
      sink(au.data(), au.size(), keyframe);
  }
};

namespace {

void output_cb(void *refcon, void * /*sourceFrameRefCon*/, OSStatus status,
               VTEncodeInfoFlags /*flags*/, CMSampleBufferRef sample) {
  static_cast<VtEncoder::Impl *>(refcon)->on_encoded(status, sample);
}

void set_int(VTCompressionSessionRef s, CFStringRef key, int32_t v) {
  CFNumberRef n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &v);
  VTSessionSetProperty(s, key, n);
  CFRelease(n);
}

} // namespace

VtEncoder::VtEncoder() : impl_(new Impl()) {}
VtEncoder::~VtEncoder() { delete impl_; }

void VtEncoder::open(const EncParams &params, PacketSink sink) {
  impl_->params = params;
  impl_->sink = std::move(sink);
  impl_->hevc = params.codec == Codec::hevc;

  const CMVideoCodecType codec =
      impl_->hevc ? kCMVideoCodecType_HEVC : kCMVideoCodecType_H264;

  // Ask for a BGRA, IOSurface-backed input pool so frames can be uploaded to
  // the GPU without an extra copy.
  const int32_t fmt = kCVPixelFormatType_32BGRA;
  CFNumberRef fmt_n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &fmt);
  CFNumberRef w_n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &params.width);
  CFNumberRef h_n =
      CFNumberCreate(nullptr, kCFNumberSInt32Type, &params.height);
  const void *keys[] = {kCVPixelBufferPixelFormatTypeKey,
                        kCVPixelBufferWidthKey, kCVPixelBufferHeightKey,
                        kCVPixelBufferIOSurfacePropertiesKey};
  CFDictionaryRef empty =
      CFDictionaryCreate(nullptr, nullptr, nullptr, 0,
                         &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
  const void *vals[] = {fmt_n, w_n, h_n, empty};
  CFDictionaryRef src_attrs = CFDictionaryCreate(
      nullptr, keys, vals, 4, &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);

  OSStatus st = VTCompressionSessionCreate(
      nullptr, params.width, params.height, codec, nullptr, src_attrs, nullptr,
      output_cb, impl_, &impl_->session);

  if (st == noErr) {
    st = CVPixelBufferPoolCreate(nullptr, nullptr, src_attrs, &impl_->pool);
  }

  CFRelease(src_attrs);
  CFRelease(empty);
  CFRelease(fmt_n);
  CFRelease(w_n);
  CFRelease(h_n);

  if (st != noErr || !impl_->session) {
    impl_->close();
    throw std::runtime_error(fourcc_err("VTCompressionSessionCreate", st));
  }

  VTSessionSetProperty(impl_->session, kVTCompressionPropertyKey_RealTime,
                       kCFBooleanTrue);
  // No B-frames: lowest latency and a stream the simple muxers handle cleanly.
  VTSessionSetProperty(impl_->session,
                       kVTCompressionPropertyKey_AllowFrameReordering,
                       kCFBooleanFalse);
  VTSessionSetProperty(
      impl_->session, kVTCompressionPropertyKey_ProfileLevel,
      impl_->hevc ? kVTProfileLevel_HEVC_Main_AutoLevel
                  : kVTProfileLevel_H264_High_AutoLevel);
  set_int(impl_->session, kVTCompressionPropertyKey_AverageBitRate,
          params.bitrate_kbps * 1000);
  set_int(impl_->session, kVTCompressionPropertyKey_ExpectedFrameRate,
          params.fps);
  // ~2s GOP, both by frame count and duration (whichever comes first).
  set_int(impl_->session, kVTCompressionPropertyKey_MaxKeyFrameInterval,
          params.fps * 2);
  {
    double secs = 2.0;
    CFNumberRef n = CFNumberCreate(nullptr, kCFNumberDoubleType, &secs);
    VTSessionSetProperty(
        impl_->session,
        kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, n);
    CFRelease(n);
  }

  VTCompressionSessionPrepareToEncodeFrames(impl_->session);
}

void VtEncoder::encode(const uint8_t *rgba, int width, int height,
                       size_t stride, int64_t timestamp) {
  if (!impl_->session)
    throw std::runtime_error("VtEncoder::encode before open");

  CVPixelBufferRef pb = nullptr;
  if (CVPixelBufferPoolCreatePixelBuffer(nullptr, impl_->pool, &pb) !=
          kCVReturnSuccess ||
      !pb)
    throw std::runtime_error("VtEncoder: pixel buffer pool exhausted");

  CVPixelBufferLockBaseAddress(pb, 0);
  auto *dst = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(pb));
  const size_t dst_stride = CVPixelBufferGetBytesPerRow(pb);
  const int w = width < impl_->params.width ? width : impl_->params.width;
  const int h = height < impl_->params.height ? height : impl_->params.height;
  for (int y = 0; y < h; ++y) {
    const uint8_t *s = rgba + static_cast<size_t>(y) * stride;
    uint8_t *d = dst + static_cast<size_t>(y) * dst_stride;
    for (int x = 0; x < w; ++x) {
      // RGBA -> BGRA: swap R and B.
      d[0] = s[2];
      d[1] = s[1];
      d[2] = s[0];
      d[3] = s[3];
      s += 4;
      d += 4;
    }
  }
  CVPixelBufferUnlockBaseAddress(pb, 0);

  const CMTime pts = CMTimeMake(timestamp, impl_->params.fps);
  const CMTime dur = CMTimeMake(1, impl_->params.fps);
  VTEncodeInfoFlags info = 0;
  OSStatus st = VTCompressionSessionEncodeFrame(impl_->session, pb, pts, dur,
                                                nullptr, nullptr, &info);
  CVPixelBufferRelease(pb);
  if (st != noErr)
    throw std::runtime_error(fourcc_err("VTCompressionSessionEncodeFrame", st));

  // Force this frame out now so the sink fires synchronously on this thread,
  // before encode() returns -- the recorder's pts bookkeeping depends on it.
  VTCompressionSessionCompleteFrames(impl_->session, pts);
  if (impl_->callback_failed) {
    impl_->callback_failed = false;
    throw std::runtime_error(impl_->callback_error);
  }
}

void VtEncoder::set_bitrate(int bitrate_kbps) {
  if (impl_->session)
    set_int(impl_->session, kVTCompressionPropertyKey_AverageBitRate,
            bitrate_kbps * 1000);
}

void VtEncoder::flush() {
  if (impl_->session)
    VTCompressionSessionCompleteFrames(impl_->session, kCMTimeInvalid);
}

void VtEncoder::close() { impl_->close(); }

bool VtEncoder::is_open() const { return impl_->session != nullptr; }

} // namespace frametap::enc
