// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Content-addressed bytes under cache_dir()/objects/<sha256 hex>: page
// snapshots, their documents and the pictures they showed
// (snapshot.hpp, docs/HISTORY.md "E"). An object is never changed once
// written; it only goes when the cache is pruned (by size, least recently
// used first), so every reader must cope with a missing object.
//
// The URL cache (url_cache.hpp) hard-links each body it writes in here,
// so a picture kept by a snapshot costs no second copy, and a newer answer
// for the same URL (written as a new file) leaves the old object intact.
// Safe from any thread (plain files, written atomically).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>

namespace xanatoo {

// sha256 of `bytes` as 64 lowercase hex digits: an object's id.
std::string object_hash(std::string_view bytes);
// True for 64 lowercase hex digits.
bool is_object_hash(std::string_view hash);

// cache_dir()/objects/<hash>; empty without a cache directory or for a
// malformed hash. The file may not exist.
std::string object_path(std::string_view hash);
bool object_exists(std::string_view hash);

// Store `bytes` (a no-op when the object is there). Returns its hash, or
// empty when nothing could be written.
std::string object_put(std::string_view bytes);
// Store the file `file`, whose bytes hash to `hash`, by hard link (a copy
// where links do not work). False when it is not stored afterwards.
bool object_put_file(const std::string& file, std::string_view hash);
// Make `file` the object `hash` (hard link, else copy), replacing what is
// there: a cache file restored from a snapshot's copy.
bool object_link_to(std::string_view hash, const std::string& file);

std::optional<std::string> object_read(std::string_view hash);

// Marks the object as used now (pruning keeps recently used ones).
void object_touch(std::string_view hash);

// Everything under objects/ beyond `max_bytes`, least recently used first;
// the objects in `keep` (pinned, snapshot_pinned_objects) stay and do not
// count. For a background thread at start-up.
void object_prune(uint64_t max_bytes,
                  const std::unordered_set<std::string>& keep = {});

}  // namespace xanatoo
