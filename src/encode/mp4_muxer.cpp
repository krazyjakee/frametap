#include "encode/mp4_muxer.h"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace frametap::enc {
namespace {

using Bytes = std::vector<uint8_t>;

void put_u16(Bytes &v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}
void put_u32(Bytes &v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x >> 24));
  v.push_back(static_cast<uint8_t>(x >> 16));
  v.push_back(static_cast<uint8_t>(x >> 8));
  v.push_back(static_cast<uint8_t>(x));
}
void put_u64(Bytes &v, uint64_t x) {
  for (int i = 7; i >= 0; --i)
    v.push_back(static_cast<uint8_t>(x >> (i * 8)));
}
void put_bytes(Bytes &v, const uint8_t *d, size_t n) { v.insert(v.end(), d, d + n); }
void put_fourcc(Bytes &v, const char *s) { v.insert(v.end(), s, s + 4); }

// Begin a box: write a placeholder size + type, return the size-field offset.
size_t box_begin(Bytes &v, const char *type) {
  size_t pos = v.size();
  put_u32(v, 0);
  put_fourcc(v, type);
  return pos;
}
void box_end(Bytes &v, size_t pos) {
  uint32_t size = static_cast<uint32_t>(v.size() - pos);
  v[pos] = static_cast<uint8_t>(size >> 24);
  v[pos + 1] = static_cast<uint8_t>(size >> 16);
  v[pos + 2] = static_cast<uint8_t>(size >> 8);
  v[pos + 3] = static_cast<uint8_t>(size);
}

// Split an Annex-B buffer into NAL payloads (start codes removed, trailing
// zero bytes trimmed).
std::vector<std::pair<const uint8_t *, size_t>> split_nals(const uint8_t *d,
                                                           size_t n) {
  std::vector<std::pair<const uint8_t *, size_t>> out;
  auto sc_len = [&](size_t p) -> int {
    if (p + 3 <= n && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 1)
      return 3;
    if (p + 4 <= n && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 0 &&
        d[p + 3] == 1)
      return 4;
    return 0;
  };
  size_t p = 0;
  while (p < n && sc_len(p) == 0)
    ++p;
  while (p < n) {
    int sc = sc_len(p);
    size_t start = p + sc;
    size_t q = start;
    while (q < n && sc_len(q) == 0)
      ++q;
    size_t len = q - start;
    while (len > 0 && d[start + len - 1] == 0)
      --len;
    if (len > 0)
      out.emplace_back(d + start, len);
    p = q;
  }
  return out;
}

// Strip emulation-prevention bytes (00 00 03 -> 00 00) for header parsing.
Bytes to_rbsp(const uint8_t *d, size_t n) {
  Bytes r;
  r.reserve(n);
  int zeros = 0;
  for (size_t i = 0; i < n; ++i) {
    uint8_t b = d[i];
    if (zeros >= 2 && b == 3) {
      zeros = 0;
      continue;
    }
    r.push_back(b);
    zeros = (b == 0) ? zeros + 1 : 0;
  }
  return r;
}

Bytes build_avcc(const Bytes &sps, const Bytes &pps) {
  Bytes v;
  v.push_back(1); // configurationVersion
  v.push_back(sps.size() > 1 ? sps[1] : 0); // AVCProfileIndication
  v.push_back(sps.size() > 2 ? sps[2] : 0); // profile_compatibility
  v.push_back(sps.size() > 3 ? sps[3] : 0); // AVCLevelIndication
  v.push_back(0xff);                        // reserved(6) + lengthSizeMinusOne=3
  v.push_back(0xe1);                        // reserved(3) + numOfSPS=1
  put_u16(v, static_cast<uint16_t>(sps.size()));
  put_bytes(v, sps.data(), sps.size());
  v.push_back(1); // numOfPPS
  put_u16(v, static_cast<uint16_t>(pps.size()));
  put_bytes(v, pps.data(), pps.size());
  return v;
}

Bytes build_hvcc(const Bytes &vps, const Bytes &sps, const Bytes &pps) {
  // general profile_tier_level: 12 bytes following the 2-byte NAL header and
  // one byte of vps_id/max_sub_layers/nesting in the SPS RBSP.
  Bytes rbsp = to_rbsp(sps.data(), sps.size());
  uint8_t ptl[12] = {0};
  if (rbsp.size() >= 15)
    std::memcpy(ptl, &rbsp[3], 12);

  Bytes v;
  v.push_back(1);      // configurationVersion
  v.push_back(ptl[0]); // general_profile_space/tier_flag/profile_idc
  put_bytes(v, &ptl[1], 4);  // general_profile_compatibility_flags
  put_bytes(v, &ptl[5], 6);  // general_constraint_indicator_flags
  v.push_back(ptl[11]);      // general_level_idc
  v.push_back(0xf0);         // reserved(4) + min_spatial_segmentation_idc hi
  v.push_back(0x00);         // min_spatial_segmentation_idc lo
  v.push_back(0xfc);         // reserved(6) + parallelismType=0
  v.push_back(0xfd);         // reserved(6) + chromaFormat=1 (4:2:0)
  v.push_back(0xf8);         // reserved(5) + bitDepthLumaMinus8=0
  v.push_back(0xf8);         // reserved(5) + bitDepthChromaMinus8=0
  put_u16(v, 0);             // avgFrameRate
  // constantFrameRate=0, numTemporalLayers=1, temporalIdNested=0, lenSize-1=3
  v.push_back((1 << 3) | 3);
  v.push_back(3); // numOfArrays
  auto put_array = [&](int nal_type, const Bytes &nal) {
    v.push_back(static_cast<uint8_t>(0x80 | nal_type)); // complete + type
    put_u16(v, 1);                                      // numNalus
    put_u16(v, static_cast<uint16_t>(nal.size()));
    put_bytes(v, nal.data(), nal.size());
  };
  put_array(32, vps);
  put_array(33, sps);
  put_array(34, pps);
  return v;
}

const uint8_t kMatrix[36] = {0x00, 0x01, 0x00, 0x00, 0, 0, 0, 0, 0,
                             0,    0,    0,    0,    0, 0x00, 0x01, 0x00, 0x00,
                             0,    0,    0,    0,    0, 0, 0,    0,    0,    0,
                             0x40, 0x00, 0x00, 0x00};

void put_full_box_header(Bytes &v, uint8_t version, uint32_t flags) {
  v.push_back(version);
  v.push_back(static_cast<uint8_t>(flags >> 16));
  v.push_back(static_cast<uint8_t>(flags >> 8));
  v.push_back(static_cast<uint8_t>(flags));
}

// MPEG-4 descriptor: 1-byte tag + 1-byte length (descriptors here are small) +
// payload.
void put_descriptor(Bytes &v, uint8_t tag, const Bytes &payload) {
  v.push_back(tag);
  v.push_back(static_cast<uint8_t>(payload.size())); // < 128
  put_bytes(v, payload.data(), payload.size());
}

// esds contents for an AAC track (without the box header).
Bytes build_esds(const Bytes &asc) {
  Bytes dec_specific;
  dec_specific.insert(dec_specific.end(), asc.begin(), asc.end());

  Bytes dec_config;
  dec_config.push_back(0x40); // objectTypeIndication: Audio ISO/IEC 14496-3
  dec_config.push_back(0x15); // streamType=5 (audio) | upStream=0 | reserved=1
  dec_config.push_back(0);    // bufferSizeDB (24-bit)
  put_u16(dec_config, 0);
  put_u32(dec_config, 0);     // maxBitrate
  put_u32(dec_config, 0);     // avgBitrate
  put_descriptor(dec_config, 0x05, dec_specific); // DecoderSpecificInfo

  Bytes sl;
  sl.push_back(0x02); // predefined SL config

  Bytes es;
  put_u16(es, 0); // ES_ID
  es.push_back(0); // flags
  put_descriptor(es, 0x04, dec_config); // DecoderConfigDescriptor
  put_descriptor(es, 0x06, sl);         // SLConfigDescriptor

  Bytes v;
  put_full_box_header(v, 0, 0);
  put_descriptor(v, 0x03, es); // ES_Descriptor
  return v;
}

} // namespace

Mp4Muxer::~Mp4Muxer() { close(); }

void Mp4Muxer::open(const std::string &path, bool hevc, int width, int height,
                    int fps) {
  file_.open(path, std::ios::binary | std::ios::trunc);
  if (!file_)
    throw std::runtime_error("MP4: cannot open output file: " + path);
  hevc_ = hevc;
  width_ = width;
  height_ = height;
  fps_ = fps > 0 ? fps : 60;
  finalized_ = false;
  samples_.clear();
  sps_.clear();
  pps_.clear();
  vps_.clear();
  mdat_payload_ = 0;

  // ftyp
  Bytes ftyp;
  size_t b = box_begin(ftyp, "ftyp");
  put_fourcc(ftyp, "isom");
  put_u32(ftyp, 0x200);
  put_fourcc(ftyp, "isom");
  put_fourcc(ftyp, "iso2");
  put_fourcc(ftyp, hevc_ ? "hvc1" : "avc1");
  put_fourcc(ftyp, "mp41");
  box_end(ftyp, b);
  file_.write(reinterpret_cast<const char *>(ftyp.data()),
              static_cast<std::streamsize>(ftyp.size()));

  // mdat with a 64-bit largesize, patched on close.
  Bytes mdat;
  put_u32(mdat, 1); // size==1 -> use largesize
  put_fourcc(mdat, "mdat");
  mdat_size_pos_ = static_cast<uint64_t>(file_.tellp()) + mdat.size();
  for (int i = 0; i < 8; ++i)
    mdat.push_back(0); // largesize placeholder
  file_.write(reinterpret_cast<const char *>(mdat.data()),
              static_cast<std::streamsize>(mdat.size()));
}

void Mp4Muxer::write_access_unit(const uint8_t *data, size_t size,
                                 bool keyframe, uint64_t pts_90k) {
  if (!file_ || finalized_)
    return;

  Bytes sample;
  for (auto [nal, len] : split_nals(data, size)) {
    int type = hevc_ ? ((nal[0] >> 1) & 0x3f) : (nal[0] & 0x1f);
    bool is_param = hevc_ ? (type == 32 || type == 33 || type == 34)
                          : (type == 7 || type == 8);
    if (is_param) {
      Bytes ps(nal, nal + len);
      if (hevc_) {
        if (type == 32 && vps_.empty())
          vps_ = std::move(ps);
        else if (type == 33 && sps_.empty())
          sps_ = std::move(ps);
        else if (type == 34 && pps_.empty())
          pps_ = std::move(ps);
      } else {
        if (type == 7 && sps_.empty())
          sps_ = std::move(ps);
        else if (type == 8 && pps_.empty())
          pps_ = std::move(ps);
      }
      continue; // parameter sets live in stsd, not in the sample
    }
    put_u32(sample, static_cast<uint32_t>(len)); // length prefix
    put_bytes(sample, nal, len);
  }
  if (sample.empty())
    return;

  file_.write(reinterpret_cast<const char *>(sample.data()),
              static_cast<std::streamsize>(sample.size()));
  samples_.push_back({static_cast<uint32_t>(sample.size()), keyframe, pts_90k});
  mdat_payload_ += sample.size();
}

void Mp4Muxer::set_audio(int sample_rate, int channels, const uint8_t *asc,
                         size_t asc_len, int samples_per_frame,
                         uint64_t start_delay_90k) {
  if (finalized_)
    return;
  has_audio_ = true;
  audio_rate_ = sample_rate > 0 ? sample_rate : 48000;
  audio_channels_ = channels > 0 ? channels : 2;
  audio_frame_samples_ = samples_per_frame > 0 ? samples_per_frame : 1024;
  audio_start_delay_90k_ = start_delay_90k;
  audio_asc_.assign(asc, asc + asc_len);
}

void Mp4Muxer::write_audio_sample(const uint8_t *data, size_t size) {
  if (!has_audio_ || finalized_ || size == 0)
    return;
  audio_data_.insert(audio_data_.end(), data, data + size);
  audio_sample_sizes_.push_back(static_cast<uint32_t>(size));
}

void Mp4Muxer::finalize() {
  const bool with_audio = has_audio_ && !audio_data_.empty();

  // Append the audio samples as a second chunk at the end of mdat.
  file_.seekp(0, std::ios::end);
  uint64_t audio_chunk_offset = 0;
  if (with_audio) {
    audio_chunk_offset = static_cast<uint64_t>(file_.tellp());
    file_.write(reinterpret_cast<const char *>(audio_data_.data()),
                static_cast<std::streamsize>(audio_data_.size()));
    mdat_payload_ += audio_data_.size();
  }

  // Patch the mdat largesize (now covers video + audio payload).
  const uint64_t mdat_box_size = 16 + mdat_payload_;
  file_.seekp(static_cast<std::streamoff>(mdat_size_pos_));
  uint8_t big[8];
  for (int i = 0; i < 8; ++i)
    big[i] = static_cast<uint8_t>(mdat_box_size >> ((7 - i) * 8));
  file_.write(reinterpret_cast<const char *>(big), 8);
  file_.seekp(0, std::ios::end);

  const uint32_t timescale = 90000;
  const uint32_t default_delta = timescale / static_cast<uint32_t>(fps_);
  const uint32_t n = static_cast<uint32_t>(samples_.size());
  const uint64_t chunk_offset = mdat_size_pos_ + 8;

  // Real per-sample durations from frame presentation times (run-length
  // encoded for stts). The last frame has no successor, so it reuses the
  // previous gap (or the nominal frame time for a single-frame clip).
  std::vector<std::pair<uint32_t, uint32_t>> stts_runs; // (count, delta)
  uint32_t duration = 0;
  for (uint32_t i = 0; i < n; ++i) {
    uint32_t d;
    if (i + 1 < n)
      d = static_cast<uint32_t>(samples_[i + 1].pts - samples_[i].pts);
    else
      d = (n >= 2) ? static_cast<uint32_t>(samples_[i].pts -
                                           samples_[i - 1].pts)
                   : default_delta;
    if (d == 0)
      d = 1;
    if (!stts_runs.empty() && stts_runs.back().second == d)
      stts_runs.back().first++;
    else
      stts_runs.emplace_back(1, d);
    duration += d;
  }

  // Audio track timing (in the audio media timescale and the 90 kHz movie
  // timescale). The empty-edit delay pushes audio presentation to match when
  // capture actually started relative to the first video frame.
  const uint32_t audio_n = static_cast<uint32_t>(audio_sample_sizes_.size());
  const uint32_t audio_delta = static_cast<uint32_t>(audio_frame_samples_);
  const uint32_t audio_media_dur = audio_n * audio_delta; // audio timescale
  const uint32_t audio_movie_dur = static_cast<uint32_t>(
      static_cast<uint64_t>(audio_n) * audio_delta * timescale / audio_rate_);
  const uint32_t audio_track_dur =
      audio_movie_dur + static_cast<uint32_t>(audio_start_delay_90k_);

  // Movie duration is the longest track in the movie timescale.
  uint32_t movie_duration = duration;
  if (with_audio && audio_track_dur > movie_duration)
    movie_duration = audio_track_dur;

  Bytes config = hevc_ ? build_hvcc(vps_, sps_, pps_) : build_avcc(sps_, pps_);

  Bytes m;
  size_t moov = box_begin(m, "moov");

  // Single-chunk offset table: stco (32-bit) when the offset fits, else co64
  // (64-bit) so recordings past 4 GB still reference their samples correctly.
  auto put_chunk_offset_box = [&](uint64_t offset) {
    if (offset <= 0xFFFFFFFFull) {
      size_t b = box_begin(m, "stco");
      put_full_box_header(m, 0, 0);
      put_u32(m, 1); // entry_count
      put_u32(m, static_cast<uint32_t>(offset));
      box_end(m, b);
    } else {
      size_t b = box_begin(m, "co64");
      put_full_box_header(m, 0, 0);
      put_u32(m, 1); // entry_count
      put_u64(m, offset);
      box_end(m, b);
    }
  };

  // mvhd
  {
    size_t b = box_begin(m, "mvhd");
    put_full_box_header(m, 0, 0);
    put_u32(m, 0); // creation
    put_u32(m, 0); // modification
    put_u32(m, timescale);
    put_u32(m, movie_duration);
    put_u32(m, 0x00010000); // rate 1.0
    put_u16(m, 0x0100);     // volume 1.0
    put_u16(m, 0);          // reserved
    put_u32(m, 0);
    put_u32(m, 0);
    put_bytes(m, kMatrix, 36);
    for (int i = 0; i < 6; ++i)
      put_u32(m, 0);                       // pre_defined
    put_u32(m, with_audio ? 3 : 2);        // next_track_id
    box_end(m, b);
  }

  // trak
  {
    size_t trak = box_begin(m, "trak");
    {
      size_t b = box_begin(m, "tkhd");
      put_full_box_header(m, 0, 0x000007); // enabled | in movie | in preview
      put_u32(m, 0);
      put_u32(m, 0);
      put_u32(m, 1); // track_id
      put_u32(m, 0); // reserved
      put_u32(m, duration);
      put_u32(m, 0);
      put_u32(m, 0);
      put_u16(m, 0); // layer
      put_u16(m, 0); // alternate_group
      put_u16(m, 0); // volume (video)
      put_u16(m, 0); // reserved
      put_bytes(m, kMatrix, 36);
      put_u32(m, static_cast<uint32_t>(width_) << 16);
      put_u32(m, static_cast<uint32_t>(height_) << 16);
      box_end(m, b);
    }
    {
      size_t mdia = box_begin(m, "mdia");
      {
        size_t b = box_begin(m, "mdhd");
        put_full_box_header(m, 0, 0);
        put_u32(m, 0);
        put_u32(m, 0);
        put_u32(m, timescale);
        put_u32(m, duration);
        put_u16(m, 0x55c4); // 'und'
        put_u16(m, 0);
        box_end(m, b);
      }
      {
        size_t b = box_begin(m, "hdlr");
        put_full_box_header(m, 0, 0);
        put_u32(m, 0); // pre_defined
        put_fourcc(m, "vide");
        put_u32(m, 0);
        put_u32(m, 0);
        put_u32(m, 0);
        const char *name = "VideoHandler";
        put_bytes(m, reinterpret_cast<const uint8_t *>(name),
                  std::strlen(name) + 1);
        box_end(m, b);
      }
      {
        size_t minf = box_begin(m, "minf");
        {
          size_t b = box_begin(m, "vmhd");
          put_full_box_header(m, 0, 1);
          put_u16(m, 0); // graphicsmode
          for (int i = 0; i < 3; ++i)
            put_u16(m, 0); // opcolor
          box_end(m, b);
        }
        {
          size_t dinf = box_begin(m, "dinf");
          size_t dref = box_begin(m, "dref");
          put_full_box_header(m, 0, 0);
          put_u32(m, 1); // entry_count
          size_t url = box_begin(m, "url ");
          put_full_box_header(m, 0, 1); // self-contained
          box_end(m, url);
          box_end(m, dref);
          box_end(m, dinf);
        }
        {
          size_t stbl = box_begin(m, "stbl");
          // stsd
          {
            size_t b = box_begin(m, "stsd");
            put_full_box_header(m, 0, 0);
            put_u32(m, 1); // entry_count
            size_t se = box_begin(m, hevc_ ? "hvc1" : "avc1");
            for (int i = 0; i < 6; ++i)
              m.push_back(0);          // reserved
            put_u16(m, 1);             // data_reference_index
            put_u16(m, 0);             // pre_defined
            put_u16(m, 0);             // reserved
            for (int i = 0; i < 3; ++i)
              put_u32(m, 0);           // pre_defined
            put_u16(m, static_cast<uint16_t>(width_));
            put_u16(m, static_cast<uint16_t>(height_));
            put_u32(m, 0x00480000);    // horizresolution 72dpi
            put_u32(m, 0x00480000);    // vertresolution 72dpi
            put_u32(m, 0);             // reserved
            put_u16(m, 1);             // frame_count
            for (int i = 0; i < 32; ++i)
              m.push_back(0);          // compressorname
            put_u16(m, 0x0018);        // depth
            put_u16(m, 0xffff);        // pre_defined
            // config box (avcC / hvcC)
            size_t cb = box_begin(m, hevc_ ? "hvcC" : "avcC");
            put_bytes(m, config.data(), config.size());
            box_end(m, cb);
            box_end(m, se);
            box_end(m, b);
          }
          // stts
          {
            size_t b = box_begin(m, "stts");
            put_full_box_header(m, 0, 0);
            put_u32(m, static_cast<uint32_t>(stts_runs.size()));
            for (auto [count, d] : stts_runs) {
              put_u32(m, count);
              put_u32(m, d);
            }
            box_end(m, b);
          }
          // stss (sync samples)
          {
            Bytes idx;
            for (uint32_t i = 0; i < n; ++i)
              if (samples_[i].keyframe)
                put_u32(idx, i + 1);
            if (!idx.empty()) {
              size_t b = box_begin(m, "stss");
              put_full_box_header(m, 0, 0);
              put_u32(m, static_cast<uint32_t>(idx.size() / 4));
              put_bytes(m, idx.data(), idx.size());
              box_end(m, b);
            }
          }
          // stsc
          {
            size_t b = box_begin(m, "stsc");
            put_full_box_header(m, 0, 0);
            put_u32(m, 1); // entry_count
            put_u32(m, 1); // first_chunk
            put_u32(m, n); // samples_per_chunk
            put_u32(m, 1); // sample_description_index
            box_end(m, b);
          }
          // stsz
          {
            size_t b = box_begin(m, "stsz");
            put_full_box_header(m, 0, 0);
            put_u32(m, 0); // sample_size (0 = varying)
            put_u32(m, n);
            for (const auto &s : samples_)
              put_u32(m, s.size);
            box_end(m, b);
          }
          // stco / co64
          put_chunk_offset_box(chunk_offset);
          box_end(m, stbl);
        }
        box_end(m, minf);
      }
      box_end(m, mdia);
    }
    box_end(m, trak);
  }

  // Audio trak (AAC / mp4a)
  if (with_audio) {
    const uint32_t an = audio_n;
    const uint32_t adelta = audio_delta;
    const uint32_t aduration = audio_media_dur; // in audio timescale

    size_t trak = box_begin(m, "trak");
    {
      size_t b = box_begin(m, "tkhd");
      put_full_box_header(m, 0, 0x000007);
      put_u32(m, 0);
      put_u32(m, 0);
      put_u32(m, 2); // track_id
      put_u32(m, 0);
      put_u32(m, audio_track_dur); // movie timescale, includes empty edit
      put_u32(m, 0);
      put_u32(m, 0);
      put_u16(m, 0);      // layer
      put_u16(m, 1);      // alternate_group
      put_u16(m, 0x0100); // volume 1.0
      put_u16(m, 0);
      put_bytes(m, kMatrix, 36);
      put_u32(m, 0); // width
      put_u32(m, 0); // height
      box_end(m, b);
    }
    // edts/elst: delay audio presentation with an empty edit so it lines up
    // with the video (capture didn't necessarily start audio at frame 0).
    if (audio_start_delay_90k_ > 0) {
      size_t edts = box_begin(m, "edts");
      size_t b = box_begin(m, "elst");
      put_full_box_header(m, 0, 0);
      put_u32(m, 2); // entry_count: empty edit, then the media
      // Empty edit: media_time = -1, lasts the delay (movie timescale).
      put_u32(m, static_cast<uint32_t>(audio_start_delay_90k_));
      put_u32(m, 0xFFFFFFFF);
      put_u16(m, 1); // media_rate_integer 1.0
      put_u16(m, 0); // media_rate_fraction
      // The audio media itself, played from its start.
      put_u32(m, audio_movie_dur);
      put_u32(m, 0);
      put_u16(m, 1);
      put_u16(m, 0);
      box_end(m, b);
      box_end(m, edts);
    }
    {
      size_t mdia = box_begin(m, "mdia");
      {
        size_t b = box_begin(m, "mdhd");
        put_full_box_header(m, 0, 0);
        put_u32(m, 0);
        put_u32(m, 0);
        put_u32(m, static_cast<uint32_t>(audio_rate_));
        put_u32(m, aduration);
        put_u16(m, 0x55c4);
        put_u16(m, 0);
        box_end(m, b);
      }
      {
        size_t b = box_begin(m, "hdlr");
        put_full_box_header(m, 0, 0);
        put_u32(m, 0);
        put_fourcc(m, "soun");
        put_u32(m, 0);
        put_u32(m, 0);
        put_u32(m, 0);
        const char *name = "SoundHandler";
        put_bytes(m, reinterpret_cast<const uint8_t *>(name),
                  std::strlen(name) + 1);
        box_end(m, b);
      }
      {
        size_t minf = box_begin(m, "minf");
        {
          size_t b = box_begin(m, "smhd");
          put_full_box_header(m, 0, 0);
          put_u16(m, 0); // balance
          put_u16(m, 0); // reserved
          box_end(m, b);
        }
        {
          size_t dinf = box_begin(m, "dinf");
          size_t dref = box_begin(m, "dref");
          put_full_box_header(m, 0, 0);
          put_u32(m, 1);
          size_t url = box_begin(m, "url ");
          put_full_box_header(m, 0, 1);
          box_end(m, url);
          box_end(m, dref);
          box_end(m, dinf);
        }
        {
          size_t stbl = box_begin(m, "stbl");
          {
            size_t b = box_begin(m, "stsd");
            put_full_box_header(m, 0, 0);
            put_u32(m, 1);
            size_t se = box_begin(m, "mp4a");
            for (int i = 0; i < 6; ++i)
              m.push_back(0);  // reserved
            put_u16(m, 1);     // data_reference_index
            put_u32(m, 0);     // reserved
            put_u32(m, 0);     // reserved
            put_u16(m, static_cast<uint16_t>(audio_channels_));
            put_u16(m, 16);    // samplesize
            put_u16(m, 0);     // pre_defined
            put_u16(m, 0);     // reserved
            put_u32(m, static_cast<uint32_t>(audio_rate_) << 16);
            size_t esds = box_begin(m, "esds");
            Bytes e = build_esds(audio_asc_);
            put_bytes(m, e.data(), e.size());
            box_end(m, esds);
            box_end(m, se);
            box_end(m, b);
          }
          {
            size_t b = box_begin(m, "stts");
            put_full_box_header(m, 0, 0);
            put_u32(m, 1);
            put_u32(m, an);
            put_u32(m, adelta);
            box_end(m, b);
          }
          {
            size_t b = box_begin(m, "stsc");
            put_full_box_header(m, 0, 0);
            put_u32(m, 1);
            put_u32(m, 1);  // first_chunk
            put_u32(m, an); // samples_per_chunk
            put_u32(m, 1);
            box_end(m, b);
          }
          {
            size_t b = box_begin(m, "stsz");
            put_full_box_header(m, 0, 0);
            put_u32(m, 0);
            put_u32(m, an);
            for (uint32_t sz : audio_sample_sizes_)
              put_u32(m, sz);
            box_end(m, b);
          }
          put_chunk_offset_box(audio_chunk_offset);
          box_end(m, stbl);
        }
        box_end(m, minf);
      }
      box_end(m, mdia);
    }
    box_end(m, trak);
  }

  box_end(m, moov);

  file_.write(reinterpret_cast<const char *>(m.data()),
              static_cast<std::streamsize>(m.size()));
}

void Mp4Muxer::close() {
  if (!file_.is_open() || finalized_)
    return;
  finalized_ = true;
  finalize();
  file_.flush();
  file_.close();
}

} // namespace frametap::enc
