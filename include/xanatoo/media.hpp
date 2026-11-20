// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Video and the audio formats the built-in decoders (audio.hpp) lack, through
// a platform backend: FFmpeg on desktop builds (media_ffmpeg.cpp,
// XANATOO_FFMPEG), the NDK's AMediaExtractor + AMediaCodec on Android
// (media_android.cpp). No SDL here: a MediaSource demuxes and decodes;
// MediaPlayback runs one on a worker thread and hands audio to the sound
// callback and video frames to the main thread, with the audio as the
// clock. The player and its panel are app_audio.cpp; see
// docs/dev/CONTENT.md "Video".

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

struct ByteFeed;

// Named like a video file (.mp4 .m4v .webm .mkv .mov .ogv .avi .3gp …), by its
// path; local paths, file: and http(s) as for is_audio_url.
bool is_video_url(std::string_view url);
// Audio that only a backend decodes (.m4a .aac .opus, …): not one of the
// built-in formats (MP3, FLAC, Ogg Vorbis, WAV by name).
bool needs_media_backend(std::string_view url);
// An HLS / DASH playlist by name (.m3u8, .mpd): its pieces are other
// addresses, which the backend fetches itself.
bool is_stream_playlist_url(std::string_view url);
// A link the player takes in this build: built-in audio, or with a backend
// video and the other audio formats (playlists where the backend streams
// them, media_backend_plays_playlists).
bool is_playable_url(std::string_view url);

// The same by a response's Content-Type ("video/mp4; codecs=…"): sound or
// video the player takes in this build (the built-in audio types: MP3, FLAC,
// Ogg, WAV; with a backend every audio/* and video/*).
bool is_playable_type(std::string_view content_type);
// A type only a backend decodes (video/*, audio/mp4, audio/aac, …, and the
// HLS / DASH playlist types).
bool type_needs_media_backend(std::string_view content_type);
// application/vnd.apple.mpegurl, application/x-mpegurl, application/dash+xml.
bool is_stream_playlist_type(std::string_view content_type);

// Media Fragments URI, temporal dimension (W3C): `#t=10`, `#t=1:02.5`,
// `#t=npt:10,20`, also as one `&` part of the fragment and percent-encoded
// (`#t%3D10`, as format_locator writes it).
// The start in seconds; nullopt when the fragment names no time.
std::optional<double> parse_media_time(std::string_view fragment);
// Start and end: `t=10,20` → {10, 20}; `t=10` has no end; an end not after
// the start is none.
struct MediaTimeRange {
  double start = 0;
  std::optional<double> end;
};
std::optional<MediaTimeRange> parse_media_range(std::string_view fragment);
// An address split at a time fragment: `x.mp4#t=30` → {"x.mp4", 30};
// `x.mp4#t=30,40` also has the end (a section).
// Without one the address is the base, unchanged (other fragments stay).
struct MediaAddress {
  std::string base;
  std::optional<double> start;
  std::optional<double> end;
};
MediaAddress split_media_address(std::string_view address);
// `base#t=12.5`: seconds rounded to tenths, without trailing zeros.
std::string media_time_address(std::string_view base, double seconds);
// `base#t=12.5,20`: a section, the same rounding.
std::string media_range_address(std::string_view base, double start, double end);

// "FFmpeg 7.1", "Android MediaCodec"; "" when this build has no backend.
std::string media_backend_name();
inline bool media_backend_available() { return !media_backend_name().empty(); }
// The backend reads an http(s) file through MediaOptions::feed (FFmpeg).
bool media_backend_reads_feeds();
// The backend plays HLS / DASH playlists from their address (FFmpeg).
bool media_backend_plays_playlists();

// One decoded picture, I420 (planar Y, U, V; chroma at half size, rounded
// up), rows packed: Y is w×h bytes, U and V each ((w+1)/2)×((h+1)/2).
struct VideoFrame {
  int w = 0, h = 0;
  double pts = 0;  // seconds from the start of the file
  std::vector<uint8_t> data;
  int cw() const { return (w + 1) / 2; }
  int ch() const { return (h + 1) / 2; }
  uint8_t* y() { return data.data(); }
  uint8_t* u() { return data.data() + size_t(w) * h; }
  uint8_t* v() { return u() + size_t(cw()) * ch(); }
  const uint8_t* y() const { return data.data(); }
  const uint8_t* u() const { return data.data() + size_t(w) * h; }
  const uint8_t* v() const { return u() + size_t(cw()) * ch(); }
  void alloc(int width, int height) {
    w = width;
    h = height;
    data.resize(size_t(w) * h + 2 * size_t(cw()) * ch());
  }
};

struct MediaInfo {
  bool has_audio = false, has_video = false;
  int width = 0, height = 0;   // display size of the picture
  int rate = 0, channels = 0;  // of the float samples next() gives
  double duration = 0;         // seconds; 0 = unknown (a live stream)
  double frame_rate = 0;       // pictures per second; 0 = unknown
  std::string title;           // from the container's tags; may be ""
  std::string format;          // "H264 640×360 · AAC 44100 Hz stereo"
  bool builtin = false;        // the built-in decoders (open_audio_stream)
};

class MediaSource;

struct MediaOptions {
  std::string user_agent;  // for http(s) sources (version.hpp user_agent())
  // Set by the owner to abandon a slow open / read (checked by the backend
  // where it can interrupt; Android's extractor cannot).
  const std::atomic<bool>* cancel = nullptr;
  // Counts seek requests (MediaPlayback::seeks): a source waiting for bytes
  // may give up its read when a newer seek comes. Set by media_worker.
  const std::atomic<uint64_t>* seeks = nullptr;
  // Demux ahead on a thread of its own (FFmpeg): -1 for http(s) streams,
  // 1 always (tests), 0 never.
  int read_ahead = -1;
  // The download of an http(s) file (byte_feed.hpp) for a backend that
  // media_backend_reads_feeds: it reads the file from there (cached, seeks
  // back without the network) when the server answered byte ranges, and
  // else cancels it and fetches the address itself (also playlists).
  std::shared_ptr<ByteFeed> feed;
  // How media_worker opens the source; unset: open_media(url).
  std::function<std::unique_ptr<MediaSource>(const MediaOptions&, std::string* error)> open;
};

// A demuxer with its decoders. Not thread-safe: one worker uses it.
class MediaSource {
public:
  virtual ~MediaSource() = default;
  const MediaInfo& info() const { return info_; }

  enum class Got : uint8_t { Audio, Video, End, Error };
  // The next decoded piece: interleaved float samples appended to `audio`
  // (info().channels per frame) starting at `pts` seconds, or a picture
  // in `frame`. End at the end of the file; Error sets error().
  virtual Got next(std::vector<float>& audio, double& pts, VideoFrame& frame) = 0;
  // To the key frame at or before `seconds`; pieces before it follow and are
  // for the caller to drop. False when the source cannot seek.
  virtual bool seek(double seconds) = 0;
  const std::string& error() const { return error_; }
  // How far into the file (seconds) the source has read ahead: a stream's
  // downloaded or queued part, for the seek bar. -1 unknown / all of it.
  virtual double buffered() const { return -1; }

protected:
  MediaInfo info_;
  std::string error_;
};

// Null with `error` set when the backend cannot open `url` (a local path or
// an http(s) address), or when the build has none. Blocks while opening.
std::unique_ptr<MediaSource> open_media(const std::string& url, const MediaOptions& options,
                                        std::string* error);

// The built-in decoders (audio.hpp) as a source, reading `feed` while it
// arrives (open_audio_feed): sound that plays during its download. When the
// download completes, the exact in-memory decoder takes over at the same
// position (MP3: seek table, exact length; info().duration changes).
std::unique_ptr<MediaSource> open_audio_stream(std::shared_ptr<ByteFeed> feed,
                                               const MediaOptions& options, std::string* error);

// ---- playback pipe ----------------------------------------------------------
// Shared between a worker (opens the source, decodes ahead), the sound
// callback (media_pull_audio) and the main thread (clock, frames, seek).
// The worker thread owns a shared_ptr; dropping the owner's reference after
// media_stop never waits for it.
struct MediaPlayback {
  std::mutex m;
  std::condition_variable cv;
  std::atomic<bool> cancel{false};  // stop: the worker leaves
  std::atomic<bool> opened{false};  // info / error below are set
  std::atomic<uint64_t> seeks{0};   // media_seek calls (MediaOptions::seeks)
  MediaInfo info;  // duration may change while playing (open_audio_stream)
  std::string error;  // open or decode failure

  // Under m:
  std::deque<float> audio;        // interleaved samples, not yet played
  std::deque<VideoFrame> frames;  // in pts order
  bool source_end = false;        // the source has given everything
  bool failed = false;
  bool seek_pending = false;
  double seek_to = 0;    // target of the pending / last seek
  double buffered = -1;  // MediaSource::buffered() after the last piece
  uint64_t played = 0;   // frames handed to the device since the seek
  // Video without sound runs on the wall clock.
  // (and with sound once the sound has ended before the pictures).
  double wall_base = 0;    // seconds into the file at wall_start
  double wall_start = -1;  // monotonic seconds; -1 = paused
  bool paused = true;
  bool sound_done = false;  // the sound ran out: the wall clock takes over
  bool want_first = true;   // show the first picture after a start / seek
                            // even before it is due (a paused seek shows it)
};

// Read ahead this much sound and at least this many pictures. At most
// kMediaFramesMax pictures wait; beyond them decoding stops, unless the
// sound runs dry (then the oldest pictures are dropped: the clock is the
// sound's).
constexpr double kMediaAudioAhead = 1.0;
// Sound alone reads further ahead (cheap; rides out a stalling stream).
constexpr double kMediaAudioAheadSoundOnly = 5.0;
constexpr size_t kMediaFramesAhead = 4;
constexpr size_t kMediaFramesMax = 24;

// Runs on the worker: open (or use `src` when given), then decode ahead until
// cancelled.
void media_worker(std::shared_ptr<MediaPlayback> pb, std::string url, MediaOptions options,
                  std::unique_ptr<MediaSource> src = nullptr);
// Starts media_worker on a detached thread.
std::shared_ptr<MediaPlayback> media_start(const std::string& url, MediaOptions options);
void media_stop(MediaPlayback& pb);

// Sound callback: up to `frames` frames into `out`; returns how many
// (fewer = nothing decoded yet, or the end).
size_t media_pull_audio(MediaPlayback& pb, float* out, size_t frames);
// Seconds into the file now playing. `now` is monotonic seconds (only used
// without sound).
double media_clock(MediaPlayback& pb, double now);
// Video without sound: start or stop the wall clock.
void media_set_paused(MediaPlayback& pb, bool paused, double now);
// The latest picture due at `clock` into `out` (dropping older ones);
// false when none is due yet.
bool media_take_frame(MediaPlayback& pb, double clock, VideoFrame& out);
void media_seek(MediaPlayback& pb, double seconds, double now);
// Everything played (or failed).
bool media_finished(MediaPlayback& pb);
// Waiting for the source: nothing to play next although the end has not
// come (a slow stream, a seek in progress).
bool media_buffering(MediaPlayback& pb);

// "H264 640×360 · AAC 44100 Hz stereo" from codec names and sizes (either
// part left out when its codec name is empty).
std::string media_describe(std::string_view video_codec, int w, int h,
                           std::string_view audio_codec, int rate, int channels);

}  // namespace xanatoo
