// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Lossless binary form of a Document for page snapshots (snapshot.hpp,
// docs/HISTORY.md "E"): every Node and InlineSpan field, the node array in
// its order (so NodeIds, selections and history positions stay valid) and
// the metadata. Unlike the S-expression / XML writers it is not meant to
// be read or edited, only to give back exactly what was shown.
//
//   "XDOC" version(1 byte) meta root next_id node_count nodes…
//
// Integers are LEB128 varints (signed ones zigzag), strings a varint
// length + bytes, floats 4 little-endian bytes. A new field means a new
// version; decode_document refuses versions it does not know.
//
// When a field is added to Node, InlineSpan or DocumentMeta (types.hpp),
// add it here and bump kDocCodecVersion (test_doc_codec checks the fields).

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "xanatoo/types.hpp"

namespace xanatoo {

inline constexpr uint8_t kDocCodecVersion = 1;

std::string encode_document(const Document& doc);
// nullopt for anything that is not a complete encoding of this version
// (truncated, other version, ids out of range).
std::optional<Document> decode_document(std::string_view bytes);

}  // namespace xanatoo
