// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Locally observed Matrix catalogue for the Matrix Explorer panel: servers,
// public rooms, spaces and people seen in API responses. Thread-safe,
// bounded, stored in state_dir()/matrix-directory.txt (memory only under
// --no-save). Holds public names only, never tokens or private rooms.

#include "xanatoo/matrix.hpp"

#include <string_view>
#include <vector>

namespace xanatoo {

std::vector<MatrixDirectoryEntry> matrix_directory_list();
bool matrix_directory_add_server(std::string_view input);
void matrix_directory_merge(const std::vector<MatrixDirectoryEntry>& entries);
// matrix_directory_merge(matrix_discover_payload(json, via)).
void matrix_directory_observe(std::string_view json, std::string_view via);

}  // namespace xanatoo
