// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/spartan.hpp"

#include "xanatoo/https.hpp"
#include "xanatoo/transfer_meter.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>

#if defined(XANATOO_SPARTAN) && XANATOO_SPARTAN
#include <unistd.h>
#endif

namespace xanatoo {

bool is_spartan_url(std::string_view url) {
  if (url.size() < 10) return false;
  auto lower = [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  };
  return lower(url[0]) == 's' && lower(url[1]) == 'p' && lower(url[2]) == 'a' &&
         lower(url[3]) == 'r' && lower(url[4]) == 't' && lower(url[5]) == 'a' &&
         lower(url[6]) == 'n' && url[7] == ':' && url[8] == '/' && url[9] == '/';
}

std::optional<SpartanUrl> parse_spartan_url(std::string_view url) {
  if (!is_spartan_url(url)) return std::nullopt;
  std::string_view rest = url.substr(10);
  size_t hash = rest.find('#');
  if (hash != std::string_view::npos) rest = rest.substr(0, hash);

  size_t slash = rest.find('/');
  std::string_view authority =
      (slash == std::string_view::npos) ? rest : rest.substr(0, slash);
  std::string_view path_query =
      (slash == std::string_view::npos) ? std::string_view("/") : rest.substr(slash);

  if (authority.empty()) return std::nullopt;

  SpartanUrl out;
  out.port = 300;
  if (!authority.empty() && authority.front() == '[') {
    size_t br = authority.find(']');
    if (br == std::string_view::npos) return std::nullopt;
    out.host = std::string(authority.substr(1, br - 1));
    if (br + 1 < authority.size() && authority[br + 1] == ':') {
      out.port = std::atoi(std::string(authority.substr(br + 2)).c_str());
      if (out.port <= 0) out.port = 300;
    }
  } else {
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
        if (out.port <= 0) out.port = 300;
      } else {
        out.host = std::string(authority);
      }
    } else {
      out.host = std::string(authority);
    }
  }
  if (out.host.empty()) return std::nullopt;

  size_t q = path_query.find('?');
  if (q != std::string_view::npos) {
    out.path = std::string(path_query.substr(0, q));
    out.query = std::string(path_query.substr(q + 1));
  } else {
    out.path = std::string(path_query);
  }
  if (out.path.empty()) out.path = "/";

  std::ostringstream o;
  o << "spartan://" << out.host;
  if (out.port != 300) o << ':' << out.port;
  o << out.path;
  if (!out.query.empty()) o << '?' << out.query;
  out.canonical = o.str();
  return out;
}

std::string resolve_spartan_url(std::string_view base, std::string_view ref) {
  if (ref.empty()) return std::string(base);
  if (is_spartan_url(ref)) return std::string(ref);
  if (ref.find("://") != std::string_view::npos) return std::string(ref);

  auto b = parse_spartan_url(base);
  if (!b) return {};

  std::string path;
  if (!ref.empty() && ref.front() == '/') {
    path = std::string(ref);
  } else {
    // relative join (simple)
    std::string dir = b->path;
    size_t slash = dir.rfind('/');
    if (slash != std::string::npos)
      dir.resize(slash + 1);
    else
      dir = "/";
    path = dir + std::string(ref);
  }

  std::ostringstream o;
  o << "spartan://" << b->host;
  if (b->port != 300) o << ':' << b->port;
  o << path;
  return o.str();
}

#if defined(XANATOO_SPARTAN) && XANATOO_SPARTAN

static std::optional<SpartanResponse> spartan_fetch_once(const SpartanUrl& u,
                                                         std::string* err) {
  int fd = tcp_connect_host(u.host, u.port, err);
  if (fd < 0) return std::nullopt;

  // request-line: host SP path SP content-length CRLF [data]
  // Query becomes the data block (common client convention).
  const std::string& data = u.query;
  std::ostringstream req;
  req << u.host << ' ' << u.path << ' ' << data.size() << "\r\n";
  if (!data.empty()) req << data;
  std::string reqs = req.str();
  ssize_t w = ::write(fd, reqs.data(), reqs.size());
  if (w < 0 || static_cast<size_t>(w) != reqs.size()) {
    ::close(fd);
    if (err) *err = "spartan write failed";
    return std::nullopt;
  }

  std::string raw;
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    raw.append(buf, static_cast<size_t>(n));
    meter_bytes(static_cast<size_t>(n));
    if (raw.size() > 8 * 1024 * 1024) break;
  }
  ::close(fd);

  // Response: STATUS SP META\r\n BODY
  size_t line_end = raw.find("\r\n");
  if (line_end == std::string::npos) {
    if (err) *err = "malformed Spartan response (no header)";
    return std::nullopt;
  }
  std::string header = raw.substr(0, line_end);
  SpartanResponse resp;
  resp.final_url = u.canonical;
  if (header.empty() ||
      !std::isdigit(static_cast<unsigned char>(header[0]))) {
    if (err) *err = "malformed Spartan status";
    return std::nullopt;
  }
  resp.status = header[0] - '0';
  size_t meta_off = 1;
  if (meta_off < header.size() && header[meta_off] == ' ') ++meta_off;
  resp.meta = header.substr(meta_off);
  resp.body = raw.substr(line_end + 2);
  return resp;
}

std::optional<SpartanResponse> spartan_fetch(std::string_view url,
                                             std::string* err) {
  auto u = parse_spartan_url(url);
  if (!u) {
    if (err) *err = "not a spartan:// URL";
    return std::nullopt;
  }

  for (int hop = 0; hop < 5; ++hop) {
    auto resp = spartan_fetch_once(*u, err);
    if (!resp) return std::nullopt;
    if (resp->status == 3) {
      std::string next = resolve_spartan_url(u->canonical, resp->meta);
      if (next.empty() || !is_spartan_url(next)) {
        if (err) *err = "bad Spartan redirect target";
        return std::nullopt;
      }
      u = parse_spartan_url(next);
      if (!u) return std::nullopt;
      continue;
    }
    return resp;
  }
  if (err) *err = "too many Spartan redirects";
  return std::nullopt;
}

#endif  // XANATOO_SPARTAN

}  // namespace xanatoo
