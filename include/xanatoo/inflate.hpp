// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Small DEFLATE decoder (RFC 1951) with the gzip (RFC 1952) and zlib
// (RFC 1950) wrappers, after zlib's "puff": no dependency, so it builds the
// same on desktop and Android. Used for *.xml.gz sitemaps and HTTP
// Content-Encoding.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace xanatoo {

// Raw DEFLATE stream; `consumed` (optional) gets the bytes read. nullopt on
// corrupt input or when the output would exceed `max_out`.
std::optional<std::string> inflate_raw(std::string_view in,
                                       size_t max_out = 64u << 20,
                                       size_t* consumed = nullptr);

// gzip member (1f 8b …); the CRC is checked, the size modulo 2^32.
std::optional<std::string> gunzip(std::string_view in,
                                  size_t max_out = 64u << 20);

// zlib stream (78 xx …); the Adler-32 is checked.
std::optional<std::string> zlib_inflate(std::string_view in,
                                        size_t max_out = 64u << 20);

// Undo an HTTP Content-Encoding header value: "gzip" / "x-gzip" / "deflate"
// (zlib or raw); a list ("gzip, deflate") is undone last-first. nullopt when
// there is nothing to undo, the coding is unknown (br, zstd) or the body does
// not decode: the caller then keeps the body as it came. Some servers (the
// Wayback Machine's id_ captures) send gzip even to "Accept-Encoding:
// identity", and others label plain bodies gzip; both cases come out right.
std::optional<std::string> decode_content_encoding(std::string_view coding,
                                                   std::string_view body,
                                                   size_t max_out = 64u << 20);

inline bool is_gzip(std::string_view in) {
  return in.size() >= 2 && static_cast<unsigned char>(in[0]) == 0x1f &&
         static_cast<unsigned char>(in[1]) == 0x8b;
}

}  // namespace xanatoo
