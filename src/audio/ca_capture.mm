#include "audio/ca_capture.h"

#import <CoreMedia/CoreMedia.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <stdexcept>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Objective-C output delegate. Declared at file scope (outside the C++
// namespace) and forwards each audio sample buffer to the owning CaCapture.
// ---------------------------------------------------------------------------

@interface FTAudioOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic, assign) frametap::audio::CaCapture *owner;
@end

namespace frametap::audio {

struct CaCapture::Impl {
  SCStream *stream = nil;
  FTAudioOutput *delegate = nil;
  dispatch_queue_t queue = nullptr;
  std::vector<float> interleaved; // reused on the (serial) audio queue
};

CaCapture::CaCapture() : impl_(std::make_unique<Impl>()) {}

CaCapture::~CaCapture() { stop(); }

bool CaCapture::running() const { return impl_ && impl_->stream != nil; }


void CaCapture::start(PcmSink sink, uint64_t /*target_pid*/) {
  sink_ = std::move(sink);

  if (@available(macOS 13.0, *)) {
    // Resolve a display to attach the filter to (audio capture still requires a
    // content filter). Block until ScreenCaptureKit answers.
    __block SCShareableContent *content = nil;
    __block NSError *content_err = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCShareableContent
        getShareableContentExcludingDesktopWindows:NO
                               onScreenWindowsOnly:NO
                                 completionHandler:^(SCShareableContent *c,
                                                     NSError *e) {
                                   content = c;
                                   content_err = e;
                                   dispatch_semaphore_signal(sem);
                                 }];
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    if (!content || content.displays.count == 0)
      throw std::runtime_error(
          "CaCapture: no shareable display (screen-recording permission?)");

    SCDisplay *display = content.displays.firstObject;
    SCContentFilter *filter =
        [[SCContentFilter alloc] initWithDisplay:display
                                excludingWindows:@[]];

    SCStreamConfiguration *conf = [[SCStreamConfiguration alloc] init];
    conf.capturesAudio = YES;
    conf.sampleRate = rate_;
    conf.channelCount = channels_;
    conf.excludesCurrentProcessAudio = YES;
    // We only want audio; keep the (mandatory) video path as cheap as possible.
    conf.width = 2;
    conf.height = 2;
    conf.minimumFrameInterval = CMTimeMake(1, 1);

    impl_->delegate = [[FTAudioOutput alloc] init];
    impl_->delegate.owner = this;
    impl_->stream = [[SCStream alloc] initWithFilter:filter
                                       configuration:conf
                                            delegate:impl_->delegate];

    impl_->queue =
        dispatch_queue_create("com.frametap.audio", DISPATCH_QUEUE_SERIAL);
    NSError *add_err = nil;
    if (![impl_->stream addStreamOutput:impl_->delegate
                                   type:SCStreamOutputTypeAudio
                     sampleHandlerQueue:impl_->queue
                                  error:&add_err]) {
      impl_->stream = nil;
      throw std::runtime_error("CaCapture: addStreamOutput(audio) failed");
    }

    __block NSError *start_err = nil;
    dispatch_semaphore_t start_sem = dispatch_semaphore_create(0);
    [impl_->stream startCaptureWithCompletionHandler:^(NSError *e) {
      start_err = e;
      dispatch_semaphore_signal(start_sem);
    }];
    dispatch_semaphore_wait(start_sem, DISPATCH_TIME_FOREVER);
    if (start_err) {
      impl_->stream = nil;
      throw std::runtime_error(
          std::string("CaCapture: startCapture failed: ") +
          start_err.localizedDescription.UTF8String);
    }
  } else {
    throw std::runtime_error(
        "CaCapture: system audio capture requires macOS 13+");
  }
}

void CaCapture::stop() {
  if (!impl_ || impl_->stream == nil)
    return;
  dispatch_semaphore_t sem = dispatch_semaphore_create(0);
  [impl_->stream stopCaptureWithCompletionHandler:^(NSError *) {
    dispatch_semaphore_signal(sem);
  }];
  dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
  impl_->stream = nil;
  impl_->delegate = nil;
}

// Interleave a ScreenCaptureKit audio sample buffer into float PCM and hand it
// to the recorder. Runs on the serial audio queue.
void CaCapture::handle_sample(void *sbuf) {
  CMSampleBufferRef sample = static_cast<CMSampleBufferRef>(sbuf);
  const CMItemCount frames = CMSampleBufferGetNumSamples(sample);
  if (frames <= 0)
    return;

  CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
  const AudioStreamBasicDescription *asbd =
      fmt ? CMAudioFormatDescriptionGetStreamBasicDescription(fmt) : nullptr;
  if (!asbd)
    return;
  const int src_ch = static_cast<int>(asbd->mChannelsPerFrame);
  const bool non_interleaved =
      (asbd->mFormatFlags & kAudioFormatFlagIsNonInterleaved) != 0;
  const int out_ch = 2;

  size_t needed = 0;
  if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
          sample, &needed, nullptr, 0, nullptr, nullptr, 0, nullptr) != noErr ||
      needed == 0)
    return;
  std::vector<uint8_t> abl_storage(needed);
  auto *abl = reinterpret_cast<AudioBufferList *>(abl_storage.data());
  CMBlockBufferRef block = nullptr;
  if (CMSampleBufferGetAudioBufferListWithRetainedBlockBuffer(
          sample, nullptr, abl, needed, nullptr, nullptr, 0, &block) != noErr)
    return;

  auto &out = impl_->interleaved;
  out.assign(static_cast<size_t>(frames) * out_ch, 0.0f);

  if (non_interleaved) {
    for (int c = 0; c < out_ch; ++c) {
      const int src = c < src_ch ? c : src_ch - 1; // mono -> duplicate
      if (src < 0 || static_cast<UInt32>(src) >= abl->mNumberBuffers)
        continue;
      const auto *plane =
          reinterpret_cast<const float *>(abl->mBuffers[src].mData);
      if (!plane)
        continue;
      for (CMItemCount f = 0; f < frames; ++f)
        out[f * out_ch + c] = plane[f];
    }
  } else if (abl->mNumberBuffers > 0 && abl->mBuffers[0].mData) {
    const auto *in = reinterpret_cast<const float *>(abl->mBuffers[0].mData);
    for (CMItemCount f = 0; f < frames; ++f)
      for (int c = 0; c < out_ch; ++c)
        out[f * out_ch + c] = in[f * src_ch + (c < src_ch ? c : src_ch - 1)];
  }

  if (sink_)
    sink_(out.data(), static_cast<uint32_t>(frames));
  if (block)
    CFRelease(block);
}

} // namespace frametap::audio

@implementation FTAudioOutput
- (void)stream:(SCStream *)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
                   ofType:(SCStreamOutputType)type {
  if (type != SCStreamOutputTypeAudio || !self.owner)
    return;
  if (!CMSampleBufferDataIsReady(sampleBuffer))
    return;
  self.owner->handle_sample(sampleBuffer);
}
@end
