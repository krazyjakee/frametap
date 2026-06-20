#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

// Byte-stream transports for the MPEG-TS sink: plain UDP and (optionally) SRT.
//
// SRT support is compiled in only when libsrt is available (FRAMETAP_HAVE_SRT);
// make_srt_transport() returns nullptr otherwise, so a build without libsrt
// still links -- srt:// URLs just fail at start() time.

namespace frametap::enc {

// Parsed "scheme://host:port?k=v&..." URL.
struct ParsedUrl {
  std::string scheme;
  std::string host;
  int port = 0;
  std::map<std::string, std::string> query;
};
bool parse_url(const std::string &url, ParsedUrl &out);

class Transport {
public:
  virtual ~Transport() = default;
  // Resolve/open the socket. Returns false and fills `err` on failure.
  virtual bool open(const ParsedUrl &url, std::string &err) = 0;
  // Send one datagram/message. Returns false on a fatal transport error.
  virtual bool send(const uint8_t *data, size_t size) = 0;
  virtual void close() = 0;
};

std::unique_ptr<Transport> make_udp_transport();
std::unique_ptr<Transport> make_srt_transport(); // nullptr if built without SRT

} // namespace frametap::enc
