// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/https.hpp"
#include "xanatoo/tcp.hpp"
#include "xanatoo/http_framing.hpp"
#include "xanatoo/http_cookies.hpp"
#include "xanatoo/http_url.hpp"
#include "xanatoo/inflate.hpp"
#include "xanatoo/net_address.hpp"
#include "xanatoo/tls.hpp"
#include "xanatoo/transfer_meter.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <signal.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace xanatoo {
namespace {

// A single HTTP/1.1 connection. The TLS layer owns secure sockets; the raw
// layer owns plain sockets. No TLS handshake is attempted for http://.
class HttpConnection {
public:
  HttpConnection() = default;
  HttpConnection(const HttpConnection&) = delete;
  HttpConnection& operator=(const HttpConnection&) = delete;
  ~HttpConnection() { if (plain_fd_ >= 0) ::close(plain_fd_); }

  bool open(const HttpUrl& url, std::string* err,
            HttpRequestSecurity security = {}) {
    if (security.require_https && !url.secure) {
      if (err) *err = "authenticated request requires HTTPS";
      return false;
    }
    const int fd = xanatoo::tcp_connect(url.host, url.port, err, security.public_only);
    if (fd < 0) return false;
    fd_ = fd;
    secure_ = url.secure;
    if (secure_) return tls_.connect(fd, url.host, err, security.verify_tls);
    plain_fd_ = fd;
    return true;
  }

  bool write_request(const std::string& request, const HttpUrl& url,
                     std::string_view cookies) {
    if (!write_all(request.data(), request.size())) return false;
#if XANATOO_HTTP_COOKIES
    record_http_cookies(url, cookies);
#else
    (void)url;
    (void)cookies;
#endif
    return true;
  }

  bool write_all(const void* data, size_t size) {
    if (secure_) return tls_.write_all(data, size);
    const char* ptr = static_cast<const char*>(data);
    while (size) {
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const ssize_t n = ::send(plain_fd_, ptr, size, flags);
      if (n < 0 && errno == EINTR) continue;
      if (n <= 0) return false;
      ptr += n;
      size -= static_cast<size_t>(n);
    }
    return true;
  }

  int read(void* dst, size_t size) {
    if (secure_) return tls_.read(dst, size);
    for (;;) {
      const ssize_t n = ::recv(plain_fd_, dst, size, 0);
      if (n < 0 && errno == EINTR) continue;
      if (n < 0) read_errno_ = errno;
      return static_cast<int>(n);
    }
  }

  // An idle kept-alive connection the server has not closed: nothing to
  // read (a close or TLS close_notify would be readable).
  bool idle_alive() const {
    if (fd_ < 0 || (secure_ && !tls_.connected())) return false;
    pollfd p{fd_, POLLIN, 0};
    return ::poll(&p, 1, 0) == 0;
  }

  // Reason for the last failed read() (empty if unknown).
  std::string last_error() const {
    if (secure_) return tls_.last_error();
    return std::strerror(read_errno_);
  }

private:
  int read_errno_ = 0;
  int fd_ = -1;        // the socket (owned by tls_ or plain_fd_)
  int plain_fd_ = -1;
  bool secure_ = false;
  TlsStream tls_;
};

bool allows_cookie_jar(std::string_view extra_headers) {
#if XANATOO_HTTP_COOKIES
  if (!http_cookies_allowed()) return false;
  const auto headers = "\n" + ascii_lower(extra_headers);
  return headers.find("\ncookie:") == std::string::npos &&
         headers.find("\nauthorization:") == std::string::npos;
#else
  (void)extra_headers;
  return false;
#endif
}

std::string jar_header(const HttpUrl& url, bool use_jar) {
#if XANATOO_HTTP_COOKIES
  if (use_jar) return http_cookie_header(url);
#else
  (void)url;
  (void)use_jar;
#endif
  return {};
}

std::string header_value(const std::string& headers, const char* key) {
  const std::string lower = ascii_lower(headers);
  const std::string needle = std::string("\n") + key + ":";
  size_t at = lower.find(needle);
  if (at == std::string::npos) return {};
  at = headers.find(':', at) + 1;
  size_t end = headers.find("\r\n", at);
  // The block usually ends without a CRLF after its last header.
  if (end == std::string::npos) end = headers.size();
  std::string_view value(headers.data() + at, end - at);
  trim_ascii(value);
  return std::string(value);
}

}  // namespace
namespace {

// Decode a "Transfer-Encoding: chunked" body (hex size line, data, CRLF).
std::string dechunk(const std::string& in) {
  std::string out;
  size_t p = 0;
  while (p < in.size()) {
    size_t eol = in.find("\r\n", p);
    if (eol == std::string::npos) break;
    size_t n = std::strtoul(in.c_str() + p, nullptr, 16);
    if (n == 0) break;
    p = eol + 2;
    if (p + n > in.size()) n = in.size() - p;
    out.append(in, p, n);
    p += n + 2;
  }
  return out;
}

// Kept-alive connections between requests (https_get_follow_pooled): a few
// idle ones per host, shared by all threads, dropped after kPoolIdle.
constexpr auto kPoolIdle = std::chrono::seconds(20);
constexpr size_t kPoolMax = 16;

struct IdleConnection {
  std::string key;
  std::unique_ptr<HttpConnection> conn;
  std::chrono::steady_clock::time_point since;
};

struct ConnectionPool {
  std::mutex mutex;
  std::vector<IdleConnection> idle;  // oldest first
};

// Never destroyed: detached image workers may still use it at exit.
ConnectionPool& connection_pool() {
  static auto* pool = new ConnectionPool;
  return *pool;
}

std::string pool_key(const HttpUrl& url, const HttpRequestSecurity& sec) {
  return std::string(url.secure ? "s:" : "p:") + url.host + ":" +
         std::to_string(url.port) + (sec.verify_tls ? ":v" : ":n") +
         (sec.public_only ? ":public" : "");
}

std::unique_ptr<HttpConnection> take_idle(const std::string& key) {
  for (;;) {
    std::unique_ptr<HttpConnection> conn;
    std::vector<IdleConnection> expired;  // closed outside the lock
    {
      ConnectionPool& pool = connection_pool();
      std::lock_guard<std::mutex> lock(pool.mutex);
      const auto now = std::chrono::steady_clock::now();
      auto& idle = pool.idle;
      for (auto it = idle.begin(); it != idle.end();) {
        if (now - it->since > kPoolIdle) {
          expired.push_back(std::move(*it));
          it = idle.erase(it);
        } else {
          ++it;
        }
      }
      for (auto it = idle.rbegin(); it != idle.rend(); ++it) {
        if (it->key != key) continue;
        conn = std::move(it->conn);
        idle.erase(std::next(it).base());
        break;
      }
    }
    if (!conn || conn->idle_alive()) return conn;
  }
}

void put_idle(const std::string& key, std::unique_ptr<HttpConnection> conn) {
  IdleConnection dropped;
  ConnectionPool& pool = connection_pool();
  std::lock_guard<std::mutex> lock(pool.mutex);
  if (pool.idle.size() >= kPoolMax) {
    dropped = std::move(pool.idle.front());
    pool.idle.erase(pool.idle.begin());
  }
  pool.idle.push_back({key, std::move(conn), std::chrono::steady_clock::now()});
}

// A write to a connection the server already closed raises SIGPIPE, which
// would end the program; blocked, the write fails with EPIPE instead.
void block_sigpipe_in_this_thread() {
  thread_local bool blocked = false;
  if (blocked) return;
  blocked = true;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

struct Exchange {
  std::optional<HttpResult> result;
  bool nothing = false;   // write failed or closed before any byte arrived
  bool reusable = false;  // complete, and the server keeps the connection
};

// Send `req` over `conn` and read the response, up to its end (framing) or
// the connection closing.
Exchange exchange(HttpConnection& conn, const std::string& req_s,
                  const HttpUrl& url, std::string_view cookies, bool use_jar,
                  const HttpRequestSecurity& security, std::string* err) {
  Exchange ex;
  if (!conn.write_request(req_s, url, cookies)) {
    if (err) *err = "HTTP write failed";
    ex.nothing = true;
    return ex;
  }

  std::string data;
  HttpResponseFraming framing;
  char buf[4096];
  meter_expect(-1);
  bool metered_headers = false;
  for (;;) {
    int n = conn.read(buf, sizeof(buf));
    if (n <= 0) break;
    if (transfer_cancelled()) {
      if (err) *err = transfer_cancelled_error();
      return ex;
    }
    data.append(buf, static_cast<size_t>(n));
    meter_bytes(static_cast<size_t>(n));
    if (!metered_headers) {
      // Once the headers are in, the Content-Length tells how much is left.
      const size_t end = data.find("\r\n\r\n");
      if (end != std::string::npos) {
        metered_headers = true;
        const std::string head = data.substr(0, end);
        const std::string length = header_value(head, "content-length");
        int64_t len = -1;
        if (!length.empty() &&
            ascii_lower(header_value(head, "transfer-encoding"))
                    .find("chunked") == std::string::npos) {
          auto [p, ec] = std::from_chars(length.data(),
                                         length.data() + length.size(), len);
          if (ec != std::errc{} || len < 0) len = -1;
        }
        const int64_t have = static_cast<int64_t>(data.size() - end - 4);
        if (len >= 0) meter_expect(std::max<int64_t>(0, len - have));
      }
    }
    if (data.size() > security.max_response_bytes) {
      if (err) *err = "HTTP response exceeds size limit";
      return ex;
    }
    if (framing.update(data)) break;
  }
  if (data.empty()) {
    if (err) *err = "connection closed without a response";
    ex.nothing = true;
    return ex;
  }
  if (framing.complete()) {
    // Bytes past the end would belong to no request: do not reuse.
    ex.reusable = framing.reusable() && data.size() == framing.size();
    data.resize(framing.size());
  }

  size_t hdr_end = data.find("\r\n\r\n");
  if (hdr_end == std::string::npos) {
    if (err) *err = "malformed HTTP response";
    ex.reusable = false;
    return ex;
  }
  std::string headers = data.substr(0, hdr_end);
  HttpResult out;
  out.body = data.substr(hdr_end + 4);

  {
    size_t sp1 = headers.find(' ');
    if (sp1 != std::string::npos) {
      size_t sp2 = headers.find(' ', sp1 + 1);
      std::string code = (sp2 == std::string::npos)
                             ? headers.substr(sp1 + 1, 3)
                             : headers.substr(sp1 + 1, sp2 - sp1 - 1);
      out.status = std::atoi(code.c_str());
    }
  }

  std::string hlow = ascii_lower(headers);
  auto header_val = [&](const char* name) -> std::string {
    std::string key = std::string("\n") + name + ":";
    size_t loc = hlow.find(key);
    if (loc == std::string::npos) return {};
    size_t v0 = headers.find(':', loc);
    size_t v1 = headers.find("\r\n", v0);
    if (v0 == std::string::npos) return {};
    // The block has no CRLF after its last header.
    if (v1 == std::string::npos) v1 = headers.size();
    std::string val = headers.substr(v0 + 1, v1 - v0 - 1);
    while (!val.empty() && (val.front() == ' ' || val.front() == '\t'))
      val.erase(val.begin());
    return val;
  };
#if XANATOO_HTTP_COOKIES
  if (use_jar && out.status >= 200)
    receive_http_cookies(url, headers);
#else
  (void)use_jar;
#endif
  out.location = header_val("location");
  out.content_type = header_val("content-type");
  if (ascii_lower(header_val("transfer-encoding")).find("chunked") !=
      std::string::npos)
    out.body = dechunk(out.body);
  if (auto decoded = decode_content_encoding(header_val("content-encoding"),
                                             out.body))
    out.body = std::move(*decoded);
  ex.result = std::move(out);
  return ex;
}

std::optional<HttpResult> request_impl(std::string_view method,
                                       std::string_view url,
                                       std::string_view user_agent,
                                       std::string_view extra_headers,
                                       std::string_view body_in,
                                       std::string* err,
                                       const HttpRequestSecurity& security,
                                       bool pooled) {
  const auto parsed = parse_http_url(url);
  if (!parsed) {
    if (err) *err = "invalid http(s) URL";
    return std::nullopt;
  }

  const bool use_jar = (method == "GET" || method == "HEAD") &&
                       security.verify_tls && !security.public_only &&
                       allows_cookie_jar(extra_headers);
  const std::string cookies = jar_header(*parsed, use_jar);
  std::ostringstream req;
  const bool custom_accept = ascii_lower(extra_headers).find("accept:") !=
      std::string::npos;
  req << method << " " << parsed->target << " HTTP/1.1\r\n"
      << "Host: " << parsed->authority << "\r\n"
      << "User-Agent: " << user_agent << "\r\n";
  if (!custom_accept) req << "Accept: application/json, text/plain, */*\r\n";
  if (ascii_lower(extra_headers).find("accept-language:") == std::string::npos)
    req << "Accept-Language: " << accept_language() << "\r\n";
  if (!pooled) req << "Connection: close\r\n";
  if (!cookies.empty()) req << "Cookie: " << cookies << "\r\n";
  if (!extra_headers.empty()) req << extra_headers;
  if (!body_in.empty())
    req << "Content-Length: " << body_in.size() << "\r\n";
  req << "\r\n";
  if (!body_in.empty()) req << body_in;
  const std::string req_s = req.str();

  if (pooled) block_sigpipe_in_this_thread();
  const std::string key = pooled ? pool_key(*parsed, security) : std::string();
  for (int attempt = 0; attempt < 2; ++attempt) {
    std::unique_ptr<HttpConnection> conn;
    if (pooled && attempt == 0) conn = take_idle(key);
    const bool reused = conn != nullptr;
    if (!conn) {
      if (err) err->clear();
      conn = std::make_unique<HttpConnection>();
      if (!conn->open(*parsed, err, security)) return std::nullopt;
    }
    Exchange ex = exchange(*conn, req_s, *parsed, cookies, use_jar, security, err);
    // The server closed the idle connection meanwhile: once more, fresh.
    if (reused && ex.nothing) continue;
    if (pooled && ex.reusable) put_idle(key, std::move(conn));
    return std::move(ex.result);
  }
  return std::nullopt;
}

std::optional<HttpResult> get_follow(std::string_view url,
                                     std::string_view user_agent,
                                     std::string* err, std::string* final_url,
                                     bool pooled) {
  std::string current(url);
  std::optional<HttpResult> resp;
  bool upgraded = false;  // the last hop was an http:// → https:// upgrade
  for (int hop = 0; hop < 5; ++hop) {
    resp = request_impl("GET", current, user_agent, {}, {}, err, {}, pooled);
    if (!resp || resp->status < 300 || resp->status >= 400 ||
        resp->location.empty())
      break;
    auto next = resolve_http_redirect(current, resp->location);
    if (!next) {
      if (err) *err = "unsupported HTTP redirect: " + resp->location;
      return std::nullopt;
    }
    auto target = secure_redirect_target(current, *next, &upgraded);
    if (!target) {
      if (err) *err = "unsupported HTTP redirect: " + resp->location;
      return std::nullopt;
    }
    current = std::move(*target);
  }
  if (!resp && upgraded && err)
    *err = "HTTPS redirect to plaintext HTTP; HTTPS upgrade failed: " + *err;
  if (final_url) *final_url = current;
  return resp;
}

}  // namespace

std::optional<HttpResult> https_request(std::string_view method,
                                        std::string_view url,
                                        std::string_view user_agent,
                                        std::string_view extra_headers,
                                        std::string_view body_in,
                                        std::string* err,
                                        HttpRequestSecurity security) {
  return request_impl(method, url, user_agent, extra_headers, body_in, err,
                      security, false);
}

std::optional<HttpResult> https_get(std::string_view url,
                                    std::string_view user_agent,
                                    std::string* err) {
  return https_request("GET", url, user_agent, {}, {}, err);
}

std::optional<HttpResult> https_get_follow(std::string_view url,
                                           std::string_view user_agent,
                                           std::string* err,
                                           std::string* final_url) {
  return get_follow(url, user_agent, err, final_url, false);
}

std::optional<HttpResult> https_get_follow_pooled(std::string_view url,
                                                  std::string_view user_agent,
                                                  std::string* err,
                                                  std::string* final_url) {
  return get_follow(url, user_agent, err, final_url, true);
}

namespace {

struct ChunkDecoder {
  enum class State { Size, Data, CrLf, Done } state = State::Size;
  std::string pending;
  uint64_t left = 0;

  bool feed(std::string_view bytes, const HttpBodyCallback& sink,
            std::string* err) {
    pending.append(bytes);
    while (true) {
      if (state == State::Done) return true;
      if (state == State::Size) {
        const size_t end = pending.find("\r\n");
        if (end == std::string::npos) {
          if (pending.size() > 128) {
            if (err) *err = "oversized chunk header";
            return false;
          }
          return true;
        }
        std::string_view hex(pending.data(), end);
        hex = hex.substr(0, hex.find(';'));
        size_t n = 0;
        auto [ptr, ec] = std::from_chars(hex.data(), hex.data() + hex.size(), n,
                                         16);
        if (ec != std::errc{} || ptr != hex.data() + hex.size()) {
          if (err) *err = "invalid HTTP chunk size";
          return false;
        }
        pending.erase(0, end + 2);
        left = n;
        state = left ? State::Data : State::Done;
      } else if (state == State::Data) {
        if (pending.empty()) return true;
        const size_t n = static_cast<size_t>(
            std::min<uint64_t>(left, pending.size()));
        if (n && !sink(std::string_view(pending.data(), n))) return false;
        pending.erase(0, n);
        left -= n;
        if (!left) state = State::CrLf;
      } else if (state == State::CrLf) {
        if (pending.size() < 2) return true;
        if (pending.compare(0, 2, "\r\n") != 0) {
          if (err) *err = "invalid HTTP chunk delimiter";
          return false;
        }
        pending.erase(0, 2);
        state = State::Size;
      }
    }
  }
};

bool stream_raw(const std::string& url, std::string_view agent,
                 const HttpHeaderCallback& on_headers,
                 const HttpBodyCallback& on_body, HttpStreamHeaders& result,
                 std::string* err, std::string_view extra_headers) {
  const auto parsed = parse_http_url(url);
  if (!parsed) {
    if (err) *err = "invalid http(s) URL";
    return false;
  }
  HttpConnection conn;
  if (!conn.open(*parsed, err)) return false;
  const bool custom_accept = ascii_lower(extra_headers).find("accept:") !=
      std::string::npos;
  const bool use_jar = allows_cookie_jar(extra_headers);
  const std::string cookies = jar_header(*parsed, use_jar);
  const std::string req = "GET " + parsed->target + " HTTP/1.1\r\nHost: " + parsed->authority +
      "\r\nUser-Agent: " + std::string(agent) +
      (custom_accept ? "\r\n" : "\r\nAccept: */*\r\n") +
      "Accept-Language: " + accept_language() +
      "\r\nAccept-Encoding: identity\r\n" + std::string(extra_headers) +
      (cookies.empty() ? "" : "Cookie: " + cookies + "\r\n") +
      "Connection: close\r\n\r\n";
  if (!conn.write_request(req, *parsed, cookies)) {
    if (err) *err = "HTTP write failed";
    return false;
  }
  std::string pending;
  ChunkDecoder chunked;
  bool have_headers = false;
  bool is_chunked = false;
  int64_t left = -1;
  char buf[8192];
  uint64_t total_read = 0;
  meter_expect(-1);
  for (;;) {
    const int n = conn.read(buf, sizeof(buf));
    if (n > 0) meter_bytes(static_cast<size_t>(n));
    if (n < 0) {
      if (err) {
        std::string where = have_headers
            ? "while receiving the response body (" +
                  std::to_string(total_read) + " bytes received" +
                  (left >= 0 ? " of " + std::to_string(total_read + left) : "") +
                  (is_chunked ? ", chunked" : "") + ")"
            : "while waiting for the response headers (" +
                  std::to_string(total_read) + " bytes received)";
        *err = "Network error " + where + ": " + conn.last_error();
      }
      return false;
    }
    if (n == 0) break;
    if (transfer_cancelled()) {
      if (err) *err = transfer_cancelled_error();
      return false;
    }
    total_read += static_cast<uint64_t>(n);
    std::string_view data(buf, static_cast<size_t>(n));
    if (!have_headers) {
      pending.append(data);
      const size_t end = pending.find("\r\n\r\n");
      if (end == std::string::npos) {
        if (pending.size() > 128 * 1024) {
          if (err) *err = "HTTP headers too large";
          return false;
        }
        continue;
      }
      const std::string headers = pending.substr(0, end);
      const size_t sp = headers.find(' ');
      if (sp == std::string::npos || sp + 4 > headers.size()) {
        if (err) *err = "malformed HTTP status";
        return false;
      }
      result.status = std::atoi(headers.c_str() + sp + 1);
      result.location = header_value(headers, "location");
      result.content_type = header_value(headers, "content-type");
      result.content_disposition = header_value(headers, "content-disposition");
      result.server = header_value(headers, "server");
      result.cf_mitigated = header_value(headers, "cf-mitigated");
      result.retry_after = header_value(headers, "retry-after");
      result.content_encoding = header_value(headers, "content-encoding");
      result.etag = header_value(headers, "etag");
      result.last_modified = header_value(headers, "last-modified");
      result.content_range = header_value(headers, "content-range");
      const std::string length = header_value(headers, "content-length");
      if (!length.empty()) {
        auto [p, ec] = std::from_chars(length.data(),
                                      length.data() + length.size(), left);
        if (ec != std::errc{} || p != length.data() + length.size() ||
            left < 0) {
          if (err) *err = "invalid Content-Length";
          return false;
        }
      }
      is_chunked = ascii_lower(header_value(headers, "transfer-encoding"))
                       .find("chunked") != std::string::npos;
      result.content_length = is_chunked ? -1 : left;
      if (!is_chunked && left >= 0)
        meter_expect(std::max<int64_t>(
            0, left - static_cast<int64_t>(pending.size() - end - 4)));
      have_headers = true;
#if XANATOO_HTTP_COOKIES
      if (use_jar && result.status >= 200) receive_http_cookies(*parsed, headers);
#endif
      if (!on_headers(result)) return false;
      // Redirect bodies are irrelevant: follow Location immediately rather
      // than draining a potentially large response before the next request.
      if (result.status >= 300 && result.status < 400 &&
          !result.location.empty()) return true;
      if (!is_chunked && left == 0) return true;
      pending.erase(0, end + 4);
      data = pending;
    }
    if (is_chunked) {
      if (!chunked.feed(data, on_body, err)) return false;
      if (chunked.state == ChunkDecoder::State::Done) return true;
    } else {
      const size_t use = left < 0 ? data.size() :
          static_cast<size_t>(std::min<int64_t>(left, data.size()));
      if (use && !on_body(data.substr(0, use))) return false;
      if (left >= 0) {
        left -= static_cast<int64_t>(use);
        if (!left) return true;
      }
    }
    if (have_headers) pending.clear();
  }
  if (!have_headers) {
    if (err) *err = "incomplete HTTP headers";
    return false;
  }
  if ((is_chunked && chunked.state != ChunkDecoder::State::Done) ||
      (!is_chunked && left > 0)) {
    if (err) *err = "incomplete HTTP body";
    return false;
  }
  return true;
}

// stream_raw with the Content-Encoding undone. An encoded body is collected
// and handed on decoded at the end (the inflater works on whole buffers);
// its length is then unknown up front.
bool stream_once(const std::string& url, std::string_view agent,
                 const HttpHeaderCallback& on_headers,
                 const HttpBodyCallback& on_body, HttpStreamHeaders& result,
                 std::string* err, std::string_view extra_headers) {
  constexpr size_t kMaxDecoded = 256u << 20;
  bool encoded = false;
  std::string packed;
  const bool ok = stream_raw(url, agent,
      [&](const HttpStreamHeaders& h) {
        HttpStreamHeaders shown = h;
        std::string_view coding = h.content_encoding;
        trim_ascii(coding);
        encoded = !coding.empty() && ascii_lower(coding) != "identity" &&
                  !(h.status >= 300 && h.status < 400 && !h.location.empty());
        if (encoded) shown.content_length = -1;
        return on_headers(shown);
      },
      [&](std::string_view bytes) {
        if (!encoded) return on_body(bytes);
        if (packed.size() + bytes.size() > kMaxDecoded) {
          if (err) *err = "HTTP response exceeds size limit";
          return false;
        }
        packed.append(bytes);
        return true;
      }, result, err, extra_headers);
  if (!ok || !encoded) return ok;
  result.content_length = -1;
  if (auto decoded = decode_content_encoding(result.content_encoding, packed,
                                             kMaxDecoded))
    packed = std::move(*decoded);
  return packed.empty() || on_body(packed);
}

}  // namespace

bool https_get_follow_stream(std::string_view url, std::string_view user_agent,
                             const HttpHeaderCallback& on_headers,
                             const HttpBodyCallback& on_body,
                             std::string* err, std::string* final_url,
                             std::string_view extra_headers) {
  std::string current(url);
  bool upgraded = false;  // the last hop was an http:// → https:// upgrade
  for (int hop = 0; hop < 5; ++hop) {
    HttpStreamHeaders headers;
    const bool ok = stream_once(current, user_agent,
        [&](const HttpStreamHeaders& h) {
          if (h.status >= 300 && h.status < 400 && !h.location.empty())
            return true;
          return on_headers(h);
        },
        [&](std::string_view bytes) {
          if (headers.status >= 300 && headers.status < 400 &&
              !headers.location.empty()) return true;
          return on_body(bytes);
        }, headers, err, extra_headers);
    if (!ok) {
      if (upgraded && err)
        *err = "HTTPS redirect to plaintext HTTP; HTTPS upgrade failed: " + *err;
      return false;
    }
    if (headers.status < 300 || headers.status >= 400 ||
        headers.location.empty()) {
      if (final_url) *final_url = current;
      return true;
    }
    auto next = resolve_http_redirect(current, headers.location);
    if (!next) {
      if (err) *err = "unsupported HTTP redirect: " + headers.location;
      return false;
    }
    auto target = secure_redirect_target(current, *next, &upgraded);
    if (!target) {
      if (err) *err = "unsupported HTTP redirect: " + headers.location;
      return false;
    }
    current = std::move(*target);
  }
  if (err) *err = "too many HTTP redirects";
  return false;
}

namespace {
std::mutex g_accept_language_mutex;
std::string g_accept_language;
}  // namespace

void set_accept_language(std::string value) {
  // Header injection guard: the value is the user's text.
  std::erase_if(value, [](char c) { return c == '\r' || c == '\n'; });
  std::lock_guard<std::mutex> lock(g_accept_language_mutex);
  g_accept_language = std::move(value);
}

std::string accept_language() {
  std::lock_guard<std::mutex> lock(g_accept_language_mutex);
  return g_accept_language.empty() ? std::string("en-US,en;q=0.9")
                                   : g_accept_language;
}


}  // namespace xanatoo
