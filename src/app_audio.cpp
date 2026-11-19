// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Sound player (App::audio, docs/dev/CONTENT.md "Audio"). A link to an
// audio file loads it on a worker (local read or http(s) download, then
// open_audio, which scans the file for its length / seek table); the
// decoder then feeds SDL's audio callback (sdl::AudioOut) under a mutex. The
// controls are a well at the left of the status line: play / pause, a
// progress bar to click into, the title and time, and stop.

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>

#include "xanatoo/app.hpp"
#include "xanatoo/audio.hpp"
#include "xanatoo/file_url.hpp"
#include "xanatoo/theme.hpp"
#include "xanatoo/util.hpp"
#if defined(XANATOO_HTTPS) && XANATOO_HTTPS
#include "xanatoo/https.hpp"
#include "xanatoo/version.hpp"
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
    if (s->dec && !s->ended.load(std::memory_order_relaxed)) {
      got = xanatoo::audio_read(*s->dec, out, want);
      s->pos.fetch_add(got, std::memory_order_relaxed);
      if (got < want) s->ended.store(true, std::memory_order_release);
    }
  }
  std::fill(out + got * s->channels, out + want * s->channels, 0.f);
}

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

// An http(s) file into memory, with progress in `load`; errors in load.error.
std::vector<uint8_t> download(const std::string& url, AudioLoad& load) {
  std::vector<uint8_t> bytes;
#if defined(XANATOO_HTTPS) && XANATOO_HTTPS
  std::string err;
  int status = 0;
  bool too_large = false;
  const bool ok = xanatoo::https_get_follow_stream(
      url, xanatoo::user_agent("audio"),
      [&](const xanatoo::HttpStreamHeaders& h) {
        status = h.status;
        if (h.status < 200 || h.status >= 300) return false;
        if (h.content_length > kMaxAudioBytes) return !(too_large = true);
        load.total.store(h.content_length);
        if (h.content_length > 0) bytes.reserve(static_cast<size_t>(h.content_length));
        return true;
      },
      [&](std::string_view chunk) {
        if (load.cancel.load(std::memory_order_relaxed)) return false;
        if (static_cast<int64_t>(bytes.size() + chunk.size()) > kMaxAudioBytes)
          return !(too_large = true);
        bytes.insert(bytes.end(), chunk.begin(), chunk.end());
        load.got.store(static_cast<int64_t>(bytes.size()), std::memory_order_relaxed);
        return true;
      },
      &err);
  if (!ok && !load.cancel.load()) {
    if (too_large)
      load.error = "file too large to play";
    else if (status != 0 && (status < 200 || status >= 300))
      load.error = "HTTP " + std::to_string(status);
    else
      load.error = err.empty() ? std::string("download failed") : err;
  }
#else
  (void)url;
  load.error = "this build has no HTTP client";
#endif
  return bytes;
}

void run_load(const std::string& url, AudioLoad& load) {
  std::vector<uint8_t> bytes =
      remote(url) ? download(url, load) : read_local(url, load.error);
  if (!load.error.empty() || load.cancel.load()) return;
  load.dec = xanatoo::open_audio(std::move(bytes), &load.error);
}

void close_player(AudioPlayer& a) {
  if (a.load) a.load->cancel.store(true);
  a.load.reset();
  a.play.reset();  // closes the device (the callback has stopped) first
}

// The status line's rectangle, as draw_status_bar places it.
SDL_Rect status_rect(const App& app) {
  return {0, app.win_h - status_bar_h - static_cast<int>(app.ui_bottom), app.win_w,
          status_bar_h};
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
  if (!xanatoo::sdl::open_audio_out(s->out, s->rate, s->channels, fill, s.get())) {
    a.state = AudioState::Failed;
    a.error = std::string("no sound device: ") + SDL_GetError();
    flash_status(app, "Cannot play " + a.title + ": " + a.error);
    return;
  }
  a.play = std::move(s);
  xanatoo::sdl::pause_audio_out(a.play->out, false);
  a.state = AudioState::Playing;
  std::cerr << "audio: " << a.url << " ("
            << xanatoo::audio_format_name(xanatoo::audio_format(*a.play->dec)) << ", "
            << a.play->rate << " Hz, " << a.play->channels << " ch, "
            << xanatoo::format_audio_time(a.duration) << ")\n";
}

// Triangle (play), two bars (pause) or a square (stop) in a button rect.
void draw_glyph(App& app, const SDL_Rect& r, AudioPart part, bool playing,
                xanatoo::theme::Rgb ink) {
  namespace th = xanatoo::theme;
  const int s = std::max(6, r.h * 2 / 5);
  const int x0 = r.x + (r.w - s) / 2, y0 = r.y + (r.h - s) / 2;
  if (part == AudioPart::Stop) {
    th::fill(app.sdl, {x0, y0, s, s}, ink);
  } else if (playing) {
    const int bw = std::max(2, s / 3);
    th::fill(app.sdl, {x0, y0, bw, s}, ink);
    th::fill(app.sdl, {x0 + s - bw, y0, bw, s}, ink);
  } else {
    // Rows widen to the middle and narrow again: the tip points right.
    for (int y = 0; y < s; ++y) {
      const int half = std::min(y, s - 1 - y);
      const int w = std::max(1, half * 2 * 9 / 10);
      th::hline(app.sdl, x0 + 1, x0 + 1 + w, y0 + y, ink);
    }
  }
}

}  // namespace

bool play_audio(App& app, const std::string& url_in) {
  std::string url = url_in;
  if (xanatoo::is_file_url(url)) {
    url = xanatoo::file_url_path(url);
    if (url.empty()) {
      flash_status(app, "Not a local file URL: " + url_in);
      return false;
    }
  }
  AudioPlayer& a = app.audio;
  close_player(a);
  a = {};
  a.url = url;
  a.title = xanatoo::audio_name_of(url);
  a.state = AudioState::Loading;
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
  flash_status(app, (remote(url) ? "Downloading " : "Playing ") + a.title);
  return true;
}

void audio_toggle(App& app) {
  AudioPlayer& a = app.audio;
  if (!a.play) {
    if (a.state == AudioState::Failed && !a.url.empty()) play_audio(app, a.url);
    return;
  }
  switch (a.state) {
    case AudioState::Playing:
      xanatoo::sdl::pause_audio_out(a.play->out, true);
      a.state = AudioState::Paused;
      break;
    case AudioState::Ended:
      audio_seek(app, 0);
      [[fallthrough]];
    case AudioState::Paused:
      xanatoo::sdl::pause_audio_out(a.play->out, false);
      a.state = AudioState::Playing;
      break;
    default: break;
  }
}

void audio_stop(App& app) {
  close_player(app.audio);
  app.audio = {};
}

void audio_seek(App& app, double seconds) {
  AudioPlayer& a = app.audio;
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
  if (app.audio.play) audio_seek(app, audio_position(app) + delta);
}

double audio_position(const App& app) {
  const AudioShared* s = app.audio.play.get();
  if (!s || s->rate <= 0) return 0;
  return static_cast<double>(s->pos.load(std::memory_order_relaxed)) / s->rate;
}

void update_audio(App& app) {
  AudioPlayer& a = app.audio;
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
  if (a.play && a.state == AudioState::Playing &&
      a.play->ended.load(std::memory_order_acquire)) {
    xanatoo::sdl::pause_audio_out(a.play->out, true);
    a.state = AudioState::Ended;
  }
}

void shutdown_audio(App& app) { audio_stop(app); }

SDL_Rect audio_well_rect(const App& app) {
  if (app.audio.state == AudioState::Idle || status_bar_h <= 0) return {0, 0, 0, 0};
  const SDL_Rect bar = status_rect(app);
  const int h = bar.h - 5;
  return {bar.x + 3, bar.y + 3, std::min(bar.w / 2, 16 * h), h};
}

AudioPart audio_part_at(const App& app, int x, int y, double* frac) {
  const SDL_Rect r = audio_well_rect(app);
  if (r.w <= 0 || x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h)
    return AudioPart::None;
  if (x < r.x + r.h) return AudioPart::Toggle;
  if (x >= r.x + r.w - r.h) return AudioPart::Stop;
  if (frac) {
    const int x0 = r.x + r.h, w = std::max(1, r.w - 2 * r.h);
    *frac = std::clamp(static_cast<double>(x - x0) / w, 0.0, 1.0);
  }
  return AudioPart::Seek;
}

bool audio_press(App& app, int x, int y) {
  double frac = 0;
  switch (audio_part_at(app, x, y, &frac)) {
    case AudioPart::None: return false;
    case AudioPart::Toggle: audio_toggle(app); break;
    case AudioPart::Stop: audio_stop(app); break;
    case AudioPart::Seek:
      if (app.audio.duration > 0) audio_seek(app, frac * app.audio.duration);
      break;
  }
  return true;
}

std::string audio_hover_text(const App& app, int x, int y) {
  double frac = 0;
  const AudioPlayer& a = app.audio;
  switch (audio_part_at(app, x, y, &frac)) {
    case AudioPart::None: return {};
    case AudioPart::Toggle:
      return a.state == AudioState::Playing ? "Pause" : "Play";
    case AudioPart::Stop: return "Stop and close the player";
    case AudioPart::Seek:
      if (a.state == AudioState::Failed) return a.title + ": " + a.error;
      if (a.duration > 0 && a.play)
        return "Go to " + xanatoo::format_audio_time(frac * a.duration) + " \xE2\x80\x94 " +
               a.url;
      return a.url;
  }
  return {};
}

void draw_audio_well(App& app) {
  namespace th = xanatoo::theme;
  const SDL_Rect r = audio_well_rect(app);
  if (r.w <= 0) return;
  const th::Palette& p = th::palette();
  const AudioPlayer& a = app.audio;
  const SDL_Rect toggle{r.x, r.y, r.h, r.h};
  const SDL_Rect stop{r.x + r.w - r.h, r.y, r.h, r.h};
  const SDL_Rect mid{r.x + r.h, r.y, r.w - 2 * r.h, r.h};
  const AudioPart hot = audio_part_at(app, static_cast<int>(app.mouse_x), static_cast<int>(app.mouse_y));
  th::fill(app.sdl, toggle, hot == AudioPart::Toggle ? p.face_light : p.face);
  th::bevel(app.sdl, toggle, th::Bevel::Raised);
  th::fill(app.sdl, stop, hot == AudioPart::Stop ? p.face_light : p.face);
  th::bevel(app.sdl, stop, th::Bevel::Raised);
  const bool can_toggle = a.play || a.state == AudioState::Failed;
  draw_glyph(app, toggle, AudioPart::Toggle, a.state == AudioState::Playing,
             can_toggle ? p.text : p.text_dim);
  draw_glyph(app, stop, AudioPart::Stop, false, p.text);

  th::fill(app.sdl, mid, p.field);
  // Progress: the played part, or the downloaded part while loading.
  double frac = 0;
  std::string time;
  if (a.load) {
    const int64_t got = a.load->got.load(), total = a.load->total.load();
    if (total > 0) frac = std::clamp(static_cast<double>(got) / total, 0.0, 1.0);
    time = total > 0 ? std::to_string(static_cast<int>(frac * 100)) + "%"
                     : (got > 0 ? std::to_string(got >> 10) + " KiB" : "\xE2\x80\xA6");
  } else if (a.play) {
    const double pos = audio_position(app);
    if (a.duration > 0) frac = std::clamp(pos / a.duration, 0.0, 1.0);
    time = xanatoo::format_audio_time(pos);
    if (a.duration > 0) time += " / " + xanatoo::format_audio_time(a.duration);
  } else if (a.state == AudioState::Failed) {
    time = "failed";
  }
  if (frac > 0)
    th::fill(app.sdl,
             {mid.x, mid.y, static_cast<int>(std::lround(mid.w * frac)), mid.h},
             a.load ? p.face_dark : p.accent_soft);
  th::bevel(app.sdl, mid, th::Bevel::Sunken);

  Font& font = app.ui_font;
  const int pad = 6;
  const int ty = mid.y + (mid.h - font.line_height()) / 2 + font.ascender();
  const int time_w = font.measure(time);
  const th::Rgb ink = a.state == AudioState::Failed ? th::rgb(0x9A, 0x55, 0x10) : p.text;
  SDL_Rect clip{mid.x + 1, mid.y + 1, mid.w - 2, mid.h - 2};
  xanatoo::sdl::set_clip_rect(app.sdl, &clip);
  const std::string title = fit_text(font, a.title, std::max(0, mid.w - time_w - 3 * pad));
  app.renderer.draw_text(font, static_cast<float>(mid.x + pad), static_cast<float>(ty),
                         title, ink.r, ink.g, ink.b);
  app.renderer.draw_text(font, static_cast<float>(mid.x + mid.w - pad - time_w),
                         static_cast<float>(ty), time, p.text_dim.r, p.text_dim.g,
                         p.text_dim.b);
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
}

}  // namespace xanatoo::app
