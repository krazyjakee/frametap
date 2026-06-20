#include "decode/vt_decoder.h"

#include "encode/nal_util.h" // enc::ParamSets, annexb_to_length_prefixed

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <stdexcept>
#include <string>
#include <vector>

namespace frametap::dec {

namespace {
std::string vt_err(const char *what, OSStatus st) {
  return std::string("VtDecoder: ") + what + " failed (status " +
         std::to_string(static_cast<long>(st)) + ")";
}
} // namespace

struct VtDecoder::Impl {
  bool hevc = false;
  FrameSink sink;

  CMVideoFormatDescriptionRef fmt = nullptr;
  VTDecompressionSessionRef session = nullptr;

  // Last parameter sets used to build `fmt`; a change rebuilds the session.
  enc::ParamSets cur;
  uint64_t cur_pts = 0;
  std::vector<uint8_t> rgba; // reused output scratch

  ~Impl() { close(); }

  void close() {
    if (session) {
      VTDecompressionSessionInvalidate(session);
      CFRelease(session);
      session = nullptr;
    }
    if (fmt) {
      CFRelease(fmt);
      fmt = nullptr;
    }
  }

  bool same_params(const enc::ParamSets &p) const {
    return p.sps == cur.sps && p.pps == cur.pps && p.vps == cur.vps;
  }

  void rebuild(const enc::ParamSets &p) {
    close();
    cur = p;

    OSStatus st;
    if (hevc) {
      const uint8_t *ptrs[3] = {p.vps.data(), p.sps.data(), p.pps.data()};
      const size_t sizes[3] = {p.vps.size(), p.sps.size(), p.pps.size()};
      st = CMVideoFormatDescriptionCreateFromHEVCParameterSets(
          kCFAllocatorDefault, 3, ptrs, sizes, 4, nullptr, &fmt);
    } else {
      const uint8_t *ptrs[2] = {p.sps.data(), p.pps.data()};
      const size_t sizes[2] = {p.sps.size(), p.pps.size()};
      st = CMVideoFormatDescriptionCreateFromH264ParameterSets(
          kCFAllocatorDefault, 2, ptrs, sizes, 4, &fmt);
    }
    if (st != noErr || !fmt) {
      fmt = nullptr;
      throw std::runtime_error(vt_err("format description", st));
    }

    // Decode straight to BGRA so the only conversion is a byte swap to RGBA.
    const int32_t pf = kCVPixelFormatType_32BGRA;
    CFNumberRef pf_n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &pf);
    const void *keys[] = {kCVPixelBufferPixelFormatTypeKey};
    const void *vals[] = {pf_n};
    CFDictionaryRef dest = CFDictionaryCreate(
        nullptr, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);

    VTDecompressionOutputCallbackRecord cb{};
    cb.decompressionOutputCallback = &VtDecoder::Impl::output_trampoline;
    cb.decompressionOutputRefCon = this;
    st = VTDecompressionSessionCreate(kCFAllocatorDefault, fmt, nullptr, dest,
                                      &cb, &session);
    CFRelease(dest);
    CFRelease(pf_n);
    if (st != noErr || !session) {
      session = nullptr;
      throw std::runtime_error(vt_err("VTDecompressionSessionCreate", st));
    }
  }

  static void output_trampoline(void *refcon, void * /*src*/, OSStatus status,
                                VTDecodeInfoFlags /*flags*/,
                                CVImageBufferRef image, CMTime pts,
                                CMTime /*dur*/) {
    static_cast<Impl *>(refcon)->on_decoded(status, image, pts);
  }

  void on_decoded(OSStatus status, CVImageBufferRef image, CMTime pts) {
    if (status != noErr || !image || !sink)
      return;
    CVPixelBufferRef pb = image;
    CVPixelBufferLockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
    const int w = static_cast<int>(CVPixelBufferGetWidth(pb));
    const int h = static_cast<int>(CVPixelBufferGetHeight(pb));
    const auto *src = static_cast<const uint8_t *>(
        CVPixelBufferGetBaseAddress(pb));
    const size_t src_stride = CVPixelBufferGetBytesPerRow(pb);
    if (src) {
      rgba.resize(static_cast<size_t>(w) * h * 4);
      for (int y = 0; y < h; ++y) {
        const uint8_t *s = src + static_cast<size_t>(y) * src_stride;
        uint8_t *d = rgba.data() + static_cast<size_t>(y) * w * 4;
        for (int x = 0; x < w; ++x) {
          // BGRA -> RGBA: swap B and R.
          d[0] = s[2];
          d[1] = s[1];
          d[2] = s[0];
          d[3] = s[3];
          s += 4;
          d += 4;
        }
      }
      const uint64_t out_pts =
          (pts.timescale == 90000 && pts.value >= 0)
              ? static_cast<uint64_t>(pts.value)
              : cur_pts;
      sink(rgba.data(), w, h, out_pts);
    }
    CVPixelBufferUnlockBaseAddress(pb, kCVPixelBufferLock_ReadOnly);
  }
};

VtDecoder::VtDecoder() : impl_(new Impl()) {}
VtDecoder::~VtDecoder() { delete impl_; }

void VtDecoder::open(Codec codec, FrameSink sink) {
  impl_->hevc = codec == Codec::hevc;
  impl_->sink = std::move(sink);
}

void VtDecoder::decode(const uint8_t *annexb, size_t size, uint64_t pts_90k) {
  enc::ParamSets ps;
  std::vector<uint8_t> vcl;
  enc::annexb_to_length_prefixed(annexb, size, impl_->hevc, ps, vcl);

  // (Re)build the session when a fresh, complete parameter set arrives.
  if (ps.complete(impl_->hevc) &&
      (!impl_->session || !impl_->same_params(ps)))
    impl_->rebuild(ps);

  if (!impl_->session || vcl.empty())
    return; // still waiting for parameter sets, or params-only access unit

  CMBlockBufferRef bb = nullptr;
  OSStatus st = CMBlockBufferCreateWithMemoryBlock(
      kCFAllocatorDefault, nullptr, vcl.size(), kCFAllocatorDefault, nullptr, 0,
      vcl.size(), kCMBlockBufferAssureMemoryNowFlag, &bb);
  if (st != noErr || !bb)
    return;
  CMBlockBufferReplaceDataBytes(vcl.data(), bb, 0, vcl.size());

  CMSampleBufferRef sb = nullptr;
  const size_t sample_size = vcl.size();
  CMSampleTimingInfo timing = {kCMTimeInvalid,
                               CMTimeMake(static_cast<int64_t>(pts_90k), 90000),
                               kCMTimeInvalid};
  st = CMSampleBufferCreateReady(kCFAllocatorDefault, bb, impl_->fmt, 1, 1,
                                 &timing, 1, &sample_size, &sb);
  if (st == noErr && sb) {
    impl_->cur_pts = pts_90k;
    VTDecodeInfoFlags flags = 0;
    VTDecompressionSessionDecodeFrame(impl_->session, sb, 0, nullptr, &flags);
    CFRelease(sb);
  }
  CFRelease(bb);
}

void VtDecoder::flush() {
  if (impl_->session)
    VTDecompressionSessionWaitForAsynchronousFrames(impl_->session);
}

void VtDecoder::close() { impl_->close(); }

bool VtDecoder::is_open() const { return impl_->session != nullptr; }

} // namespace frametap::dec
