// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/gopher.hpp"

#include "xanatoo/https.hpp"
#include "xanatoo/text_escape.hpp"
#include "xanatoo/transfer_meter.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>
#include <string>

#if defined(XANATOO_GOPHER) && XANATOO_GOPHER
#include <unistd.h>
#endif

namespace xanatoo {

bool is_gopher_url(std::string_view url) {
  if (url.size() < 9) return false;
  auto lower = [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  };
  return lower(url[0]) == 'g' && lower(url[1]) == 'o' && lower(url[2]) == 'p' &&
         lower(url[3]) == 'h' && lower(url[4]) == 'e' && lower(url[5]) == 'r' &&
         url[6] == ':' && url[7] == '/' && url[8] == '/';
}

bool gopher_type_is_binary(char type) {
  switch (type) {
    case '4':  // BinHex
    case '5':  // DOS binary
    case '6':  // uuencoded
    case '9':  // binary
    case 'g':  // GIF
    case 'I':  // image
    case 's':  // sound
    case 'd':  // document (often PDF)
    case ';':  // video (some servers)
      return true;
    default:
      return false;
  }
}

namespace {

// gopher://host[:port]/<type><selector>[?query], selector percent-encoded.
std::string gopher_url_string(std::string_view host, int port, char type,
                              std::string_view selector,
                              std::string_view query) {
  std::string o = "gopher://";
  if (host.find(':') != std::string_view::npos) {
    o += '[';
    o += host;
    o += ']';
  } else {
    o += host;
  }
  if (port != 70) o += ':' + std::to_string(port);
  o += '/';
  o += type;
  o += percent_encode_path(selector);
  if (!query.empty()) o += '?' + percent_encode_path(query);
  return o;
}

}  // namespace

std::optional<GopherUrl> parse_gopher_url(std::string_view url) {
  if (!is_gopher_url(url)) return std::nullopt;
  std::string_view rest = url.substr(9);  // after gopher://
  // strip fragment
  size_t hash = rest.find('#');
  if (hash != std::string_view::npos) rest = rest.substr(0, hash);

  const size_t auth_end = rest.find_first_of("/?");
  std::string_view authority = rest.substr(0, auth_end);
  std::string_view path =
      auth_end == std::string_view::npos ? std::string_view{}
                                         : rest.substr(auth_end);

  if (authority.empty()) return std::nullopt;

  GopherUrl out;
  out.port = 70;
  if (!authority.empty() && authority.front() == '[') {
    size_t br = authority.find(']');
    if (br == std::string_view::npos) return std::nullopt;
    out.host = std::string(authority.substr(1, br - 1));
    if (br + 1 < authority.size() && authority[br + 1] == ':') {
      out.port = std::atoi(std::string(authority.substr(br + 2)).c_str());
      if (out.port <= 0) out.port = 70;
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
        if (out.port <= 0) out.port = 70;
      } else {
        out.host = std::string(authority);
      }
    } else {
      out.host = std::string(authority);
    }
  }
  if (out.host.empty()) return std::nullopt;

  // Path: /<type><selector>, empty → type '1', selector "". A '?' only
  // starts a query for type 7 (search); elsewhere it belongs to the
  // selector. RFC 4266 also allows the search after an encoded TAB (%09).
  out.type = '1';
  std::string_view query;
  if (!path.empty() && path.front() == '/') {
    path.remove_prefix(1);
    if (!path.empty()) {
      out.type = path.front();
      path.remove_prefix(1);
    }
  } else if (!path.empty() && path.front() == '?') {
    path = {};  // "gopher://host?x": nothing to search on the root menu
  }
  if (out.type == '7') {
    if (size_t q = path.find('?'); q != std::string_view::npos) {
      query = path.substr(q + 1);
      path = path.substr(0, q);
    }
  }
  out.selector = percent_decode(path);
  out.query = percent_decode(query);
  if (size_t tab = out.selector.find('\t'); tab != std::string::npos) {
    if (out.type == '7' && out.query.empty()) {
      out.query = out.selector.substr(tab + 1);
      if (size_t t2 = out.query.find('\t'); t2 != std::string::npos)
        out.query.resize(t2);  // a third field is Gopher+; ignored
    }
    out.selector.resize(tab);
  }

  out.canonical = gopher_url_string(out.host, out.port, out.type,
                                    out.selector, out.query);
  return out;
}

std::string gopher_item_url(const GopherMenuItem& it) {
  // "URL:" selectors on type h items point to another protocol (common
  // convention: gopher://host/hURL:https://…).
  if (it.type == 'h' && it.selector.rfind("URL:", 0) == 0)
    return it.selector.substr(4);
  return gopher_url_string(it.host, it.port, it.type, it.selector, {});
}

std::string resolve_gopher_url(std::string_view base, std::string_view ref) {
  if (ref.empty()) return std::string(base);
  if (is_gopher_url(ref)) return std::string(ref);
  if (ref.find("://") != std::string_view::npos) return std::string(ref);
  if (ref.rfind("mailto:", 0) == 0) return std::string(ref);

  auto b = parse_gopher_url(base);
  if (!b) return {};
  const std::string authority =
      gopher_url_string(b->host, b->port, '1', {}, {});  // ends in "/1"
  const std::string root = authority.substr(0, authority.size() - 1);

  // "/<type><selector>": a gopher path on the same server.
  if (ref.front() == '/') {
    auto u = parse_gopher_url(root + std::string(ref.substr(1)));
    return u ? u->canonical : std::string{};
  }
  // "?query": search again on the same item.
  if (ref.front() == '?') {
    auto u = parse_gopher_url(gopher_url_string(b->host, b->port, b->type,
                                                b->selector, {}) +
                              std::string(ref));
    return u ? u->canonical : std::string{};
  }
  // Anything else is relative to the selector's directory with the same
  // item type (e.g. links inside an HTML item served over Gopher).
  std::string sel = b->selector;
  size_t slash = sel.rfind('/');
  sel.resize(slash == std::string::npos ? 0 : slash + 1);
  std::string_view r = ref;
  if (size_t h = r.find('#'); h != std::string_view::npos) r = r.substr(0, h);
  sel += percent_decode(r);
  return gopher_url_string(b->host, b->port, b->type, sel, {});
}

std::vector<GopherMenuItem> parse_gopher_menu(std::string_view data) {
  std::vector<GopherMenuItem> items;
  size_t pos = 0;
  while (pos < data.size()) {
    size_t eol = data.find('\n', pos);
    std::string_view line =
        (eol == std::string_view::npos) ? data.substr(pos)
                                        : data.substr(pos, eol - pos);
    pos = (eol == std::string_view::npos) ? data.size() : eol + 1;
    if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
    if (line.empty() || line == ".") continue;
    if (line.size() < 1) continue;

    GopherMenuItem it;
    it.type = line[0];
    std::string_view rest = line.substr(1);
    auto next_tab = [&](std::string_view& s) -> std::string_view {
      size_t t = s.find('\t');
      if (t == std::string_view::npos) {
        std::string_view r = s;
        s = {};
        return r;
      }
      std::string_view r = s.substr(0, t);
      s.remove_prefix(t + 1);
      return r;
    };
    it.label = std::string(next_tab(rest));
    it.selector = std::string(next_tab(rest));
    it.host = std::string(next_tab(rest));
    std::string_view port_sv = next_tab(rest);
    if (!port_sv.empty()) {
      it.port = std::atoi(std::string(port_sv).c_str());
      if (it.port <= 0) it.port = 70;
    }
    items.push_back(std::move(it));
  }
  return items;
}

std::string gopher_menu_to_markdown(const std::vector<GopherMenuItem>& items,
                                    std::string_view base_url) {
  auto base = parse_gopher_url(base_url);
  std::string o = "# Gopher menu\n\n";
  // Info lines ('i', and errors '3') keep their spacing: a run of them is
  // one preformatted block (menus are often laid out or drawn in ASCII).
  std::string info;
  auto flush_info = [&] {
    while (!info.empty() && info.back() == '\n') info.pop_back();
    size_t lead = 0;
    while (lead < info.size() && info[lead] == '\n') ++lead;
    if (lead < info.size()) {
      const std::string body = info.substr(lead);
      const std::string fence = markdown_code_fence(body);
      o += fence + '\n' + body + '\n' + fence + "\n\n";
    }
    info.clear();
  };
  bool in_list = false;
  for (const auto& it : items) {
    if (it.type == 'i' || it.type == '3') {
      if (in_list) {
        o += '\n';
        in_list = false;
      }
      info += it.label;
      info += '\n';
      continue;
    }
    flush_info();
    GopherMenuItem target = it;
    if (target.host.empty() && base) {
      target.host = base->host;
      target.port = base->port;
    }
    std::string href = gopher_item_url(target);
    if (href.find_first_of(" ()<>") != std::string::npos)
      href = percent_encode_path(percent_decode(href));
    const char* kind = "";
    switch (it.type) {
      case '0': kind = " [text]"; break;
      case '1': kind = " [menu]"; break;
      case '7': kind = " [search]"; break;
      case '9':
      case 'g':
      case 'I':
      case 's':
      case 'd':
      case '4':
      case '5': kind = " [binary]"; break;
      case 'h': kind = " [html]"; break;
      default: break;
    }
    // Link labels are taken verbatim up to the first "](" (parse.cpp), so
    // that sequence is the only one a label must not contain.
    std::string label = it.label;
    for (size_t k = label.find("]("); k != std::string::npos;
         k = label.find("](", k + 2))
      label.insert(k + 1, " ");
    o += "- [" + label + kind + "](" + href + ")\n";
    in_list = true;
  }
  flush_info();
  return o;
}

#if defined(XANATOO_GOPHER) && XANATOO_GOPHER

std::optional<GopherResponse> gopher_fetch(std::string_view url,
                                           std::string* err) {
  auto u = parse_gopher_url(url);
  if (!u) {
    if (err) *err = "not a gopher:// URL";
    return std::nullopt;
  }

  // Type 7 search with no query yet: ask the user before contacting the server.
  if (u->type == '7' && u->query.empty()) {
    GopherResponse resp;
    resp.needs_input = true;
    resp.final_url = u->canonical;
    resp.input_prompt = "Gopher search";
    resp.mime_hint = "text/plain";
    return resp;
  }

  int fd = tcp_connect_host(u->host, u->port, err);
  if (fd < 0) return std::nullopt;

  // Request: selector [TAB query] CRLF
  std::string req = u->selector;
  if (u->type == '7' && !u->query.empty()) {
    req.push_back('\t');
    req += u->query;
  }
  req += "\r\n";
  ssize_t w = ::write(fd, req.data(), req.size());
  if (w < 0 || static_cast<size_t>(w) != req.size()) {
    ::close(fd);
    if (err) *err = "gopher write failed";
    return std::nullopt;
  }

  std::string data;
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n <= 0) break;
    data.append(buf, static_cast<size_t>(n));
    meter_bytes(static_cast<size_t>(n));
    if (data.size() > 32 * 1024 * 1024) break;  // 32 MiB for binary items
  }
  ::close(fd);

  GopherResponse resp;
  resp.final_url = u->canonical;

  if (u->type == '1' || (u->type == '7' && !u->query.empty())) {
    // Search results and directories are both menus.
    resp.is_menu = true;
    resp.menu = parse_gopher_menu(data);
    resp.body = gopher_menu_to_markdown(resp.menu, u->canonical);
    resp.mime_hint = "text/gopher-menu";
  } else if (u->type == '0' || u->type == 'h' || u->type == 'i') {
    resp.is_menu = false;
    resp.body = std::move(data);
    resp.mime_hint = (u->type == 'h') ? "text/html" : "text/plain";
  } else if (gopher_type_is_binary(u->type)) {
    resp.is_binary = true;
    resp.body = std::move(data);
    switch (u->type) {
      case 'g': resp.mime_hint = "image/gif"; break;
      case 'I': resp.mime_hint = "image/*"; break;
      case 's': resp.mime_hint = "audio/*"; break;
      case 'd': resp.mime_hint = "application/pdf"; break;
      default: resp.mime_hint = "application/octet-stream"; break;
    }
  } else {
    resp.is_menu = false;
    resp.body = "# Unsupported Gopher item type\n\n"
                "Type `" +
                std::string(1, u->type) +
                "` is not rendered inline (" +
                std::to_string(data.size()) +
                " bytes received).\n";
    resp.mime_hint = "text/plain";
  }
  return resp;
}

#endif  // XANATOO_GOPHER

}  // namespace xanatoo
