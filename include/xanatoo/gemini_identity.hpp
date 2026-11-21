// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Gemini client certificates ("identities"). A Gemini server knows a client
// by the fingerprint of the certificate it presents, so an identity is an
// account: its private key is the password. Each identity is used only
// below the URL prefixes ("scopes") the user chose for it; every other
// request goes out without a certificate. The Identity panel
// (app_identity.cpp) manages them.
//
// Store: `state_dir()/gemini-identities`, an S-expression file with the PEM
// certificate and key of each identity, owner-only (0600). No TLS or SDL
// dependency here: certificates are made and hashed by tls.hpp.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

struct GeminiIdentity {
  std::string sha256;    // certificate fingerprint (lowercase hex): the key
  std::string name;      // the user's label (also the certificate's CN)
  std::string created;   // "YYYY-MM-DD"
  std::string cert_pem;
  std::string key_pem;
  // URL prefixes "gemini://host[:port]/path" (no query or fragment).
  std::vector<std::string> scopes;
  bool saved = false;    // in the store (false: this session only)
};

// ---- scopes (pure) ----

// The canonical scope of `url` without query and fragment, host lower-case,
// default port dropped: "gemini://host/path". Empty for a non-Gemini URL.
std::string gemini_scope_of_page(std::string_view url);
// "gemini://host[:port]/": the whole server.
std::string gemini_scope_of_site(std::string_view url);
// True when the scope covers `url`: same host and port, and the URL's path
// starts with the scope's path at a segment boundary ("/a" covers "/a",
// "/a/b" and "/a?q", not "/ab"; a scope ending in "/" covers what is below).
bool gemini_scope_covers(std::string_view scope, std::string_view url);
// The identity for `url`: the one whose covering scope is longest; nullptr
// when none covers it.
const GeminiIdentity* gemini_identity_for(
    const std::vector<GeminiIdentity>& identities, std::string_view url);
// Its covering scope (the longest), or empty.
std::string gemini_identity_scope_for(const GeminiIdentity& identity,
                                      std::string_view url);

// ---- store ----
// `directory` is the state directory (empty: no store, session only).
// `persist=false` (--no-save) keeps every change of this process in memory;
// the store is only read then. All calls are thread-safe (fetch workers
// read the identity of each request).

std::vector<GeminiIdentity> gemini_identities_list(std::string_view directory,
                                                   std::string* err = nullptr);
// The identity to present for `url`, or nullopt.
std::optional<GeminiIdentity> gemini_identity_for_url(
    std::string_view directory, std::string_view url);
// Add a new identity (its fingerprint must not be known yet).
bool gemini_identity_add(std::string_view directory, bool persist,
                         GeminiIdentity identity, std::string* err = nullptr);
bool gemini_identity_remove(std::string_view directory, bool persist,
                            std::string_view sha256, std::string* err = nullptr);
bool gemini_identity_rename(std::string_view directory, bool persist,
                            std::string_view sha256, std::string_view name,
                            std::string* err = nullptr);
// Use the identity below `scope`; the same scope leaves every other identity
// (one identity per scope: this is how the user switches).
bool gemini_identity_use(std::string_view directory, bool persist,
                         std::string_view sha256, std::string_view scope,
                         std::string* err = nullptr);
// Remove one scope of one identity.
bool gemini_identity_drop_scope(std::string_view directory, bool persist,
                                std::string_view sha256, std::string_view scope,
                                std::string* err = nullptr);
// Remove every scope (of any identity) covering `url`: it goes out without
// a certificate again.
bool gemini_identity_stop_using(std::string_view directory, bool persist,
                                std::string_view url, std::string* err = nullptr);

// ---- file format (pure, for tests) ----
std::string gemini_identities_serialize(const std::vector<GeminiIdentity>& ids);
// nullopt (and `err`) for a malformed store.
std::optional<std::vector<GeminiIdentity>> gemini_identities_parse(
    std::string_view text, std::string* err = nullptr);

}  // namespace xanatoo
