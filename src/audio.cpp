// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/audio.hpp"

#include "xanatoo/util.hpp"

#define DR_MP3_NO_STDIO
#include "dr_mp3.h"
#define DR_FLAC_NO_STDIO
#include "dr_flac.h"
#define DR_WAV_NO_STDIO
#include "dr_wav.h"
#define STB_VORBIS_HEADER_ONLY
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis.c"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace xanatoo {

struct AudioDecoder {
  std::vector<uint8_t> bytes;  // what the decoders read from
  AudioFormat format = AudioFormat::Unknown;
  int channels = 0;
  int rate = 0;
  uint64_t frames = 0;
  uint64_t pos = 0;  // next frame audio_read returns (WAV / Vorbis)
  std::string title;
  drmp3 mp3{};
  bool mp3_open = false;
  std::vector<drmp3_seek_point> mp3_seek;
  drflac* flac = nullptr;
  drwav wav{};
  bool wav_open = false;
  stb_vorbis* vorbis = nullptr;
};

void AudioDecoderDeleter::operator()(AudioDecoder* d) const {
  if (!d) return;
  if (d->mp3_open) drmp3_uninit(&d->mp3);
  if (d->flac) drflac_close(d->flac);
  if (d->wav_open) drwav_uninit(&d->wav);
  if (d->vorbis) stb_vorbis_close(d->vorbis);
  delete d;
}

namespace {

// Size of an ID3v2 tag at the start (header, body, footer), else 0.
size_t id3v2_size(const uint8_t* p, size_t n) {
  if (n < 10 || std::memcmp(p, "ID3", 3) != 0) return 0;
  for (int i = 6; i < 10; ++i)
    if (p[i] & 0x80) return 0;
  const size_t body = (size_t(p[6]) << 21) | (size_t(p[7]) << 14) |
                      (size_t(p[8]) << 7) | size_t(p[9]);
  return 10 + body + ((p[5] & 0x10) ? 10 : 0);
}

bool mp3_frame_sync(const uint8_t* p, size_t n) {
  // 11 sync bits, a version other than "reserved", layer I-III, bitrate
  // and sample rate indices that are not "bad" / "reserved".
  return n >= 4 && p[0] == 0xFF && (p[1] & 0xE0) == 0xE0 &&
         ((p[1] >> 3) & 3) != 1 && ((p[1] >> 1) & 3) != 0 &&
         (p[2] >> 4) != 15 && ((p[2] >> 2) & 3) != 3;
}

void append_utf8(std::string& out, uint32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xC0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xE0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (c & 0x3F));
  }
}

// An ID3 text frame body (encoding byte + text) as UTF-8.
std::string id3_text(const uint8_t* p, size_t n) {
  if (n < 1) return {};
  const uint8_t enc = p[0];
  ++p;
  --n;
  std::string out;
  if (enc == 0) {  // ISO-8859-1
    for (size_t i = 0; i < n && p[i]; ++i) append_utf8(out, p[i]);
  } else if (enc == 3) {  // UTF-8
    for (size_t i = 0; i < n && p[i]; ++i) out += static_cast<char>(p[i]);
  } else {  // UTF-16 with BOM (1) or big-endian (2)
    bool be = enc == 2;
    size_t i = 0;
    if (enc == 1 && n >= 2) {
      if (p[0] == 0xFE && p[1] == 0xFF) be = true, i = 2;
      else if (p[0] == 0xFF && p[1] == 0xFE) be = false, i = 2;
    }
    auto unit = [&](size_t k) -> uint32_t {
      return be ? (uint32_t(p[k]) << 8) | p[k + 1] : (uint32_t(p[k + 1]) << 8) | p[k];
    };
    for (; i + 1 < n; i += 2) {
      uint32_t c = unit(i);
      if (c == 0) break;
      if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) {
        const uint32_t lo = unit(i + 2);
        if (lo >= 0xDC00 && lo < 0xE000) {
          c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
          i += 2;
        }
      }
      append_utf8(out, c);
    }
  }
  std::string_view v(out);
  trim_ascii(v);
  return std::string(v);
}

// The title (TIT2, v2.2 TT2) of an ID3v2 tag at the start of `p`.
std::string id3v2_title(const uint8_t* p, size_t n) {
  const size_t size = id3v2_size(p, n);
  if (size == 0) return {};
  const int version = p[3];
  if (p[5] & 0x80) return {};  // unsynchronised tags: rare, skipped
  const size_t end = std::min(n, size);
  size_t at = 10;
  if (version >= 3 && (p[5] & 0x40) && at + 4 <= end) {  // extended header
    const size_t ext = version == 4
                           ? (size_t(p[10] & 0x7F) << 21) | (size_t(p[11] & 0x7F) << 14) |
                                 (size_t(p[12] & 0x7F) << 7) | size_t(p[13] & 0x7F)
                           : ((size_t(p[10]) << 24) | (size_t(p[11]) << 16) |
                              (size_t(p[12]) << 8) | size_t(p[13])) + 4;
    at += ext;
  }
  const size_t head = version == 2 ? 6 : 10;
  while (at + head <= end && p[at] != 0) {
    size_t len;
    bool title;
    if (version == 2) {
      len = (size_t(p[at + 3]) << 16) | (size_t(p[at + 4]) << 8) | p[at + 5];
      title = std::memcmp(p + at, "TT2", 3) == 0;
    } else {
      const uint8_t* s = p + at + 4;
      len = version == 4 ? (size_t(s[0] & 0x7F) << 21) | (size_t(s[1] & 0x7F) << 14) |
                               (size_t(s[2] & 0x7F) << 7) | size_t(s[3] & 0x7F)
                         : (size_t(s[0]) << 24) | (size_t(s[1]) << 16) |
                               (size_t(s[2]) << 8) | size_t(s[3]);
      title = std::memcmp(p + at, "TIT2", 4) == 0;
    }
    at += head;
    if (len > end - at) break;
    if (title) return id3_text(p + at, len);
    at += len;
  }
  return {};
}

// "TITLE=..." from a Vorbis comment, case-insensitively.
bool comment_title(std::string_view c, std::string& out) {
  if (c.size() < 6 || !starts_with_ci(c, "title=")) return false;
  std::string_view v = c.substr(6);
  trim_ascii(v);
  out = std::string(v);
  return !out.empty();
}

void flac_meta(void* user, drflac_metadata* m) {
  auto* d = static_cast<AudioDecoder*>(user);
  if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT || !d->title.empty())
    return;
  drflac_vorbis_comment_iterator it;
  drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount,
                                      m->data.vorbis_comment.pComments);
  drflac_uint32 len = 0;
  while (const char* c = drflac_next_vorbis_comment(&it, &len))
    if (comment_title(std::string_view(c, len), d->title)) return;
}

}  // namespace

AudioFormat sniff_audio(const uint8_t* p, size_t n) {
  if (n >= 12 && (std::memcmp(p, "RIFF", 4) == 0 || std::memcmp(p, "RF64", 4) == 0) &&
      std::memcmp(p + 8, "WAVE", 4) == 0)
    return AudioFormat::Wav;
  if (n >= 4 && std::memcmp(p, "fLaC", 4) == 0) return AudioFormat::Flac;
  if (n >= 36 && std::memcmp(p, "OggS", 4) == 0) {
    // First page: 27-byte header, segment table, then the first packet.
    const size_t first = 27 + p[26];
    if (first + 8 <= n) {
      if (std::memcmp(p + first, "\x01vorbis", 7) == 0) return AudioFormat::Vorbis;
      if (std::memcmp(p + first, "OpusHead", 8) == 0) return AudioFormat::Opus;
    }
    return AudioFormat::Unknown;
  }
  if (const size_t tag = id3v2_size(p, n)) {
    if (tag + 4 <= n && std::memcmp(p + tag, "fLaC", 4) == 0) return AudioFormat::Flac;
    return AudioFormat::Mp3;  // ID3v2 is an MP3 thing
  }
  if (mp3_frame_sync(p, n)) return AudioFormat::Mp3;
  return AudioFormat::Unknown;
}

const char* audio_format_name(AudioFormat f) {
  switch (f) {
    case AudioFormat::Wav: return "WAV";
    case AudioFormat::Mp3: return "MP3";
    case AudioFormat::Flac: return "FLAC";
    case AudioFormat::Vorbis: return "Ogg Vorbis";
    case AudioFormat::Opus: return "Opus";
    case AudioFormat::Unknown: break;
  }
  return "unknown";
}

bool url_has_extension(std::string_view url, std::initializer_list<std::string_view> exts) {
  const size_t colon = url.find(':');
  const size_t slash = url.find('/');
  if (colon != std::string_view::npos &&
      (slash == std::string_view::npos || colon < slash) &&
      !starts_with_ci(url, "http://") && !starts_with_ci(url, "https://") &&
      !starts_with_ci(url, "file:") && !(colon == 1 && slash == 2))  // C:/path
    return false;
  const std::string_view path = url.substr(0, url.find_first_of("?#"));
  for (std::string_view ext : exts)
    if (path_ends_with_ci(path, ext)) return true;
  return false;
}

bool is_audio_url(std::string_view url) {
  return url_has_extension(url, {".mp3", ".ogg", ".oga", ".flac", ".wav", ".opus"});
}

AudioDecoderPtr open_audio(std::vector<uint8_t> bytes, std::string* error) {
  auto fail = [&](std::string why) {
    if (error) *error = std::move(why);
    return AudioDecoderPtr{};
  };
  AudioDecoderPtr d(new AudioDecoder);
  d->bytes = std::move(bytes);
  const uint8_t* p = d->bytes.data();
  const size_t n = d->bytes.size();
  d->format = sniff_audio(p, n);
  switch (d->format) {
    case AudioFormat::Mp3: {
      if (!drmp3_init_memory(&d->mp3, p, n, nullptr)) return fail("not a readable MP3 file");
      d->mp3_open = true;
      d->channels = static_cast<int>(d->mp3.channels);
      d->rate = static_cast<int>(d->mp3.sampleRate);
      // One scan for the seek table (else every seek decodes from the start)
      // and the length.
      drmp3_uint32 count = 1024;
      d->mp3_seek.resize(count);
      if (drmp3_calculate_seek_points(&d->mp3, &count, d->mp3_seek.data()) && count > 0) {
        d->mp3_seek.resize(count);
        drmp3_bind_seek_table(&d->mp3, count, d->mp3_seek.data());
      } else {
        d->mp3_seek.clear();
      }
      d->frames = drmp3_get_pcm_frame_count(&d->mp3);
      drmp3_seek_to_pcm_frame(&d->mp3, 0);
      d->title = id3v2_title(p, n);
      break;
    }
    case AudioFormat::Flac:
      d->flac = drflac_open_memory_with_metadata(p, n, flac_meta, d.get(), nullptr);
      if (!d->flac) return fail("not a readable FLAC file");
      d->channels = d->flac->channels;
      d->rate = static_cast<int>(d->flac->sampleRate);
      d->frames = d->flac->totalPCMFrameCount;
      if (d->title.empty()) d->title = id3v2_title(p, n);
      break;
    case AudioFormat::Wav:
      if (!drwav_init_memory(&d->wav, p, n, nullptr)) return fail("not a readable WAV file");
      d->wav_open = true;
      d->channels = d->wav.channels;
      d->rate = static_cast<int>(d->wav.sampleRate);
      d->frames = d->wav.totalPCMFrameCount;
      break;
    case AudioFormat::Vorbis: {
      if (n > static_cast<size_t>(INT32_MAX)) return fail("file too large");
      int err = 0;
      d->vorbis = stb_vorbis_open_memory(p, static_cast<int>(n), &err, nullptr);
      if (!d->vorbis) return fail("not a readable Ogg Vorbis file");
      const stb_vorbis_info info = stb_vorbis_get_info(d->vorbis);
      d->channels = info.channels;
      d->rate = static_cast<int>(info.sample_rate);
      d->frames = static_cast<uint64_t>(stb_vorbis_stream_length_in_samples(d->vorbis));
      const stb_vorbis_comment c = stb_vorbis_get_comment(d->vorbis);
      for (int i = 0; i < c.comment_list_length; ++i)
        if (comment_title(c.comment_list[i], d->title)) break;
      break;
    }
    case AudioFormat::Opus:
      return fail("Opus audio is not supported yet");
    case AudioFormat::Unknown:
      return fail("not an audio file xanatoo can play (MP3, FLAC, Ogg Vorbis, WAV)");
  }
  if (d->channels < 1 || d->channels > 8 || d->rate < 1000 || d->rate > 384000)
    return fail("unsupported channel count or sample rate");
  return d;
}

AudioFormat audio_format(const AudioDecoder& d) { return d.format; }
int audio_channels(const AudioDecoder& d) { return d.channels; }
int audio_rate(const AudioDecoder& d) { return d.rate; }
uint64_t audio_frames(const AudioDecoder& d) { return d.frames; }
std::string audio_title(const AudioDecoder& d) { return d.title; }

size_t audio_read(AudioDecoder& d, float* out, size_t frames) {
  if (frames == 0) return 0;
  switch (d.format) {
    case AudioFormat::Mp3:
      return static_cast<size_t>(drmp3_read_pcm_frames_f32(&d.mp3, frames, out));
    case AudioFormat::Flac:
      return static_cast<size_t>(drflac_read_pcm_frames_f32(d.flac, frames, out));
    case AudioFormat::Wav:
      return static_cast<size_t>(drwav_read_pcm_frames_f32(&d.wav, frames, out));
    case AudioFormat::Vorbis: {
      size_t done = 0;
      while (done < frames) {
        const size_t want = std::min<size_t>(frames - done, 1 << 16);
        const int got = stb_vorbis_get_samples_float_interleaved(
            d.vorbis, d.channels, out + done * d.channels,
            static_cast<int>(want * d.channels));
        if (got <= 0) break;
        done += static_cast<size_t>(got);
      }
      return done;
    }
    default:
      return 0;
  }
}

bool audio_seek(AudioDecoder& d, uint64_t frame) {
  if (d.frames > 0) frame = std::min(frame, d.frames);
  switch (d.format) {
    case AudioFormat::Mp3: return drmp3_seek_to_pcm_frame(&d.mp3, frame) != 0;
    case AudioFormat::Flac: return drflac_seek_to_pcm_frame(d.flac, frame) != 0;
    case AudioFormat::Wav: return drwav_seek_to_pcm_frame(&d.wav, frame) != 0;
    case AudioFormat::Vorbis:
      return stb_vorbis_seek(d.vorbis, static_cast<unsigned int>(frame)) != 0;
    default: return false;
  }
}

std::string audio_decoder_versions() {
  // stb_vorbis has no version call; keep this in step with external/.
  return std::string("dr_mp3 ") + drmp3_version_string() + ", dr_flac " +
         drflac_version_string() + ", dr_wav " + drwav_version_string() +
         ", stb_vorbis 1.25";
}

std::string format_audio_time(double seconds) {
  if (!(seconds > 0)) seconds = 0;  // also NaN
  const long s = static_cast<long>(seconds);
  char buf[32];
  if (s >= 3600)
    std::snprintf(buf, sizeof buf, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
  else
    std::snprintf(buf, sizeof buf, "%ld:%02ld", s / 60, s % 60);
  return buf;
}

std::string audio_name_of(std::string_view url) {
  std::string_view path = url.substr(0, url.find_first_of("?#"));
  while (!path.empty() && path.back() == '/') path.remove_suffix(1);
  if (const size_t slash = path.find_last_of("/\\"); slash != std::string_view::npos)
    path = path.substr(slash + 1);
  std::string out;
  for (size_t i = 0; i < path.size(); ++i) {
    auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    if (path[i] == '%' && i + 2 < path.size() && hex(path[i + 1]) >= 0 &&
        hex(path[i + 2]) >= 0) {
      out += static_cast<char>(hex(path[i + 1]) * 16 + hex(path[i + 2]));
      i += 2;
    } else {
      out += path[i];
    }
  }
  return out;
}

}  // namespace xanatoo
