// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// NNTP network side (RFC 3977): one connection per page, plain TCP on 119
// or TLS on 563. Reader commands only — MODE READER, LIST ACTIVE /
// NEWSGROUPS, GROUP, OVER (XOVER), ARTICLE, QUIT. No AUTHINFO, no POST.

#include "xanatoo/nntp.hpp"

#if defined(XANATOO_NNTP) && XANATOO_NNTP

#include "xanatoo/tcp.hpp"
#include "xanatoo/transfer_meter.hpp"
#if defined(XANATOO_HTTPS) && XANATOO_HTTPS && !defined(__EMSCRIPTEN__)
#define XANATOO_NNTP_TLS 1
#include "xanatoo/tls.hpp"
#endif

#include <algorithm>
#include <cstdlib>
#include <memory>

#include <unistd.h>

namespace xanatoo {

namespace {

constexpr size_t kMaxReply = 32u * 1024 * 1024;  // one multi-line reply
constexpr int64_t kGroupWindow = 150;            // overview entries per page
constexpr int64_t kThreadWindow = 400;           // either side of an article
constexpr size_t kMaxThreadArticles = 150;

struct Conn {
  int fd = -1;
#ifdef XANATOO_NNTP_TLS
  std::unique_ptr<TlsStream> tls;
#endif
  std::string buf;   // received, not yet consumed
  std::string error;

  ~Conn() {
#ifdef XANATOO_NNTP_TLS
    if (tls) {
      tls->close();
      return;
    }
#endif
    if (fd >= 0) ::close(fd);
  }
};

bool conn_write(Conn& c, const std::string& s) {
#ifdef XANATOO_NNTP_TLS
  if (c.tls) return c.tls->write_all(s.data(), s.size());
#endif
  size_t off = 0;
  while (off < s.size()) {
    const ssize_t w = ::write(c.fd, s.data() + off, s.size() - off);
    if (w <= 0) return false;
    off += static_cast<size_t>(w);
  }
  return true;
}

// More bytes into c.buf; false on close / error / cancel.
bool conn_fill(Conn& c) {
  char tmp[16384];
  long n;
#ifdef XANATOO_NNTP_TLS
  if (c.tls) n = c.tls->read(tmp, sizeof(tmp));
  else
#endif
    n = ::read(c.fd, tmp, sizeof(tmp));
  if (transfer_cancelled()) {
    c.error = transfer_cancelled_error();
    return false;
  }
  if (n <= 0) {
    c.error = n == 0 ? "connection closed by the server" : "read failed or timed out";
    return false;
  }
  meter_bytes(static_cast<size_t>(n));
  c.buf.append(tmp, static_cast<size_t>(n));
  return true;
}

// One line without CRLF.
bool read_line(Conn& c, std::string& line, size_t& pos) {
  for (;;) {
    const size_t nl = c.buf.find('\n', pos);
    if (nl != std::string::npos) {
      line.assign(c.buf, pos, nl - pos);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      pos = nl + 1;
      return true;
    }
    if (c.buf.size() - pos > 1024 * 1024) {
      c.error = "line too long";
      return false;
    }
    if (!conn_fill(c)) return false;
  }
}

int status_of(const std::string& line) {
  if (line.size() < 3) return 0;
  return std::atoi(line.substr(0, 3).c_str());
}

std::string reply_error(const std::string& cmd, const std::string& line) {
  const int code = status_of(line);
  if (code == 480 || code == 481 || code == 483)
    return "the server needs a login to read (" + line +
           "); xanatoo reads anonymously only";
  if (code == 502) return "the server does not allow reading: " + line;
  return cmd + ": " + (line.empty() ? std::string("no reply") : line);
}

// Status line of a command; empty on a connection error.
std::string command(Conn& c, const std::string& cmd) {
  if (!conn_write(c, cmd + "\r\n")) {
    c.error = "write failed";
    return {};
  }
  std::string line;
  size_t pos = 0;
  if (!read_line(c, line, pos)) return {};
  c.buf.erase(0, pos);
  return line;
}

// The dot-terminated data block after a successful status line.
bool read_block(Conn& c, std::string& out) {
  size_t pos = 0;
  std::string line;
  for (;;) {
    if (!read_line(c, line, pos)) return false;
    if (line == ".") break;
    if (!line.empty() && line[0] == '.') line.erase(0, 1);  // dot-stuffing
    out += line;
    out += '\n';
    if (out.size() > kMaxReply) {
      c.error = "reply too large";
      return false;
    }
    if (pos > 65536) {
      c.buf.erase(0, pos);
      pos = 0;
    }
  }
  c.buf.erase(0, pos);
  return true;
}

// Command with a multi-line answer: data, or nullopt with `err` set.
std::optional<std::string> multi(Conn& c, const std::string& cmd, int ok,
                                 std::string* err, std::string* status = nullptr) {
  const std::string line = command(c, cmd);
  if (status) *status = line;
  if (line.empty()) {
    if (err) *err = c.error;
    return std::nullopt;
  }
  if (status_of(line) != ok) {
    if (err) *err = reply_error(cmd, line);
    return std::nullopt;
  }
  std::string data;
  if (!read_block(c, data)) {
    if (err) *err = c.error;
    return std::nullopt;
  }
  return data;
}

std::unique_ptr<Conn> open_conn(const NntpRef& r, std::string* err) {
  auto c = std::make_unique<Conn>();
  c->fd = tcp_connect(r.host, r.port, err);
  if (c->fd < 0) return nullptr;
  if (r.tls) {
#ifdef XANATOO_NNTP_TLS
    c->tls = std::make_unique<TlsStream>();
    const int fd = c->fd;
    c->fd = -1;  // the stream owns it now
    if (!c->tls->connect(fd, r.host, err, true)) return nullptr;
#else
    if (err) *err = "nntps:// needs a build with TLS";
    return nullptr;
#endif
  }
  std::string greeting;
  size_t pos = 0;
  if (!read_line(*c, greeting, pos)) {
    if (err) *err = c->error;
    return nullptr;
  }
  c->buf.erase(0, pos);
  const int code = status_of(greeting);
  if (code != 200 && code != 201) {
    if (err) *err = "server refused the connection: " + greeting;
    return nullptr;
  }
  const std::string mode = command(*c, "MODE READER");
  if (mode.empty()) {
    if (err) *err = c->error;
    return nullptr;
  }
  if (status_of(mode) == 502) {
    if (err) *err = reply_error("MODE READER", mode);
    return nullptr;
  }
  return c;
}

void quit(Conn& c) { command(c, "QUIT"); }

struct GroupStatus {
  int64_t count = 0, low = 0, high = 0;
};

std::optional<GroupStatus> select_group(Conn& c, const std::string& group,
                                        std::string* err) {
  const std::string line = command(c, "GROUP " + group);
  if (line.empty()) {
    if (err) *err = c.error;
    return std::nullopt;
  }
  if (status_of(line) == 411) {
    if (err) *err = "no such group on this server: " + group;
    return std::nullopt;
  }
  if (status_of(line) != 211) {
    if (err) *err = reply_error("GROUP", line);
    return std::nullopt;
  }
  GroupStatus g;
  char name[512];
  long long n = 0, lo = 0, hi = 0;
  if (std::sscanf(line.c_str() + 3, "%lld %lld %lld %511s", &n, &lo, &hi, name) < 3) {
    if (err) *err = "unreadable GROUP reply: " + line;
    return std::nullopt;
  }
  g.count = n;
  g.low = lo;
  g.high = hi;
  return g;
}

std::optional<std::vector<NntpOverview>> overview(Conn& c, int64_t first,
                                                  int64_t last, std::string* err) {
  if (last < first) return std::vector<NntpOverview>{};
  const std::string range = std::to_string(first) + "-" + std::to_string(last);
  std::string status;
  auto data = multi(c, "OVER " + range, 224, err, &status);
  if (!data && (status_of(status) == 500 || status_of(status) == 501))
    data = multi(c, "XOVER " + range, 224, err, &status);
  if (!data) {
    if (status_of(status) == 423) return std::vector<NntpOverview>{};  // empty range
    return std::nullopt;
  }
  return parse_nntp_overview(*data);
}

std::optional<Document> fetch_groups(Conn& c, const NntpRef& r, std::string* err) {
  const std::string cmd = r.wildmat.empty() ? "LIST ACTIVE" : "LIST ACTIVE " + r.wildmat;
  auto data = multi(c, cmd, 215, err);
  if (!data) return std::nullopt;
  std::vector<NntpGroupInfo> groups = parse_nntp_active(*data);
  if (!groups.empty() && groups.size() <= kNntpMaxListed) {
    std::string e;
    const std::string dcmd =
        r.wildmat.empty() ? "LIST NEWSGROUPS" : "LIST NEWSGROUPS " + r.wildmat;
    if (auto desc = multi(c, dcmd, 215, &e)) apply_nntp_descriptions(groups, *desc);
  }
  quit(c);
  return nntp_groups_document(std::move(groups), r);
}

std::optional<Document> fetch_group(Conn& c, const NntpRef& r, std::string* err) {
  auto g = select_group(c, r.group, err);
  if (!g) return std::nullopt;
  const int64_t last = g->high - (r.page - 1) * kGroupWindow;
  const int64_t first = std::max(g->low, last - kGroupWindow + 1);
  std::vector<NntpOverview> over;
  if (g->count > 0 && last >= g->low) {
    auto o = overview(c, first, last, err);
    if (!o) return std::nullopt;
    over = std::move(*o);
  }
  quit(c);
  return nntp_group_document(over, r, last >= g->low ? first : g->low, g->low,
                             g->count);
}

std::optional<NntpArticle> fetch_article(Conn& c, const std::string& which,
                                         std::string* err, bool* missing) {
  std::string status;
  auto data = multi(c, "ARTICLE " + which, 220, err, &status);
  if (!data) {
    const int code = status_of(status);
    if (missing) *missing = code == 423 || code == 430;
    return std::nullopt;
  }
  NntpArticle a = parse_nntp_article(*data);
  a.number = std::atoll(status.c_str() + std::min<size_t>(4, status.size()));
  return a;
}

std::optional<Document> fetch_thread(Conn& c, const NntpRef& r, std::string* err) {
  auto g = select_group(c, r.group, err);
  if (!g) return std::nullopt;
  if (r.article < g->low || r.article > g->high) {
    if (err) *err = "article " + std::to_string(r.article) + " is not on the server (" +
                    r.group + " has " + std::to_string(g->low) + "–" +
                    std::to_string(g->high) + ")";
    return std::nullopt;
  }
  std::vector<int64_t> members;
  std::string e;
  if (auto o = overview(c, std::max(g->low, r.article - kThreadWindow),
                        std::min(g->high, r.article + kThreadWindow), &e))
    members = nntp_thread_members(*o, r.article);
  if (members.empty()) members.push_back(r.article);
  if (members.size() > kMaxThreadArticles) {
    // The ones nearest to the asked-for article.
    std::sort(members.begin(), members.end(), [&](int64_t a, int64_t b) {
      return std::llabs(a - r.article) < std::llabs(b - r.article);
    });
    members.resize(kMaxThreadArticles);
    std::sort(members.begin(), members.end());
  }
  std::vector<NntpArticle> articles;
  for (int64_t n : members) {
    bool missing = false;
    std::string ae;
    auto a = fetch_article(c, std::to_string(n), &ae, &missing);
    if (a) {
      a->number = n;
      articles.push_back(std::move(*a));
    } else if (!missing) {
      if (err) *err = ae;
      return std::nullopt;
    }
  }
  quit(c);
  if (articles.empty()) {
    if (err) *err = "article " + std::to_string(r.article) + " is gone";
    return std::nullopt;
  }
  return nntp_thread_document(articles, r);
}

std::optional<Document> fetch_msgid(Conn& c, const NntpRef& r, std::string* err) {
  bool missing = false;
  auto a = fetch_article(c, "<" + r.message_id + ">", err, &missing);
  if (!a) {
    if (missing && err) *err = "no article <" + r.message_id + "> on " + r.host;
    return std::nullopt;
  }
  quit(c);
  return nntp_article_document(*a, r);
}

}  // namespace

std::optional<Document> nntp_fetch(std::string_view url, std::string* err) {
  auto r = parse_nntp_url(url);
  if (!r) {
    if (err) *err = "not a news: / nntp: address";
    return std::nullopt;
  }
  auto c = open_conn(*r, err);
  if (!c) return std::nullopt;
  switch (r->view) {
    case NntpView::Groups: return fetch_groups(*c, *r, err);
    case NntpView::Group: return fetch_group(*c, *r, err);
    case NntpView::Article: return fetch_thread(*c, *r, err);
    case NntpView::MessageId: return fetch_msgid(*c, *r, err);
  }
  return std::nullopt;
}

}  // namespace xanatoo

#endif  // XANATOO_NNTP
