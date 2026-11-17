// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Read-only Matrix client (Client-Server API, https://spec.matrix.org/).
// Addresses use the spec's own matrix: URI scheme (sigils dropped):
//   matrix:r/matrix:matrix.org              room alias #matrix:matrix.org
//   matrix:roomid/abc:example.org?via=x.org room id !abc:example.org
//   matrix:roomid/abc:x.org/e/ev            event $ev in that room
//   matrix:u/alice:example.org              user @alice:example.org
// plus one xanatoo extension for the public room directory:
//   matrix:directory/matrix.org[?since=TOKEN][&q=words]
// Anonymous requests can read room directories, room summaries and
// profiles. Room timelines and space contents need an access token for the
// configured homeserver (Credentials dialog); it is sent to that server only.

#include "xanatoo/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

enum class MatrixRefKind : uint8_t { Alias, RoomId, User, Directory };

struct MatrixRef {
  MatrixRefKind kind = MatrixRefKind::Alias;
  std::string id;        // with sigil: #a:s, !r:s / !r, @u:s; server name (Directory)
  std::string event;     // $event (optional, rooms only)
  std::vector<std::string> via;
  std::string from;      // timeline pagination token (rooms)
  std::string since;     // directory pagination token
  std::string query;     // directory search words
};

bool is_matrix_ref(std::string_view s);
std::optional<MatrixRef> parse_matrix_ref(std::string_view s);
std::string format_matrix_ref(const MatrixRef& ref);

// "#a:s" / "!r:s" / "@u:s" / "$e" ids → matrix: ref ("" for anything else).
std::string matrix_ref_for_id(std::string_view id);
// https://matrix.to/#/<id>[/<event>][?via=…] → matrix: ref ("" otherwise).
std::string matrix_ref_from_url(std::string_view url);
// matrix: ref → https://matrix.to/#/… (directory refs: "").
std::string matrix_to_url(std::string_view ref);

// Server part of a Matrix id ("" for v12 room ids, which have none).
std::string matrix_id_server(std::string_view id);
// Valid server name: hostname / IPv4 / [IPv6], optional :port; lower-cased.
// "" for anything else (so it is safe to put into a URL).
std::string matrix_server_name(std::string_view input);
// Percent-encode everything but RFC 3986 unreserved characters.
std::string matrix_escape(std::string_view s);

// Unix milliseconds → "2026-11-17T12:34:56Z".
std::string matrix_iso_time(int64_t ms);

// Documents built from API JSON, without network I/O.
// /publicRooms response.
Document matrix_directory_document(std::string_view server,
                                   std::string_view json,
                                   const MatrixRef& ref,
                                   std::string_view note = {});
// Room summary (/room_summary) plus optional /messages, /hierarchy and
// /context bodies; `note` explains what could not be shown.
struct MatrixRoomData {
  std::string room_id;
  std::string summary_json;
  std::string messages_json;   // /messages (dir=b)
  std::string context_json;    // /context/{event}
  std::string hierarchy_json;  // /hierarchy (spaces)
  std::vector<std::string> via;
  std::string note;
};
Document matrix_room_document(const MatrixRef& ref, const MatrixRoomData& data);
Document matrix_profile_document(std::string_view user_id, std::string_view json,
                                 std::string_view note = {});

// Observed directory entries in one successful API response (no I/O).
enum class MatrixEntryKind : uint8_t { Server, Room, Space, Person };
struct MatrixDirectoryEntry {
  MatrixEntryKind kind = MatrixEntryKind::Server;
  std::string name;    // server name, #alias / !room id, @user id
  std::string title;   // room name / display name
  std::string topic;   // room topic (short)
  std::string ref;     // matrix: address
  int64_t members = 0;
};
// `via` is the server a room was seen on (v12 room ids name none).
std::vector<MatrixDirectoryEntry> matrix_discover_payload(std::string_view json,
                                                          std::string_view via);

#if defined(XANATOO_MATRIX) && XANATOO_MATRIX
std::optional<Document> matrix_fetch(std::string_view url,
                                     std::string* err = nullptr);
#endif

}  // namespace xanatoo
