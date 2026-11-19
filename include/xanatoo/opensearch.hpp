// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Search engines a web site offers through OpenSearch (search.hpp engines
// added by the user from the page; docs/SEARCH.md).

#include <optional>
#include <string>
#include <string_view>

#include "xanatoo/search.hpp"

namespace xanatoo {

// An OpenSearch description (the XML a page links with <link rel="search"
// type="application/opensearchdescription+xml">) as an engine: ShortName,
// the GET <Url type="text/html"> template (an RSS / Atom one when there is
// no HTML one: kind feed) with {searchTerms} → %s, optional {x?} dropped,
// <Param> children appended. Searches the description's host (`sites`).
// nullopt with `err` when it has no usable template.
std::optional<SearchEngine> parse_opensearch(std::string_view xml,
                                             std::string_view description_url,
                                             std::string* err = nullptr);
// "search.example.org" → "example-org" (an engine id).
std::string engine_id_for_host(std::string_view host);
// One (engine …) form for search.sexp, readable by parse_engines.
std::string engine_sexp(const SearchEngine& e);
// Appends `e` to user_data_dir()/search.sexp (replacing an entry of the same
// id); false with `err` when there is no place to write.
bool save_user_engine(const SearchEngine& e, std::string* err = nullptr);

#if defined(XANATOO_WEB) && XANATOO_WEB
// Fetches the description at `url` and reads it (blocking: worker thread).
std::optional<SearchEngine> fetch_opensearch(std::string_view url,
                                             std::string* err = nullptr);
#endif

}  // namespace xanatoo
