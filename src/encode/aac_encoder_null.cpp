#include "encode/aac_encoder.h"

#include <stdexcept>

// Placeholder AAC encoder for platforms that don't yet compile the vendored
// vo-aacenc sources (currently Windows, where system-audio capture is also
// stubbed). It never produces packets: recordings/streams there are video-only,
// so this is never reached at runtime -- open() throws defensively if it ever
// is. Replaced by the real aac_encoder.cpp once a Windows audio backend lands.

namespace frametap::enc {

AacEncoder::~AacEncoder() = default;

void AacEncoder::open(int, int, int, PacketSink) {
  throw std::runtime_error("AAC encoding not implemented on this platform");
}

void AacEncoder::encode(const float *, uint32_t) {}
void AacEncoder::encode_block() {}
void AacEncoder::flush() {}
void AacEncoder::close() {}

} // namespace frametap::enc
