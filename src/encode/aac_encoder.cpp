#include "encode/aac_encoder.h"

#include <common/include/cmnMemory.h>
#include <common/include/voAAC.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace frametap::enc {
namespace {

int freq_index(int rate) {
  static const int table[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                              22050, 16000, 12000, 11025, 8000,  7350};
  for (int i = 0; i < 13; ++i)
    if (table[i] == rate)
      return i;
  return 3; // default 48 kHz
}

} // namespace

AacEncoder::~AacEncoder() { close(); }

void AacEncoder::open(int sample_rate, int channels, int bitrate_bps,
                      PacketSink sink) {
  sample_rate_ = sample_rate > 0 ? sample_rate : 48000;
  channels_ = channels > 0 ? channels : 2;
  sink_ = std::move(sink);
  flushed_ = false;
  buffer_.clear();

  auto *api = new VO_AUDIO_CODECAPI();
  if (voGetAACEncAPI(api) != VO_ERR_NONE) {
    delete api;
    throw std::runtime_error("audio: voGetAACEncAPI failed");
  }

  auto *mem = new VO_MEM_OPERATOR();
  mem->Alloc = cmnMemAlloc;
  mem->Copy = cmnMemCopy;
  mem->Free = cmnMemFree;
  mem->Set = cmnMemSet;
  mem->Check = cmnMemCheck;

  VO_CODEC_INIT_USERDATA ud;
  ud.memflag = VO_IMF_USERMEMOPERATOR;
  ud.memData = mem;

  VO_HANDLE handle = nullptr;
  if (api->Init(&handle, VO_AUDIO_CodingAAC, &ud) != VO_ERR_NONE) {
    delete api;
    delete mem;
    throw std::runtime_error("audio: vo-aacenc Init failed");
  }

  AACENC_PARAM p{};
  p.sampleRate = sample_rate_;
  p.bitRate = bitrate_bps > 0 ? bitrate_bps : 160000;
  p.nChannels = channels_;
  p.adtsUsed = 0; // raw AAC; muxers add ADTS / esds themselves
  if (api->SetParam(handle, VO_PID_AAC_ENCPARAM, &p) != VO_ERR_NONE) {
    api->Uninit(handle);
    delete api;
    delete mem;
    throw std::runtime_error("audio: vo-aacenc SetParam failed (bad rate or "
                             "bitrate for channel count)");
  }

  enc_ = handle;
  api_ = api;
  mem_ = mem;

  // Synthesize the 2-byte AAC-LC AudioSpecificConfig.
  const int fi = freq_index(sample_rate_);
  asc_.resize(2);
  asc_[0] = static_cast<uint8_t>((2 << 3) | ((fi >> 1) & 0x7));
  asc_[1] = static_cast<uint8_t>(((fi & 1) << 7) | ((channels_ & 0xF) << 3));
}

void AacEncoder::encode_block() {
  auto *api = static_cast<VO_AUDIO_CODECAPI *>(api_);
  const size_t block = static_cast<size_t>(frame_size_) * channels_;

  VO_CODECBUFFER in{};
  in.Buffer = reinterpret_cast<VO_PBYTE>(buffer_.data());
  in.Length = block * sizeof(int16_t);
  api->SetInputData(enc_, &in);

  uint8_t outbuf[8192];
  VO_CODECBUFFER out{};
  VO_AUDIO_OUTPUTINFO info{};
  out.Buffer = outbuf;
  out.Length = sizeof(outbuf);
  if (api->GetOutputData(enc_, &out, &info) == VO_ERR_NONE && out.Length > 0 &&
      sink_)
    sink_(outbuf, out.Length);

  buffer_.erase(buffer_.begin(),
                buffer_.begin() + static_cast<std::ptrdiff_t>(block));
}

void AacEncoder::encode(const float *interleaved, uint32_t frames) {
  if (!enc_ || flushed_)
    return;
  const size_t n = static_cast<size_t>(frames) * channels_;
  const size_t old = buffer_.size();
  buffer_.resize(old + n);
  for (size_t i = 0; i < n; ++i) {
    float s = interleaved[i];
    s = std::min(1.0f, std::max(-1.0f, s));
    buffer_[old + i] = static_cast<int16_t>(std::lrintf(s * 32767.0f));
  }

  const size_t block = static_cast<size_t>(frame_size_) * channels_;
  while (buffer_.size() >= block)
    encode_block();
}

void AacEncoder::flush() {
  if (!enc_ || flushed_)
    return;
  flushed_ = true;
  // Pad a trailing partial frame with silence so the tail isn't dropped.
  const size_t block = static_cast<size_t>(frame_size_) * channels_;
  if (!buffer_.empty()) {
    buffer_.resize(block, 0);
    encode_block();
  }
}

void AacEncoder::close() {
  if (enc_ && !flushed_)
    flush();
  auto *api = static_cast<VO_AUDIO_CODECAPI *>(api_);
  if (api && enc_)
    api->Uninit(enc_);
  delete api;
  delete static_cast<VO_MEM_OPERATOR *>(mem_);
  api_ = nullptr;
  mem_ = nullptr;
  enc_ = nullptr;
}

} // namespace frametap::enc
