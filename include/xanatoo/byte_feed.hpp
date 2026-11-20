// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// A remote file arriving over time (a download on one thread) for readers
// on others: the built-in audio decoders and FFmpeg read from it while it
// downloads, and reads past what arrived wait. docs/dev/CONTENT.md "Media
// cache".
//
// A feed whose server answers byte ranges is `sparse`: it knows the file's
// length and which parts arrived (`have`). A read far from where the
// download writes asks for its position (`want`), the download fetches from
// there and fills the gaps it may afterwards.
//
// The bytes live in memory, or in a file of the media cache
// (feed_open_cached): a cached file keeps what arrived for the next time it
// plays, and what is complete plays without the network.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xanatoo {

struct ByteFeed {
  ByteFeed() = default;
  ByteFeed(const ByteFeed&) = delete;
  ByteFeed& operator=(const ByteFeed&) = delete;
  ~ByteFeed();  // a cached feed writes down what it has

  std::mutex m;
  std::condition_variable cv;
  // Where the bytes are, under m: `bytes` (in memory), or `file` (cached).
  std::vector<uint8_t> bytes;
  std::FILE* file = nullptr;
  std::string path;   // the cache file; its map is path + ".map"
  std::string url;    // cached: the address it is the bytes of
  bool sparse = false;
  std::vector<std::pair<int64_t, int64_t>> have;  // under m: [from, to), sorted, apart
  bool done = false;   // under m: nothing more comes
  std::string error;   // under m, once done: why it ended early ("" = all of it)
  bool answered = false;          // under m: the server answered (or the cache had it all)
  std::string content_type;       // under m, from the answer
  std::string etag, last_modified;
  int64_t mapped = 0;             // under m: `got` when the map was last written
  // Limits of the download (tests make them small).
  int64_t ahead = int64_t(64) << 20;       // at most this far ahead of the reader
  int64_t fill_whole = int64_t(128) << 20; // gaps behind the reader filled up to this size
  int64_t limit = int64_t(512) << 20;      // larger files are refused

  std::atomic<int64_t> got{0};      // bytes arrived, for progress
  std::atomic<int64_t> total{-1};   // expected (Content-Length), -1 unknown
  std::atomic<bool> cancel{false};  // nobody reads any more: stop the download
  std::atomic<int64_t> at{0};       // where the download writes next
  std::atomic<int64_t> want{-1};    // sparse: a reader waits here, far from `at`
  std::atomic<int64_t> reading{0};  // where the reader reads
};

// The media cache: a feed for `url` whose bytes go to a file in `dir`, with
// what an earlier feed of it left there. Several feeds of one address do not
// share the file: while another is alive (for up to `wait_ms`) this gives
// null, as when the file cannot be made; take a memory feed then. Older
// files go when the directory grows past `budget` bytes.
std::shared_ptr<ByteFeed> feed_open_cached(const std::string& dir, const std::string& url,
                                           int64_t budget, int wait_ms = 2000);

void feed_append(ByteFeed& f, std::string_view chunk);
// The download is ranged from now on: the file is `total` bytes long.
void feed_begin_sparse(ByteFeed& f, int64_t total);
// Bytes at `offset` (sparse; else at or before the end).
void feed_write(ByteFeed& f, int64_t offset, std::string_view chunk);
void feed_end(ByteFeed& f, std::string error = {});
// The whole file arrived.
bool feed_complete(ByteFeed& f);
// Where the bytes that arrived from `pos` on end (`pos` itself: none there).
int64_t feed_reach(ByteFeed& f, int64_t pos);
// Waits until [from, to) arrived, the feed is done, or `give_up` says so
// (checked every 50 ms; -1 then); else feed_reach(from). A byte far from
// where the download writes is `want`ed.
int64_t feed_wait(ByteFeed& f, int64_t from, int64_t to, const std::function<bool()>& give_up);
// Up to `n` bytes from `at` that arrived, into `out`; how many. Notes `at`
// as where the reader is.
size_t feed_copy(ByteFeed& f, int64_t at, void* out, size_t n);
// The bytes from the start that arrived, at most `max` of them.
std::vector<uint8_t> feed_head(ByteFeed& f, size_t max);
// A complete feed's bytes at `*data` / `*size`: in memory where they are
// (the vector given back stays empty), else read from the cache file into
// the vector given back (keep it while using them).
std::vector<uint8_t> feed_whole(ByteFeed& f, const uint8_t** data, size_t* size);
// Waits for the server's answer (feed->answered) or the end; false when
// `give_up` said so first.
bool feed_wait_answer(ByteFeed& f, const std::function<bool()>& give_up);

// What a request of feed_download got back.
struct FeedHead {
  int64_t start = 0;    // the answer begins at this byte
  int64_t total = -1;   // of a file this long (-1 unknown)
  bool ranged = false;  // a 206 answer
  std::string content_type, etag, last_modified;
};
// One request: the file from byte `from` on (asking for that range). `head`
// first, then the body in pieces to `body`; either returning false stops it.
// "" when it ended (or was stopped), else why it failed.
using FeedFetch = std::function<std::string(int64_t from,
                                            const std::function<bool(const FeedHead&)>& head,
                                            const std::function<bool(std::string_view)>& body)>;
// Downloads into `f` until it is complete, failed or cancelled, then
// feed_end. Asks for ranges from the start; with a server that answers
// them it jumps to where a reader `want`s, stays at most `ahead` bytes in
// front of the reader, fills the gaps of files up to `fill_whole`, and
// picks up a broken connection where it broke. A cached feed whose file
// changed on the server (length, ETag, Last-Modified) starts over.
void feed_download(ByteFeed& f, const FeedFetch& fetch);

}  // namespace xanatoo
