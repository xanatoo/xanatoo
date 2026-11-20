// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Audio file decoding (pure, no SDL): MP3, FLAC, Ogg Vorbis and WAV
// through the vendored dr_mp3 / dr_flac / dr_wav / stb_vorbis, from bytes in
// memory to interleaved float samples. The player that sends them to the
// sound device is app_audio.cpp; see docs/dev/CONTENT.md "Audio".

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace xanatoo {

enum class AudioFormat : uint8_t {
  Unknown,
  Wav,
  Mp3,
  Flac,
  Vorbis,
  Opus,  // recognised, not decoded (yet)
};

// The format of a file from its first bytes (magic numbers; MP3 by ID3 tag
// or frame sync), not its name.
AudioFormat sniff_audio(const uint8_t* data, size_t size);
const char* audio_format_name(AudioFormat f);

// An address named like an audio file we may play (.mp3 .ogg .oga .flac
// .wav .opus), by its path, ignoring query and fragment. Local paths,
// file: URLs and http(s) addresses; other schemes are not ("mailto:").
bool is_audio_url(std::string_view url);
// The same test for other names: `url` (local path, file:, http(s)) has a
// path ending in one of `exts` (".mp4"; any case).
bool url_has_extension(std::string_view url, std::initializer_list<std::string_view> exts);

// An open decoder. It keeps the bytes it decodes from.
struct AudioDecoder;
struct AudioDecoderDeleter {
  void operator()(AudioDecoder* d) const;
};
using AudioDecoderPtr = std::unique_ptr<AudioDecoder, AudioDecoderDeleter>;

// Null with `error` set when the bytes are no audio we can decode.
AudioDecoderPtr open_audio(std::vector<uint8_t> bytes, std::string* error);

AudioFormat audio_format(const AudioDecoder& d);
int audio_channels(const AudioDecoder& d);
int audio_rate(const AudioDecoder& d);
// Length in sample frames; 0 = unknown. Estimated for a streaming MP3
// without a length header (audio_streaming).
uint64_t audio_frames(const AudioDecoder& d);
// Title from the file's tags (Vorbis / FLAC comments, ID3v2 TIT2); may be "".
std::string audio_title(const AudioDecoder& d);

// Up to `frames` interleaved frames into `out` (frames × channels floats);
// returns the number decoded, fewer only at the end.
size_t audio_read(AudioDecoder& d, float* out, size_t frames);
// To sample frame `frame` (clamped to the length). False when the decoder
// cannot seek there.
bool audio_seek(AudioDecoder& d, uint64_t frame);

// "MP3 · 44100 Hz · stereo".
std::string audio_describe(const AudioDecoder& d);

// ---- streaming --------------------------------------------------------------
// Bytes arriving over time (a download on one thread) for a decoder on
// another: its reads past what arrived wait for more. Once `done` nothing is
// written any more and the bytes stay put.
//
// A feed whose server takes range requests is `sparse`: `bytes` is as long
// as the file and `have` says which parts arrived. A read far from where the
// download is writes its position to `want`, and feed_download fetches from
// there; the gaps are filled afterwards.
struct ByteFeed {
  std::mutex m;
  std::condition_variable cv;
  std::vector<uint8_t> bytes;  // under m
  bool sparse = false;         // under m
  std::vector<std::pair<int64_t, int64_t>> have;  // under m, sparse: [from, to), sorted, apart
  bool done = false;           // under m: nothing more comes
  std::string error;           // under m, once done: why it ended early ("" = all of it)
  std::atomic<int64_t> got{0};      // bytes arrived, for progress
  std::atomic<int64_t> total{-1};   // expected (Content-Length), -1 unknown
  std::atomic<bool> cancel{false};  // nobody reads any more: stop the download
  std::atomic<int64_t> at{0};       // where the download writes next
  std::atomic<int64_t> want{-1};    // sparse: a reader waits here, far from `at`
  std::atomic<int64_t> reading{0};  // where the reader reads
};
void feed_append(ByteFeed& f, std::string_view chunk);
// The download is ranged from now on: `bytes` gets the file's `total` length.
void feed_begin_sparse(ByteFeed& f, int64_t total);
// Bytes at `offset` (sparse; else only at or before the end).
void feed_write(ByteFeed& f, int64_t offset, std::string_view chunk);
void feed_end(ByteFeed& f, std::string error = {});
// The whole file arrived.
bool feed_complete(ByteFeed& f);
// Where the bytes that arrived from `pos` on end (`pos` itself: none there).
int64_t feed_reach(ByteFeed& f, int64_t pos);

// One request of feed_download: the file from byte `from` on (from > 0 asks
// for a range). `head(start, total, ranged)` first: the answer starts at
// `start` of `total` bytes (-1 unknown), `ranged` for a 206 answer; then the
// body in pieces to `body`. Either returning false stops the request. ""
// when it ended (or was stopped), else why it failed.
using FeedFetch = std::function<std::string(
    int64_t from, const std::function<bool(int64_t, int64_t, bool)>& head,
    const std::function<bool(std::string_view)>& body)>;
// Downloads into `f` until it is complete, failed or cancelled, then
// feed_end. Asks for ranges from the start; with a server that answers
// them, jumps to where the reader `want`s, fills the gaps after, and picks up
// a broken connection where it broke.
void feed_download(ByteFeed& f, const FeedFetch& fetch);

// What makes a waiting read give up: the owner's stop flag, and a counter of
// seek requests (a newer seek than audio_rearm saw abandons the read).
struct FeedWait {
  const std::atomic<bool>* cancel = nullptr;
  const std::atomic<uint64_t>* seeks = nullptr;
};

// The format of a feed's file from its first bytes (waits for them; for an
// ID3v2 tag until the bytes behind it, so FLAC behind a tag is FLAC).
AudioFormat feed_sniff(ByteFeed& f, const FeedWait& wait);
// MP3, FLAC and WAV decode while the file arrives; others need all of it.
bool audio_streamable(AudioFormat f);

// A decoder reading from `feed`. A complete feed opens like open_audio (from
// memory, MP3 with its seek table and exact length). An incomplete one opens
// as soon as the header is there for the streamable formats, reading on as
// bytes come (audio_streaming); the others wait for the whole file. Null with
// `error` set as for open_audio, or when the feed failed / was given up.
AudioDecoderPtr open_audio_feed(std::shared_ptr<ByteFeed> feed, const FeedWait& wait,
                                std::string* error);
// Reading from a feed that had not arrived when it opened: reads past the
// bytes so far wait. Such an MP3 has no seek table (seeks decode from the
// start or on from here) and its length is an estimate from the bitrate; an
// open_audio_feed of the complete feed replaces it with the exact decoder.
bool audio_streaming(const AudioDecoder& d);
// Before a read or seek: seeks requested until now no longer interrupt it.
void audio_rearm(AudioDecoder& d);
// The last read stopped short for a newer seek or a cancel, not the end.
bool audio_interrupted(const AudioDecoder& d);

// "dr_mp3 0.7.5, dr_flac …": the vendored decoders, for About / the report.
std::string audio_decoder_versions();

// "1:05", "12:34", "1:02:03".
std::string format_audio_time(double seconds);

// The last path element of an address, percent-decoded, without query and
// fragment: a title when the file names none.
std::string audio_name_of(std::string_view url);

}  // namespace xanatoo
