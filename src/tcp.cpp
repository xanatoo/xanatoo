// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/tcp.hpp"
#include "xanatoo/net_address.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <string>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace xanatoo {

int tcp_connect(const std::string& host, int port, std::string* err,
                bool public_only) {
  addrinfo hints {};
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_family = AF_UNSPEC;
  addrinfo* res = nullptr;
  std::string port_s = std::to_string(port);
  if (const int gai = getaddrinfo(host.c_str(), port_s.c_str(), &hints, &res)) {
    if (err) *err = "DNS lookup failed for " + host + " (" + gai_strerror(gai) + ")";
    return -1;
  }
  int fd = -1;
  int last_errno = 0;
  bool attempted = false;
  for (addrinfo* p = res; p; p = p->ai_next) {
    if (public_only && !public_ip_address(p->ai_addr)) continue;
    attempted = true;
    fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0) continue;
    timeval tv;
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (public_only) {
      // Untrusted auto-discovery must not block on an unresponsive host for
      // the default OS TCP connect timeout (sometimes many minutes).
      const int flags = fcntl(fd, F_GETFL, 0);
      if (flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0) {
        int result = ::connect(fd, p->ai_addr, p->ai_addrlen);
        if (result < 0 && errno == EINPROGRESS) {
          pollfd wait{fd, POLLOUT, 0};
          if (::poll(&wait, 1, 4000) > 0) {
            socklen_t len = sizeof(result);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &result, &len) == 0 &&
                result == 0) result = 0;
            else result = -1;
          } else result = -1;
        }
        fcntl(fd, F_SETFL, flags);
        if (result == 0) break;
      }
    } else if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
      break;
    }
    last_errno = errno;
    ::close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0 && err) *err = attempted ? "connect failed to " + host + ":" +
                                            port_s + " (" +
                                            std::strerror(last_errno) + ")" :
                          "private/local address excluded for " + host;
  return fd;
}

}  // namespace xanatoo
