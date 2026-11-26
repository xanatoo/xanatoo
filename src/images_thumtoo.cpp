// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// thumtoo image backend (see image_backend.hpp). A thumbnail request runs on
// a submitter or thumtoo worker: size probe (cached), then the cache-only TileSynth
// lookup, and on a miss one tile pyramid from the scale that still covers
// kMaxThumbEdge up; every waiting edge of that file is then composed from
// the tiles. Callbacks run inline (default Executor); results
// are handed to the main thread through `done`.

#include "xanatoo/image_backend.hpp"

#include "xanatoo/paths.hpp"
#include "xanatoo/util.hpp"

#include <thumtoo/client.hpp>
#include <thumtoo/constants.hpp>
#include <thumtoo/image.hpp>
#include <thumtoo/uri.hpp>

#include <vips/vips.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <thread>

namespace fs = std::filesystem;

namespace xanatoo {

namespace {

// Largest thumbnail edge ImageCache asks for (its 128 / 256 / 512 / 1024
// steps).
constexpr int kMaxThumbEdge = 1024;
constexpr unsigned kWorkers = 2;

// sRGB + alpha, 8 bit, EXIF orientation applied; consumes `img`.
bool vips_to_rgba(VipsImage* img, std::vector<unsigned char>& rgba, int& w,
                  int& h) {
  // Replaces `img` by the operation's output (read after the call).
  VipsImage* out = nullptr;
  auto step = [&img, &out](int rc) {
    g_object_unref(img);
    img = out;
    out = nullptr;
    return rc == 0 && img;
  };
  if (!step(vips_autorot(img, &out, nullptr))) return false;
  if (!step(vips_colourspace(img, &out, VIPS_INTERPRETATION_sRGB, nullptr)))
    return false;
  if (vips_image_get_format(img) != VIPS_FORMAT_UCHAR &&
      !step(vips_cast_uchar(img, &out, nullptr)))
    return false;
  if (vips_image_get_bands(img) == 3 &&
      !step(vips_bandjoin_const1(img, &out, 255.0, nullptr)))
    return false;
  if (vips_image_get_bands(img) != 4) {
    g_object_unref(img);
    return false;
  }
  w = vips_image_get_width(img);
  h = vips_image_get_height(img);
  size_t len = 0;
  void* mem = vips_image_write_to_memory(img, &len);
  g_object_unref(img);
  if (!mem) return false;
  auto* p = static_cast<unsigned char*>(mem);
  rgba.assign(p, p + len);
  g_free(mem);
  return len == static_cast<size_t>(w) * h * 4;
}

// Shared root of the thumtoo cache: $XDG_CACHE_HOME/thumtoo (beside
// xanatoo's own cache dir, so Android keeps it app-private).
fs::path thumtoo_cache_root() {
  const std::string own = cache_dir();
  if (own.empty()) return {};
  return fs::path(own).parent_path() / "thumtoo";
}

}  // namespace

struct ImageBackend {
  struct Waiter {
    std::string key;
    int edge = 0;
  };

  std::mutex mutex;
  std::map<std::string, std::vector<Waiter>> inflight;  // uri → waiting thumbs
  std::deque<DecodedImage> done;
  // Destroy after the submitter joins, then join the workers that use the queues.
  std::unique_ptr<thumtoo::Client> client;
#ifndef __EMSCRIPTEN__
  struct Request {
    std::string key;
    std::string path;
    int edge;
  };
  std::condition_variable_any ready;
  std::deque<Request> requests;
  std::jthread submitter;  // stopped and joined before client destruction

  void submit(std::stop_token stop);
#endif
  void request_thumb(const std::string& key, const std::string& path, int edge);

  void finish(const std::string& uri) {
    std::vector<Waiter> waiters;
    {
      std::lock_guard<std::mutex> lock(mutex);
      auto it = inflight.find(uri);
      if (it == inflight.end()) return;
      waiters = std::move(it->second);
      inflight.erase(it);
    }
    std::vector<DecodedImage> out;
    for (const Waiter& wt : waiters) {
      DecodedImage d;
      d.key = wt.key;
      try {
        if (auto px = client->get_pixels_from_tiles(uri, wt.edge)) {
          if (VipsImage* img = vips_image_new_from_buffer(
                  px->bytes.data(), px->bytes.size(), nullptr, nullptr)) {
            if (!vips_to_rgba(img, d.rgba, d.w, d.h)) d.rgba.clear();
          }
        }
      } catch (...) {
        // A decoder failure invalidates this thumbnail, not the app or its
        // other waiting images. Some decoders throw non-std exceptions.
        d.rgba.clear();
      }
      if (d.rgba.empty()) std::cerr << "thumbnail: cannot load " << uri << "\n";
      out.push_back(std::move(d));
    }
    std::lock_guard<std::mutex> lock(mutex);
    for (DecodedImage& d : out) done.push_back(std::move(d));
  }

  void on_size(const std::string& uri, const thumtoo::SizeReply& reply) {
    if (!reply.size || reply.size->width <= 0 || reply.size->height <= 0) {
      finish(uri);
      return;
    }
    const int native = std::max(reply.size->width, reply.size->height);
    // Cached tiles serve only when fine enough: get_pixels_from_tiles falls
    // back to the finest stored scale, so a pyramid built for a smaller
    // largest edge (512 before) would keep big images small for good.
    if (auto px = client->get_pixels_from_tiles(uri, kMaxThumbEdge)) {
      const int need = std::min(native, kMaxThumbEdge) * 9 / 10;
      if (std::max(px->width, px->height) >= need) {
        finish(uri);
        return;
      }
    }
    // Coarsest scale whose long edge still covers the largest thumbnail.
    int min_scale = 0;
    while (thumtoo::dim_at_tile_scale(native, min_scale + 1) >=
           kMaxThumbEdge * 9 / 10)
      ++min_scale;
    client->request_tile_pyramid(
        uri, min_scale, -1,
        [this](std::string u, int, int, int, std::optional<thumtoo::TileBlob>) {
          finish(u);
        });
  }
};

std::shared_ptr<ImageBackend> image_backend_open() {
  const fs::path root = thumtoo_cache_root();
  if (root.empty()) return nullptr;
  auto b = std::make_shared<ImageBackend>();
  try {
    b->client = thumtoo::Client::open(root, {}, kWorkers);
    if (!b->client) return nullptr;
    thumtoo::image_library_init();
#ifndef __EMSCRIPTEN__
    b->submitter = std::jthread([backend = b.get()](std::stop_token stop) {
      backend->submit(stop);
    });
#endif
    return b;
  } catch (const std::exception& ex) {
    std::cerr << "thumtoo cache " << root.string() << ": " << ex.what()
              << " (images without it)\n";
  } catch (...) {
    std::cerr << "thumtoo cache " << root.string()
              << ": unexpected decoder exception (images without it)\n";
  }
  return nullptr;
}

bool image_backend_decode(const std::string& path, long long max_pixels,
                          std::vector<unsigned char>& rgba, int& w, int& h) {
  thumtoo::image_library_init();
  VipsImage* img = vips_image_new_from_file(path.c_str(), "access",
                                            VIPS_ACCESS_SEQUENTIAL, nullptr);
  if (!img) {
    std::cerr << "image load failed: " << path << " (" << vips_error_buffer()
              << ")\n";
    vips_error_clear();
    return false;
  }
  if (static_cast<long long>(vips_image_get_width(img)) *
          vips_image_get_height(img) > max_pixels) {
    std::cerr << "image too large: " << path << "\n";
    g_object_unref(img);
    return false;
  }
  return vips_to_rgba(img, rgba, w, h);
}

void ImageBackend::request_thumb(const std::string& key,
                                 const std::string& path, int edge) {
  std::string uri;
  try {
    uri = thumtoo::file_uri_from_path(fs::absolute(path));
    {
      std::lock_guard<std::mutex> lock(mutex);
      auto [it, fresh] = inflight.try_emplace(uri);
      it->second.push_back({key, std::min(edge, kMaxThumbEdge)});
      if (!fresh) return;  // that file's pyramid is already on its way
    }
    client->request_size(uri, [this](std::string u, thumtoo::SizeReply r) {
      try {
        on_size(u, r);
      } catch (...) {
        finish(u);  // complete as failed rather than wait indefinitely
      }
    });
  } catch (...) {
    // A request can fail before it reaches the client's queue. Retire all
    // waiters explicitly, including when URI construction itself failed.
    std::lock_guard<std::mutex> lock(mutex);
    auto it = inflight.find(uri);
    if (it != inflight.end()) {
      for (const ImageBackend::Waiter& wt : it->second) {
        DecodedImage d;
        d.key = wt.key;
        done.push_back(std::move(d));
      }
      inflight.erase(it);
    } else {
      DecodedImage d;
      d.key = key;
      done.push_back(std::move(d));
    }
  }
}

#ifndef __EMSCRIPTEN__
void ImageBackend::submit(std::stop_token stop) {
  while (!stop.stop_requested()) {
    Request request;
    {
      std::unique_lock lock(mutex);
      ready.wait(lock, stop, [this] { return !requests.empty(); });
      if (stop.stop_requested()) return;
      request = std::move(requests.front());
      requests.pop_front();
    }
    // request_size can call back inline on a cache hit. Both the lookup
    // and any tile composition and decoding it triggers belong here.
    request_thumb(request.key, request.path, request.edge);
  }
}
#endif

void image_backend_request_thumb(ImageBackend& b, const std::string& key,
                                 const std::string& path, int edge) {
#ifndef __EMSCRIPTEN__
  {
    std::lock_guard lock(b.mutex);
    b.requests.push_back({key, path, edge});
  }
  b.ready.notify_one();
#else
  b.request_thumb(key, path, edge);
#endif
}

std::vector<DecodedImage> image_backend_take_done(ImageBackend& b,
                                                 size_t limit) {
  std::lock_guard lock(b.mutex);
  const size_t count = std::min(limit, b.done.size());
  std::vector<DecodedImage> out;
  out.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    out.push_back(std::move(b.done.front()));
    b.done.pop_front();
  }
  return out;
}

std::string image_backend_vips_version() { return vips_version_string(); }

std::vector<std::string> image_backend_suffixes() {
  std::set<std::string> suffs;
  if (VIPS_INIT("xanatoo") == 0) {  // no-op once thumtoo started it
    vips_foreign_map(
        "VipsForeignLoad",
        [](void* cls, void* a, void*) -> void* {
          auto& out = *static_cast<std::set<std::string>*>(a);
          if (const char** s = static_cast<VipsForeignClass*>(cls)->suffs)
            for (; *s; ++s) out.insert(ascii_lower(std::string_view(*s)));
          return nullptr;
        },
        &suffs, nullptr);
  }
  return {suffs.begin(), suffs.end()};
}

}  // namespace xanatoo
