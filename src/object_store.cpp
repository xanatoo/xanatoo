// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/object_store.hpp"

#include "xanatoo/images.hpp"  // prune_cache_dir
#include "xanatoo/paths.hpp"
#include "xanatoo/util.hpp"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace xanatoo {
namespace {

std::string objects_dir() {
  const std::string cache = cache_dir();
  if (cache.empty()) return {};
  return (fs::path(cache) / "objects").string();
}

// A temporary name next to `path`, unique within the process (several
// workers may store the same object at once).
std::string temp_name(const std::string& path) {
  static const uint64_t salt = std::random_device{}();
  static std::atomic<uint64_t> counter{0};
  return path + ".part" + std::to_string(salt) + "-" +
         std::to_string(counter.fetch_add(1));
}

}  // namespace

std::string object_hash(std::string_view bytes) { return sha256_hex(bytes); }

bool is_object_hash(std::string_view hash) {
  if (hash.size() != 64) return false;
  for (char c : hash)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

std::string object_path(std::string_view hash) {
  if (!is_object_hash(hash)) return {};
  const std::string dir = objects_dir();
  if (dir.empty()) return {};
  return (fs::path(dir) / std::string(hash)).string();
}

bool object_exists(std::string_view hash) {
  const std::string p = object_path(hash);
  std::error_code ec;
  return !p.empty() && fs::is_regular_file(p, ec);
}

std::string object_put(std::string_view bytes) {
  const std::string hash = object_hash(bytes);
  const std::string path = object_path(hash);
  if (path.empty()) return {};
  std::error_code ec;
  if (fs::is_regular_file(path, ec)) return hash;
  if (!ensure_dir(objects_dir())) return {};
  const std::string tmp = temp_name(path);
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      std::cerr << "object store: cannot write " << tmp << "\n";
      fs::remove(tmp, ec);
      return {};
    }
  }
  fs::rename(tmp, path, ec);
  if (ec) {
    fs::remove(tmp, ec);
    return fs::is_regular_file(path, ec) ? hash : std::string();
  }
  return hash;
}

namespace {
// Hard link `from` as `to` (atomically replacing it), else a copy.
bool link_or_copy(const std::string& from, const std::string& to) {
  std::error_code ec;
  const std::string tmp = temp_name(to);
  fs::create_hard_link(from, tmp, ec);
  if (ec) {
    ec.clear();
    fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      fs::remove(tmp, ec);
      return false;
    }
  }
  fs::rename(tmp, to, ec);
  if (ec) {
    std::error_code ignore;
    fs::remove(tmp, ignore);
    return false;
  }
  return true;
}
}  // namespace

bool object_put_file(const std::string& file, std::string_view hash) {
  const std::string path = object_path(hash);
  if (path.empty()) return false;
  std::error_code ec;
  if (fs::is_regular_file(path, ec)) return true;
  if (!ensure_dir(objects_dir())) return false;
  return link_or_copy(file, path) || fs::is_regular_file(path, ec);
}

bool object_link_to(std::string_view hash, const std::string& file) {
  const std::string path = object_path(hash);
  std::error_code ec;
  if (path.empty() || !fs::is_regular_file(path, ec)) return false;
  fs::create_directories(fs::path(file).parent_path(), ec);
  return link_or_copy(path, file);
}

std::optional<std::string> object_read(std::string_view hash) {
  const std::string path = object_path(hash);
  if (path.empty()) return std::nullopt;
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream ss;
  ss << in.rdbuf();
  if (in.bad()) return std::nullopt;
  return std::move(ss).str();
}

void object_touch(std::string_view hash) {
  const std::string path = object_path(hash);
  if (path.empty()) return;
  std::error_code ec;
  fs::last_write_time(path, fs::file_time_type::clock::now(), ec);
}

void object_prune(uint64_t max_bytes) {
  const std::string dir = objects_dir();
  if (!dir.empty()) prune_cache_dir(dir, max_bytes);
}

}  // namespace xanatoo
