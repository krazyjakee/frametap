#include "encode/aac_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
}

#include <stdexcept>

namespace frametap::enc {

AacEncoder::~AacEncoder() { close(); }

void AacEncoder::open(int sample_rate, int channels, int bitrate_bps,
                      PacketSink sink) {
  sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
  channels_ = channels > 0 ? channels : 2;
  sink_ = std::move(sink);
  flushed_ = false;
  pts_ = 0;

  const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
  if (!codec)
    throw std::runtime_error("audio: no AAC encoder in libavcodec");

  ctx_ = avcodec_alloc_context3(codec);
  if (!ctx_)
    throw std::runtime_error("audio: avcodec_alloc_context3 failed");

  ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
  ctx_->sample_rate = sample_rate_;
  ctx_->bit_rate = bitrate_bps > 0 ? bitrate_bps : 160000;
  av_channel_layout_default(&ctx_->ch_layout, channels_);
  ctx_->time_base = {1, sample_rate_};
  // Put AudioSpecificConfig in extradata and emit raw (non-ADTS) frames.
  ctx_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  if (avcodec_open2(ctx_, codec, nullptr) < 0)
    throw std::runtime_error("audio: avcodec_open2(aac) failed");

  frame_size_ = ctx_->frame_size > 0 ? ctx_->frame_size : 1024;
  if (ctx_->extradata && ctx_->extradata_size > 0)
    asc_.assign(ctx_->extradata, ctx_->extradata + ctx_->extradata_size);

  frame_ = av_frame_alloc();
  frame_->format = AV_SAMPLE_FMT_FLTP;
  frame_->nb_samples = frame_size_;
  frame_->sample_rate = sample_rate_;
  av_channel_layout_copy(&frame_->ch_layout, &ctx_->ch_layout);
  if (av_frame_get_buffer(frame_, 0) < 0)
    throw std::runtime_error("audio: av_frame_get_buffer failed");

  pkt_ = av_packet_alloc();
  buffer_.clear();
}

void AacEncoder::drain() {
  while (true) {
    int r = avcodec_receive_packet(ctx_, pkt_);
    if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
      break;
    if (r < 0)
      break;
    if (sink_)
      sink_(pkt_->data, static_cast<size_t>(pkt_->size));
    av_packet_unref(pkt_);
  }
}

void AacEncoder::send_buffered_frame() {
  if (av_frame_make_writable(frame_) < 0)
    return;
  // Deinterleave the first frame_size_ sample-frames into planar float.
  for (int ch = 0; ch < channels_; ++ch) {
    auto *dst = reinterpret_cast<float *>(frame_->data[ch]);
    for (int i = 0; i < frame_size_; ++i)
      dst[i] = buffer_[static_cast<size_t>(i) * channels_ + ch];
  }
  frame_->pts = pts_;
  pts_ += frame_size_;

  if (avcodec_send_frame(ctx_, frame_) >= 0)
    drain();

  buffer_.erase(buffer_.begin(),
                buffer_.begin() +
                    static_cast<std::ptrdiff_t>(frame_size_) * channels_);
}

void AacEncoder::encode(const float *interleaved, uint32_t frames) {
  if (!ctx_ || flushed_)
    return;
  buffer_.insert(buffer_.end(), interleaved,
                 interleaved + static_cast<size_t>(frames) * channels_);
  const size_t block = static_cast<size_t>(frame_size_) * channels_;
  while (buffer_.size() >= block)
    send_buffered_frame();
}

void AacEncoder::flush() {
  if (!ctx_ || flushed_)
    return;
  flushed_ = true;
  // Pad a trailing partial frame with silence so the tail isn't dropped.
  const size_t block = static_cast<size_t>(frame_size_) * channels_;
  if (!buffer_.empty()) {
    buffer_.resize(block, 0.0f);
    send_buffered_frame();
  }
  avcodec_send_frame(ctx_, nullptr); // enter draining mode
  drain();
}

void AacEncoder::close() {
  if (ctx_ && !flushed_)
    flush();
  if (pkt_)
    av_packet_free(&pkt_);
  if (frame_)
    av_frame_free(&frame_);
  if (ctx_)
    avcodec_free_context(&ctx_);
}

} // namespace frametap::enc
