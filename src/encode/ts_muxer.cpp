#include "encode/ts_muxer.h"

#include <algorithm>
#include <cstring>

namespace frametap::enc {
namespace {

constexpr int kPidPat = 0x0000;
constexpr int kPidPmt = 0x1000;
constexpr int kPidVideo = 0x0100;
constexpr int kPidAudio = 0x0101;

constexpr uint8_t kStreamIdVideo = 0xE0;
constexpr uint8_t kStreamIdAudio = 0xC0;

constexpr size_t kTsPacket = 188;

void put_u16(std::vector<uint8_t> &v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}

// MPEG-2 systems CRC-32 (poly 0x04C11DB7, MSB-first, init 0xFFFFFFFF, no xorout).
uint32_t crc32_mpeg(const uint8_t *d, size_t n) {
  uint32_t crc = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) {
    crc ^= static_cast<uint32_t>(d[i]) << 24;
    for (int b = 0; b < 8; ++b)
      crc = (crc & 0x80000000u) ? (crc << 1) ^ 0x04C11DB7u : (crc << 1);
  }
  return crc;
}

void append_crc(std::vector<uint8_t> &s) {
  uint32_t crc = crc32_mpeg(s.data(), s.size());
  s.push_back(static_cast<uint8_t>(crc >> 24));
  s.push_back(static_cast<uint8_t>(crc >> 16));
  s.push_back(static_cast<uint8_t>(crc >> 8));
  s.push_back(static_cast<uint8_t>(crc));
}

int adts_freq_index(int rate) {
  static const int table[] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                              22050, 16000, 12000, 11025, 8000,  7350};
  for (int i = 0; i < 13; ++i)
    if (table[i] == rate)
      return i;
  return 3; // default 48 kHz
}

// Encode a 33-bit, 90 kHz timestamp into the 5-byte PTS/DTS field.
void put_pts(std::vector<uint8_t> &v, uint8_t prefix, uint64_t pts) {
  v.push_back(static_cast<uint8_t>((prefix << 4) | (((pts >> 30) & 0x7) << 1) | 1));
  v.push_back(static_cast<uint8_t>((pts >> 22) & 0xFF));
  v.push_back(static_cast<uint8_t>((((pts >> 15) & 0x7F) << 1) | 1));
  v.push_back(static_cast<uint8_t>((pts >> 7) & 0xFF));
  v.push_back(static_cast<uint8_t>(((pts & 0x7F) << 1) | 1));
}

} // namespace

void TsMuxer::init(bool hevc, bool has_audio, int audio_rate,
                   int audio_channels, ByteSink sink) {
  hevc_ = hevc;
  has_audio_ = has_audio;
  audio_rate_ = audio_rate > 0 ? audio_rate : 48000;
  audio_channels_ = audio_channels > 0 ? audio_channels : 2;
  sample_rate_index_ = adts_freq_index(audio_rate_);
  sink_ = std::move(sink);
}

uint8_t TsMuxer::next_cc(int pid) {
  uint8_t *cc = nullptr;
  switch (pid) {
  case kPidPat: cc = &pat_cc_; break;
  case kPidPmt: cc = &pmt_cc_; break;
  case kPidVideo: cc = &video_cc_; break;
  default: cc = &audio_cc_; break;
  }
  uint8_t cur = *cc & 0x0F;
  *cc = (cur + 1) & 0x0F;
  return cur;
}

void TsMuxer::emit_psi() {
  emit_pat();
  emit_pmt();
}

void TsMuxer::emit_pat() {
  std::vector<uint8_t> s;
  s.push_back(0x00); // table_id
  const size_t len_pos = s.size();
  s.push_back(0xB0); // ssi=1, '0', reserved=11, length hi nibble (patched)
  s.push_back(0x00); // length lo (patched)
  put_u16(s, 0x0001); // transport_stream_id
  s.push_back(0xC1);  // reserved=11, version=0, current_next=1
  s.push_back(0x00);  // section_number
  s.push_back(0x00);  // last_section_number
  put_u16(s, 0x0001); // program_number
  put_u16(s, static_cast<uint16_t>(0xE000 | kPidPmt));
  const uint32_t section_length = (s.size() - (len_pos + 2)) + 4;
  s[len_pos] = static_cast<uint8_t>(0xB0 | ((section_length >> 8) & 0x0F));
  s[len_pos + 1] = static_cast<uint8_t>(section_length & 0xFF);
  append_crc(s);

  uint8_t pkt[kTsPacket];
  std::memset(pkt, 0xFF, sizeof(pkt));
  pkt[0] = 0x47;
  pkt[1] = static_cast<uint8_t>(0x40 | ((kPidPat >> 8) & 0x1F));
  pkt[2] = static_cast<uint8_t>(kPidPat & 0xFF);
  pkt[3] = static_cast<uint8_t>(0x10 | next_cc(kPidPat));
  pkt[4] = 0x00; // pointer_field
  std::memcpy(pkt + 5, s.data(), s.size());
  sink_(pkt, kTsPacket);
}

void TsMuxer::emit_pmt() {
  std::vector<uint8_t> s;
  s.push_back(0x02); // table_id
  const size_t len_pos = s.size();
  s.push_back(0xB0);
  s.push_back(0x00);
  put_u16(s, 0x0001); // program_number
  s.push_back(static_cast<uint8_t>(0xC0 | ((pmt_version_ & 0x1F) << 1) | 1));
  s.push_back(0x00); // section_number
  s.push_back(0x00); // last_section_number
  put_u16(s, static_cast<uint16_t>(0xE000 | kPidVideo)); // PCR_PID
  put_u16(s, static_cast<uint16_t>(0xF000 | 0));         // program_info_length
  // Video ES.
  s.push_back(hevc_ ? 0x24 : 0x1B);
  put_u16(s, static_cast<uint16_t>(0xE000 | kPidVideo));
  put_u16(s, static_cast<uint16_t>(0xF000 | 0));
  // Audio ES (AAC ADTS).
  if (has_audio_) {
    s.push_back(0x0F);
    put_u16(s, static_cast<uint16_t>(0xE000 | kPidAudio));
    put_u16(s, static_cast<uint16_t>(0xF000 | 0));
  }
  const uint32_t section_length = (s.size() - (len_pos + 2)) + 4;
  s[len_pos] = static_cast<uint8_t>(0xB0 | ((section_length >> 8) & 0x0F));
  s[len_pos + 1] = static_cast<uint8_t>(section_length & 0xFF);
  append_crc(s);

  uint8_t pkt[kTsPacket];
  std::memset(pkt, 0xFF, sizeof(pkt));
  pkt[0] = 0x47;
  pkt[1] = static_cast<uint8_t>(0x40 | ((kPidPmt >> 8) & 0x1F));
  pkt[2] = static_cast<uint8_t>(kPidPmt & 0xFF);
  pkt[3] = static_cast<uint8_t>(0x10 | next_cc(kPidPmt));
  pkt[4] = 0x00; // pointer_field
  std::memcpy(pkt + 5, s.data(), s.size());
  sink_(pkt, kTsPacket);
}

void TsMuxer::emit_pes(int pid, uint8_t stream_id, const uint8_t *payload,
                       size_t len, uint64_t pts_90k, bool with_pcr, bool rai) {
  const bool video = (stream_id & 0xF0) == 0xE0;

  scratch_.clear();
  scratch_.push_back(0x00);
  scratch_.push_back(0x00);
  scratch_.push_back(0x01);
  scratch_.push_back(stream_id);
  // PES_packet_length: 0 (unbounded) for video, exact for audio.
  const uint16_t pes_len = video ? 0 : static_cast<uint16_t>(len + 8);
  put_u16(scratch_, pes_len);
  scratch_.push_back(0x84); // '10', data_alignment_indicator=1
  scratch_.push_back(0x80); // PTS_DTS_flags='10'
  scratch_.push_back(0x05); // PES_header_data_length
  put_pts(scratch_, 0x2, pts_90k);
  scratch_.insert(scratch_.end(), payload, payload + len);

  const uint8_t *pes = scratch_.data();
  const size_t total = scratch_.size();
  size_t offset = 0;
  bool first = true;

  while (offset < total) {
    uint8_t pkt[kTsPacket];
    pkt[0] = 0x47;
    pkt[1] = static_cast<uint8_t>((first ? 0x40 : 0x00) | ((pid >> 8) & 0x1F));
    pkt[2] = static_cast<uint8_t>(pid & 0xFF);

    // Adaptation field content (after the length byte). Sized for a full
    // packet's worth of stuffing.
    uint8_t af[kTsPacket];
    size_t af_len = 0;
    bool want_af = false;
    if (first && (with_pcr || rai)) {
      want_af = true;
      af[af_len++] = static_cast<uint8_t>(rai ? 0x40 : 0x00); // flags
      if (with_pcr) {
        af[0] |= 0x10; // PCR_flag
        const uint64_t base = pts_90k;
        af[af_len++] = static_cast<uint8_t>((base >> 25) & 0xFF);
        af[af_len++] = static_cast<uint8_t>((base >> 17) & 0xFF);
        af[af_len++] = static_cast<uint8_t>((base >> 9) & 0xFF);
        af[af_len++] = static_cast<uint8_t>((base >> 1) & 0xFF);
        af[af_len++] = static_cast<uint8_t>(((base & 1) << 7) | 0x7E);
        af[af_len++] = 0x00; // PCR extension
      }
    }

    const size_t remaining = total - offset;
    size_t fixed = 4 + (want_af ? (1 + af_len) : 0);
    size_t space = kTsPacket - fixed;

    if (remaining < space) {
      // Pad with adaptation-field stuffing so the PES ends on a packet boundary.
      if (!want_af) {
        want_af = true;
        af[af_len++] = 0x00; // empty flags
      }
      const size_t af_total = kTsPacket - 4 - 1 - remaining;
      while (af_len < af_total)
        af[af_len++] = 0xFF;
      space = remaining;
    }

    pkt[3] = static_cast<uint8_t>((want_af ? 0x30 : 0x10) | next_cc(pid));
    size_t pos = 4;
    if (want_af) {
      pkt[pos++] = static_cast<uint8_t>(af_len);
      std::memcpy(pkt + pos, af, af_len);
      pos += af_len;
    }
    const size_t take = std::min(space, remaining);
    std::memcpy(pkt + pos, pes + offset, take);
    pos += take;
    offset += take;
    // pos is always exactly 188 here (stuffing path pads, else space fills it).
    sink_(pkt, kTsPacket);
    first = false;
  }
}

void TsMuxer::write_video(const uint8_t *annexb, size_t size, bool keyframe,
                          uint64_t pts_90k) {
  if (!sink_ || size == 0)
    return;
  if (keyframe)
    emit_psi(); // let mid-stream joiners lock on before the IDR
  emit_pes(kPidVideo, kStreamIdVideo, annexb, size, pts_90k, true, keyframe);
}

void TsMuxer::write_audio(const uint8_t *aac, size_t size, uint64_t pts_90k) {
  if (!sink_ || !has_audio_ || size == 0)
    return;
  const size_t frame_len = size + 7;
  const int profile_minus1 = 1; // AAC-LC
  uint8_t adts[7];
  adts[0] = 0xFF;
  adts[1] = 0xF1; // MPEG-4, no CRC
  adts[2] = static_cast<uint8_t>((profile_minus1 << 6) |
                                 (sample_rate_index_ << 2) |
                                 ((audio_channels_ >> 2) & 0x1));
  adts[3] = static_cast<uint8_t>(((audio_channels_ & 0x3) << 6) |
                                 ((frame_len >> 11) & 0x3));
  adts[4] = static_cast<uint8_t>((frame_len >> 3) & 0xFF);
  adts[5] = static_cast<uint8_t>(((frame_len & 0x7) << 5) | 0x1F);
  adts[6] = 0xFC;

  std::vector<uint8_t> buf;
  buf.reserve(frame_len);
  buf.insert(buf.end(), adts, adts + 7);
  buf.insert(buf.end(), aac, aac + size);
  emit_pes(kPidAudio, kStreamIdAudio, buf.data(), buf.size(), pts_90k, false,
           false);
}

} // namespace frametap::enc
