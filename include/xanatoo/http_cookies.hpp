// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/http_url.hpp"

#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

struct HttpCookie {
  std::string origin, path, name, value;
  bool enabled = true;
  int64_t expires = 0;  // Unix seconds, zero for an imported or session cookie
};

struct CookieSite {
  std::string origin;
  bool enabled = false;
  std::string last_path, last_names;
};

struct CookieSnapshot {
  std::vector<CookieSite> sites;
  std::vector<HttpCookie> cookies;
};

#if XANATOO_HTTP_COOKIES
// A foreground fetch can use the jar and reports whether credentials were sent.
class HttpCookieScope {
public:
  explicit HttpCookieScope(bool& sent);
  ~HttpCookieScope();
  HttpCookieScope(const HttpCookieScope&) = delete;
  HttpCookieScope& operator=(const HttpCookieScope&) = delete;

private:
  bool* previous_;
};

bool http_cookies_allowed();
bool http_cookies_sent();
bool http_cookie_site_enabled(const HttpUrl& url);
std::string cookie_origin(const HttpUrl& url);
bool import_http_cookie(std::string_view scope, std::string_view pair);
size_t import_netscape_cookies(std::string_view scope, std::istream& input);
bool set_cookie_site(std::string_view origin, bool enabled);
bool edit_http_cookie(const HttpCookie& key, bool remove, bool enabled);
CookieSnapshot http_cookie_snapshot();
std::string http_cookie_header(const HttpUrl& url);
void record_http_cookies(const HttpUrl& url, std::string_view header);
void receive_http_cookies(const HttpUrl& url, std::string_view headers);
#endif

}  // namespace xanatoo
