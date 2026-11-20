// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Media backends' common part (media.hpp): which addresses go to the
// player, and the playback pipe between the decoding worker, the sound
// callback and the main thread. The backends themselves are
// media_ffmpeg.cpp and media_android.cpp.

#include "xanatoo/media.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

#include "xanatoo/audio.hpp"
#include "xanatoo/text_escape.hpp"
#include "xanatoo/util.hpp"

namespace xanatoo {

bool is_video_url(std::string_view url) {
  return url_has_extension(
      url, {".mp4", ".m4v", ".webm", ".mkv", ".mov", ".ogv", ".avi", ".3gp", ".mpg", ".mpeg"});
}

bool needs_media_backend(std::string_view url) {
  return is_video_url(url) ||
         url_has_extension(url, {".opus", ".m4a", ".aac", ".mka", ".weba"});
}

bool is_playable_url(std::string_view url) {
  return is_audio_url(url) || (needs_media_backend(url) && media_backend_available());
}

namespace {
// "audio/mpeg" from "Audio/MPEG; charset=…".
std::string mime_base(std::string_view content_type) {
  std::string_view t = content_type.substr(0, content_type.find(';'));
  trim_ascii(t);
  return ascii_lower(t);
}

bool builtin_audio_type(const std::string& t) {
  for (const char* k : {"audio/mpeg", "audio/mp3", "audio/mpeg3", "audio/x-mpeg", "audio/x-mp3",
                        "audio/flac", "audio/x-flac", "audio/wav", "audio/x-wav", "audio/wave",
                        "audio/vnd.wave", "audio/ogg", "audio/vorbis", "application/ogg"})
    if (t == k) return true;
  return false;
}
}  // namespace

bool type_needs_media_backend(std::string_view content_type) {
  const std::string t = mime_base(content_type);
  return !builtin_audio_type(t) && (starts_with(t, "audio/") || starts_with(t, "video/"));
}

bool is_playable_type(std::string_view content_type) {
  const std::string t = mime_base(content_type);
  return builtin_audio_type(t) ||
         (type_needs_media_backend(content_type) && media_backend_available());
}

namespace {
// "10", "10.5", "1:02", "1:02:03.5"; nullopt for anything else.
std::optional<double> parse_clock(std::string_view s) {
  if (s.empty()) return std::nullopt;
  double total = 0, part = 0, scale = 0;  // scale > 0: in the decimals
  int colons = 0;
  bool digit = false;
  for (char c : s) {
    if (c >= '0' && c <= '9') {
      digit = true;
      if (scale > 0) {
        part += (c - '0') * scale;
        scale /= 10;
      } else {
        part = part * 10 + (c - '0');
      }
    } else if (c == '.' && scale == 0 && digit) {
      scale = 0.1;
    } else if (c == ':' && scale == 0 && digit && colons < 2) {
      total = (total + part) * 60;
      part = 0;
      digit = false;
      ++colons;
    } else {
      return std::nullopt;
    }
  }
  if (!digit) return std::nullopt;
  return total + part;
}

}  // namespace

std::optional<double> parse_media_time(std::string_view fragment) {
  const std::string frag = percent_decode(fragment);
  std::string_view rest = frag;
  while (!rest.empty()) {
    const size_t amp = rest.find('&');
    std::string_view part = rest.substr(0, amp);
    rest = amp == std::string_view::npos ? std::string_view{} : rest.substr(amp + 1);
    if (part.size() < 2 || part.substr(0, 2) != "t=") continue;
    part.remove_prefix(2);
    if (part.substr(0, 4) == "npt:") part.remove_prefix(4);
    const size_t comma = part.find(',');
    const std::string_view start = part.substr(0, comma);
    if (start.empty()) return 0.0;  // "t=,20": from the start
    return parse_clock(start);
  }
  return std::nullopt;
}

MediaAddress split_media_address(std::string_view address) {
  const size_t hash = address.find('#');
  if (hash != std::string_view::npos)
    if (auto t = parse_media_time(address.substr(hash + 1)))
      return {std::string(address.substr(0, hash)), t};
  return {std::string(address), std::nullopt};
}

std::string media_time_address(std::string_view base, double seconds) {
  const long tenths = std::lround(std::max(0.0, seconds) * 10);
  std::string t = std::to_string(tenths / 10);
  if (tenths % 10) t += "." + std::to_string(tenths % 10);
  return std::string(base) + "#t=" + t;
}

#if !(defined(XANATOO_FFMPEG) && XANATOO_FFMPEG) && !defined(__ANDROID__)
std::string media_backend_name() { return {}; }

std::unique_ptr<MediaSource> open_media(const std::string&, const MediaOptions&,
                                        std::string* error) {
  if (error) *error = "this build plays no video (built without FFmpeg)";
  return nullptr;
}
#endif

std::string media_describe(std::string_view video_codec, int w, int h,
                           std::string_view audio_codec, int rate, int channels) {
  std::string v, a;
  if (!video_codec.empty())
    v = std::string(video_codec) + " " + std::to_string(w) + "\xC3\x97" + std::to_string(h);
  if (!audio_codec.empty()) {
    a = std::string(audio_codec) + " " + std::to_string(rate) + " Hz " +
        (channels == 1   ? std::string("mono")
         : channels == 2 ? std::string("stereo")
                         : std::to_string(channels) + " ch");
  }
  if (!v.empty() && !a.empty()) return v + " \xC2\xB7 " + a;
  return v.empty() ? a : v;
}

namespace {

// Decode on while the sound or the pictures run low. A full picture queue
// stops it (files decode pictures behind their sound: dropping pictures to
// read sound further ahead would drop the ones about to be due), unless
// the sound is about to run dry. Pictures alone only while the sound is not
// far ahead: a short video track must not make the worker decode a long
// file's sound into memory.
bool need_more(const MediaPlayback& pb) {
  const MediaInfo& i = pb.info;
  const size_t per_s = size_t(std::max(1, i.rate)) * size_t(std::max(1, i.channels));
  const size_t queued = pb.audio.size();
  if (i.has_video && pb.frames.size() >= kMediaFramesMax)
    return i.has_audio && queued < per_s / 4;
  const double ahead = i.has_video ? kMediaAudioAhead : kMediaAudioAheadSoundOnly;
  if (i.has_audio && queued < size_t(ahead * per_s)) return true;
  if (i.has_video && pb.frames.size() < kMediaFramesAhead)
    return !i.has_audio || queued < 8 * per_s;
  return false;
}

}  // namespace

void media_worker(std::shared_ptr<MediaPlayback> pb, std::string url, MediaOptions options,
                  std::unique_ptr<MediaSource> src) {
  if (!src) {
    std::string err;
    options.cancel = &pb->cancel;
    options.seeks = &pb->seeks;
    src = options.open ? options.open(options, &err) : open_media(url, options, &err);
    if (!src) {
      {
        std::lock_guard lock(pb->m);
        pb->error = err.empty() ? "cannot open" : err;
        pb->failed = true;
        pb->source_end = true;
      }
      pb->opened.store(true, std::memory_order_release);
      return;
    }
  }
  {
    std::lock_guard lock(pb->m);
    pb->info = src->info();
  }
  pb->opened.store(true, std::memory_order_release);
  const int ch = std::max(1, src->info().channels);
  const int rate = std::max(1, src->info().rate);
  double drop_before = -1;  // pieces before the last seek's target
  std::vector<float> chunk;
  VideoFrame frame;
  for (;;) {
    {
      std::unique_lock lock(pb->m);
      pb->cv.wait(lock, [&] {
        return pb->cancel.load() || pb->seek_pending || (!pb->source_end && need_more(*pb));
      });
      if (pb->cancel.load()) return;
      if (pb->seek_pending) {
        pb->seek_pending = false;
        const double t = pb->seek_to;
        lock.unlock();
        src->seek(t);
        lock.lock();
        if (pb->seek_pending) continue;  // a newer seek came meanwhile
        pb->audio.clear();
        pb->frames.clear();
        pb->source_end = false;
        drop_before = t;
        continue;
      }
    }
    chunk.clear();
    double pts = 0;
    const MediaSource::Got got = src->next(chunk, pts, frame);
    std::lock_guard lock(pb->m);
    pb->info.duration = src->info().duration;  // may become exact
    if (pb->seek_pending || pb->cancel.load()) continue;  // stale piece
    switch (got) {
      case MediaSource::Got::Audio: {
        size_t skip = 0;  // frames before the seek target
        const size_t n = chunk.size() / size_t(ch);
        if (pts < drop_before)
          skip = std::min(n, size_t((drop_before - pts) * rate + 0.5));
        pb->audio.insert(pb->audio.end(), chunk.begin() + std::ptrdiff_t(skip * ch),
                         chunk.begin() + std::ptrdiff_t(n * ch));
        break;
      }
      case MediaSource::Got::Video:
        if (frame.pts + 1e-3 < drop_before) break;
        pb->frames.push_back(std::move(frame));
        frame = {};
        while (pb->frames.size() > kMediaFramesMax) pb->frames.pop_front();
        break;
      case MediaSource::Got::End: pb->source_end = true; break;
      case MediaSource::Got::Error:
        pb->failed = true;
        pb->error = src->error();
        pb->source_end = true;
        break;
    }
  }
}

std::shared_ptr<MediaPlayback> media_start(const std::string& url, MediaOptions options) {
  auto pb = std::make_shared<MediaPlayback>();
  std::thread([pb, url, options]() mutable { media_worker(pb, url, std::move(options)); })
      .detach();
  return pb;
}

void media_stop(MediaPlayback& pb) {
  pb.cancel.store(true);
  std::lock_guard lock(pb.m);
  pb.cv.notify_all();
}

size_t media_pull_audio(MediaPlayback& pb, float* out, size_t frames) {
  std::lock_guard lock(pb.m);
  const size_t ch = size_t(std::max(1, pb.info.channels));
  const size_t n = std::min(frames, pb.audio.size() / ch);
  std::copy_n(pb.audio.begin(), n * ch, out);
  pb.audio.erase(pb.audio.begin(), pb.audio.begin() + std::ptrdiff_t(n * ch));
  pb.played += n;
  pb.cv.notify_one();
  return n;
}

namespace {
double wall_clock(const MediaPlayback& pb, double now) {
  return pb.wall_start < 0 ? pb.wall_base : pb.wall_base + (now - pb.wall_start);
}
}  // namespace

double media_clock(MediaPlayback& pb, double now) {
  std::lock_guard lock(pb.m);
  if (pb.info.has_audio && !pb.sound_done) {
    const double t = pb.seek_to + double(pb.played) / std::max(1, pb.info.rate);
    // The sound ran out before the pictures: go on by the wall clock.
    if (pb.source_end && pb.audio.empty() && !pb.frames.empty()) {
      pb.sound_done = true;
      pb.wall_base = t;
      pb.wall_start = pb.paused ? -1 : now;
    }
    return t;
  }
  return wall_clock(pb, now);
}

void media_set_paused(MediaPlayback& pb, bool paused, double now) {
  std::lock_guard lock(pb.m);
  if (paused == pb.paused) return;
  pb.paused = paused;
  if (paused) {
    pb.wall_base = wall_clock(pb, now);
    pb.wall_start = -1;
  } else {
    pb.wall_start = now;
  }
}

bool media_take_frame(MediaPlayback& pb, double clock, VideoFrame& out) {
  std::lock_guard lock(pb.m);
  bool got = false;
  if (pb.want_first && !pb.frames.empty()) {
    out = std::move(pb.frames.front());
    pb.frames.pop_front();
    pb.want_first = false;
    got = true;
  }
  while (!pb.frames.empty() && pb.frames.front().pts <= clock + 0.005) {
    out = std::move(pb.frames.front());
    pb.frames.pop_front();
    got = true;
  }
  if (got) pb.cv.notify_one();
  return got;
}

void media_seek(MediaPlayback& pb, double seconds, double now) {
  std::lock_guard lock(pb.m);
  if (pb.info.duration > 0) seconds = std::min(seconds, pb.info.duration);
  pb.seek_pending = true;
  pb.seeks.fetch_add(1);
  pb.seek_to = std::max(0.0, seconds);
  pb.audio.clear();
  pb.frames.clear();
  pb.played = 0;
  pb.source_end = false;
  pb.sound_done = false;
  pb.want_first = true;
  pb.wall_base = pb.seek_to;
  pb.wall_start = pb.paused ? -1 : now;
  pb.cv.notify_all();
}

bool media_finished(MediaPlayback& pb) {
  std::lock_guard lock(pb.m);
  return pb.failed || (pb.source_end && !pb.seek_pending && pb.audio.empty() && pb.frames.empty());
}

bool media_buffering(MediaPlayback& pb) {
  std::lock_guard lock(pb.m);
  if (pb.failed || pb.source_end) return false;
  if (pb.seek_pending) return true;
  return pb.info.has_audio && !pb.sound_done ? pb.audio.empty() : pb.frames.empty();
}

}  // namespace xanatoo
