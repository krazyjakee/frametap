#include "decode/ts_demux.h"

#include <utility>

namespace frametap::dec {
namespace {

constexpr int kPidPat = 0x0000;
constexpr size_t kTsPacket = 188;

// Decode a 33-bit, 90 kHz PTS from the 5-byte PES timestamp field.
uint64_t read_pts(const uint8_t *b) {
  return (static_cast<uint64_t>(b[0] & 0x0E) << 29) |
         (static_cast<uint64_t>(b[1]) << 22) |
         (static_cast<uint64_t>(b[2] & 0xFE) << 14) |
         (static_cast<uint64_t>(b[3]) << 7) |
         (static_cast<uint64_t>(b[4]) >> 1);
}

// Find the elementary payload inside a complete PES packet and its PTS. Returns
// false if the buffer isn't a well-formed PES with a PTS.
bool split_pes(const std::vector<uint8_t> &pes, size_t &payload_off,
               uint64_t &pts) {
  if (pes.size() < 9 || pes[0] != 0x00 || pes[1] != 0x00 || pes[2] != 0x01)
    return false;
  const uint8_t pts_dts_flags = pes[7] & 0xC0;
  const uint8_t header_data_len = pes[8];
  payload_off = 9 + header_data_len;
  if (payload_off > pes.size())
    return false;
  if (pts_dts_flags & 0x80) {
    if (pes.size() < 14)
      return false;
    pts = read_pts(&pes[9]);
  } else {
    pts = 0;
  }
  return true;
}

} // namespace

void TsDemuxer::init(VideoSink video, AudioSink audio) {
  video_sink_ = std::move(video);
  audio_sink_ = std::move(audio);
}

void TsDemuxer::feed(const uint8_t *data, size_t size) {
  partial_.insert(partial_.end(), data, data + size);
  size_t i = 0;
  while (partial_.size() - i >= kTsPacket) {
    if (partial_[i] != 0x47) {
      ++i; // resync: scan forward to the next sync byte
      continue;
    }
    handle_packet(&partial_[i]);
    i += kTsPacket;
  }
  if (i > 0)
    partial_.erase(partial_.begin(), partial_.begin() + i);
}

void TsDemuxer::handle_packet(const uint8_t *pkt) {
  const bool pusi = pkt[1] & 0x40;
  const int pid = ((pkt[1] & 0x1F) << 8) | pkt[2];
  const int afc = (pkt[3] >> 4) & 0x3;

  size_t payload_off = 4;
  bool rai = false;
  if (afc == 2 || afc == 3) {
    const int af_len = pkt[4];
    if (af_len > 0)
      rai = pkt[5] & 0x40; // random_access_indicator
    payload_off = 5 + static_cast<size_t>(af_len);
  }
  if (afc == 2 || payload_off >= kTsPacket)
    return; // adaptation only / no payload

  const uint8_t *payload = pkt + payload_off;
  const size_t payload_len = kTsPacket - payload_off;

  // PSI tables (PAT / PMT) carry a pointer_field before the section.
  if (pid == kPidPat || pid == pmt_pid_) {
    if (!pusi)
      return; // our sections never span packets
    const size_t ptr = payload[0];
    const size_t sec_off = 1 + ptr;
    if (sec_off >= payload_len)
      return;
    const uint8_t *section = payload + sec_off;
    const size_t section_avail = payload_len - sec_off;
    if (pid == kPidPat)
      parse_pat(section, section_avail);
    else
      parse_pmt(section, section_avail);
    return;
  }

  PesBuf *buf = nullptr;
  bool is_video = false;
  if (pid == video_pid_) {
    buf = &video_;
    is_video = true;
  } else if (pid == audio_pid_) {
    buf = &audio_;
  } else {
    return; // unknown PID
  }

  if (pusi) {
    if (buf->started)
      is_video ? flush_video() : flush_audio();
    buf->data.clear();
    buf->started = true;
    buf->keyframe = rai;
  }
  if (buf->started)
    buf->data.insert(buf->data.end(), payload, payload + payload_len);
}

void TsDemuxer::parse_pat(const uint8_t *s, size_t len) {
  if (len < 8 || s[0] != 0x00)
    return;
  const size_t section_length = ((s[1] & 0x0F) << 8) | s[2];
  const size_t end = 3 + section_length; // includes 4-byte CRC
  if (end > len)
    return;
  const size_t prog_end = end - 4; // strip CRC
  for (size_t i = 8; i + 4 <= prog_end; i += 4) {
    const int prog = (s[i] << 8) | s[i + 1];
    const int pid = ((s[i + 2] & 0x1F) << 8) | s[i + 3];
    if (prog != 0) {
      pmt_pid_ = pid;
      break;
    }
  }
}

void TsDemuxer::parse_pmt(const uint8_t *s, size_t len) {
  if (len < 12 || s[0] != 0x02)
    return;
  const size_t section_length = ((s[1] & 0x0F) << 8) | s[2];
  const size_t end = 3 + section_length;
  if (end > len)
    return;
  const size_t es_end = end - 4; // strip CRC
  const size_t program_info_len = ((s[10] & 0x0F) << 8) | s[11];
  size_t i = 12 + program_info_len;
  while (i + 5 <= es_end) {
    const int stype = s[i];
    const int epid = ((s[i + 1] & 0x1F) << 8) | s[i + 2];
    const size_t es_info_len = ((s[i + 3] & 0x0F) << 8) | s[i + 4];
    if (stype == 0x1B) { // H.264
      video_pid_ = epid;
      hevc_ = false;
    } else if (stype == 0x24) { // HEVC
      video_pid_ = epid;
      hevc_ = true;
    } else if (stype == 0x0F) { // AAC (ADTS)
      audio_pid_ = epid;
    }
    i += 5 + es_info_len;
  }
}

void TsDemuxer::flush_video() {
  size_t off = 0;
  uint64_t pts = 0;
  if (video_sink_ && split_pes(video_.data, off, pts) &&
      off < video_.data.size())
    video_sink_(video_.data.data() + off, video_.data.size() - off,
                video_.keyframe, pts);
  video_.started = false;
}

void TsDemuxer::flush_audio() {
  size_t off = 0;
  uint64_t pts = 0;
  if (audio_sink_ && split_pes(audio_.data, off, pts) &&
      off < audio_.data.size())
    audio_sink_(audio_.data.data() + off, audio_.data.size() - off, pts);
  audio_.started = false;
}

void TsDemuxer::flush() {
  if (video_.started)
    flush_video();
  if (audio_.started)
    flush_audio();
}

} // namespace frametap::dec
