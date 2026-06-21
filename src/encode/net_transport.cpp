#include "encode/net_transport.h"

#include "encode/net_compat.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef FRAMETAP_HAVE_SRT
#include <srt/srt.h>
#endif

namespace frametap::enc {

bool parse_url(const std::string &url, ParsedUrl &out) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos)
    return false;
  out.scheme = url.substr(0, scheme_end);

  size_t pos = scheme_end + 3;
  const size_t auth_end = url.find_first_of("/?", pos);
  std::string authority = url.substr(pos, auth_end - pos);

  // Strip a leading '@' (e.g. udp://@:1234 -- a bind hint we treat as no host).
  if (!authority.empty() && authority[0] == '@')
    authority.erase(0, 1);

  const auto colon = authority.rfind(':');
  if (colon == std::string::npos)
    return false;
  out.host = authority.substr(0, colon);
  out.port = std::atoi(authority.substr(colon + 1).c_str());
  if (out.port <= 0)
    return false;

  // Query string.
  const auto q = url.find('?', pos);
  if (q != std::string::npos) {
    std::string qs = url.substr(q + 1);
    size_t i = 0;
    while (i < qs.size()) {
      size_t amp = qs.find('&', i);
      std::string kv = qs.substr(i, amp - i);
      const auto eq = kv.find('=');
      if (eq != std::string::npos)
        out.query[kv.substr(0, eq)] = kv.substr(eq + 1);
      if (amp == std::string::npos)
        break;
      i = amp + 1;
    }
  }
  return true;
}

namespace {

// 7 TS packets per datagram (1316 bytes) is the conventional MPEG-TS payload.
class UdpTransport : public Transport {
public:
  bool open(const ParsedUrl &url, std::string &err) override {
    net_global_init();
    std::string host = url.host.empty() ? "127.0.0.1" : url.host;
    char port[16];
    std::snprintf(port, sizeof(port), "%d", url.port);

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    if (int r = getaddrinfo(host.c_str(), port, &hints, &res); r != 0) {
      err = std::string("udp: getaddrinfo: ") + gai_strerror(r);
      return false;
    }
    for (addrinfo *ai = res; ai; ai = ai->ai_next) {
      fd_ = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd_ == kInvalidSocket)
        continue;
      if (::connect(fd_, ai->ai_addr,
                    static_cast<int>(ai->ai_addrlen)) == 0)
        break;
      close_socket(fd_);
      fd_ = kInvalidSocket;
    }
    freeaddrinfo(res);
    if (fd_ == kInvalidSocket) {
      err = "udp: could not open socket to " + host + ":" + port;
      return false;
    }
    return true;
  }

  bool send(const uint8_t *data, size_t size) override {
    if (fd_ == kInvalidSocket)
      return false;
    return sock_send(fd_, data, size) == static_cast<long>(size);
  }

  void close() override {
    if (fd_ != kInvalidSocket) {
      close_socket(fd_);
      fd_ = kInvalidSocket;
    }
  }

  ~UdpTransport() override { close(); }

private:
  socket_t fd_ = kInvalidSocket;
};

#ifdef FRAMETAP_HAVE_SRT
class SrtTransport : public Transport {
public:
  bool open(const ParsedUrl &url, std::string &err) override {
    srt_startup();
    // Quiet SRT's own logging: the non-blocking accept poll otherwise emits an
    // error line per attempt ("no pending connection available").
    srt_setloglevel(LOG_CRIT);
    sock_ = srt_create_socket();
    if (sock_ == SRT_INVALID_SOCK) {
      err = std::string("srt: create_socket: ") + srt_getlasterror_str();
      return false;
    }
    int live = SRTT_LIVE;
    srt_setsockopt(sock_, 0, SRTO_TRANSTYPE, &live, sizeof(live));

    // SRT's live profile adds a 120 ms receiver buffer (SRTO_LATENCY) to absorb
    // public-internet jitter. On a LAN / localhost that is pure added delay, so
    // honor an optional ?latency=<ms> from the URL. It's set on both peers
    // (caller and listener) since the effective latency is the max of the two
    // ends; set it before bind/connect so the accepted socket inherits it.
    if (const auto lq = url.query.find("latency"); lq != url.query.end()) {
      int ms = std::atoi(lq->second.c_str());
      if (ms >= 0 && ms <= 5000)
        srt_setsockopt(sock_, 0, SRTO_LATENCY, &ms, sizeof(ms));
    }

    const auto it = url.query.find("mode");
    const bool listener =
        (it != url.query.end() && it->second == "listener") ||
        url.host.empty() || url.host == "0.0.0.0";

    std::string host = url.host.empty() ? "0.0.0.0" : url.host;
    char port[16];
    std::snprintf(port, sizeof(port), "%d", url.port);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    if (int r = getaddrinfo(host.c_str(), port, &hints, &res); r != 0) {
      err = std::string("srt: getaddrinfo: ") + gai_strerror(r);
      return false;
    }

    bool ok = false;
    bool cancelled = false;
    if (listener) {
      if (srt_bind(sock_, res->ai_addr, res->ai_addrlen) != SRT_ERROR &&
          srt_listen(sock_, 1) != SRT_ERROR) {
        SRTSOCKET client = SRT_INVALID_SOCK;
        if (cancel_) {
          // Poll a non-blocking accept so a stop request can interrupt the
          // wait for a peer instead of deadlocking teardown.
          int no = 0;
          srt_setsockopt(sock_, 0, SRTO_RCVSYN, &no, sizeof(no));
          while (!cancel_->load()) {
            client = srt_accept(sock_, nullptr, nullptr);
            if (client != SRT_INVALID_SOCK)
              break;
            int sys = 0;
            if (srt_getlasterror(&sys) != SRT_EASYNCRCV)
              break; // a real error, not "no pending connection yet"
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
          }
          if (client == SRT_INVALID_SOCK && cancel_->load())
            cancelled = true;
        } else {
          client = srt_accept(sock_, nullptr, nullptr);
        }
        if (client != SRT_INVALID_SOCK) {
          srt_close(sock_);
          sock_ = client;
          // The accepted socket inherits the listener's options; restore
          // blocking I/O so send()/recv() behave normally.
          int yes = 1;
          srt_setsockopt(sock_, 0, SRTO_RCVSYN, &yes, sizeof(yes));
          srt_setsockopt(sock_, 0, SRTO_SNDSYN, &yes, sizeof(yes));
          ok = true;
        }
      }
    } else {
      ok = srt_connect(sock_, res->ai_addr, res->ai_addrlen) != SRT_ERROR;
    }
    freeaddrinfo(res);
    if (!ok) {
      err = cancelled ? std::string() : std::string("srt: ") +
                                            srt_getlasterror_str();
      return false;
    }
    return true;
  }

  void set_cancel(std::atomic<bool> *cancel) override { cancel_ = cancel; }

  bool send(const uint8_t *data, size_t size) override {
    if (sock_ == SRT_INVALID_SOCK)
      return false;
    return srt_sendmsg2(sock_, reinterpret_cast<const char *>(data),
                        static_cast<int>(size), nullptr) != SRT_ERROR;
  }

  long recv(uint8_t *data, size_t cap) override {
    if (sock_ == SRT_INVALID_SOCK)
      return -1;
    int n = srt_recvmsg2(sock_, reinterpret_cast<char *>(data),
                         static_cast<int>(cap), nullptr);
    if (n == SRT_ERROR) {
      // A closed connection reports SRT_ECONNLOST; treat it as orderly EOF.
      int sys = 0;
      if (srt_getlasterror(&sys) == SRT_ECONNLOST)
        return 0;
      return -1;
    }
    return n;
  }

  void close() override {
    if (sock_ != SRT_INVALID_SOCK) {
      srt_close(sock_);
      sock_ = SRT_INVALID_SOCK;
    }
  }

  ~SrtTransport() override { close(); }

private:
  SRTSOCKET sock_ = SRT_INVALID_SOCK;
  std::atomic<bool> *cancel_ = nullptr;
};
#endif // FRAMETAP_HAVE_SRT

} // namespace

std::unique_ptr<Transport> make_udp_transport() {
  return std::make_unique<UdpTransport>();
}

std::unique_ptr<Transport> make_srt_transport() {
#ifdef FRAMETAP_HAVE_SRT
  return std::make_unique<SrtTransport>();
#else
  return nullptr;
#endif
}

} // namespace frametap::enc
