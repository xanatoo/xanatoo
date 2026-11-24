// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include "xanatoo/types.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

// Detect Nostr identifiers / URLs that xanatoo can open:
//   note1…  nevent1…  nostr:note1…  nostr:nevent1…
//   nostr:<64-hex>  (raw event id)
//   npub1…  nprofile1…  nostr:npub1…  (an author's recent notes)
//   nostr:recent  (live kind-1 sample from public relays)
//   nostr:groups, nostr:groups@wss://relay.example (NIP-29 discovery)
//   nostr:group@wss://relay.example#<hex encoded group ID> (group timeline)
bool is_nostr_ref(std::string_view s);

// Internal, lossless relay-scoped group address (not an official NIP-19 code).
// Never use a bare group ID as a globally unique identity.
std::string nostr_group_ref(std::string_view relay, std::string_view group_id);

// Decode NIP-19 note1/nevent1 (or 64-hex) to a 64-char lowercase hex event id.
// Optionally collect relay hints from nevent TLVs into *relays.
std::optional<std::string> nostr_decode_event_id(std::string_view ref,
                                                 std::vector<std::string>* relays = nullptr);

// Decode NIP-19 npub1/nprofile1 to a 64-char lowercase hex pubkey.
// Optionally collect relay hints from nprofile TLVs into *relays.
std::optional<std::string> nostr_decode_pubkey(std::string_view ref,
                                               std::vector<std::string>* relays = nullptr);

// Bech32-encode a 64-hex id with the given prefix ("npub", "note");
// empty if hex is not 64 hex digits.
std::string nostr_bech32(std::string_view hrp, std::string_view hex);

// The "tags" array of an event's JSON: one string vector per tag.
std::vector<std::vector<std::string>> nostr_event_tags(std::string_view event_json);

// Note text → blocks under `parent`. Blank lines split paragraphs; http(s)
// URLs, nostr:npub/nprofile/note/nevent refs and NIP-08 "#[n]" become Link
// spans (mentions as "@name" from `names`, hex pubkey → display name).
// Media URLs (NIP-92 imeta "m" type, else file extension) read "[image]" /
// "[video]" / "[audio]"; images also get an Image node after their paragraph.
void nostr_append_content(Document& doc, NodeId parent, std::string_view content,
                          const std::vector<std::vector<std::string>>& tags,
                          const std::map<std::string, std::string>& names);

// Hex pubkeys of the nostr:npub1… / nostr:nprofile1… mentions in a note.
std::vector<std::string> nostr_mentioned_pubkeys(std::string_view content);

// NIP-10 tags for a kind-1 reply to the event (`parent_id`, by
// `parent_pubkey`, with `parent_tags`): marked "root" / "reply" e tags (the
// root from the parent's own root tag, else its first e tag, else the parent
// itself) and p tags for the parent's author and everyone it tagged, without
// `self`. `relay_hint` is where the parent was seen (may be empty).
std::vector<std::vector<std::string>> nostr_reply_tags(
    std::string_view parent_id, std::string_view parent_pubkey,
    const std::vector<std::vector<std::string>>& parent_tags,
    std::string_view relay_hint = {}, std::string_view self = {});

// What one relay said to a published event (nostr_publish).
struct NostrPublishResult {
  std::string relay;
  bool accepted = false;
  std::string message;  // the relay's reason, or the connection error
};

#if defined(XANATOO_NOSTR) && XANATOO_NOSTR
// Fetch a root note and kind-1 replies from public relays (blocking, short
// timeout), or for npub/nprofile refs the author's profile and recent notes.
// Builds a Document with Thread + Message nodes for the existing layout path;
// author names link to nostr:npub1…, dates to nostr:note1….
// On failure returns nullopt and sets *err.
std::optional<Document> nostr_fetch_thread(std::string_view ref,
                                           std::string* err = nullptr);

// Send a signed event to relays (default: nostr_publish_relays()) in
// parallel and collect their NIP-01 OK answers (blocking, ~10 s at most).
// An accepted event is also cached locally.
std::vector<std::string> nostr_publish_relays();
std::vector<NostrPublishResult> nostr_publish(std::string_view event_json,
                                              std::vector<std::string> relays = {});

// A kind-1 reply to event `parent_id` as `secret_hex` (blocking): finds the
// parent in the cache or on relays, tags it (nostr_reply_tags plus the
// text's mentions), signs and publishes. Empty results with *err set when
// it never got as far as the relays; *event_id gets the reply's id.
std::vector<NostrPublishResult> nostr_reply(std::string_view parent_id,
                                            std::string_view text,
                                            const std::string& secret_hex,
                                            std::string* event_id,
                                            std::string* err);
#endif

}  // namespace xanatoo
