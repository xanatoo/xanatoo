// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/finger.hpp"
#include "xanatoo/text_escape.hpp"

#include "xanatoo/https.hpp"
#include "xanatoo/transfer_meter.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>

#if defined(XANATOO_FINGER) && XANATOO_FINGER
#include <unistd.h>
#endif

namespace xanatoo {

bool is_finger_url(std::string_view url) {
  if (url.size() < 9) return false;
  auto lower = [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  };
  return lower(url[0]) == 'f' && lower(url[1]) == 'i' && lower(url[2]) == 'n' &&
         lower(url[3]) == 'g' && lower(url[4]) == 'e' && lower(url[5]) == 'r' &&
         url[6] == ':' && url[7] == '/' && url[8] == '/';
}

std::optional<FingerUrl> parse_finger_url(std::string_view url) {
  if (!is_finger_url(url)) return std::nullopt;
  std::string_view rest = url.substr(9);
  size_t hash = rest.find('#');
  if (hash != std::string_view::npos) rest = rest.substr(0, hash);

  // Forms: host, host/user, user@host, user@host:port
  FingerUrl out;
  out.port = 79;

  size_t at = rest.find('@');
  if (at != std::string_view::npos) {
    out.user = std::string(rest.substr(0, at));
    rest = rest.substr(at + 1);
  }

  size_t slash = rest.find('/');
  std::string_view authority =
      (slash == std::string_view::npos) ? rest : rest.substr(0, slash);
  if (slash != std::string_view::npos && out.user.empty()) {
    std::string_view path = rest.substr(slash + 1);
    // take first path segment as user
    size_t next = path.find('/');
    out.user = std::string(
        (next == std::string_view::npos) ? path : path.substr(0, next));
  }

  if (authority.empty()) return std::nullopt;

  size_t colon = authority.rfind(':');
  if (colon != std::string_view::npos && colon + 1 < authority.size()) {
    bool digits = true;
    for (size_t i = colon + 1; i < authority.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(authority[i]))) {
        digits = false;
        break;
      }
    }
    if (digits) {
      out.host = std::string(authority.substr(0, colon));
      out.port = std::atoi(std::string(authority.substr(colon + 1)).c_str());
      if (out.port <= 0) out.port = 79;
    } else {
      out.host = std::string(authority);
    }
  } else {
    out.host = std::string(authority);
  }
  if (out.host.empty()) return std::nullopt;

  std::ostringstream o;
  o << "finger://";
  if (!out.user.empty()) o << out.user << '@';
  o << out.host;
  if (out.port != 79) o << ':' << out.port;
  out.canonical = o.str();
  return out;
}

#if defined(XANATOO_FINGER) && XANATOO_FINGER

std::optional<FingerResponse> finger_fetch(std::string_view url,
                                           std::string* err) {
  auto u = parse_finger_url(url);
  if (!u) {
    if (err) *err = "not a finger:// URL";
    return std::nullopt;
  }

  int fd = tcp_connect_host(u->host, u->port, err);
  if (fd < 0) return std::nullopt;

  // RFC 1288: send query + CRLF. Empty query is valid.
  std::string req = u->user + "\r\n";
  ssize_t w = ::write(fd, req.data(), req.size());
  if (w < 0 || static_cast<size_t>(w) != req.size()) {
    ::close(fd);
    if (err) *err = "finger write failed";
    return std::nullopt;
  }

  std::string data;
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    data.append(buf, static_cast<size_t>(n));
    meter_bytes(static_cast<size_t>(n));
    if (data.size() > 1 * 1024 * 1024) break;  // 1 MiB cap for finger
  }
  ::close(fd);

  FingerResponse resp;
  resp.final_url = u->canonical;
  // Present as a simple document
  // A fence longer than any backtick run in the reply, so it stays verbatim.
  const std::string fence = markdown_code_fence(data);
  resp.body = "# Finger: " + markdown_escape_inline(u->canonical) + "\n\n" +
              fence + "\n" + data + "\n" + fence + "\n";
  return resp;
}

#endif  // XANATOO_FINGER

}  // namespace xanatoo
