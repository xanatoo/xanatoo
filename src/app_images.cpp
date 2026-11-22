// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Remote images: ImageCache queues the http(s) / gemini / spartan / gopher /
// nex sources layout asks for (the ones on screen first, want_image_now);
// a few detached workers download each one into the URL cache
// (url_cache.hpp, cache_dir()/urls/) and
// prebuild its inline thumbnail (stb builds; thumtoo makes it on request),
// then the main thread marks it done and relayouts (the image's size
// changes from placeholder to real). Thumbnails finished by thumtoo also
// arrive here (ImageCache::poll).

#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

#include <algorithm>
#include <cmath>

#include "xanatoo/app.hpp"
#include "xanatoo/theme.hpp"
#include "xanatoo/http_status.hpp"
#include "xanatoo/gemini.hpp"
#include "xanatoo/gopher.hpp"
#include "xanatoo/nex.hpp"
#include "xanatoo/spartan.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/util.hpp"
#include "xanatoo/url_cache.hpp"
#if defined(XANATOO_HTTPS) && XANATOO_HTTPS
#include "xanatoo/https.hpp"
#include "xanatoo/version.hpp"
#endif

namespace fs = std::filesystem;

namespace xanatoo::app {

namespace {

// Arrivals closer together than this share one relayout (a page with
// hundreds of thumbnails would otherwise relayout every frame).
constexpr Uint32 kImageRelayoutMs = 150;

#ifndef __EMSCRIPTEN__
constexpr int kMaxWorkers = 4;
constexpr uint64_t kUrlCacheBytes = 512ull * 1024 * 1024;
constexpr int64_t kUrlCacheRowAgeMs = 180ll * 24 * 3600 * 1000;  // ~6 months
constexpr uint64_t kThumbCacheBytes = 128ull * 1024 * 1024;
// Web PDFs / DjVu / comics shown as pages (html_page.cpp `PagesCopy`).
constexpr uint64_t kDocumentCacheBytes = 1024ull * 1024 * 1024;

// The bytes of the small-net image `url` (gemini / spartan / gopher / nex),
// or false with `err` set (empty when the client is not built in).
bool fetch_smallnet_image(const std::string& url, std::string& body,
                          std::string& err) {
#if defined(XANATOO_GEMINI) && XANATOO_GEMINI
  if (xanatoo::is_gemini_url(url)) {
    auto r = xanatoo::gemini_fetch(url, &err);
    if (!r) return false;
    if (r->status < 20 || r->status >= 30) {
      err = "status " + std::to_string(r->status) + " " + r->meta;
      return false;
    }
    body = std::move(r->body);
    return true;
  }
#endif
#if defined(XANATOO_SPARTAN) && XANATOO_SPARTAN
  if (xanatoo::is_spartan_url(url)) {
    auto r = xanatoo::spartan_fetch(url, &err);
    if (!r) return false;
    if (r->status != 2) {
      err = "status " + std::to_string(r->status) + " " + r->meta;
      return false;
    }
    body = std::move(r->body);
    return true;
  }
#endif
#if defined(XANATOO_GOPHER) && XANATOO_GOPHER
  if (xanatoo::is_gopher_url(url)) {
    auto r = xanatoo::gopher_fetch(url, &err);
    if (!r) return false;
    if (r->is_menu || r->needs_input) {
      err = "no image";
      return false;
    }
    body = std::move(r->body);
    return true;
  }
#endif
#if defined(XANATOO_NEX) && XANATOO_NEX
  if (xanatoo::is_nex_url(url)) {
    auto r = xanatoo::nex_fetch(url, &err);
    if (!r) return false;
    if (r->is_directory) {
      err = "no image";
      return false;
    }
    body = std::move(r->body);
    return true;
  }
#endif
  (void)url;
  (void)body;
  return false;
}

// Download `url` to its cache file and build the inline thumbnail.
// `page`: the answer was not a picture (an HTML page named like one,
// "/wiki/File:X.jpg"); the URL cache remembers it either way.
bool download_image(const std::string& url, bool& page) {
  page = false;
  std::string body;
  std::string err;
  if (!xanatoo::starts_with_ci(url, "http://") &&
      !xanatoo::starts_with_ci(url, "https://")) {
    const bool got = fetch_smallnet_image(url, body, err);
    if (!got || body.empty()) {
      std::cerr << "image: " << url << ": "
                << (!err.empty() ? err : got ? "no image" : "no client") << "\n";
      return false;
    }
  } else {
#if defined(XANATOO_HTTPS) && XANATOO_HTTPS
    // Pooled: a page's images mostly come from one host, and a TLS
    // handshake per thumbnail costs more than the thumbnail.
    std::string final_url;
    auto r = xanatoo::https_get_follow_pooled(
        url, xanatoo::user_agent("image fetch"), &err, &final_url);
    auto cache_entry = [&](const std::string& u, const std::string& fu,
                           const xanatoo::HttpResult& res) {
      xanatoo::UrlCacheEntry e;
      e.url = u;
      e.final_url = fu;
      e.status = res.status;
      e.content_type = res.content_type;
      return e;
    };
    if (r && r->status == 200 && !xanatoo::content_type_is_image(r->content_type) &&
        !xanatoo::looks_like_image(r->body)) {
      // Kept as what it is: the page, which navigation then fetches as one.
      const std::string_view bytes = r->body;
      const xanatoo::UrlCacheEntry entry = cache_entry(url, final_url, *r);
      xanatoo::url_cache_put(entry,
                             r->body.size() < 12 * 1024 * 1024 ? &bytes
                                                               : nullptr);
      std::cerr << "image: " << url << ": not an image ("
                << (r->content_type.empty() ? "no type" : r->content_type)
                << ")\n";
      const std::string type = xanatoo::ascii_lower(
          r->content_type.substr(0, r->content_type.find(';')));
      page = type == "text/html" || type == "application/xhtml+xml";
      return false;
    }
    if (!r || r->status != 200 || r->body.empty()) {
      std::cerr << "image: " << url << ": "
                << (r ? http_status_error(r->status) : err) << "\n";
      return false;
    }
    // https_request stops reading at 12 MiB: anything that size is cut off.
    if (r->body.size() >= 12 * 1024 * 1024) {
      std::cerr << "image: " << url << ": too large\n";
      return false;
    }
    // The URL cache writes the bytes where ImageCache reads them.
    const std::string_view bytes = r->body;
    const xanatoo::UrlCacheEntry entry = cache_entry(url, final_url, *r);
    if (!xanatoo::url_cache_put(entry,
                                &bytes))
      return false;
#if defined(XANATOO_THUMTOO) && XANATOO_THUMTOO
    return true;
#else
    return xanatoo::build_thumb_file(xanatoo::remote_image_path(url),
                                     xanatoo::kRemoteThumbEdge);
#endif
#else
    return false;
#endif
  }
  if (!xanatoo::store_remote_image(url, body)) return false;
#if defined(XANATOO_THUMTOO) && XANATOO_THUMTOO
  return true;  // thumtoo builds the thumbnail when layout asks for it
#else
  return xanatoo::build_thumb_file(xanatoo::remote_image_path(url),
                                   xanatoo::kRemoteThumbEdge);
#endif
}

void image_worker(std::shared_ptr<ImageFetchShared> sh) {
  for (;;) {
    std::string url;
    std::shared_ptr<xanatoo::TransferMeter> meter;
    {
      std::lock_guard<std::mutex> lock(sh->mutex);
      if (sh->queue.empty()) {
        --sh->workers;
        return;
      }
      url = std::move(sh->queue.front());
      sh->queue.pop_front();
      meter = std::make_shared<xanatoo::TransferMeter>();
      sh->active[url] = meter;
    }
    bool ok = false;
    bool page = false;
    {
      xanatoo::TransferMeterScope scope(meter.get());
      try {
        ok = download_image(url, page);
      } catch (const std::exception& ex) {
        std::cerr << "image: " << url << ": " << ex.what() << "\n";
      } catch (...) {
        // A decoder error must not escape this detached worker thread.
        std::cerr << "image: " << url << ": unexpected decoder exception\n";
      }
    }
    std::lock_guard<std::mutex> lock(sh->mutex);
    sh->active.erase(url);
    if (page) sh->pages.push_back(url);
    sh->done.emplace_back(std::move(url), ok);
  }
}
#endif

}  // namespace

void want_image_now(App& app, const std::string& url) {
  app.images_on_screen.push_back(url);
}

void poll_images(App& app) {
  std::vector<std::pair<std::string, bool>> done;
  std::vector<std::string> pages;  // "images" that were web pages
#ifdef __EMSCRIPTEN__
  // No network or pthreads in the offline web edition. Complete queued
  // remote requests as unavailable without scheduling any workers.
  for (std::string& url : app.images.take_requests())
    done.emplace_back(std::move(url), false);
  app.images_on_screen.clear();
#else
  ImageFetchShared& sh = *app.image_fetch;
  {
    std::lock_guard<std::mutex> lock(sh.mutex);
    for (std::string& url : app.images.take_requests())
      sh.queue.push_back(std::move(url));
    // What is on screen goes first (in draw order); the rest keeps its
    // document order behind it.
    std::vector<std::string>& seen = app.images_on_screen;
    auto front = sh.queue.begin();
    for (const std::string& url : seen) {
      auto it = std::find(front, sh.queue.end(), url);
      if (it == sh.queue.end()) continue;
      std::rotate(front, it, it + 1);
      ++front;
    }
    seen.clear();
    while (sh.workers < kMaxWorkers &&
           static_cast<size_t>(sh.workers) < sh.queue.size()) {
      ++sh.workers;
      std::thread(image_worker, app.image_fetch).detach();
    }
    done.swap(sh.done);
    pages.swap(sh.pages);
  }
#endif
  const bool thumbs = app.images.poll();
  for (const auto& [url, ok] : done) app.images.remote_done(url, ok);
  for (const std::string& url : pages) gallery_image_was_page(app, url);
  if (!done.empty() || thumbs) app.image_relayout_due = true;
  if (!app.image_relayout_due) return;
  const Uint32 now = xanatoo::sdl::ticks();
  if (now - app.image_relayout_at < kImageRelayoutMs) return;
  app.image_relayout_due = false;
  app.image_relayout_at = now;
  // Placeholders became images (or vanished): heights changed.
  keep_views(app);
  for (Pane& p : app.panes) p.height_cache.clear();
  app.needs_layout = true;
  // The Context panel's own layout too: a preview image laid out as a
  // placeholder would otherwise keep the placeholder's aspect ratio.
  app.context_needs_layout = true;
}

ImageLoadState image_load_state(App& app, const std::string& url) {
  ImageLoadState st;
#ifndef __EMSCRIPTEN__
  ImageFetchShared& sh = *app.image_fetch;
  std::lock_guard<std::mutex> lock(sh.mutex);
  if (auto it = sh.active.find(url); it != sh.active.end()) {
    st.active = true;
    st.received = it->second->received.load(std::memory_order_relaxed);
    st.expected = it->second->expected.load(std::memory_order_relaxed);
  } else {
    st.queued = std::find(sh.queue.begin(), sh.queue.end(), url) != sh.queue.end();
  }
#else
  (void)app;
  (void)url;
#endif
  return st;
}

void draw_image_progress_bar(App& app, const SDL_Rect& r,
                             const ImageLoadState& st, xanatoo::Rgba track) {
  namespace th = xanatoo::theme;
  if (r.w <= 0 || r.h <= 0) return;
  SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(app.sdl, track.r, track.g, track.b, track.a);
  xanatoo::sdl::fill_rect(app.sdl, &r);
  const th::Rgb accent = th::palette().accent;
  if (st.active && st.expected > 0 &&
      st.received <= static_cast<uint64_t>(st.expected)) {
    const int w = static_cast<int>(static_cast<double>(r.w) *
                                   static_cast<double>(st.received) /
                                   static_cast<double>(st.expected));
    if (w > 0) th::fill(app.sdl, SDL_Rect{r.x, r.y, w, r.h}, accent);
    return;
  }
  // Size unknown (queued, chunked, thumbnail building): a sweeping segment,
  // dimmer while the download has not started.
  const float t = static_cast<float>(xanatoo::sdl::ticks()) / 1000.f;
  const float phase = std::fmod(t * 0.8f, 1.f);
  const int seg_w = std::max(8, r.w / 4);
  const int seg_x = r.x - seg_w + static_cast<int>(phase * (r.w + seg_w));
  const int x0 = std::max(r.x, seg_x);
  const int x1 = std::min(r.x + r.w, seg_x + seg_w);
  if (x1 > x0)
    th::fill(app.sdl, SDL_Rect{x0, r.y, x1 - x0, r.h}, accent,
             st.queued ? 110 : 255);
}

void draw_image_loading(App& app, int pi, const SDL_Rect& clip,
                        float draw_scroll) {
  Pane& pane = app.panes[pi];
  xanatoo::Font& font = app.ui_font;
  const int bar_h = std::max(3, static_cast<int>(4.f * app.ui_scale));
  const int pad = std::max(4, static_cast<int>(8.f * app.ui_scale));
  xanatoo::sdl::set_clip_rect(app.sdl, &clip);
  for (const PositionedImage& im : pane.layout.images) {
    if (im.page_w > 0) continue;  // fixed-layout pages: tiles of their own
    const SDL_Rect r{pane.x + static_cast<int>(im.x),
                     static_cast<int>(std::floor(im.y - draw_scroll)),
                     static_cast<int>(im.width), static_cast<int>(im.height)};
    if (r.w <= 0 || r.h <= 0 || r.y >= clip.y + clip.h || r.y + r.h <= clip.y ||
        r.x >= clip.x + clip.w || r.x + r.w <= clip.x)
      continue;
    // The same lookup the raster drew the placeholder from (cached).
    const xanatoo::Image& img = im.thumb > 0
                                    ? app.images.thumb(im.src, im.base, im.thumb)
                                    : app.images.get(im.src, im.base);
    if (img.ok && img.texture) continue;
    const bool remote = xanatoo::is_remote_image(im.src);
    if (img.pending && remote) want_image_now(app, im.src);
    const ImageLoadState st =
        img.pending && remote ? image_load_state(app, im.src) : ImageLoadState{};
    // Tiny (inline icons): a bar along the bottom edge, no words.
    if (r.w < 48 || r.h < font.line_height() + bar_h + 2 * pad) {
      if (img.pending)
        draw_image_progress_bar(app, SDL_Rect{r.x, r.y + r.h - 2, r.w, 2}, st,
                                xanatoo::Rgba{200, 200, 210, 255});
      continue;
    }
    std::string label;
    if (!img.pending) {
      label = "Cannot load image";
    } else if (!remote) {
      label = "Preparing image…";
    } else if (st.active) {
      label = "Loading image…";
      const std::string bytes = xanatoo::transfer_label(
          st.received, st.expected, 0);  // no ETA: rate unknown here
      if (!bytes.empty()) label += "  " + bytes;
    } else {
      label = st.queued ? "Waiting to load image…" : "Loading image…";
    }
    const int bw = std::min(r.w - 2 * pad, static_cast<int>(240.f * app.ui_scale));
    const int line = font.line_height();
    const int block_h = line + (img.pending ? pad / 2 + bar_h : 0);
    const int top = r.y + (r.h - block_h) / 2;
    label = fit_text(font, label, r.w - 2 * pad);
    const int lw = font.measure(label);
    const uint8_t grey = img.pending ? 90 : 150;
    app.renderer.draw_text(font, static_cast<float>(r.x + (r.w - lw) / 2),
                           static_cast<float>(top + font.ascender()), label,
                           grey, grey, static_cast<uint8_t>(grey + 10));
    if (img.pending)
      draw_image_progress_bar(
          app, SDL_Rect{r.x + (r.w - bw) / 2, top + line + pad / 2, bw, bar_h}, st,
          xanatoo::Rgba{200, 200, 210, 255});
  }
}

void prune_image_caches() {
#ifndef __EMSCRIPTEN__
  const std::string dir = cache_dir();
  if (dir.empty()) return;
  std::thread([dir] {
    // Downloaded URLs (images, pages; url_cache.hpp): bodies by size,
    // metadata rows by age. images/ held them before the URL cache.
    prune_cache_dir((fs::path(dir) / "urls").string(), kUrlCacheBytes);
    xanatoo::url_cache_prune_rows(kUrlCacheRowAgeMs);
    prune_cache_dir((fs::path(dir) / "images").string(), 0);
    prune_cache_dir((fs::path(dir) / "thumbs").string(), kThumbCacheBytes);
    prune_cache_dir((fs::path(dir) / "documents").string(), kDocumentCacheBytes);
  }).detach();
#endif
}

}  // namespace xanatoo::app
