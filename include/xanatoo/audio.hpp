// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Audio file decoding (pure, no SDL): MP3, FLAC, Ogg Vorbis and WAV
// through the vendored dr_mp3 / dr_flac / dr_wav / stb_vorbis, from bytes in
// memory to interleaved float samples. The player that sends them to the
// sound device is app_audio.cpp; see docs/dev/CONTENT.md "Audio".

#pragma once

#include <cstddef>
#include <cstdint>
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
// Length in sample frames; 0 = unknown.
uint64_t audio_frames(const AudioDecoder& d);
// Title from the file's tags (Vorbis / FLAC comments, ID3v2 TIT2); may be "".
std::string audio_title(const AudioDecoder& d);

// Up to `frames` interleaved frames into `out` (frames × channels floats);
// returns the number decoded, fewer only at the end.
size_t audio_read(AudioDecoder& d, float* out, size_t frames);
// To sample frame `frame` (clamped to the length). False when the decoder
// cannot seek there.
bool audio_seek(AudioDecoder& d, uint64_t frame);

// "dr_mp3 0.7.5, dr_flac …": the vendored decoders, for About / the report.
std::string audio_decoder_versions();

// "1:05", "12:34", "1:02:03".
std::string format_audio_time(double seconds);

// The last path element of an address, percent-decoded, without query and
// fragment: a title when the file names none.
std::string audio_name_of(std::string_view url);

}  // namespace xanatoo
