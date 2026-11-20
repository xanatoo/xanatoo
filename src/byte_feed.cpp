// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// ByteFeed: a remote file arriving over time, in memory or in the media
// cache (byte_feed.hpp; docs/dev/CONTENT.md "Media cache").

#include "xanatoo/byte_feed.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>

#include "xanatoo/paths.hpp"
#include "xanatoo/util.hpp"

namespace fs = std::filesystem;

namespace xanatoo {

namespace {

// A read waits for a byte at most this far behind where the download
// writes; farther, the download jumps there.
constexpr int64_t kFeedJump = 256 * 1024;
// A cached feed writes its map down after this many new bytes.
constexpr int64_t kMapEvery = int64_t(8) << 20;

int seek_file(std::FILE* f, int64_t at) {
#ifdef _WIN32
  return _fseeki64(f, at, SEEK_SET);
#else
  return fseeko(f, static_cast<off_t>(at), SEEK_SET);
#endif
}

// Where the bytes that arrived from `pos` on end. Under f.m.
int64_t reach_locked(const ByteFeed& f, int64_t pos) {
  for (const auto& [from, to] : f.have)
    if (from <= pos && pos < to) return to;
  return pos;
}

// The first missing byte at or after `pos` of a sparse feed, -1 when none is.
// Under f.m.
int64_t gap_locked(const ByteFeed& f, int64_t pos) {
  if (!f.sparse) return -1;
  const int64_t g = reach_locked(f, std::max<int64_t>(pos, 0));
  return g < f.total.load() ? g : -1;
}

// [from, to) arrived. Under f.m.
void add_extent(ByteFeed& f, int64_t from, int64_t to) {
  std::vector<std::pair<int64_t, int64_t>> have;
  have.reserve(f.have.size() + 1);
  bool placed = false;
  for (const auto& e : f.have) {
    if (e.second < from) {
      have.push_back(e);
    } else if (e.first > to) {
      if (!placed) have.emplace_back(from, to), placed = true;
      have.push_back(e);
    } else {
      from = std::min(from, e.first);
      to = std::max(to, e.second);
    }
  }
  if (!placed) have.emplace_back(from, to);
  f.have = std::move(have);
  int64_t sum = 0;
  for (const auto& e : f.have) sum += e.second - e.first;
  f.got.store(sum, std::memory_order_relaxed);
}

bool store_write(ByteFeed& f, int64_t at, const char* p, size_t n) {
  if (f.file) return seek_file(f.file, at) == 0 && std::fwrite(p, 1, n, f.file) == n;
  if (static_cast<uint64_t>(at) + n > f.bytes.size()) f.bytes.resize(size_t(at) + n);
  std::memcpy(f.bytes.data() + at, p, n);
  return true;
}

size_t store_read(ByteFeed& f, int64_t at, void* out, size_t n) {
  if (!n) return 0;
  if (f.file) return seek_file(f.file, at) == 0 ? std::fread(out, 1, n, f.file) : 0;
  if (at < 0 || static_cast<uint64_t>(at) >= f.bytes.size()) return 0;
  n = std::min(n, f.bytes.size() - size_t(at));
  std::memcpy(out, f.bytes.data() + at, n);
  return n;
}

std::string one_line(std::string s) {
  std::erase_if(s, [](char c) { return c == '\r' || c == '\n'; });
  return s;
}

std::string map_path(const std::string& path) { return path + ".map"; }

// What a cached feed has, for the next feed of the address. Under f.m.
void write_map_locked(ByteFeed& f) {
  if (!f.file) return;
  std::fflush(f.file);  // the bytes before the map that claims them
  std::string s = "xanatoo-media-cache 1\nurl " + one_line(f.url) +
                  "\ntotal " + std::to_string(f.sparse ? f.total.load() : -1) +
                  "\ntype " + one_line(f.content_type) + "\netag " + one_line(f.etag) +
                  "\nmodified " + one_line(f.last_modified) + "\n";
  if (f.sparse)
    for (const auto& [from, to] : f.have)
      s += "have " + std::to_string(from) + " " + std::to_string(to) + "\n";
  write_file_atomic(map_path(f.path), s);
  f.mapped = f.got.load();
}

// An earlier feed's map of `url` at `path` into `f` (sparse, its extents);
// false when there is none for it (or it does not fit the file).
bool read_map(ByteFeed& f, const std::string& url) {
  std::ifstream in(map_path(f.path), std::ios::binary);
  std::string line;
  if (!in || !std::getline(in, line) || line != "xanatoo-media-cache 1") return false;
  std::map<std::string, std::string> kv;
  std::vector<std::pair<int64_t, int64_t>> have;
  while (std::getline(in, line)) {
    const size_t sp = line.find(' ');
    const std::string key = line.substr(0, sp);
    const std::string value = sp == std::string::npos ? std::string() : line.substr(sp + 1);
    if (key == "have") {
      long long a = -1, b = -1;
      if (std::sscanf(value.c_str(), "%lld %lld", &a, &b) != 2 || a < 0 || b <= a) return false;
      have.emplace_back(a, b);
    } else {
      kv[key] = value;
    }
  }
  if (kv["url"] != url) return false;
  const long long total = std::atoll(kv["total"].c_str());
  if (total <= 0) return false;
  std::error_code ec;
  const auto size = fs::file_size(f.path, ec);
  for (const auto& [a, b] : have)
    if (b > total || ec || int64_t(size) < b) return false;
  f.sparse = true;
  f.total.store(total);
  f.content_type = kv["type"];
  f.etag = kv["etag"];
  f.last_modified = kv["modified"];
  f.have.clear();
  for (const auto& [a, b] : have) add_extent(f, a, b);
  f.mapped = f.got.load();
  return true;
}

// The live cached feeds by file, so two do not write one file.
std::mutex g_live_m;
std::map<std::string, std::weak_ptr<ByteFeed>> g_live;

bool live_locked(const std::string& path) {
  const auto it = g_live.find(path);
  return it != g_live.end() && !it->second.expired();
}

// Removes the least recently used cache files of `dir` past `budget` bytes,
// never a live one.
void prune(const std::string& dir, int64_t budget) {
  struct Entry {
    fs::file_time_type when;
    int64_t size;
    std::string path;
  };
  std::vector<Entry> entries;
  int64_t sum = 0;
  std::error_code ec;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    std::error_code ec2;
    if (!e.is_regular_file(ec2) || e.path().has_extension()) continue;
    const std::string p = e.path().string();
    int64_t size = int64_t(e.file_size(ec2));
    size += int64_t(fs::file_size(map_path(p), ec2));
    entries.push_back({e.last_write_time(ec2), size, p});
    sum += size;
  }
  std::sort(entries.begin(), entries.end(),
            [](const Entry& a, const Entry& b) { return a.when < b.when; });
  std::lock_guard lock(g_live_m);
  for (const Entry& e : entries) {
    if (sum <= budget) break;
    if (live_locked(e.path)) continue;
    std::error_code ec2;
    fs::remove(e.path, ec2);
    fs::remove(map_path(e.path), ec2);
    sum -= e.size;
  }
}

}  // namespace

ByteFeed::~ByteFeed() {
  if (!file) return;
  if (have.empty()) {  // nothing to keep
    std::fclose(file);
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove(map_path(path), ec);
    return;
  }
  write_map_locked(*this);
  std::fclose(file);
}

std::shared_ptr<ByteFeed> feed_open_cached(const std::string& dir, const std::string& url,
                                           int64_t budget, int wait_ms) {
  if (dir.empty() || !ensure_dir(dir)) return nullptr;
  // FNV-1a of the address: stable across builds, unlike std::hash.
  uint64_t h = 1469598103934665603ull;
  for (unsigned char c : url) {
    h ^= c;
    h *= 1099511628211ull;
  }
  char name[32];
  std::snprintf(name, sizeof name, "%016llx", static_cast<unsigned long long>(h));
  const std::string path = (fs::path(dir) / name).string();
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
  std::shared_ptr<ByteFeed> f;
  for (;;) {
    {
      std::lock_guard lock(g_live_m);
      std::erase_if(g_live, [](const auto& e) { return e.second.expired(); });
      if (!live_locked(path)) {
        f = std::make_shared<ByteFeed>();
        f->path = path;
        f->url = url;
        if (read_map(*f, url)) f->file = std::fopen(path.c_str(), "r+b");
        if (!f->file) {
          f = std::make_shared<ByteFeed>();  // start over
          f->path = path;
          f->url = url;
          std::error_code ec;
          fs::remove(map_path(path), ec);
          f->file = std::fopen(path.c_str(), "w+b");
        }
        if (!f->file) return nullptr;
        g_live[path] = f;
        break;
      }
    }
    if (std::chrono::steady_clock::now() >= until) return nullptr;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  std::error_code ec;
  fs::last_write_time(path, fs::file_time_type::clock::now(), ec);  // recently used
  prune(dir, budget);
  return f;
}

void feed_append(ByteFeed& f, std::string_view chunk) {
  int64_t end;
  {
    std::lock_guard lock(f.m);
    end = f.sparse ? f.at.load() : reach_locked(f, 0);
  }
  feed_write(f, end, chunk);
}

void feed_begin_sparse(ByteFeed& f, int64_t total) {
  {
    std::lock_guard lock(f.m);
    if (f.done || f.sparse || total <= 0 || reach_locked(f, 0) > total) return;
    if (!f.file) f.bytes.resize(static_cast<size_t>(total));
    f.sparse = true;
    f.total.store(total);
  }
  f.cv.notify_all();
}

void feed_write(ByteFeed& f, int64_t offset, std::string_view chunk) {
  {
    std::lock_guard lock(f.m);
    if (f.done || offset < 0 || chunk.empty()) return;
    int64_t n = static_cast<int64_t>(chunk.size());
    if (f.sparse) {
      const int64_t total = f.total.load();
      if (offset >= total) return;
      n = std::min(n, total - offset);
    } else if (offset > reach_locked(f, 0)) {
      return;  // only on from what is there
    }
    if (!store_write(f, offset, chunk.data(), size_t(n))) {
      f.done = true;
      f.error = "cannot write the media cache";
    } else {
      add_extent(f, offset, offset + n);
      f.at.store(offset + n);
      if (f.file && f.got.load() - f.mapped >= kMapEvery) write_map_locked(f);
    }
  }
  f.cv.notify_all();
}

void feed_end(ByteFeed& f, std::string error) {
  {
    std::lock_guard lock(f.m);
    if (f.done) return;
    f.done = true;
    f.error = std::move(error);
    write_map_locked(f);
  }
  f.cv.notify_all();
}

bool feed_complete(ByteFeed& f) {
  std::lock_guard lock(f.m);
  return f.done && f.error.empty();
}

int64_t feed_reach(ByteFeed& f, int64_t pos) {
  std::lock_guard lock(f.m);
  return reach_locked(f, pos);
}

int64_t feed_wait(ByteFeed& f, int64_t from, int64_t to, const std::function<bool()>& give_up) {
  std::unique_lock lock(f.m);
  for (;;) {
    const int64_t total = f.total.load();
    const int64_t end = total >= 0 ? std::min(to, total) : to;
    const int64_t gap = reach_locked(f, from);
    if (gap >= end || f.done) return gap;
    if (give_up && give_up()) return -1;
    f.reading.store(gap, std::memory_order_relaxed);  // the download follows
    if (f.sparse) {
      const int64_t at = f.at.load();
      if (gap < at || gap > at + kFeedJump) f.want.store(gap);
    }
    // Timed: the give-up flags belong to others and do not signal us.
    f.cv.wait_for(lock, std::chrono::milliseconds(50));
  }
}

size_t feed_copy(ByteFeed& f, int64_t at, void* out, size_t n) {
  std::lock_guard lock(f.m);
  f.reading.store(at, std::memory_order_relaxed);
  const int64_t there = reach_locked(f, at) - at;
  return store_read(f, at, out, std::min<size_t>(n, size_t(std::max<int64_t>(there, 0))));
}

std::vector<uint8_t> feed_head(ByteFeed& f, size_t max) {
  std::lock_guard lock(f.m);
  std::vector<uint8_t> v(std::min<size_t>(max, size_t(reach_locked(f, 0))));
  v.resize(store_read(f, 0, v.data(), v.size()));
  return v;
}

std::vector<uint8_t> feed_whole(ByteFeed& f, const uint8_t** data, size_t* size) {
  std::lock_guard lock(f.m);
  const auto n = static_cast<size_t>(reach_locked(f, 0));
  if (!f.file) {
    *data = f.bytes.data();
    *size = std::min(n, f.bytes.size());
    return {};
  }
  std::vector<uint8_t> v(n);
  v.resize(store_read(f, 0, v.data(), n));
  *data = v.data();
  *size = v.size();
  return v;
}

bool feed_wait_answer(ByteFeed& f, const std::function<bool()>& give_up) {
  std::unique_lock lock(f.m);
  while (!f.answered && !f.done) {
    if (give_up && give_up()) return false;
    f.cv.wait_for(lock, std::chrono::milliseconds(50));
  }
  return true;
}

void feed_download(ByteFeed& f, const FeedFetch& fetch) {
  constexpr int kRetries = 3;  // broken connections in a row without a byte
  constexpr int64_t kIdle = -1, kComplete = -2;
  int failures = 0;
  std::string error;
  bool checked = false;  // the first answer compared with what the cache had
  auto wanted = [&] {    // a missing byte a reader waits for, or -1
    const int64_t w = f.want.exchange(-1);
    return w >= 0 && feed_reach(f, w) == w ? w : -1;
  };
  // Where to go on: the gap a reader waits for, the next in front of the
  // reader (when it has come near), the first of a small file.
  auto next = [&]() -> int64_t {
    if (const int64_t w = wanted(); w >= 0) return w;
    std::lock_guard lock(f.m);
    const int64_t r = f.reading.load();
    const bool whole = f.total.load() <= f.fill_whole;
    if (const int64_t g = gap_locked(f, r); g >= 0 && (whole || g - r <= f.ahead / 2)) return g;
    const int64_t first = gap_locked(f, 0);
    if (first < 0) return kComplete;
    return whole ? first : kIdle;
  };
  int64_t from = 0;
  {
    std::lock_guard lock(f.m);
    if (f.sparse) {  // what the cache had: on from the first gap
      from = gap_locked(f, 0);
      if (from < 0) f.answered = true;  // all of it
    }
  }
  if (from < 0) {
    feed_end(f);
    return;
  }
  while (!f.cancel.load()) {
    bool sparse;
    {
      std::lock_guard lock(f.m);
      sparse = f.sparse;
    }
    const int64_t before = f.got.load();
    int64_t off = from, jump = -1;
    bool ranged = false, refused = false;
    std::string err = fetch(
        from,
        [&](const FeedHead& h) {
          if (h.total > f.limit) return !(refused = true);
          {
            std::lock_guard lock(f.m);
            if (!checked) {
              checked = true;
              auto differs = [](const std::string& a, const std::string& b) {
                return !a.empty() && !b.empty() && a != b;
              };
              if (f.sparse && (h.total != f.total.load() || differs(h.etag, f.etag) ||
                               differs(h.last_modified, f.last_modified))) {
                f.have.clear();  // the file changed: start over
                f.got.store(0);
                f.sparse = false;
                f.total.store(-1);
                f.bytes.clear();
                sparse = false;
              }
              f.content_type = h.content_type;
              f.etag = h.etag;
              f.last_modified = h.last_modified;
            }
          }
          if (h.ranged && h.total > 0 && !sparse) {
            feed_begin_sparse(f, h.total);
          } else if (!sparse) {
            f.total.store(h.total);
          }
          {
            // Answered once it is clear whether it is sparse (a reader decides
            // by that).
            std::lock_guard lock(f.m);
            sparse = f.sparse;
            f.answered = true;
          }
          f.cv.notify_all();
          ranged = h.ranged && sparse;
          off = ranged ? h.start : 0;  // a whole answer starts over
          f.at.store(off);
          return true;
        },
        [&](std::string_view chunk) {
          if (f.cancel.load(std::memory_order_relaxed)) return false;
          if (!ranged && off + int64_t(chunk.size()) > f.limit) return !(refused = true);
          feed_write(f, off, chunk);
          off += static_cast<int64_t>(chunk.size());
          if (!ranged) return true;
          if (const int64_t w = wanted(); w >= 0) {
            if (w < off || w > off + kFeedJump) {
              jump = w;
              return false;
            }
            f.want.store(w);  // near: comes anyway (asked again if not)
          }
          if (f.total.load() > f.fill_whole && off - f.reading.load() > f.ahead)
            return false;  // far enough ahead (on when the reader comes near)
          return feed_reach(f, off) == off;  // ran into what is there: next gap
        });
    if (f.cancel.load()) break;
    if (refused) {
      error = "file too large to play";
      break;
    }
    if (jump >= 0) {
      from = jump;
      continue;
    }
    if (!err.empty()) {
      if (!sparse) {
        error = std::move(err);
        break;
      }
      failures = f.got.load() > before ? 0 : failures + 1;
      if (failures > kRetries) {
        error = std::move(err);
        break;
      }
    } else if (!sparse) {
      break;  // all of it
    }
    while ((from = next()) == kIdle && !f.cancel.load()) {
      // Nothing to fetch until the reader moves on (or wants a byte).
      std::unique_lock lock(f.m);
      f.cv.wait_for(lock, std::chrono::milliseconds(100));
    }
    if (from == kComplete) break;
  }
  if (error.empty() && f.cancel.load()) error = "stopped";
  feed_end(f, std::move(error));
}

}  // namespace xanatoo
