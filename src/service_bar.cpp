// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Which service an address belongs to, and its strip (service_bar.hpp).
// Services with sections of their own describe them next to their parser:
// hn_service_bar (hackernews.cpp), forum_service_bar (forums.cpp and
// forum_*.cpp), nntp_service_bar (nntp.cpp). The others are small enough
// to live here.

#include "xanatoo/service_bar.hpp"

#include "xanatoo/archive.hpp"
#include "xanatoo/forums.hpp"
#include "xanatoo/hackernews.hpp"
#include "xanatoo/native.hpp"
#include "xanatoo/nntp.hpp"
#include "xanatoo/util.hpp"
#include "xanatoo/video_sites.hpp"
#include "xanatoo/wayback.hpp"
#include "xanatoo/wikipedia.hpp"
#include "xanatoo/youtube.hpp"

#include <cmath>

namespace xanatoo {

namespace {

bool is_web(std::string_view a) {
  return starts_with_ci(a, "https://") || starts_with_ci(a, "http://");
}

// "a/b/c?q" → {"a", "b", "c"} (query and fragment dropped).
std::vector<std::string_view> path_parts(std::string_view path) {
  path = path.substr(0, path.find_first_of("?#"));
  std::vector<std::string_view> out;
  while (!path.empty()) {
    const size_t slash = path.find('/');
    if (slash != 0) out.push_back(path.substr(0, slash));
    if (slash == std::string_view::npos) break;
    path.remove_prefix(slash + 1);
  }
  return out;
}

std::optional<ServiceBar> wikipedia_bar(const WikipediaRef& r) {
  ServiceBar bar;
  bar.id = r.lang == "en" ? "wikipedia" : "wikipedia:" + r.lang;
  bar.name = r.lang == "en" ? "Wikipedia" : "Wikipedia (" + r.lang + ")";
  bar.color = 0x3366cc;
  auto page = [&](const char* title) {
    WikipediaRef p;
    p.lang = r.lang;
    p.title = title;
    return wikipedia_ref_string(p);
  };
  bar.home = page("Main_Page");
  bar.rows.push_back({"", {{"Main page", bar.home, r.title == "Main_Page"},
                           {"Current events", page("Portal:Current_events"),
                            r.title == "Portal:Current_events"}}});
  return bar;
}

std::optional<ServiceBar> reddit_bar(std::string_view ref) {
  // reddit:r/NAME[/SORT]/…, reddit:user/NAME, reddit:r/NAME/comments/…
  const auto parts = path_parts(ref.substr(ref.find(':') + 1));
  ServiceBar bar;
  bar.id = "reddit";
  bar.name = "Reddit";
  bar.color = 0xff4500;
  bar.home = "reddit:r/popular/";
  std::string sub;
  std::string sort;
  bool listing = false;
  if (parts.size() >= 2 && parts[0] == "r") {
    sub = std::string(parts[1]);
    listing = parts.size() == 2 ||
              (parts.size() == 3 && parts[2] != "comments" && parts[2] != "wiki");
    if (parts.size() == 3 && listing) sort = std::string(parts[2]);
  }
  ServiceRow& row = bar.rows.emplace_back();
  row.links.push_back({"Popular", "reddit:r/popular/", listing && sub == "popular"});
  row.links.push_back({"All", "reddit:r/all/", listing && sub == "all"});
  if (!sub.empty() && sub != "popular" && sub != "all")
    row.links.push_back({"r/" + sub, "reddit:r/" + sub + "/", false});
  if (!sub.empty()) {
    static const char* const kSorts[][2] = {
        {"", "Hot"}, {"new", "New"}, {"top", "Top"}, {"rising", "Rising"}};
    for (const auto& s : kSorts)
      row.links.push_back(
          {s[1], "reddit:r/" + sub + "/" + (s[0][0] ? std::string(s[0]) + "/" : ""),
           listing && (sort == s[0] || (sort == "hot" && !s[0][0]))});
  }
  return bar;
}

std::optional<ServiceBar> lemmy_bar(std::string_view ref) {
  // lemmy:HOST/path
  std::string_view rest = ref.substr(ref.find(':') + 1);
  const size_t slash = rest.find('/');
  const std::string host(rest.substr(0, slash));
  if (host.empty()) return std::nullopt;
  const auto parts = path_parts(
      slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1));
  ServiceBar bar;
  bar.id = "lemmy:" + host;
  bar.name = host;
  bar.color = 0x14854f;
  bar.home = "lemmy:" + host + "/";
  ServiceRow& row = bar.rows.emplace_back();
  row.links.push_back({"Front page", bar.home, parts.empty()});
  if (parts.size() >= 2 && parts[0] == "c")
    row.links.push_back({"c/" + std::string(parts[1]),
                         bar.home + "c/" + std::string(parts[1]),
                         parts.size() == 2});
  return bar;
}

std::optional<ServiceBar> youtube_bar(const YtRef& r) {
  ServiceBar bar;
  bar.id = "youtube";
  bar.name = "YouTube";
  bar.color = 0xff0000;
  if (r.kind == YtRef::Kind::Video && r.replies.empty()) {
    YtRef top = r, newest = r;
    top.page = newest.page = 1;
    top.newest = false;
    newest.newest = true;
    bar.rows.push_back({"", {{"Top comments", youtube_ref_string(top), !r.newest},
                             {"Newest comments", youtube_ref_string(newest),
                              r.newest}}});
  }
  return bar;
}

std::optional<ServiceBar> video_site_bar(const VsRef& r) {
  ServiceBar bar;
  VsRef home;
  home.site = r.site;
  home.host = r.host;
  bar.home = video_site_ref_string(home);
  bar.name = video_site_name(r.site);
  switch (r.site) {
    case VideoSite::PeerTube:
      bar.id = "peertube:" + r.host;
      if (!r.host.empty()) bar.name = r.host;
      bar.color = 0xf1680d;
      break;
    case VideoSite::Odysee: bar.id = "odysee"; bar.color = 0xe50054; break;
    case VideoSite::Dailymotion: bar.id = "dailymotion"; bar.color = 0x0066dc; break;
    case VideoSite::MediaCCC: bar.id = "mediaccc"; bar.color = 0x3a8f3a; break;
  }
  ServiceRow& row = bar.rows.emplace_back();
  row.links.push_back({"Home", bar.home, r.kind == VsRef::Kind::Home});
  if (r.site == VideoSite::MediaCCC) {
    VsRef index = home;
    index.kind = VsRef::Kind::Index;
    row.links.push_back({"Conferences", video_site_ref_string(index),
                         r.kind == VsRef::Kind::Index});
  }
  return bar;
}

// Gemini, Gopher, Spartan, Nex, Finger: one strip per host, in a colour of
// its own, so capsules are told apart at a glance.
std::optional<ServiceBar> small_net_bar(std::string_view address) {
  static constexpr const char* kSchemes[] = {"gemini://", "gopher://",
                                             "spartan://", "nex://", "finger://"};
  for (const char* scheme : kSchemes) {
    if (!starts_with_ci(address, scheme)) continue;
    std::string_view rest = address.substr(std::string_view(scheme).size());
    const std::string host = ascii_lower(rest.substr(0, rest.find_first_of("/?#")));
    if (host.empty()) return std::nullopt;
    ServiceBar bar;
    bar.id = std::string(scheme) + host;
    bar.name = host;
    bar.home = std::string(scheme) + host + "/";
    if (std::string_view(scheme) == "gopher://") bar.home += "1/";
    return bar;
  }
  return std::nullopt;
}

// The strip of a native ref or non-web address (no forms mixed).
std::optional<ServiceBar> ref_bar(std::string_view a) {
  if (is_hn_ref(a)) return hn_service_bar(web_url_for(a));
  if (auto r = parse_forum_ref(a)) return forum_service_bar(*r);
  if (auto r = parse_nntp_url(a)) return nntp_service_bar(*r);
  if (auto r = parse_wikipedia_ref(a)) return wikipedia_bar(*r);
  if (is_reddit_ref(a)) return reddit_bar(a);
  if (is_lemmy_ref(a)) return lemmy_bar(a);
  if (auto r = parse_youtube_ref(a)) return youtube_bar(*r);
  if (auto r = parse_video_site_ref(a)) return video_site_bar(*r);
  if (parse_ia_ref(a)) {
    ServiceBar bar;
    bar.id = "ia";
    bar.name = "Internet Archive";
    bar.color = 0x3b6ea5;
    return bar;
  }
  if (auto r = parse_wayback_ref(a)) {
    ServiceBar bar;
    bar.id = "wayback";
    bar.name = "Wayback Machine";
    bar.color = 0xab2e33;
    bar.rows.push_back({"", {{"Live page", r->url, false}}});
    return bar;
  }
  return small_net_bar(a);
}

}  // namespace

std::optional<ServiceBar> service_bar(std::string_view address) {
  trim_ascii(address);
  if (address.empty()) return std::nullopt;
  std::optional<ServiceBar> bar;
  const bool web = is_web(address);
  if (web) {
    // The web page of a service ("https://news.ycombinator.com/newest",
    // "https://en.wikipedia.org/wiki/X") has its native form's strip.
    const std::string ref = native_url_for(address);
    if (ref.empty()) return std::nullopt;
    bar = ref_bar(ref);
  } else {
    bar = ref_bar(address);
  }
  if (!bar) return std::nullopt;
  // Links keep the form of the page: web pages lead to web pages, native
  // views to native views.
  auto keep_form = [&](std::string& ref) {
    if (ref.empty()) return;
    if (web) {
      if (!is_web(ref))
        if (std::string url = web_url_for(ref); !url.empty()) ref = std::move(url);
    } else if (is_web(ref)) {
      if (std::string native = native_url_for(ref); !native.empty())
        ref = std::move(native);
    }
  };
  keep_form(bar->home);
  for (ServiceRow& row : bar->rows)
    for (ServiceLink& l : row.links)
      if (bar->id != "wayback") keep_form(l.ref);
  if (!bar->color) bar->color = service_color_for(bar->id);
  return bar;
}

uint32_t service_color_for(std::string_view id) {
  // FNV-1a → hue; fixed saturation and value keep text on the tint legible.
  uint32_t h = 2166136261u;
  for (unsigned char c : id) h = (h ^ c) * 16777619u;
  const float hue = static_cast<float>(h % 360u) / 60.f;
  const float v = 0.62f, s = 0.70f;
  const float c = v * s;
  const float x = c * (1.f - std::abs(std::fmod(hue, 2.f) - 1.f));
  float r = 0, g = 0, b = 0;
  switch (static_cast<int>(hue)) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
  }
  const float m = v - c;
  auto byte = [&](float f) { return static_cast<uint32_t>(std::lround((f + m) * 255.f)); };
  return byte(r) << 16 | byte(g) << 8 | byte(b);
}

}  // namespace xanatoo
