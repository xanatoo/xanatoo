// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Where an HTTP/1.1 response ends, so a connection can be kept open for the
// next request instead of being read until the server closes it. Pure code
// (no sockets): https.cpp feeds it the bytes read so far.

#include <cstddef>
#include <string_view>

namespace xanatoo {

class HttpResponseFraming {
public:
  // `data`: the whole response received so far (status line onwards; it only
  // ever grows between calls). True once it holds the complete response,
  // which is then `size()` bytes long (anything after that is not part of
  // it). Responses delimited only by the connection closing never complete.
  bool update(std::string_view data);

  bool complete() const { return complete_; }
  size_t size() const { return size_; }
  // Headers seen and the body's end known without the connection closing
  // (Content-Length, chunked, or a status without a body).
  bool delimited() const { return delimited_; }
  // The server lets the connection be reused: HTTP/1.1, delimited, and no
  // "Connection: close".
  bool reusable() const { return reusable_; }
  // Malformed framing (bad Content-Length or chunk size): read to the end
  // of the connection and never reuse it.
  bool broken() const { return broken_; }

private:
  enum class Body { Unknown, Length, Chunked, UntilClose };
  Body body_ = Body::Unknown;
  size_t body_start_ = 0;
  size_t length_ = 0;
  size_t chunk_at_ = 0;  // next chunk-size line (Chunked)
  size_t size_ = 0;
  bool complete_ = false;
  bool delimited_ = false;
  bool reusable_ = false;
  bool broken_ = false;
};

}  // namespace xanatoo
