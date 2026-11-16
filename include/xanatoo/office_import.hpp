// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Built-in DOCX / ODT readers for systems without pandoc. ZIP → XML → simple
// HTML → Markdown (same path as the native EPUB importer). Not pixel-perfect:
// paragraphs, headings, lists, links, emphasis, and basic tables only.

#include <optional>
#include <string>
#include <string_view>

namespace xanatoo {

bool is_docx_path(std::string_view path);
bool is_odt_path(std::string_view path);
bool is_office_path(std::string_view path);

// `zip` is the raw .docx / .odt bytes. On success returns GFM Markdown
// (optional YAML title when found). nullopt + err on failure.
std::optional<std::string> docx_to_markdown(std::string_view zip,
                                            std::string* err = nullptr);
std::optional<std::string> odt_to_markdown(std::string_view zip,
                                           std::string* err = nullptr);
std::optional<std::string> office_to_markdown(std::string_view path_hint,
                                              std::string_view zip,
                                              std::string* err = nullptr);

}  // namespace xanatoo
