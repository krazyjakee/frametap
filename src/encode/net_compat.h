#pragma once

// Cross-platform socket primitives for the streaming transports (POSIX +
// Winsock2). Keeps the #ifdef noise out of net_transport.cpp / rtmp_sink.cpp.

#include <cstddef>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

namespace frametap::enc {
using socket_t = SOCKET;
inline constexpr socket_t kInvalidSocket = INVALID_SOCKET;
inline int close_socket(socket_t s) { return ::closesocket(s); }
// Winsock needs one-time process init before any socket call.
inline void net_global_init() {
  static bool done = false;
  if (!done) {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    done = true;
  }
}
} // namespace frametap::enc

#else
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

namespace frametap::enc {
using socket_t = int;
inline constexpr socket_t kInvalidSocket = -1;
inline int close_socket(socket_t s) { return ::close(s); }
inline void net_global_init() {}
} // namespace frametap::enc
#endif

namespace frametap::enc {
// Thin wrappers so call sites don't need the char*/int casts Winsock wants.
inline long sock_send(socket_t s, const void *buf, size_t len) {
  return static_cast<long>(
      ::send(s, static_cast<const char *>(buf), static_cast<int>(len), 0));
}
inline long sock_recv(socket_t s, void *buf, size_t len) {
  return static_cast<long>(
      ::recv(s, static_cast<char *>(buf), static_cast<int>(len), 0));
}
} // namespace frametap::enc
