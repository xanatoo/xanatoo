// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Sound player (App::audio, docs/dev/CONTENT.md "Audio"). A link to an
// audio file loads it on a worker (local read or http(s) download, then
// open_audio, which scans the file for its length / seek table); the
// decoder then feeds SDL's audio callback (sdl::AudioOut) under a mutex. The
// controls are the Media panel (WindowRole::Media): title and format, a
// seek bar (click or drag), times, back / play-pause / forward / stop, A–B
// markers and loop, and a volume slider. media_layout places them for drawing, clicks and hover.
// Video and the formats only a backend decodes (media.hpp) go through a
// MediaPlayback instead: its worker decodes ahead, the same callback pulls
// the sound, and update_audio shows the picture due at the sound's clock
// (the wall clock without sound) in a texture above the controls.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <tuple>
#include <utility>

#include "xanatoo/app.hpp"
#include "xanatoo/audio.hpp"
#include "xanatoo/file_url.hpp"
#include "xanatoo/paths.hpp"
#include "xanatoo/theme.hpp"
#include "xanatoo/util.hpp"
#include "xanatoo/version.hpp"
#if defined(XANATOO_HTTPS) && XANATOO_HTTPS
#include "xanatoo/http_url.hpp"
#include "xanatoo/https.hpp"
#endif

namespace xanatoo::app {

// Lives behind a shared_ptr in App::audio; SDL's callback holds its address.
struct AudioShared {
  std::mutex m;  // decoder and pos; held briefly by the callback and seeks
  xanatoo::AudioDecoderPtr dec;
  int channels = 0;
  int rate = 0;
  uint64_t frames = 0;
  std::atomic<uint64_t> pos{0};  // frames handed to the device
  std::atomic<bool> ended{false};
  std::atomic<float> gain{1.f};  // from the volume slider
  std::shared_ptr<xanatoo::MediaPlayback> media;  // set: sound from the pipe
  xanatoo::sdl::AudioOut out;
  ~AudioShared() { xanatoo::sdl::close_audio_out(out); }  // before `dec`
};

struct AudioLoad {
  std::atomic<bool> done{false};
  std::atomic<bool> cancel{false};
  std::atomic<int64_t> got{0};     // bytes so far (downloads)
  std::atomic<int64_t> total{-1};  // expected bytes, -1 unknown
  xanatoo::AudioDecoderPtr dec;    // set when done and it opened
  std::string error;
};

namespace {

// Larger files are refused (they are decoded from memory).
constexpr int64_t kMaxAudioBytes = int64_t(512) << 20;

void fill(void* user, float* out, int frames) {
  auto* s = static_cast<AudioShared*>(user);
  const size_t want = static_cast<size_t>(frames);
  size_t got = 0;
  {
    std::lock_guard lock(s->m);
    if (s->media) {
      got = xanatoo::media_pull_audio(*s->media, out, want);
      s->pos.fetch_add(got, std::memory_order_relaxed);
    } else if (s->dec && !s->ended.load(std::memory_order_relaxed)) {
      got = xanatoo::audio_read(*s->dec, out, want);
      s->pos.fetch_add(got, std::memory_order_relaxed);
      if (got < want) s->ended.store(true, std::memory_order_release);
    }
  }
  const float g = s->gain.load(std::memory_order_relaxed);
  if (g != 1.f)
    for (size_t i = 0; i < got * s->channels; ++i) out[i] *= g;
  std::fill(out + got * s->channels, out + want * s->channels, 0.f);
}

// Slider position → gain: squared, so the slider's middle sounds like half.
float gain_of(float volume) { return volume * volume; }

std::vector<uint8_t> read_local(const std::string& path, std::string& error) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    error = "cannot open " + path;
    return {};
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  in.seekg(0, std::ios::beg);
  if (size < 0 || size > kMaxAudioBytes) {
    error = "file too large to play";
    return {};
  }
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
    error = "cannot read " + path;
    return {};
  }
  return bytes;
}

bool remote(const std::string& url) {
  return xanatoo::starts_with_ci(url, "http://") ||
         xanatoo::starts_with_ci(url, "https://");
}

// An http(s) file in chunks to `chunk` (false: stop), its announced size to
// `size` (-1 unknown) first. "" when all of it came, else why not ("" too
// when `cancel` stopped it).
std::string fetch_chunks(const std::string& url, const std::atomic<bool>& cancel,
                         const std::function<void(int64_t)>& size,
                         const std::function<bool(std::string_view)>& chunk) {
#if defined(XANATOO_HTTPS) && XANATOO_HTTPS
  std::string err;
  int status = 0;
  bool too_large = false;
  int64_t got = 0;
  const bool ok = xanatoo::https_get_follow_stream(
      url, xanatoo::user_agent("audio"),
      [&](const xanatoo::HttpStreamHeaders& h) {
        status = h.status;
        if (h.status < 200 || h.status >= 300) return false;
        if (h.content_length > kMaxAudioBytes) return !(too_large = true);
        size(h.content_length);
        return true;
      },
      [&](std::string_view bytes) {
        if (cancel.load(std::memory_order_relaxed)) return false;
        if ((got += static_cast<int64_t>(bytes.size())) > kMaxAudioBytes)
          return !(too_large = true);
        return chunk(bytes);
      },
      &err);
  if (ok || cancel.load()) return {};
  if (too_large) return "file too large to play";
  if (status != 0 && (status < 200 || status >= 300)) return "HTTP " + std::to_string(status);
  return err.empty() ? std::string("download failed") : err;
#else
  (void)url, (void)cancel, (void)size, (void)chunk;
  return "this build has no HTTP client";
#endif
}

// An http(s) file into memory, with progress in `load`; errors in load.error.
std::vector<uint8_t> download(const std::string& url, AudioLoad& load) {
  std::vector<uint8_t> bytes;
  load.error = fetch_chunks(
      url, load.cancel,
      [&](int64_t total) {
        load.total.store(total);
        if (total > 0) bytes.reserve(static_cast<size_t>(total));
      },
      [&](std::string_view chunk) {
        bytes.insert(bytes.end(), chunk.begin(), chunk.end());
        load.got.store(static_cast<int64_t>(bytes.size()), std::memory_order_relaxed);
        return true;
      });
  return bytes;
}

// The download behind a streamed file, on its own thread: into `feed`
// until done, failed or given up (feed->cancel). Asks for byte ranges, so a
// seek far ahead does not wait for the bytes before it (feed_download).
void download_feed(const std::string& url, const std::shared_ptr<xanatoo::ByteFeed>& feed) {
#if defined(XANATOO_HTTPS) && XANATOO_HTTPS
  xanatoo::feed_download(*feed, [&](int64_t from, const auto& head, const auto& body) {
    std::string err;
    int status = 0;
    bool stopped = false;
    const bool ok = xanatoo::https_get_follow_stream(
        url, xanatoo::user_agent("audio"),
        [&](const xanatoo::HttpStreamHeaders& h) {
          status = h.status;
          if (h.status < 200 || h.status >= 300) return false;
          xanatoo::FeedHead fh;
          fh.total = h.content_length;
          if (h.status == 206) {
            const auto r = xanatoo::parse_content_range(h.content_range);
            if (!r || r->first != from) return false;
            fh.start = r->first;
            fh.total = r->total;
            fh.ranged = true;
          }
          fh.content_type = h.content_type;
          fh.etag = h.etag;
          fh.last_modified = h.last_modified;
          return head(fh) || !(stopped = true);
        },
        [&](std::string_view bytes) { return body(bytes) || !(stopped = true); }, &err,
        nullptr, "Range: bytes=" + std::to_string(from) + "-\r\n");
    if (ok || stopped || feed->cancel.load()) return std::string();
    if (status != 0 && (status < 200 || status >= 300)) return "HTTP " + std::to_string(status);
    if (status == 206) return std::string("bad Content-Range");
    return err.empty() ? std::string("download failed") : err;
  });
#else
  (void)url;
  xanatoo::feed_end(*feed, "this build has no HTTP client");
#endif
}

#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
// The media cache (cache_dir()/media/): this much in all, half of it for
// one file.
constexpr int64_t kMediaCacheBytes = int64_t(2) << 30;

// A feed of a remote file with its download started: in the media cache,
// else (no cache directory, or the file busy) in memory when `memory`.
std::shared_ptr<xanatoo::ByteFeed> start_feed(const std::string& url, bool memory) {
  std::shared_ptr<xanatoo::ByteFeed> feed;
  if (const std::string cache = xanatoo::cache_dir(); !cache.empty())
    feed = xanatoo::feed_open_cached(cache + "/media", url, kMediaCacheBytes);
  if (feed) {
    feed->limit = kMediaCacheBytes / 2;
  } else if (memory) {
    feed = std::make_shared<xanatoo::ByteFeed>();
    feed->limit = kMaxAudioBytes;
  } else {
    return nullptr;
  }
  std::thread([url, feed] { download_feed(url, feed); }).detach();
  return feed;
}
#endif

void run_load(const std::string& url, AudioLoad& load) {
  std::vector<uint8_t> bytes =
      remote(url) ? download(url, load) : read_local(url, load.error);
  if (!load.error.empty() || load.cancel.load()) return;
  load.dec = xanatoo::open_audio(std::move(bytes), &load.error);
}

// Monotonic seconds, for the clock of video without sound.
double now_s() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void close_player(AudioPlayer& a) {
  if (a.visit) xanatoo::store_visit_leave(a.visit, xanatoo::now_epoch_ms());
  a.visit = 0;
  if (a.load) a.load->cancel.store(true);
  a.load.reset();
  if (a.feed) a.feed->cancel.store(true);  // its download stops
  a.feed.reset();
  a.play.reset();  // closes the device (the callback has stopped) first
  if (a.media) xanatoo::media_stop(*a.media);  // its worker leaves on its own
  a.media.reset();
  if (a.video_tex) SDL_DestroyTexture(a.video_tex);
  a.video_tex = nullptr;
  a.tex_w = a.tex_h = 0;
}

// The download a streamed built-in file plays from (null when there is
// none, or the backend took the address over).
xanatoo::ByteFeed* live_feed(const AudioPlayer& a) {
  return a.feed && !a.feed->cancel.load(std::memory_order_relaxed) ? a.feed.get() : nullptr;
}

// How much of a download is here (0..1); -1 unknown.
double loaded_fraction(int64_t got, int64_t total) {
  return total > 0 ? std::clamp(static_cast<double>(got) / total, 0.0, 1.0) : -1;
}

// Something is open: sound, pictures or both.
bool have_media(const AudioPlayer& a) {
  return a.play || (a.media && a.media->opened.load() && a.state != AudioState::Loading &&
                    a.state != AudioState::Failed);
}

// Room for a video's picture at its size (up to 800 pixels wide), when the
// panel floats free.
void fit_panel_to_video(App& app);

// The backend's file is open: open the sound device, start the clock.
void start_media(App& app) {
  AudioPlayer& a = app.audio;
  xanatoo::MediaPlayback& m = *a.media;
  {
    std::lock_guard lock(m.m);
    if (m.failed) {
      if (a.feed) a.feed->cancel.store(true);  // no use downloading on
      a.state = AudioState::Failed;
      a.error = m.error;
      flash_status(app, "Cannot play " + a.title + ": " + a.error);
      return;
    }
  }
  const xanatoo::MediaInfo& info = m.info;
  if (!info.title.empty()) a.title = info.title;
  a.duration = info.duration;
  a.format = info.format;
  if (info.has_audio) {
    auto s = std::make_shared<AudioShared>();
    s->media = a.media;
    s->channels = info.channels;
    s->rate = info.rate;
    s->gain.store(gain_of(a.volume));
    if (!xanatoo::sdl::open_audio_out(s->out, s->rate, s->channels, fill, s.get())) {
      a.state = AudioState::Failed;
      a.error = std::string("no sound device: ") + SDL_GetError();
      flash_status(app, "Cannot play " + a.title + ": " + a.error);
      return;
    }
    a.play = std::move(s);
    xanatoo::sdl::pause_audio_out(a.play->out, false);
  }
  if (info.has_video) {
    a.video_w = info.width;
    a.video_h = info.height;
    if (info.frame_rate > 0) a.frame_dt = 1.0 / info.frame_rate;
    fit_panel_to_video(app);
  }
  xanatoo::media_set_paused(m, false, now_s());
  a.state = AudioState::Playing;
  if (a.start_at > 0) audio_seek(app, a.start_at);
  a.start_at = -1;
  std::cerr << "media: " << a.url << " (" << info.format << ", "
            << xanatoo::format_audio_time(a.duration) << ", "
            << (info.builtin ? std::string("built-in") : xanatoo::media_backend_name())
            << (!live_feed(a)                       ? ""
                : !info.builtin                     ? ", through the media cache"
                : xanatoo::feed_complete(*a.feed) ? ", from the media cache"
                                                    : ", streaming")
            << ")\n";
}

// The picture due now into the texture.
void show_frame(App& app) {
  AudioPlayer& a = app.audio;
  xanatoo::VideoFrame f;
  if (!xanatoo::media_take_frame(*a.media, xanatoo::media_clock(*a.media, now_s()), f) ||
      f.w <= 0 || f.h <= 0)
    return;
  // Pictures one after the other give the rate when the file did not.
  if (const double d = f.pts - a.frame_pts; a.frame_pts >= 0 && d > 0.001 && d < 0.5 &&
                                            a.media->info.frame_rate <= 0)
    a.frame_dt = a.frame_dt > 0 ? std::min(a.frame_dt, d) : d;
  a.frame_pts = f.pts;
  a.step_to = -1;
  if (!a.video_tex || a.tex_w != f.w || a.tex_h != f.h) {
    if (a.video_tex) SDL_DestroyTexture(a.video_tex);
    a.video_tex = SDL_CreateTexture(app.sdl, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
                                    f.w, f.h);
    a.tex_w = a.video_tex ? f.w : 0;
    a.tex_h = a.video_tex ? f.h : 0;
    if (!a.video_tex) {
      std::cerr << "media: no video texture: " << SDL_GetError() << "\n";
      return;
    }
  }
  SDL_UpdateYUVTexture(a.video_tex, nullptr, f.y(), f.w, f.u(), f.cw(), f.v(), f.cw());
}

void start_playing(App& app) {
  AudioPlayer& a = app.audio;
  auto s = std::make_shared<AudioShared>();
  s->dec = std::move(a.load->dec);
  a.load.reset();
  s->channels = xanatoo::audio_channels(*s->dec);
  s->rate = xanatoo::audio_rate(*s->dec);
  s->frames = xanatoo::audio_frames(*s->dec);
  const std::string t = xanatoo::audio_title(*s->dec);
  if (!t.empty()) a.title = t;
  a.duration = s->rate > 0 ? static_cast<double>(s->frames) / s->rate : 0.0;
  a.format = xanatoo::audio_describe(*s->dec);
  s->gain.store(gain_of(a.volume));
  if (!xanatoo::sdl::open_audio_out(s->out, s->rate, s->channels, fill, s.get())) {
    a.state = AudioState::Failed;
    a.error = std::string("no sound device: ") + SDL_GetError();
    flash_status(app, "Cannot play " + a.title + ": " + a.error);
    return;
  }
  a.play = std::move(s);
  if (a.start_at > 0) audio_seek(app, a.start_at);
  a.start_at = -1;
  xanatoo::sdl::pause_audio_out(a.play->out, false);
  a.state = AudioState::Playing;
  std::cerr << "audio: " << a.url << " ("
            << xanatoo::audio_format_name(xanatoo::audio_format(*a.play->dec)) << ", "
            << a.play->rate << " Hz, " << a.play->channels << " ch, "
            << xanatoo::format_audio_time(a.duration) << ")\n";
}

// The panel's parts, placed in its client rect (shared by draw, click and
// hover: the same hit-test as the action).
enum class MediaPart : uint8_t {
  None, Video, Back, Toggle, Forward, Stop, AB, Loop, Seek, Volume, FileMark, MarkA, MarkB,
  Fullscreen
};

// Fullscreen: the controls hide this long after the pointer last moved.
constexpr double kControlsLinger = 1.0;

// The shortest A–B section (seconds).
constexpr double kMinSection = 0.2;
// The section the A–B button sets around the current position (seconds).
constexpr double kNewSection = 10;

struct MediaLayout {
  SDL_Rect client{};
  SDL_Rect video{};  // the picture's area above the controls (w = 0: none)
  int controls_h = 0;
  int pad = 0;
  int title_y = 0, info_y = 0, times_y = 0;  // text baselines
  // Fullscreen: the strip of controls over the picture's bottom (w = 0: the
  // panel, where they are under it).
  SDL_Rect controls{};
  SDL_Rect seek{}, back{}, toggle{}, forward{}, stop{}, ab{}, loop{}, volume{};
  SDL_Rect full{};  // the fullscreen button, right end of the row (w = 0: no video)
  SDL_Rect file_mark{};  // ribbon right of the title while bookmarked (w = 0: none)
  // Tabs under the seek bar to drag the A / B markers by (w = 0: none).
  SDL_Rect mark_a{}, mark_b{};
  int volume_label_x = 0, buttons_y = 0;
};

MediaLayout media_layout(const App& app) {
  MediaLayout L;
  const bool fs = app.media_full.on;
  int cx = 0, cy = 0, cw = std::max(1, app.win_w), ch = std::max(1, app.win_h);
  if (!fs) std::tie(cx, cy, cw, ch) = float_client_rect(const_cast<App&>(app), app.media_win);
  L.client = {cx, cy, cw, ch};
  const xanatoo::Font& f = app.ui_font;
  const int lh = f.line_height();
  L.pad = std::max(8, lh / 2);
  int y = cy + L.pad;
  L.title_y = y + f.ascender();
  y += lh + 2;
  L.info_y = y + f.ascender();
  y += lh + L.pad;
  const int bar_h = std::max(10, lh * 2 / 3);
  L.seek = {cx + L.pad, y, std::max(10, cw - 2 * L.pad), bar_h};
  y += bar_h + 4;
  // While the A–B markers are on, their tabs get a row under the bar.
  const AudioPlayer& a = app.audio;
  const bool tabs = a.ab && a.duration > 0 && a.state != AudioState::Idle &&
                    a.state != AudioState::Loading && a.state != AudioState::Failed;
  const int tab_w = std::max(12, lh), tab_h = std::max(12, lh);
  if (tabs) y += tab_h;
  L.times_y = y + f.ascender();
  y += lh + L.pad;
  const int b = lh + 14;
  L.buttons_y = y;
  int x = cx + L.pad;
  for (SDL_Rect* r : {&L.back, &L.toggle, &L.forward, &L.stop}) {
    *r = {x, y, r == &L.toggle ? b + b / 2 : b, b};
    x += r->w + 6;
  }
  L.stop.x += 6;  // a little apart from the transport buttons
  L.ab = {L.stop.x + L.stop.w + 12, y,
          std::max(b, const_cast<App&>(app).ui_font.measure("A-B") + 12), b};
  L.loop = {L.ab.x + L.ab.w + 6, y, b, b};
  const int vx = L.loop.x + L.loop.w + 2 * L.pad;
  L.volume_label_x = vx;
  const int label_w = lh * 2;
  int row_end = cx + cw - L.pad;
  if (app.audio.video_w > 0) {
    L.full = {row_end - b, y, b, b};
    row_end = L.full.x - L.pad;
  }
  const int vw = std::min(160, row_end - (vx + label_w));
  L.volume = {vx + label_w, y + (b - bar_h) / 2, std::max(0, vw), bar_h};
  L.controls_h = y + b + L.pad - cy;
  if (tabs) {
    auto tab = [&](double t) {
      const int x = L.seek.x + static_cast<int>(std::lround(
                                   L.seek.w * std::clamp(t / a.duration, 0.0, 1.0)));
      return SDL_Rect{x - tab_w / 2, L.seek.y + L.seek.h + 4, tab_w, tab_h - 2};
    };
    L.mark_a = tab(a.ab_a);
    L.mark_b = tab(a.ab_b);
  }
  if (!app.audio.file_mark.empty() && app.audio.state != AudioState::Idle) {
    const int rw = std::max(8, lh / 2), rh = std::max(12, lh * 4 / 5);
    L.file_mark = {cx + cw - L.pad - rw, L.title_y - f.ascender() + (lh - rh) / 2, rw, rh};
  }
  // A video takes the room above the controls, which move down; fullscreen
  // it takes the whole window and the controls lie over its bottom.
  if (app.audio.video_w > 0 || fs) {
    const int vh = std::max(0, ch - L.controls_h);
    L.video = {cx, cy, cw, fs ? ch : vh};
    if (fs) L.controls = {cx, cy + vh, cw, ch - vh};
    for (SDL_Rect* r :
         {&L.seek, &L.back, &L.toggle, &L.forward, &L.stop, &L.ab, &L.loop, &L.volume,
          &L.full, &L.file_mark, &L.mark_a, &L.mark_b})
      r->y += vh;
    L.title_y += vh;
    L.info_y += vh;
    L.times_y += vh;
    L.buttons_y += vh;
  }
  // Controls hidden: the picture takes the whole panel, nothing else is there.
  if (media_bare(app)) {
    MediaLayout bare;
    bare.client = L.client;
    bare.video = L.client;
    bare.pad = L.pad;
    return bare;
  }
  return L;
}

bool inside(const SDL_Rect& r, int x, int y) {
  return r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Fullscreen: whether the controls are showing (pointer moved lately, not
// playing, a drag, or the pointer on them).
bool controls_shown(const App& app, const MediaLayout& L) {
  if (!app.media_full.on) return true;
  const AudioPlayer& a = app.audio;
  return a.state != AudioState::Playing || a.drag != AudioPlayer::Drag::None ||
         now_s() - app.media_full.moved_at < kControlsLinger ||
         inside(L.controls, static_cast<int>(app.mouse_x), static_cast<int>(app.mouse_y));
}

MediaPart media_part_at(const App& app, const MediaLayout& L, int x, int y) {
  const bool fs = app.media_full.on;
  if (!inside(L.client, x, y) ||
      (!fs && (!app.media_panel_open || float_shaded(app.media_win))))
    return MediaPart::None;
  if ((fs && !controls_shown(app, L)) || media_bare(app)) return MediaPart::Video;
  // Bars take a few pixels more height: they are thin.
  auto grow = [](SDL_Rect r) { return SDL_Rect{r.x - 2, r.y - 6, r.w + 4, r.h + 12}; };
  if (inside(L.full, x, y)) return MediaPart::Fullscreen;
  if (inside(L.back, x, y)) return MediaPart::Back;
  if (inside(L.toggle, x, y)) return MediaPart::Toggle;
  if (inside(L.forward, x, y)) return MediaPart::Forward;
  if (inside(L.stop, x, y)) return MediaPart::Stop;
  if (inside(L.ab, x, y)) return MediaPart::AB;
  if (inside(L.loop, x, y)) return MediaPart::Loop;
  if (inside(L.file_mark, x, y)) return MediaPart::FileMark;
  // The marker tabs before the bar (its grown rect reaches them); when they
  // overlap, the one whose centre is nearer.
  {
    const bool in_a = inside(L.mark_a, x, y), in_b = inside(L.mark_b, x, y);
    if (in_a && in_b)
      return std::abs(x - (L.mark_a.x + L.mark_a.w / 2)) <
                     std::abs(x - (L.mark_b.x + L.mark_b.w / 2))
                 ? MediaPart::MarkA
                 : MediaPart::MarkB;
    if (in_a) return MediaPart::MarkA;
    if (in_b) return MediaPart::MarkB;
  }
  if (inside(grow(L.seek), x, y)) return MediaPart::Seek;
  if (L.volume.w > 0 && inside(grow(L.volume), x, y)) return MediaPart::Volume;
  if (inside(L.video, x, y) && !inside(L.controls, x, y)) return MediaPart::Video;
  return MediaPart::None;
}

double frac_in(const SDL_Rect& r, int x) {
  return std::clamp(static_cast<double>(x - r.x) / std::max(1, r.w), 0.0, 1.0);
}

// Bookmarks are shown and placed only once the length is known.
bool seekable(const AudioPlayer& a) {
  return a.duration > 0 && a.state != AudioState::Loading && a.state != AudioState::Failed &&
         a.state != AudioState::Idle;
}

// x of position `t` on the seek bar.
int seek_x(const SDL_Rect& bar, double t, double duration) {
  return bar.x + static_cast<int>(std::lround(bar.w * std::clamp(t / duration, 0.0, 1.0)));
}

// The position bookmark drawn nearest to x, within a few pixels.
const AudioPlayer::Mark* mark_near(const AudioPlayer& a, const SDL_Rect& bar, int x) {
  if (!seekable(a)) return nullptr;
  const AudioPlayer::Mark* best = nullptr;
  int best_d = 5;
  for (const AudioPlayer::Mark& m : a.marks)
    if (const int d = std::abs(seek_x(bar, m.t, a.duration) - x); d < best_d) {
      best_d = d;
      best = &m;
    }
  return best;
}

// The shortest bookmarked section (`#t=a,b`) over x on the seek bar.
const AudioPlayer::Mark* section_at(const AudioPlayer& a, const SDL_Rect& bar, int x) {
  if (!seekable(a)) return nullptr;
  const AudioPlayer::Mark* best = nullptr;
  for (const AudioPlayer::Mark& m : a.marks)
    if (m.end > m.t && x >= seek_x(bar, m.t, a.duration) - 2 &&
        x <= seek_x(bar, m.end, a.duration) + 2 && (!best || m.end - m.t < best->end - best->t))
      best = &m;
  return best;
}

// Sets the A–B markers to a section (on).
void set_section(AudioPlayer& a, double from, double to) {
  if (a.duration > 0) to = std::min(to, a.duration);
  from = std::max(0.0, from);
  if (to - from < kMinSection) to = from + kMinSection;
  a.ab = true;
  a.ab_a = from;
  a.ab_b = to;
  a.at_b = false;
}

// "0:10–0:20".
std::string section_text(double from, double to) {
  return xanatoo::format_audio_time(from) + "\xE2\x80\x93" + xanatoo::format_audio_time(to);
}

// A bookmark ribbon (as in the document margin): body, notch at the foot.
void ribbon(App& app, const SDL_Rect& r, xanatoo::Rgba c, Uint8 alpha) {
  SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
  SDL_SetRenderDrawColor(app.sdl, c.r, c.g, c.b, alpha);
  const int notch = std::max(3, r.w / 2);
  const SDL_Rect body{r.x, r.y, r.w, r.h - notch};
  xanatoo::sdl::fill_rect(app.sdl, &body);
  for (int k = 0; k < notch; ++k) {
    const int yy = r.y + r.h - notch + k;
    const int leg = std::max(1, r.w / 2 - k);
    xanatoo::sdl::draw_line(app.sdl, r.x, yy, r.x + leg - 1, yy);
    xanatoo::sdl::draw_line(app.sdl, r.x + r.w - leg, yy, r.x + r.w - 1, yy);
  }
}

// Finds this file's bookmarks again when they or the file change.
void update_media_marks(App& app) {
  AudioPlayer& a = app.audio;
  if (a.marks_valid && a.marks_gen == app.bookmarks_gen && a.marks_url == a.url) return;
  a.marks_valid = true;
  a.marks_gen = app.bookmarks_gen;
  a.marks_url = a.url;
  a.marks.clear();
  a.file_mark.clear();
  if (a.url.empty()) return;
  const std::string self = canonical_address(a.url);
  for (const xanatoo::Bookmark& b : app.bookmarks) {
    const xanatoo::MediaAddress m = xanatoo::split_media_address(b.address);
    if (!xanatoo::is_playable_url(m.base) || canonical_address(m.base) != self) continue;
    if (m.start)
      a.marks.push_back({*m.start, m.end ? *m.end : -1.0, b.address});
    else if (a.file_mark.empty())
      a.file_mark = b.address;
  }
  std::sort(a.marks.begin(), a.marks.end(),
            [](const AudioPlayer::Mark& x, const AudioPlayer::Mark& y) { return x.t < y.t; });
}

// A triangle pointing right (dir = 1) or left (-1), `s` high, from x0.
void triangle(App& app, int x0, int y0, int s, int dir, xanatoo::theme::Rgb ink) {
  const int len = s * 9 / 10;
  for (int y = 0; y < s; ++y) {
    const int half = std::min(y, s - 1 - y);
    const int w = std::max(1, half * 2 * len / s);
    if (dir > 0)
      xanatoo::theme::hline(app.sdl, x0, x0 + w, y0 + y, ink);
    else
      xanatoo::theme::hline(app.sdl, x0 + len - w, x0 + len, y0 + y, ink);
  }
}

// `playing`: the toggle shows pause; A–B and Loop are pressed in (on); the
// fullscreen button shows corners pointing in (back to the panel).
void draw_button(App& app, const SDL_Rect& r, MediaPart part, bool hot, bool enabled,
                 bool playing) {
  namespace th = xanatoo::theme;
  const th::Palette& p = th::palette();
  const bool down = playing && (part == MediaPart::AB || part == MediaPart::Loop);
  th::fill(app.sdl, r, down ? p.accent_soft : hot && enabled ? p.face_light : p.face);
  th::bevel(app.sdl, r, down ? th::Bevel::Sunken : th::Bevel::Raised);
  const th::Rgb ink = enabled ? p.text : p.text_dim;
  const int s = std::max(6, r.h * 2 / 5);
  const int cx = r.x + r.w / 2, y0 = r.y + (r.h - s) / 2;
  switch (part) {
    case MediaPart::Stop:
      th::fill(app.sdl, {cx - s / 2, y0, s, s}, ink);
      break;
    case MediaPart::Toggle:
      if (playing) {
        const int bw = std::max(2, s / 3);
        th::fill(app.sdl, {cx - s / 2, y0, bw, s}, ink);
        th::fill(app.sdl, {cx + s / 2 - bw, y0, bw, s}, ink);
      } else {
        triangle(app, cx - s * 2 / 5, y0, s, 1, ink);
      }
      break;
    case MediaPart::Back:
      triangle(app, cx - s * 9 / 10, y0, s, -1, ink);
      triangle(app, cx, y0, s, -1, ink);
      break;
    case MediaPart::Forward:
      triangle(app, cx - s * 9 / 10, y0, s, 1, ink);
      triangle(app, cx, y0, s, 1, ink);
      break;
    case MediaPart::AB: {
      xanatoo::Font& f = app.ui_font;
      const std::string label = "A-B";
      app.renderer.draw_text(f, static_cast<float>(cx - f.measure(label) / 2),
                             static_cast<float>(r.y + (r.h - f.line_height()) / 2 + f.ascender()),
                             label, ink.r, ink.g, ink.b);
      break;
    }
    case MediaPart::Loop: {
      // A closed loop of two strokes with a gap top right, an arrow into it.
      const int lw = s * 2, lh = s, t = std::max(2, s / 6);
      const int x0 = cx - lw / 2, y1 = y0, x1 = x0 + lw, y2 = y1 + lh;
      const int gap = x0 + lw / 2;
      th::fill(app.sdl, {x0, y1, gap - x0, t}, ink);
      th::fill(app.sdl, {x1 - t, y1 + t + 1, t, y2 - y1 - t - 1}, ink);
      th::fill(app.sdl, {x0, y2 - t, lw, t}, ink);
      th::fill(app.sdl, {x0, y1, t, lh}, ink);
      const int ah = std::max(7, s * 2 / 3);
      triangle(app, gap, y1 + t / 2 - ah / 2, ah, 1, ink);
      break;
    }
    case MediaPart::Fullscreen: {
      // Four corners of a frame, pointing out (enter) or in (leave).
      // Pointing in, the arms are shorter: the corners keep apart.
      const int fw = s * 3 / 2, fh = s, t = std::max(2, s / 6);
      const int arm = playing ? std::max(3, fh * 3 / 8) : std::max(3, s / 2);
      const int x0 = cx - fw / 2, x1 = x0 + fw;
      for (int corner = 0; corner < 4; ++corner) {
        const bool right = corner & 1, bottom = corner & 2;
        // Out: the corner at the frame's corner, arms along its edges. In:
        // the corner pulled to the middle, arms reaching back outwards.
        int px = right ? x1 - t : x0, py = bottom ? y0 + fh - t : y0;
        int hx = right ? x1 - arm : x0, vy = bottom ? y0 + fh - arm : y0;
        if (playing) {
          px = right ? x1 - arm : x0 + arm - t;
          py = bottom ? y0 + fh - arm : y0 + arm - t;
          hx = right ? px : x0;
          vy = bottom ? py : y0;
        }
        th::fill(app.sdl, {hx, py, arm, t}, ink);
        th::fill(app.sdl, {px, vy, t, arm}, ink);
      }
      break;
    }
    default: break;
  }
}

// A marker's tab under the seek bar: a raised flag with its letter.
void draw_marker_tab(App& app, const SDL_Rect& r, const char* letter, bool hot) {
  namespace th = xanatoo::theme;
  const th::Palette& p = th::palette();
  th::fill(app.sdl, r, hot ? p.accent_soft : p.face_light);
  th::bevel(app.sdl, r, th::Bevel::Raised);
  xanatoo::Font& f = app.ui_font_bold;
  app.renderer.draw_text(f, static_cast<float>(r.x + (r.w - f.measure(letter)) / 2 + 1),
                         static_cast<float>(r.y + (r.h - f.line_height()) / 2 + f.ascender()),
                         letter, p.text.r, p.text.g, p.text.b);
}

// The raised knob of a bar at `frac`.
void draw_knob(App& app, const SDL_Rect& r, double frac) {
  namespace th = xanatoo::theme;
  const int w = static_cast<int>(std::lround(r.w * std::clamp(frac, 0.0, 1.0)));
  const int kw = std::max(6, r.h * 2 / 3);
  const SDL_Rect k{std::clamp(r.x + w - kw / 2, r.x - kw / 2, r.x + r.w - kw / 2), r.y - 3, kw,
                   r.h + 6};
  th::fill(app.sdl, k, th::palette().face_light);
  th::bevel(app.sdl, k, th::Bevel::Raised);
}

// A sunken trough filled to `frac`, with a knob at the end when `knob`.
// `loaded` (0..1): how much of the file is here, shaded behind the fill.
void draw_bar(App& app, const SDL_Rect& r, double frac, xanatoo::theme::Rgb fillc, bool knob,
              double loaded = 0) {
  namespace th = xanatoo::theme;
  const th::Palette& p = th::palette();
  th::fill(app.sdl, r, p.field);
  if (const int lw = static_cast<int>(std::lround(r.w * std::clamp(loaded, 0.0, 1.0))); lw > 0)
    th::fill(app.sdl, {r.x, r.y, lw, r.h}, p.face_dark);
  const int w = static_cast<int>(std::lround(r.w * std::clamp(frac, 0.0, 1.0)));
  if (w > 0) th::fill(app.sdl, {r.x, r.y, w, r.h}, fillc);
  th::bevel(app.sdl, r, th::Bevel::Sunken);
  if (knob) draw_knob(app, r, frac);
}

void apply_drag(App& app, int x) {
  AudioPlayer& a = app.audio;
  const MediaLayout L = media_layout(app);
  if (a.drag == AudioPlayer::Drag::Seek && a.duration > 0)
    audio_seek(app, frac_in(L.seek, x) * a.duration);
  else if (a.drag == AudioPlayer::Drag::Volume)
    audio_set_volume(app, static_cast<float>(frac_in(L.volume, x)));
  else if (a.drag == AudioPlayer::Drag::MarkA && a.ab && a.duration > 0)
    a.ab_a = std::clamp(frac_in(L.seek, x) * a.duration, 0.0, std::max(0.0, a.ab_b - kMinSection));
  else if (a.drag == AudioPlayer::Drag::MarkB && a.ab && a.duration > 0) {
    a.ab_b = std::clamp(frac_in(L.seek, x) * a.duration, a.ab_a + kMinSection,
                        std::max(a.ab_a + kMinSection, a.duration));
    // Moved behind the playing position: the section ends there now.
    if (audio_position(app) > a.ab_b) a.last_pos = a.ab_b - 0.001;
  }
  a.drag_x = x;
}

}  // namespace

bool play_audio(App& app, const std::string& address, std::string_view mime,
                std::string_view name) {
  // `#t=30` (Media Fragments): where to start; the rest is the file.
  const xanatoo::MediaAddress split = xanatoo::split_media_address(address);
  const std::string& url_in = split.base;
  std::string url = url_in;
  if (xanatoo::is_file_url(url)) {
    url = xanatoo::file_url_path(url);
    if (url.empty()) {
      flash_status(app, "Not a local file URL: " + url_in);
      return false;
    }
  }
  AudioPlayer& a = app.audio;
  // The file in the player already: only go to the position (no reload).
  if (split.start && a.url == url && have_media(a) && a.state != AudioState::Failed) {
    if (a.visit) xanatoo::store_visit_leave(a.visit, xanatoo::now_epoch_ms());
    a.visit = 0;
    if (split.end) set_section(a, *split.start, *split.end);
    audio_seek(app, *split.start);
    if (a.state != AudioState::Playing) audio_toggle(app);
    app.media_panel_open = true;
    flash_status(app, a.title + " at " + xanatoo::format_audio_time(*split.start));
    return true;
  }
  if (split.start && a.url == url && a.state == AudioState::Loading) {
    a.start_at = *split.start;  // still opening: start there
    if (split.end) set_section(a, *split.start, *split.end);
    return true;
  }
  close_player(a);
  const float volume = a.volume;
  const bool loop = a.loop;
  a = {};
  a.volume = volume;
  a.loop = loop;
  a.url = url;
  a.mime = mime;
  if (split.start) a.start_at = *split.start;
  // `#t=a,b`: a section, played from A to B (Media Fragments).
  if (split.end) set_section(a, *split.start, *split.end);
  a.title = name.empty() ? xanatoo::audio_name_of(url) : std::string(name);
  a.state = AudioState::Loading;
  const bool backend = xanatoo::media_backend_available();
  if (backend && (xanatoo::is_hls_url(url) ||
                  (mime.empty() ? xanatoo::needs_media_backend(url)
                                : xanatoo::type_needs_media_backend(mime)))) {
    xanatoo::MediaOptions options;
    options.hls = xanatoo::is_hls_type(mime);
    options.user_agent = xanatoo::user_agent("video");
#if !defined(__EMSCRIPTEN__)
    // Through the media cache, so what arrived stays (seeks back, replays).
    if (remote(url) && !options.hls && !xanatoo::is_hls_url(url) &&
        xanatoo::media_backend_reads_feeds())
      a.feed = options.feed = start_feed(url, false);
#endif
    a.media = xanatoo::media_start(url, std::move(options));
    app.media_panel_open = true;
    flash_status(app, "Opening " + a.title);
    return true;
  }
#if !defined(__EMSCRIPTEN__) || defined(__EMSCRIPTEN_PTHREADS__)
  // A remote file of a built-in format plays while it downloads: the
  // download fills a feed, the pipe's worker decodes from it.
  if (remote(url)) {
    auto feed = start_feed(url, true);
    xanatoo::MediaOptions options;
    options.user_agent = xanatoo::user_agent("audio");
    options.open = [url, feed, backend](const xanatoo::MediaOptions& o, std::string* error) {
      // What only a backend streams (Ogg Vorbis / Opus, or the type said
      // MP3 and it is not) goes there, from the address itself.
      xanatoo::FeedWait wait;
      wait.cancel = o.cancel;
      const xanatoo::AudioFormat f = xanatoo::feed_sniff(*feed, wait);
      if (backend && !xanatoo::audio_streamable(f) && !o.cancel->load()) {
        xanatoo::MediaOptions through = o;
        if (xanatoo::media_backend_reads_feeds())
          through.feed = feed;  // on with what arrived
        else
          feed->cancel.store(true);
        return xanatoo::open_media(url, through, error);
      }
      return xanatoo::open_audio_stream(feed, o, error);
    };
    a.feed = feed;
    a.media = xanatoo::media_start(url, std::move(options));
    app.media_panel_open = true;
    flash_status(app, "Opening " + a.title);
    return true;
  }
#endif
  a.load = std::make_shared<AudioLoad>();
#if defined(__EMSCRIPTEN__) && !defined(__EMSCRIPTEN_PTHREADS__)
  run_load(url, *a.load);  // no threads in this build
  a.load->done.store(true, std::memory_order_release);
#else
  // Detached like page fetches: stopping only drops our reference.
  std::thread(
      [url](std::shared_ptr<AudioLoad> load) {
        run_load(url, *load);
        load->done.store(true, std::memory_order_release);
      },
      a.load)
      .detach();
#endif
  app.media_panel_open = true;  // the controls
  flash_status(app, (remote(url) ? "Downloading " : "Playing ") + a.title);
  return true;
}

void audio_toggle(App& app) {
  AudioPlayer& a = app.audio;
  if (a.state == AudioState::Failed && !a.url.empty()) {
    const int64_t visit = std::exchange(a.visit, 0);  // the same visit
    const std::string mime = a.mime;
    play_audio(app, a.url, mime);  // try again
    app.audio.visit = visit;
    return;
  }
  if (!have_media(a)) return;
  auto pause = [&](bool paused) {
    if (a.play) xanatoo::sdl::pause_audio_out(a.play->out, paused);
    if (a.media) xanatoo::media_set_paused(*a.media, paused, now_s());
    a.state = paused ? AudioState::Paused : AudioState::Playing;
  };
  switch (a.state) {
    case AudioState::Playing:
      pause(true);
      break;
    case AudioState::Ended:
      audio_seek(app, a.ab ? a.ab_a : 0);
      pause(false);
      break;
    case AudioState::Paused:
      if (a.at_b && a.ab) audio_seek(app, a.ab_a);  // the section again
      pause(false);
      break;
    default: break;
  }
}

void audio_stop(App& app) {
  close_player(app.audio);
  const float volume = app.audio.volume;
  const bool loop = app.audio.loop;
  app.audio = {};
  app.audio.volume = volume;
  app.audio.loop = loop;
}

void audio_toggle_ab(App& app) {
  AudioPlayer& a = app.audio;
  if (a.ab) {
    a.ab = false;
    a.at_b = false;
    return;
  }
  if (!seekable(a)) {
    flash_status(app, "A-B markers need a file of known length");
    return;
  }
  const double pos = audio_position(app);
  const double span = std::min(kNewSection, a.duration);
  double from = pos, to = pos + span;
  if (to > a.duration) {
    to = a.duration;
    from = std::max(0.0, to - span);
  }
  set_section(a, from, to);
  a.last_pos = pos;
  flash_status(app, "A-B " + section_text(a.ab_a, a.ab_b) + ": drag A and B on the seek bar");
}

void audio_toggle_loop(App& app) {
  AudioPlayer& a = app.audio;
  a.loop = !a.loop;
  flash_status(app, !a.loop ? std::string("Loop off")
                    : a.ab  ? "Looping the section " + section_text(a.ab_a, a.ab_b)
                            : std::string("Looping the whole file"));
}

void audio_set_volume(App& app, float volume) {
  app.audio.volume = std::clamp(volume, 0.f, 1.f);
  if (app.audio.play) app.audio.play->gain.store(gain_of(app.audio.volume));
}

void audio_seek(App& app, double seconds) {
  AudioPlayer& a = app.audio;
  a.at_b = false;
  a.last_pos = seconds;  // a jump past B is not B reached
  a.step_to = -1;
  if (a.media && have_media(a)) {
    xanatoo::media_seek(*a.media, seconds, now_s());
    if (a.play) xanatoo::sdl::flush_audio_out(a.play->out);
    if (a.state == AudioState::Ended) a.state = AudioState::Paused;
    return;
  }
  if (!a.play) return;
  AudioShared& s = *a.play;
  uint64_t frame = static_cast<uint64_t>(std::max(0.0, seconds) * s.rate);
  if (s.frames > 0) frame = std::min(frame, s.frames);
  {
    std::lock_guard lock(s.m);
    if (!xanatoo::audio_seek(*s.dec, frame)) return;
    s.pos.store(frame);
    s.ended.store(s.frames > 0 && frame >= s.frames);
  }
  xanatoo::sdl::flush_audio_out(s.out);
  if (a.state == AudioState::Ended && !s.ended.load()) a.state = AudioState::Paused;
}

void audio_skip(App& app, double delta) {
  if (have_media(app.audio)) audio_seek(app, audio_position(app) + delta);
}

void audio_step_frame(App& app, int dir) {
  AudioPlayer& a = app.audio;
  if (!a.media || !have_media(a) || a.video_w <= 0 || !seekable(a)) return;
  if (a.state == AudioState::Playing) audio_toggle(app);  // pause
  const double dt = a.frame_dt > 0 ? a.frame_dt : 1.0 / 25;
  const double at = a.step_to >= 0 ? a.step_to : a.frame_pts >= 0 ? a.frame_pts : audio_position(app);
  // A seek shows the first picture at or after its target: aim a quarter
  // picture before the one wanted (the worker drops those before it).
  const double want = std::max(0.0, at + (dir > 0 ? dt : -dt));
  if (a.duration > 0 && want >= a.duration) return;
  audio_seek(app, std::max(0.0, want - dt / 4));
  a.step_to = want;
}

double audio_position(const App& app) {
  const AudioPlayer& a = app.audio;
  if (a.media) {
    if (!have_media(a)) return 0;
    const double t = xanatoo::media_clock(*a.media, now_s());
    return a.duration > 0 ? std::min(t, a.duration) : t;
  }
  const AudioShared* s = app.audio.play.get();
  if (!s || s->rate <= 0) return 0;
  return static_cast<double>(s->pos.load(std::memory_order_relaxed)) / s->rate;
}

namespace {
// While the A–B markers are on: playing across B goes back to A (loop) or
// pauses there. Only a crossing counts, so a seek past B plays on.
void follow_section(App& app) {
  AudioPlayer& a = app.audio;
  if (a.state != AudioState::Playing) return;
  const double pos = audio_position(app);
  const double last = std::exchange(a.last_pos, pos);
  if (!a.ab || a.drag == AudioPlayer::Drag::Seek || last < 0 || last >= a.ab_b ||
      pos < a.ab_b)
    return;
  if (a.loop) {
    audio_seek(app, a.ab_a);
  } else {
    audio_toggle(app);  // pause
    a.at_b = true;
  }
}
}  // namespace

void update_audio(App& app) {
  AudioPlayer& a = app.audio;
  // Fullscreen ends with the video: stopped, the panel closed, or a file
  // without pictures in the player (a new one may still be opening).
  if (app.media_full.on &&
      (!app.media_panel_open || a.state == AudioState::Idle ||
       (a.video_w <= 0 && a.state != AudioState::Loading)))
    toggle_media_fullscreen(app);
  update_media_marks(app);
  // A drag in the panel follows the pointer until the button comes up.
  if (a.drag != AudioPlayer::Drag::None) {
    if (!xanatoo::sdl::left_button_down() || (!app.media_panel_open && !app.media_full.on)) {
      a.drag = AudioPlayer::Drag::None;
    } else if (const int x = static_cast<int>(app.mouse_x); x != a.drag_x) {
      apply_drag(app, x);
    }
  }
  if (a.load && a.load->done.load(std::memory_order_acquire)) {
    if (a.load->dec) {
      start_playing(app);
    } else {
      a.state = AudioState::Failed;
      a.error = a.load->error;
      a.load.reset();
      flash_status(app, "Cannot play " + a.title + ": " + a.error);
    }
  }
  if (a.media) {
    if (a.state == AudioState::Loading && a.media->opened.load(std::memory_order_acquire))
      start_media(app);
    if (have_media(a)) {
      {
        std::lock_guard lock(a.media->m);  // exact once a stream has all of it
        if (a.media->info.duration > 0) a.duration = a.media->info.duration;
      }
      if (a.video_w > 0) show_frame(app);
      if (a.state == AudioState::Playing && xanatoo::media_finished(*a.media)) {
        bool failed;
        {
          std::lock_guard lock(a.media->m);
          failed = a.media->failed;
        }
        if (!failed && a.loop) {
          audio_seek(app, a.ab ? a.ab_a : 0);  // round again, still playing
        } else {
          if (a.play) xanatoo::sdl::pause_audio_out(a.play->out, true);
          xanatoo::media_set_paused(*a.media, true, now_s());
          std::lock_guard lock(a.media->m);
          if (a.media->failed) {
            if (a.feed) a.feed->cancel.store(true);
            a.state = AudioState::Failed;
            a.error = a.media->error;
          } else {
            a.state = AudioState::Ended;
          }
        }
      }
      follow_section(app);
    }
    return;
  }
  if (a.play && a.state == AudioState::Playing &&
      a.play->ended.load(std::memory_order_acquire)) {
    if (a.loop) {
      audio_seek(app, a.ab ? a.ab_a : 0);  // the device keeps running
    } else {
      xanatoo::sdl::pause_audio_out(a.play->out, true);
      a.state = AudioState::Ended;
    }
  }
  if (a.play) follow_section(app);
}

void shutdown_audio(App& app) {
  if (app.media_full.on) toggle_media_fullscreen(app);
  audio_stop(app);
}

void toggle_media_panel(App& app) {
  if (app.media_panel_open) {
    close_media_panel(app);
    return;
  }
  app.media_panel_open = true;
  focus_panel(app, WindowRole::Media);
}

void close_media_panel(App& app) {
  app.media_panel_open = false;
  audio_stop(app);
}

namespace {
void fit_panel_to_video(App& app) {
  FloatWindow& w = app.media_win;
  const AudioPlayer& a = app.audio;
  if (w.dock != Dock::None || a.video_w <= 0 || a.video_h <= 0 || app.media_full.on) return;
  const MediaLayout L = media_layout(app);
  const int extra_w = w.w - L.client.w, extra_h = w.h - L.client.h;
  // Up to 800 pixels wide, and no lower than the window minus room for the
  // status line: a smaller picture rather than buttons out of reach.
  int vw = std::clamp(a.video_w, 320, 800);
  vw = std::min(vw, app.win_w - extra_w - 40);
  const int room_h = app.win_h - w.y - 48 - L.controls_h - extra_h;
  vw = std::max(160, std::min(vw, room_h * a.video_w / a.video_h));
  const int vh = vw * a.video_h / a.video_w;
  w.w = std::max(w.w, vw + extra_w);
  w.h = vh + L.controls_h + extra_h;
  w.x = std::clamp(w.x, 0, std::max(0, app.win_w - w.w));
  w.y = std::clamp(w.y, 0, std::max(0, app.win_h - 48 - w.h));
}
}  // namespace

void click_media_panel(App& app, int mx, int my) {
  AudioPlayer& a = app.audio;
  const MediaLayout L = media_layout(app);
  switch (media_part_at(app, L, mx, my)) {
    case MediaPart::None: break;
    case MediaPart::Back: audio_skip(app, -10); break;
    case MediaPart::Forward: audio_skip(app, 30); break;
    case MediaPart::Video: {
      // A double click goes fullscreen / back (and undoes the first
      // click's play / pause).
      const Uint32 now = xanatoo::sdl::ticks();
      Uint32& last = app.media_full.last_video_click;
      audio_toggle(app);
      if (last != 0 && now - last < 400) {
        last = 0;
        toggle_media_fullscreen(app);
      } else {
        last = now;
      }
      break;
    }
    case MediaPart::Toggle: audio_toggle(app); break;
    case MediaPart::Fullscreen: toggle_media_fullscreen(app); break;
    case MediaPart::Stop: audio_stop(app); break;
    case MediaPart::AB: audio_toggle_ab(app); break;
    case MediaPart::Loop: audio_toggle_loop(app); break;
    case MediaPart::MarkA:
    case MediaPart::MarkB:
      a.drag = media_part_at(app, L, mx, my) == MediaPart::MarkA ? AudioPlayer::Drag::MarkA
                                                                 : AudioPlayer::Drag::MarkB;
      a.drag_x = mx;  // moves only once the pointer does
      break;
    case MediaPart::Seek:
      if (have_media(a) && a.duration > 0) {
        a.drag = AudioPlayer::Drag::Seek;
        // A press on a bookmark's line goes to exactly its position; a
        // section's also sets the A–B markers to it.
        if (const AudioPlayer::Mark* m = mark_near(a, L.seek, mx)) {
          if (m->end > m->t) set_section(a, m->t, m->end);
          audio_seek(app, m->t);
          a.drag_x = mx;
        } else {
          apply_drag(app, mx);
        }
      }
      break;
    case MediaPart::Volume:
      a.drag = AudioPlayer::Drag::Volume;
      apply_drag(app, mx);
      break;
    case MediaPart::FileMark: break;
  }
}

bool right_click_media_panel(App& app, int mx, int my) {
  if (!app.media_full.on && (!app.media_panel_open || float_shaded(app.media_win)))
    return false;
  const MediaLayout L = media_layout(app);
  if (!inside(L.client, mx, my)) return false;
  AudioPlayer& a = app.audio;
  if (a.state == AudioState::Idle || a.url.empty()) return true;
  update_media_marks(app);
  MediaMenu& menu = app.media_menu;
  menu = {};
  menu.open = true;
  menu.t = audio_position(app);
  if (media_part_at(app, L, mx, my) == MediaPart::Seek && seekable(a)) {
    if (const AudioPlayer::Mark* m = mark_near(a, L.seek, mx)) {
      menu.t = m->t;
      menu.mark = m->address;
    } else {
      menu.t = frac_in(L.seek, mx) * a.duration;
    }
  }
  // A bookmark at the same tenth of a second is the same bookmark.
  if (menu.mark.empty()) {
    const std::string at = xanatoo::media_time_address("", menu.t);
    for (const AudioPlayer::Mark& m : a.marks)
      if (m.end < 0 && xanatoo::media_time_address("", m.t) == at) menu.mark = m.address;
  }
  // The section: a bookmarked one under the pointer, else the markers.
  const MediaPart part = media_part_at(app, L, mx, my);
  const AudioPlayer::Mark* sec = part == MediaPart::Seek ? section_at(a, L.seek, mx) : nullptr;
  if (sec) {
    menu.sec_a = sec->t;
    menu.sec_b = sec->end;
    menu.sec_mark = sec->address;
  } else if (a.ab && seekable(a)) {
    menu.sec_a = a.ab_a;
    menu.sec_b = a.ab_b;
    const std::string at = xanatoo::media_range_address("", a.ab_a, a.ab_b);
    for (const AudioPlayer::Mark& m : a.marks)
      if (m.end > m.t && xanatoo::media_range_address("", m.t, m.end) == at)
        menu.sec_mark = m.address;
  }
  app.ui.open_media_menu(static_cast<float>(mx), static_cast<float>(my));
  return true;
}

std::string canonical_media_address(std::string_view address) {
  const xanatoo::MediaAddress m = xanatoo::split_media_address(address);
  const std::string base = canonical_address(m.base);
  if (m.start && m.end) return xanatoo::media_range_address(base, *m.start, *m.end);
  return m.start ? xanatoo::media_time_address(base, *m.start) : base;
}

void media_action(App& app, const std::string& action) {
  MediaMenu menu = std::exchange(app.media_menu, {});
  AudioPlayer& a = app.audio;
  if (!menu.open || a.url.empty()) return;
  const std::string file = canonical_address(a.url);
  const std::string at = xanatoo::media_time_address(file, menu.t);
  const std::string when = xanatoo::format_audio_time(menu.t);
  if (action == "bookmark-time") {
    xanatoo::Bookmark b;
    b.address = at;
    b.title = a.title + " at " + when;
    b.created = today();
    add_bookmark(app, std::move(b));
  } else if (action == "unbookmark-time") {
    remove_bookmark(app, menu.mark.empty() ? at : menu.mark);
  } else if (action == "bookmark-file") {
    xanatoo::Bookmark b;
    b.address = file;
    b.title = a.title;
    b.created = today();
    add_bookmark(app, std::move(b));
  } else if (action == "unbookmark-file") {
    update_media_marks(app);
    remove_bookmark(app, a.file_mark.empty() ? file : a.file_mark);
  } else if (action == "seek") {
    if (seekable(a)) audio_seek(app, menu.t);
  } else if (action == "copy-time") {
    SDL_SetClipboardText(at.c_str());
    flash_status(app, "Copied " + at);
  } else if (action == "set-a" || action == "set-b") {
    if (!seekable(a)) return;
    const double t = std::clamp(menu.t, 0.0, a.duration);
    if (action == "set-a")
      set_section(a, t, a.ab && a.ab_b > t + kMinSection ? a.ab_b : t + kNewSection);
    else
      set_section(a, a.ab && a.ab_a < t - kMinSection ? a.ab_a : std::max(0.0, t - kNewSection), t);
    if (audio_position(app) > a.ab_b) a.last_pos = a.ab_b - 0.001;  // ends there now
  } else if (menu.sec_b > menu.sec_a) {
    const std::string sec = xanatoo::media_range_address(file, menu.sec_a, menu.sec_b);
    if (action == "bookmark-section") {
      xanatoo::Bookmark b;
      b.address = sec;
      b.title = a.title + " " + section_text(menu.sec_a, menu.sec_b);
      b.created = today();
      add_bookmark(app, std::move(b));
    } else if (action == "unbookmark-section") {
      remove_bookmark(app, menu.sec_mark.empty() ? sec : menu.sec_mark);
    } else if (action == "copy-section") {
      SDL_SetClipboardText(sec.c_str());
      flash_status(app, "Copied " + sec);
    } else if (action == "use-section" && seekable(a)) {
      set_section(a, menu.sec_a, menu.sec_b);
      audio_seek(app, a.ab_a);
    }
  }
}

std::string media_panel_hover(const App& app, int mx, int my) {
  const AudioPlayer& a = app.audio;
  const MediaLayout L = media_layout(app);
  switch (media_part_at(app, L, mx, my)) {
    case MediaPart::None: return {};
    case MediaPart::Back: return "Back 10 seconds";
    case MediaPart::Forward: return "Forward 30 seconds";
    case MediaPart::Video:
      return std::string(a.state == AudioState::Playing ? "Click to pause" : "Click to play") +
             (app.media_full.on ? ", double-click to leave fullscreen"
                                : ", double-click for fullscreen") +
             (media_bare(app) ? " (H: show the controls)" : "");
    case MediaPart::Fullscreen:
      return app.media_full.on ? "Leave fullscreen (Esc)" : "Fullscreen (F)";
    case MediaPart::Toggle:
      return a.state == AudioState::Playing ? "Pause"
             : a.state == AudioState::Failed ? "Try again"
                                              : "Play";
    case MediaPart::Stop: return "Stop";
    case MediaPart::AB:
      return a.ab ? "A-B markers on, " + section_text(a.ab_a, a.ab_b) + " (click: off)"
                  : std::string("A-B markers: mark a section to loop, bookmark or copy");
    case MediaPart::Loop:
      return std::string(a.loop ? "Loop on: " : "Loop: ") +
             (a.ab ? "the section " + section_text(a.ab_a, a.ab_b) : std::string("the whole file")) +
             (a.loop ? " (click: off)" : "");
    case MediaPart::MarkA:
      return "A at " + xanatoo::format_audio_time(a.ab_a) + " (drag to move)";
    case MediaPart::MarkB:
      return "B at " + xanatoo::format_audio_time(a.ab_b) + " (drag to move)";
    case MediaPart::Seek:
      if (const AudioPlayer::Mark* m = mark_near(a, L.seek, mx))
        return (m->end > m->t ? "Bookmarked section " + section_text(m->t, m->end)
                              : "Bookmark at " + xanatoo::format_audio_time(m->t)) +
               " (right-click: bookmark menu)";
      if (have_media(a) && a.duration > 0)
        return "Go to " + xanatoo::format_audio_time(frac_in(L.seek, mx) * a.duration) +
               " (right-click: bookmark menu)";
      return {};
    case MediaPart::FileMark: return "This file is bookmarked";
    case MediaPart::Volume:
      return "Volume " + std::to_string(static_cast<int>(std::lround(a.volume * 100))) + "%";
  }
  return {};
}

namespace {

// The picture, letterboxed in black at its display aspect.
void draw_media_video(App& app, const MediaLayout& L) {
  namespace th = xanatoo::theme;
  const AudioPlayer& a = app.audio;
  if (L.video.w <= 0 || L.video.h <= 0) return;
  th::fill(app.sdl, L.video, th::rgb(0, 0, 0));
  if (a.video_tex && a.video_w > 0 && a.video_h > 0) {
    const double s = std::min(double(L.video.w) / a.video_w, double(L.video.h) / a.video_h);
    const int w = std::max(1, int(a.video_w * s)), h = std::max(1, int(a.video_h * s));
    const SDL_Rect dst{L.video.x + (L.video.w - w) / 2, L.video.y + (L.video.h - h) / 2, w, h};
    xanatoo::sdl::copy(app.sdl, a.video_tex, nullptr, &dst);
  }
}

// Title, info line, seek bar, times, buttons and volume.
void draw_media_controls(App& app, const MediaLayout& L) {
  namespace th = xanatoo::theme;
  const th::Palette& p = th::palette();
  const AudioPlayer& a = app.audio;
  const SDL_Rect& c = L.client;
  const int text_w = std::max(0, c.w - 2 * L.pad);
  const float tx = static_cast<float>(c.x + L.pad);

  // Title and what it is (or why not).
  std::string title = a.title, info;
  th::Rgb info_ink = p.text_dim;
  switch (a.state) {
    case AudioState::Idle:
      title = "Nothing playing";
      info = xanatoo::media_backend_available()
                 ? "Open a link to a sound or video file."
                 : "Open a link to an MP3, FLAC, Ogg Vorbis or WAV file.";
      break;
    case AudioState::Loading: {
      const xanatoo::ByteFeed* feed = live_feed(a);
      const int64_t got = a.load ? a.load->got.load() : feed ? feed->got.load() : 0;
      const int64_t total = a.load ? a.load->total.load() : feed ? feed->total.load() : -1;
      info = total > 0 ? "Downloading " + std::to_string(got * 100 / total) + "%"
             : got > 0 ? "Downloading " + std::to_string(got >> 10) + " KiB"
             : a.media ? std::string("Opening\xE2\x80\xA6")
                       : std::string("Loading\xE2\x80\xA6");
      break;
    }
    case AudioState::Failed:
      info = a.error;
      info_ink = th::rgb(0x9A, 0x55, 0x10);
      break;
    default:
      info = a.format;
      // Playing, but the stream has not kept up (or a seek waits for it).
      if (a.state == AudioState::Playing && a.media && xanatoo::media_buffering(*a.media)) {
        info = "Buffering\xE2\x80\xA6";
        if (const xanatoo::ByteFeed* feed = live_feed(a)) {
          const double f = loaded_fraction(feed->got.load(), feed->total.load());
          if (f >= 0) info += " " + std::to_string(static_cast<int>(f * 100)) + "% loaded";
        }
      }
      break;
  }
  // Fullscreen has no status line: what the pointer is on shows here.
  if (app.media_full.on) {
    std::string over = media_panel_hover(app, static_cast<int>(app.mouse_x),
                                         static_cast<int>(app.mouse_y));
    if (!over.empty() && a.state != AudioState::Failed) {
      info = std::move(over);
      info_ink = p.text_dim;
    }
  }
  const int title_w = L.file_mark.w > 0 ? std::max(0, L.file_mark.x - 6 - (c.x + L.pad)) : text_w;
  app.renderer.draw_text(app.ui_font_bold, tx, static_cast<float>(L.title_y),
                         fit_text(app.ui_font_bold, title, title_w), p.text.r, p.text.g,
                         p.text.b);
  if (L.file_mark.w > 0) ribbon(app, L.file_mark, app.style.bookmark, 230);
  app.renderer.draw_text(app.ui_font, tx, static_cast<float>(L.info_y),
                         fit_text(app.ui_font, info, text_w), info_ink.r, info_ink.g,
                         info_ink.b);

  // Seek bar: played part (downloaded part while loading) and times.
  const double pos = audio_position(app);
  double frac = 0;
  if (a.load) {
    const int64_t got = a.load->got.load(), total = a.load->total.load();
    if (total > 0) frac = static_cast<double>(got) / total;
  } else if (have_media(a) && a.duration > 0) {
    frac = pos / a.duration;
  }
  double loaded = 0;  // a stream: how far it has been read ahead
  if (a.media && have_media(a) && a.duration > 0) {
    std::lock_guard lock(a.media->m);
    loaded = a.media->buffered / a.duration;
  }
  draw_bar(app, L.seek, frac, a.load ? p.face_dark : p.accent, false, loaded);
  // Bookmarked positions: a short green line across the bar each.
  if (seekable(a)) {
    const xanatoo::Rgba& bc = app.style.bookmark;
    SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.sdl, bc.r, bc.g, bc.b, 255);
    for (const AudioPlayer::Mark& m : a.marks) {
      const int x = std::min(seek_x(L.seek, m.t, a.duration), L.seek.x + L.seek.w - 2);
      const SDL_Rect line{x - 1, L.seek.y - 2, 2, L.seek.h + 4};
      xanatoo::sdl::fill_rect(app.sdl, &line);
      if (m.end > m.t) {  // a section: a strip along the top of the bar
        const int x2 = seek_x(L.seek, m.end, a.duration);
        const SDL_Rect strip{x, L.seek.y - 2, std::max(1, x2 - x), 3};
        xanatoo::sdl::fill_rect(app.sdl, &strip);
      }
    }
  }
  // The A–B section: shaded on the bar, a line at each end.
  if (L.mark_a.w > 0) {
    const int xa = seek_x(L.seek, a.ab_a, a.duration), xb = seek_x(L.seek, a.ab_b, a.duration);
    SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.sdl, p.accent.r, p.accent.g, p.accent.b, 80);
    const SDL_Rect span{xa, L.seek.y, std::max(1, xb - xa), L.seek.h};
    xanatoo::sdl::fill_rect(app.sdl, &span);
    // A bracket under the bar from A to B, down into the tabs.
    const int by = L.seek.y + L.seek.h + 1;
    th::fill(app.sdl, {xa, by, std::max(1, xb - xa), 2}, p.text);
    for (const int x : {xa, xb})
      th::fill(app.sdl, {std::min(x, L.seek.x + L.seek.w - 2) - 1, L.seek.y - 2, 2,
                         L.mark_a.y - L.seek.y + 2},
               p.text);
  }
  if (have_media(a) && a.duration > 0) draw_knob(app, L.seek, frac);
  if (have_media(a)) {
    const std::string left = xanatoo::format_audio_time(pos);
    const std::string right = a.duration > 0 ? xanatoo::format_audio_time(a.duration) : "";
    app.renderer.draw_text(app.ui_font, tx, static_cast<float>(L.times_y), left,
                           p.text_dim.r, p.text_dim.g, p.text_dim.b);
    app.renderer.draw_text(app.ui_font,
                           static_cast<float>(L.seek.x + L.seek.w - app.ui_font.measure(right)),
                           static_cast<float>(L.times_y), right, p.text_dim.r,
                           p.text_dim.g, p.text_dim.b);
  }
  // Marker tabs over the times (B first: A stays on top when they meet).
  const MediaPart hot = media_part_at(app, L, static_cast<int>(app.mouse_x),
                                      static_cast<int>(app.mouse_y));
  if (L.mark_a.w > 0) {
    draw_marker_tab(app, L.mark_b, "B",
                    hot == MediaPart::MarkB || a.drag == AudioPlayer::Drag::MarkB);
    draw_marker_tab(app, L.mark_a, "A",
                    hot == MediaPart::MarkA || a.drag == AudioPlayer::Drag::MarkA);
  }

  // Buttons and volume.
  const bool have = have_media(a);
  draw_button(app, L.back, MediaPart::Back, hot == MediaPart::Back, have, false);
  draw_button(app, L.toggle, MediaPart::Toggle, hot == MediaPart::Toggle,
              have || a.state == AudioState::Failed, a.state == AudioState::Playing);
  draw_button(app, L.forward, MediaPart::Forward, hot == MediaPart::Forward, have, false);
  draw_button(app, L.stop, MediaPart::Stop, hot == MediaPart::Stop,
              a.state != AudioState::Idle, false);
  draw_button(app, L.ab, MediaPart::AB, hot == MediaPart::AB, seekable(a), a.ab);
  draw_button(app, L.loop, MediaPart::Loop, hot == MediaPart::Loop, true, a.loop);
  if (L.full.w > 0)
    draw_button(app, L.full, MediaPart::Fullscreen, hot == MediaPart::Fullscreen, true,
                app.media_full.on);
  if (L.volume.w > 0) {
    const int ty = L.buttons_y + (L.back.h - app.ui_font.line_height()) / 2 +
                   app.ui_font.ascender();
    app.renderer.draw_text(app.ui_font, static_cast<float>(L.volume_label_x),
                           static_cast<float>(ty), "Vol", p.text_dim.r, p.text_dim.g,
                           p.text_dim.b);
    draw_bar(app, L.volume, a.volume, p.accent_soft, true);
  }
}

}  // namespace

void draw_media_panel(App& app) {
  if (!app.media_panel_open) return;
  FloatWindow& w = app.media_win;
  w.scroll = 0;
  if (!draw_floating_frame(app, w, "Media", float_active(app, w))) return;
  if (app.media_full.on) return;  // the picture is on the whole window
  const MediaLayout L = media_layout(app);
  xanatoo::sdl::set_clip_rect(app.sdl, &L.client);
  draw_media_video(app, L);
  if (!media_bare(app)) draw_media_controls(app, L);
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
}

void draw_media_fullscreen(App& app) {
  namespace th = xanatoo::theme;
  if (!app.media_full.on) return;
  const MediaLayout L = media_layout(app);
  xanatoo::sdl::set_clip_rect(app.sdl, &L.client);
  SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_NONE);
  th::fill(app.sdl, L.client, th::rgb(0, 0, 0));
  draw_media_video(app, L);
  const bool shown = controls_shown(app, L);
  if (shown) {
    // The panel's face, a little see-through, under the controls.
    const th::Rgb face = th::palette().face;
    SDL_SetRenderDrawBlendMode(app.sdl, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(app.sdl, face.r, face.g, face.b, 225);
    xanatoo::sdl::fill_rect(app.sdl, &L.controls);
    draw_media_controls(app, L);
  }
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
  // The pointer hides with the controls (shown again on leaving).
  if (app.media_full.cursor_hidden == shown) {
    app.media_full.cursor_hidden = !shown;
    xanatoo::sdl::show_cursor(shown);
  }
}

void toggle_media_fullscreen(App& app) {
  MediaFullscreen& f = app.media_full;
  if (f.on) {
    f.on = false;
    if (!f.window_was_fullscreen) xanatoo::sdl::set_fullscreen(app.window, false);
    if (f.cursor_hidden) xanatoo::sdl::show_cursor(true);
    f.cursor_hidden = false;
    app.audio.drag = AudioPlayer::Drag::None;
    return;
  }
  if (app.audio.video_w <= 0) {
    flash_status(app, "No video playing");
    return;
  }
  f.on = true;
  f.window_was_fullscreen = xanatoo::sdl::fullscreen(app.window);
  if (!f.window_was_fullscreen) xanatoo::sdl::set_fullscreen(app.window, true);
  f.moved_at = now_s();
  f.last_video_click = 0;
  app.audio.drag = AudioPlayer::Drag::None;
  app.media_panel_open = true;
}

bool media_bare(const App& app) {
  return app.media_controls_hidden && !app.media_full.on && app.audio.video_w > 0;
}

void toggle_media_controls(App& app) {
  if (!app.media_controls_hidden && app.audio.video_w <= 0) {
    flash_status(app, "No video playing");
    return;
  }
  app.media_controls_hidden = !app.media_controls_hidden;
  app.audio.drag = AudioPlayer::Drag::None;
  if (app.media_controls_hidden) flash_status(app, "Controls hidden (H or right-click to show)");
}

namespace {

// The player's keys, fullscreen and in the focused panel: F (and fullscreen
// Esc / F11) in / out of fullscreen, Space / K / P play-pause, Left / Right
// (J / L) 10 s (Shift: 60 s), ',' / '.' a picture back / on, Home / End,
// Up / Down and M the volume. False for any other key, or with Ctrl / Alt.
bool player_key(App& app, SDL_Keycode key, SDL_Keymod mod) {
  if (mod & (KMOD_CTRL | KMOD_ALT)) return false;
  const double step = (mod & KMOD_SHIFT) ? 60 : 10;
  AudioPlayer& a = app.audio;
  switch (key) {
    case SDLK_ESCAPE:
    case SDLK_F11:
      if (!app.media_full.on) return false;
      toggle_media_fullscreen(app);
      return true;
    case SDLK_f:
      toggle_media_fullscreen(app);
      return true;
    case SDLK_h:
      if (app.media_full.on) return false;
      toggle_media_controls(app);
      return true;
    case SDLK_SPACE:
    case SDLK_k:
    case SDLK_p:
      audio_toggle(app);
      return true;
    case SDLK_LEFT:
    case SDLK_j:
      audio_skip(app, -step);
      return true;
    case SDLK_RIGHT:
    case SDLK_l:
      audio_skip(app, step);
      return true;
    case SDLK_COMMA:
      audio_step_frame(app, -1);
      return true;
    case SDLK_PERIOD:
      audio_step_frame(app, 1);
      return true;
    case SDLK_HOME:
      if (have_media(a)) audio_seek(app, 0);
      return true;
    case SDLK_END:
      if (have_media(a) && a.duration > 0) audio_seek(app, std::max(0.0, a.duration - 1));
      return true;
    case SDLK_UP:
      audio_set_volume(app, a.volume + 0.1f);
      return true;
    case SDLK_DOWN:
      audio_set_volume(app, a.volume - 0.1f);
      return true;
    case SDLK_m:
      audio_set_volume(app, a.volume > 0 ? 0.f : 1.f);
      return true;
    default:
      return false;
  }
}

// Fullscreen: the player's keys, and all other plain keys are taken too
// (the hidden page gets none). With Ctrl / Alt (Ctrl+Q, …) they go on.
bool fullscreen_key(App& app, const SDL_Event& ev) {
  const SDL_Keymod mod = xanatoo::sdl::keymod(ev);
  if (mod & (KMOD_CTRL | KMOD_ALT)) return false;
  player_key(app, xanatoo::sdl::keycode(ev), mod);
  app.media_full.moved_at = now_s();  // show where it went
  return true;
}

}  // namespace

bool media_panel_key(App& app, const SDL_Event& ev) {
  if (app.media_full.on || !app.media_panel_open || float_shaded(app.media_win) ||
      app.key_focus != KeyFocus::Panel || app.panel_focus != WindowRole::Media)
    return false;
  if (!player_key(app, xanatoo::sdl::keycode(ev), xanatoo::sdl::keymod(ev))) return false;
  app.eat_text_input = true;  // Space / ',' / '.' type nothing
  return true;
}

bool media_fullscreen_event(App& app, const SDL_Event& ev) {
  if (!app.media_full.on) return false;
  MediaFullscreen& f = app.media_full;
  using K = xanatoo::sdl::EventKind;
  switch (xanatoo::sdl::event_kind(ev)) {
    case K::MouseMotion:
      app.mouse_x = static_cast<float>(ev.motion.x);
      app.mouse_y = static_cast<float>(ev.motion.y);
      f.moved_at = now_s();
      return true;
    case K::MouseButtonDown: {
      const int x = static_cast<int>(ev.button.x), y = static_cast<int>(ev.button.y);
      app.mouse_x = static_cast<float>(x);
      app.mouse_y = static_cast<float>(y);
      // A press while the controls are hidden only shows them.
      const MediaLayout L = media_layout(app);
      const bool shown = controls_shown(app, L) || !inside(L.controls, x, y);
      f.moved_at = now_s();
      if (ev.button.button == SDL_BUTTON_LEFT && shown)
        click_media_panel(app, x, y);
      else if (ev.button.button == SDL_BUTTON_RIGHT)
        right_click_media_panel(app, x, y);
      return true;
    }
    case K::MouseWheel:
      if (ev.wheel.y != 0)
        audio_set_volume(app, app.audio.volume + (ev.wheel.y > 0 ? 0.05f : -0.05f));
      f.moved_at = now_s();
      return true;
    case K::KeyDown:
      return fullscreen_key(app, ev);
    case K::MouseButtonUp:
    case K::KeyUp:
    case K::TextInput:
    case K::FingerDown:
    case K::FingerMotion:
    case K::FingerUp:
      return true;
    default:
      return false;
  }
}

}  // namespace xanatoo::app
