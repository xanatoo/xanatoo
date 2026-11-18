// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Shared user state in state_dir()/xanatoo.sqlite (docs/HISTORY.md "A"):
// bookmarks and the minibuffer / location-bar input histories, later also
// browsing history. Every change is a single-row write, so several xanatoo
// processes add and remove entries without overwriting each other (the old
// text files were rewritten whole, last writer wins).
//
// The old text files are imported once (state_dir()/bookmarks,
// minibuf-<name>.history) and then left alone, except that the bookmarks
// file is rewritten from the database after each change as a readable
// export. Without SQLite, or when the database cannot be opened, every
// call falls back to those text files.

#include "xanatoo/util.hpp"  // Bookmark

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

// Use `db_path` instead of the default database and `text_dir` instead of
// state_dir() for the text files (tests). Empty = defaults. Closes an
// open database.
void statedb_use_path(std::string db_path, std::string text_dir = {});

// True when another connection (another process) has changed the database
// since the previous call. Cheap (one PRAGMA); false without a database.
bool statedb_changed_elsewhere();

// Bookmarks in the order they were added.
std::vector<Bookmark> stored_bookmarks();
// Adds `b` (its address must not be stored yet) / removes the bookmark
// stored under exactly `address`. `all` is the in-memory list after the
// change, written to the text file when there is no database. False on
// I/O error.
bool store_bookmark_add(const Bookmark& b, const std::vector<Bookmark>& all);
bool store_bookmark_remove(std::string_view address,
                           const std::vector<Bookmark>& all);

// Input history ring `name` ("url", "path"), oldest first, at most
// `max_entries`.
std::vector<std::string> stored_input_history(std::string_view name,
                                              size_t max_entries = 200);
// Moves `entry` to the newest end of ring `name` (in memory in `hist` and
// in the store), keeping at most `max_entries`.
void record_input_history(std::string_view name, std::vector<std::string>& hist,
                          std::string_view entry, size_t max_entries = 200);

}  // namespace xanatoo
