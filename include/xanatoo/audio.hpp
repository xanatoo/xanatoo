// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Audio file decoding (pure, no SDL): MP3, FLAC, Ogg Vorbis and WAV
// through the vendored dr_mp3 / dr_flac / dr_wav / stb_vorbis, from bytes in
// memory to interleaved float samples. The player that sends them to the
// sound device is app_audio.cpp; see docs/dev/CONTENT.md "Audio".

#pragma once

#include "xanatoo/byte_feed.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
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
// Reading a ByteFeed (byte_feed.hpp) while it downloads.

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
