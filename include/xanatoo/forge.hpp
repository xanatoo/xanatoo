// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/service_bar.hpp"
#include "xanatoo/types.hpp"

namespace xanatoo {

enum class ForgeView {
  Home, Search, About, Tree, Blob, Raw, Releases, Tags,
  Issues, Changes, Issue, Change, Reviews, ReviewComments
};

struct ForgeRef {
  bool github = false;
  ForgeView view = ForgeView::Home;
  std::string project;
  std::string query;
  std::string path;      // decoded repository path
  std::string revision;  // empty = default branch
  std::string number;
  int page = 1;
  std::string after, before;  // GitLab discussion cursors
};

struct GitFile {
  std::string url;
  std::string filename;
};

bool is_git_ref(std::string_view url);
std::optional<GitFile> parse_git_ref(std::string_view url);

bool is_forge_ref(std::string_view url);
std::optional<ForgeRef> parse_forge_ref(std::string_view url);
std::string forge_ref_string(const ForgeRef& ref);
std::string forge_web_url(const ForgeRef& ref);
std::string forge_ref_from_web(std::string_view url);
std::string forge_up_url(ForgeRef ref);
std::optional<ServiceBar> forge_service_bar(std::string_view url);
std::string forge_raw_url(const ForgeRef& ref);
std::string forge_filename(const ForgeRef& ref);

#if defined(XANATOO_HTTPS) && XANATOO_HTTPS
std::optional<Document> forge_fetch(const ForgeRef& ref, std::string* err);
#endif

}  // namespace xanatoo
