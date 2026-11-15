// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/nex.hpp"

#include "xanatoo/https.hpp"
#include "xanatoo/transfer_meter.hpp"

#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>

#if defined(XANATOO_NEX) && XANATOO_NEX
#include <unistd.h>
#endif

namespace xanatoo {

bool is_nex_url(std::string_view url) {
  if (url.size() < 6) return false;
  auto lower = [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  };
  return lower(url[0]) == 'n' && lower(url[1]) == 'e' && lower(url[2]) == 'x' &&
         url[3] == ':' && url[4] == '/' && url[5] == '/';
}

std::optional<NexUrl> parse_nex_url(std::string_view url) {
  if (!is_nex_url(url)) return std::nullopt;
  std::string_view rest = url.substr(6);
  size_t hash = rest.find('#');
  if (hash != std::string_view::npos) rest = rest.substr(0, hash);

  size_t slash = rest.find('/');
  std::string_view authority =
      (slash == std::string_view::npos) ? rest : rest.substr(0, slash);
  std::string_view path =
      (slash == std::string_view::npos) ? std::string_view{} : rest.substr(slash);

  if (authority.empty()) return std::nullopt;

  NexUrl out;
  out.port = 1900;
  if (!authority.empty() && authority.front() == '[') {
    size_t br = authority.find(']');
    if (br == std::string_view::npos) return std::nullopt;
    out.host = std::string(authority.substr(1, br - 1));
    if (br + 1 < authority.size() && authority[br + 1] == ':') {
      out.port = std::atoi(std::string(authority.substr(br + 2)).c_str());
      if (out.port <= 0) out.port = 1900;
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
        if (out.port <= 0) out.port = 1900;
      } else {
        out.host = std::string(authority);
      }
    } else {
      out.host = std::string(authority);
    }
  }
  if (out.host.empty()) return std::nullopt;

  out.path = std::string(path);  // may be empty

  std::ostringstream o;
  o << "nex://" << out.host;
  if (out.port != 1900) o << ':' << out.port;
  o << out.path;
  out.canonical = o.str();
  return out;
}

std::string resolve_nex_url(std::string_view base, std::string_view ref) {
  if (ref.empty()) return std::string(base);
  if (is_nex_url(ref)) return std::string(ref);
  if (ref.find("://") != std::string_view::npos) return std::string(ref);

  auto b = parse_nex_url(base);
  if (!b) return {};

  std::string path;
  if (!ref.empty() && ref.front() == '/') {
    path = std::string(ref);
  } else {
    std::string dir = b->path;
    if (!dir.empty() && dir.back() != '/') {
      size_t slash = dir.rfind('/');
      if (slash != std::string::npos)
        dir.resize(slash + 1);
      else
        dir = "/";
    }
    if (dir.empty()) dir = "/";
    path = dir + std::string(ref);
  }

  std::ostringstream o;
  o << "nex://" << b->host;
  if (b->port != 1900) o << ':' << b->port;
  o << path;
  return o.str();
}

std::string nex_directory_to_markdown(std::string_view data,
                                      std::string_view base_url) {
  std::ostringstream o;
  o << "# Nex directory\n\n";
  size_t pos = 0;
  while (pos < data.size()) {
    size_t eol = data.find('\n', pos);
    std::string_view line =
        (eol == std::string_view::npos) ? data.substr(pos)
                                        : data.substr(pos, eol - pos);
    pos = (eol == std::string_view::npos) ? data.size() : eol + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.size() >= 3 && line.substr(0, 3) == "=> ") {
      std::string_view rest = line.substr(3);
      // skip leading spaces
      while (!rest.empty() && rest.front() == ' ') rest.remove_prefix(1);
      size_t sp = rest.find(' ');
      std::string_view href_sv =
          (sp == std::string_view::npos) ? rest : rest.substr(0, sp);
      std::string_view label =
          (sp == std::string_view::npos) ? std::string_view{}
                                         : rest.substr(sp + 1);
      while (!label.empty() && label.front() == ' ') label.remove_prefix(1);
      std::string href = resolve_nex_url(base_url, href_sv);
      if (label.empty()) label = href_sv;
      o << "- [" << label << "](" << href << ")\n";
    } else if (!line.empty()) {
      o << line << "\n\n";
    }
  }
  return o.str();
}

#if defined(XANATOO_NEX) && XANATOO_NEX

std::optional<NexResponse> nex_fetch(std::string_view url, std::string* err) {
  auto u = parse_nex_url(url);
  if (!u) {
    if (err) *err = "not a nex:// URL";
    return std::nullopt;
  }

  int fd = tcp_connect_host(u->host, u->port, err);
  if (fd < 0) return std::nullopt;

  // Spec: send the path (may be empty). CRLF is accepted by most servers.
  std::string req = u->path + "\r\n";
  ssize_t w = ::write(fd, req.data(), req.size());
  if (w < 0 || static_cast<size_t>(w) != req.size()) {
    ::close(fd);
    if (err) *err = "nex write failed";
    return std::nullopt;
  }

  std::string data;
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    data.append(buf, static_cast<size_t>(n));
    meter_bytes(static_cast<size_t>(n));
    if (data.size() > 8 * 1024 * 1024) break;
  }
  ::close(fd);

  NexResponse resp;
  resp.final_url = u->canonical;
  // Empty path or trailing / → directory
  resp.is_directory =
      u->path.empty() || (!u->path.empty() && u->path.back() == '/');
  if (resp.is_directory) {
    resp.body = nex_directory_to_markdown(data, u->canonical);
  } else {
    resp.body = std::move(data);
  }
  return resp;
}

#endif  // XANATOO_NEX

}  // namespace xanatoo
