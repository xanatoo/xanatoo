// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/matrix_directory.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/util.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace xanatoo {
namespace {

constexpr size_t kMaxEntries = 10000;

struct MatrixCatalogue {
  std::mutex mutex;
  bool loaded = false;
  std::vector<MatrixDirectoryEntry> entries;
};

MatrixCatalogue& catalogue() {
  static MatrixCatalogue cat;
  return cat;
}

std::string directory_path() {
  const std::string dir = state_dir();
  return dir.empty() ? std::string() :
                       (std::filesystem::path(dir) / "matrix-directory.txt").string();
}

bool valid_entry(const MatrixDirectoryEntry& e) {
  if (e.name.empty() || e.name.size() > 255 || e.title.size() > 400 ||
      e.topic.size() > 600 || e.ref.size() > 1024)
    return false;
  for (const std::string* s : {&e.name, &e.title, &e.topic, &e.ref})
    if (s->find_first_of("\r\n") != std::string::npos) return false;
  auto ref = parse_matrix_ref(e.ref);
  if (!ref) return false;
  switch (e.kind) {
    case MatrixEntryKind::Server:
      return ref->kind == MatrixRefKind::Directory && ref->id == e.name &&
             matrix_server_name(e.name) == e.name;
    case MatrixEntryKind::Person:
      return ref->kind == MatrixRefKind::User;
    case MatrixEntryKind::Room:
    case MatrixEntryKind::Space:
      return ref->kind == MatrixRefKind::Alias || ref->kind == MatrixRefKind::RoomId;
  }
  return false;
}

bool merge_entry(std::vector<MatrixDirectoryEntry>& dst, const MatrixDirectoryEntry& e) {
  if (!valid_entry(e)) return false;
  for (auto& existing : dst) {
    // A room is one entry, whether it was seen as a room or a space.
    const bool room = [](MatrixEntryKind k) {
      return k == MatrixEntryKind::Room || k == MatrixEntryKind::Space;
    }(e.kind);
    const bool same_kind = existing.kind == e.kind ||
        (room && (existing.kind == MatrixEntryKind::Room ||
                  existing.kind == MatrixEntryKind::Space));
    if (!same_kind || existing.name != e.name) continue;
    bool changed = false;
    auto set = [&](auto& field, const auto& value, bool keep_if_empty) {
      if (keep_if_empty && value == std::decay_t<decltype(value)>{}) return;
      if (field != value) {
        field = value;
        changed = true;
      }
    };
    set(existing.kind, e.kind, false);
    if (e.title != e.name) set(existing.title, e.title, true);
    set(existing.topic, e.topic, true);
    set(existing.members, e.members, true);
    set(existing.ref, e.ref, true);
    return changed;
  }
  if (dst.size() >= kMaxEntries) return false;
  dst.push_back(e);
  return true;
}

MatrixDirectoryEntry make_server(const std::string& server) {
  MatrixRef ref;
  ref.kind = MatrixRefKind::Directory;
  ref.id = server;
  return {MatrixEntryKind::Server, server, server, {}, format_matrix_ref(ref), 0};
}

void ensure_loaded(MatrixCatalogue& c) {
  if (c.loaded) return;
  c.loaded = true;
  // Public homeservers with room directories; examples, not endorsements.
  constexpr const char* known[] = {
      "matrix.org", "mozilla.org", "gnome.org", "kde.org", "fedora.im",
      "opensuse.org", "fsfe.org", "tchncs.de", "envs.net", "nitro.chat",
      "gitter.im", "matrix.debian.social"};
  for (const char* server : known) merge_entry(c.entries, make_server(server));

  const std::string path = directory_path();
  if (path.empty()) return;
  std::error_code ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec || size > 8 * 1024 * 1024) return;
  std::ifstream in(path);
  if (!in) return;
  std::string line;
  while (c.entries.size() < kMaxEntries && std::getline(in, line)) {
    if (line.size() > 3000 || line.empty() || line[0] == '#') continue;
    std::istringstream s(line);
    MatrixDirectoryEntry item;
    int type = -1;
    if (!(s >> type >> std::quoted(item.name) >> std::quoted(item.title) >>
          std::quoted(item.topic) >> std::quoted(item.ref) >> item.members) ||
        type < 0 || type > 3)
      continue;
    item.kind = static_cast<MatrixEntryKind>(type);
    merge_entry(c.entries, item);
  }
}

void save_entries(const MatrixCatalogue& c) {
  if (no_save() || !ensure_dir(state_dir())) return;
  const std::string path = directory_path();
  if (path.empty()) return;
  std::ostringstream out;
  out << "# Xanatoo observed Matrix directory v1 (public names only)\n";
  for (const auto& e : c.entries)
    out << static_cast<int>(e.kind) << ' ' << std::quoted(e.name) << ' '
        << std::quoted(e.title) << ' ' << std::quoted(e.topic) << ' '
        << std::quoted(e.ref) << ' ' << e.members << '\n';
  write_file_atomic(path, out.str());
}

}  // namespace

std::vector<MatrixDirectoryEntry> matrix_directory_list() {
  auto& c = catalogue();
  std::lock_guard lock(c.mutex);
  ensure_loaded(c);
  auto entries = c.entries;
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
    if (a.kind != b.kind) return a.kind < b.kind;
    if (a.members != b.members) return a.members > b.members;
    if (a.title != b.title) return a.title < b.title;
    return a.name < b.name;
  });
  return entries;
}

bool matrix_directory_add_server(std::string_view input) {
  trim_ascii(input);
  if (starts_with_ci(input, "https://")) input.remove_prefix(8);
  input = input.substr(0, input.find('/'));
  const std::string server = matrix_server_name(input);
  if (server.empty()) return false;
  auto& c = catalogue();
  std::lock_guard lock(c.mutex);
  ensure_loaded(c);
  if (merge_entry(c.entries, make_server(server))) save_entries(c);
  return true;
}

void matrix_directory_merge(const std::vector<MatrixDirectoryEntry>& observed) {
  if (observed.empty()) return;
  auto& c = catalogue();
  std::lock_guard lock(c.mutex);
  ensure_loaded(c);
  bool changed = false;
  for (const auto& item : observed) changed = merge_entry(c.entries, item) || changed;
  if (changed) save_entries(c);
}

void matrix_directory_observe(std::string_view json, std::string_view via) {
  matrix_directory_merge(matrix_discover_payload(json, via));
}

}  // namespace xanatoo
