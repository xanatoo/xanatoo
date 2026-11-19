// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// Sound player (App::audio, docs/dev/CONTENT.md "Audio"). A link to an
// audio file loads it on a worker (local read or http(s) download, then
// open_audio, which scans the file for its length / seek table); the
// decoder then feeds SDL's audio callback (sdl::AudioOut) under a mutex. The
// controls are the Media panel (WindowRole::Media): title and format, a
// seek bar (click or drag), times, back / play-pause / forward / stop and a
// volume slider. media_layout places them for drawing, clicks and hover.
// Video and the formats only a backend decodes (media.hpp) go through a
// MediaPlayback instead: its worker decodes ahead, the same callback pulls
// the sound, and update_audio shows the picture due at the sound's clock
// (the wall clock without sound) in a texture above the controls.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <mutex>
#include <thread>
#include <utility>

#include "xanatoo/app.hpp"
#include "xanatoo/audio.hpp"
#include "xanatoo/file_url.hpp"
#include "xanatoo/theme.hpp"
#include "xanatoo/util.hpp"
#include "xanatoo/version.hpp"
#if defined(XANATOO_HTTPS) && XANATOO_HTTPS
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
  a.play.reset();  // closes the device (the callback has stopped) first
  if (a.media) xanatoo::media_stop(*a.media);  // its worker leaves on its own
  a.media.reset();
  if (a.video_tex) SDL_DestroyTexture(a.video_tex);
  a.video_tex = nullptr;
  a.tex_w = a.tex_h = 0;
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
    fit_panel_to_video(app);
  }
  xanatoo::media_set_paused(m, false, now_s());
  a.state = AudioState::Playing;
  std::cerr << "media: " << a.url << " (" << info.format << ", "
            << xanatoo::format_audio_time(a.duration) << ", " << xanatoo::media_backend_name()
            << ")\n";
}

// The picture due now into the texture.
void show_frame(App& app) {
  AudioPlayer& a = app.audio;
  xanatoo::VideoFrame f;
  if (!xanatoo::media_take_frame(*a.media, xanatoo::media_clock(*a.media, now_s()), f) ||
      f.w <= 0 || f.h <= 0)
    return;
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
  a.format = std::string(xanatoo::audio_format_name(xanatoo::audio_format(*s->dec))) +
             " \xC2\xB7 " + std::to_string(s->rate) + " Hz \xC2\xB7 " +
             (s->channels == 1   ? std::string("mono")
              : s->channels == 2 ? std::string("stereo")
                                 : std::to_string(s->channels) + " channels");
  s->gain.store(gain_of(a.volume));
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

// The panel's parts, placed in its client rect (shared by draw, click and
// hover: the same hit-test as the action).
enum class MediaPart : uint8_t { None, Video, Back, Toggle, Forward, Stop, Seek, Volume };

struct MediaLayout {
  SDL_Rect client{};
  SDL_Rect video{};  // the picture's area above the controls (w = 0: none)
  int controls_h = 0;
  int pad = 0;
  int title_y = 0, info_y = 0, times_y = 0;  // text baselines
  SDL_Rect seek{}, back{}, toggle{}, forward{}, stop{}, volume{};
  int volume_label_x = 0, buttons_y = 0;
};

MediaLayout media_layout(const App& app) {
  MediaLayout L;
  const FloatWindow& w = app.media_win;
  auto [cx, cy, cw, ch] = float_client_rect(const_cast<App&>(app), w);
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
  const int vx = L.stop.x + L.stop.w + 2 * L.pad;
  L.volume_label_x = vx;
  const int label_w = lh * 2;
  const int vw = std::min(160, cx + cw - L.pad - (vx + label_w));
  L.volume = {vx + label_w, y + (b - bar_h) / 2, std::max(0, vw), bar_h};
  L.controls_h = y + b + L.pad - cy;
  // A video takes the room above the controls, which move down.
  if (app.audio.video_w > 0) {
    const int vh = std::max(0, ch - L.controls_h);
    L.video = {cx, cy, cw, vh};
    for (SDL_Rect* r : {&L.seek, &L.back, &L.toggle, &L.forward, &L.stop, &L.volume}) r->y += vh;
    L.title_y += vh;
    L.info_y += vh;
    L.times_y += vh;
    L.buttons_y += vh;
  }
  return L;
}

bool inside(const SDL_Rect& r, int x, int y) {
  return r.w > 0 && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

MediaPart media_part_at(const App& app, const MediaLayout& L, int x, int y) {
  if (!app.media_panel_open || float_shaded(app.media_win) || !inside(L.client, x, y))
    return MediaPart::None;
  // Bars take a few pixels more height: they are thin.
  auto grow = [](SDL_Rect r) { return SDL_Rect{r.x - 2, r.y - 6, r.w + 4, r.h + 12}; };
  if (inside(L.video, x, y)) return MediaPart::Video;
  if (inside(L.back, x, y)) return MediaPart::Back;
  if (inside(L.toggle, x, y)) return MediaPart::Toggle;
  if (inside(L.forward, x, y)) return MediaPart::Forward;
  if (inside(L.stop, x, y)) return MediaPart::Stop;
  if (inside(grow(L.seek), x, y)) return MediaPart::Seek;
  if (L.volume.w > 0 && inside(grow(L.volume), x, y)) return MediaPart::Volume;
  return MediaPart::None;
}

double frac_in(const SDL_Rect& r, int x) {
  return std::clamp(static_cast<double>(x - r.x) / std::max(1, r.w), 0.0, 1.0);
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

void draw_button(App& app, const SDL_Rect& r, MediaPart part, bool hot, bool enabled,
                 bool playing) {
  namespace th = xanatoo::theme;
  const th::Palette& p = th::palette();
  th::fill(app.sdl, r, hot && enabled ? p.face_light : p.face);
  th::bevel(app.sdl, r, th::Bevel::Raised);
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
    default: break;
  }
}

// A sunken trough filled to `frac`, with a knob at the end when `knob`.
void draw_bar(App& app, const SDL_Rect& r, double frac, xanatoo::theme::Rgb fillc, bool knob) {
  namespace th = xanatoo::theme;
  const th::Palette& p = th::palette();
  th::fill(app.sdl, r, p.field);
  const int w = static_cast<int>(std::lround(r.w * std::clamp(frac, 0.0, 1.0)));
  if (w > 0) th::fill(app.sdl, {r.x, r.y, w, r.h}, fillc);
  th::bevel(app.sdl, r, th::Bevel::Sunken);
  if (knob) {
    const int kw = std::max(6, r.h * 2 / 3);
    const SDL_Rect k{std::clamp(r.x + w - kw / 2, r.x - kw / 2, r.x + r.w - kw / 2), r.y - 3,
                     kw, r.h + 6};
    th::fill(app.sdl, k, p.face_light);
    th::bevel(app.sdl, k, th::Bevel::Raised);
  }
}

void apply_drag(App& app, int x) {
  AudioPlayer& a = app.audio;
  const MediaLayout L = media_layout(app);
  if (a.drag == AudioPlayer::Drag::Seek && a.duration > 0)
    audio_seek(app, frac_in(L.seek, x) * a.duration);
  else if (a.drag == AudioPlayer::Drag::Volume)
    audio_set_volume(app, static_cast<float>(frac_in(L.volume, x)));
  a.drag_x = x;
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
  const float volume = a.volume;
  a = {};
  a.volume = volume;
  a.url = url;
  a.title = xanatoo::audio_name_of(url);
  a.state = AudioState::Loading;
  if (xanatoo::needs_media_backend(url) && xanatoo::media_backend_available()) {
    xanatoo::MediaOptions options;
    options.user_agent = xanatoo::user_agent("video");
    a.media = xanatoo::media_start(url, std::move(options));
    app.media_panel_open = true;
    flash_status(app, "Opening " + a.title);
    return true;
  }
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
    play_audio(app, a.url);  // try again
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
      audio_seek(app, 0);
      [[fallthrough]];
    case AudioState::Paused:
      pause(false);
      break;
    default: break;
  }
}

void audio_stop(App& app) {
  close_player(app.audio);
  const float volume = app.audio.volume;
  app.audio = {};
  app.audio.volume = volume;
}

void audio_set_volume(App& app, float volume) {
  app.audio.volume = std::clamp(volume, 0.f, 1.f);
  if (app.audio.play) app.audio.play->gain.store(gain_of(app.audio.volume));
}

void audio_seek(App& app, double seconds) {
  AudioPlayer& a = app.audio;
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

void update_audio(App& app) {
  AudioPlayer& a = app.audio;
  // A drag in the panel follows the pointer until the button comes up.
  if (a.drag != AudioPlayer::Drag::None) {
    if (!xanatoo::sdl::left_button_down() || !app.media_panel_open) {
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
      if (a.video_w > 0) show_frame(app);
      if (a.state == AudioState::Playing && xanatoo::media_finished(*a.media)) {
        if (a.play) xanatoo::sdl::pause_audio_out(a.play->out, true);
        xanatoo::media_set_paused(*a.media, true, now_s());
        std::lock_guard lock(a.media->m);
        if (a.media->failed) {
          a.state = AudioState::Failed;
          a.error = a.media->error;
        } else {
          a.state = AudioState::Ended;
        }
      }
    }
    return;
  }
  if (a.play && a.state == AudioState::Playing &&
      a.play->ended.load(std::memory_order_acquire)) {
    xanatoo::sdl::pause_audio_out(a.play->out, true);
    a.state = AudioState::Ended;
  }
}

void shutdown_audio(App& app) { audio_stop(app); }

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
  if (w.dock != Dock::None || a.video_w <= 0 || a.video_h <= 0) return;
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
    case MediaPart::Video:
    case MediaPart::Toggle: audio_toggle(app); break;
    case MediaPart::Stop: audio_stop(app); break;
    case MediaPart::Seek:
      if (have_media(a) && a.duration > 0) {
        a.drag = AudioPlayer::Drag::Seek;
        apply_drag(app, mx);
      }
      break;
    case MediaPart::Volume:
      a.drag = AudioPlayer::Drag::Volume;
      apply_drag(app, mx);
      break;
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
      return a.state == AudioState::Playing ? "Click to pause" : "Click to play";
    case MediaPart::Toggle:
      return a.state == AudioState::Playing ? "Pause"
             : a.state == AudioState::Failed ? "Try again"
                                              : "Play";
    case MediaPart::Stop: return "Stop";
    case MediaPart::Seek:
      if (have_media(a) && a.duration > 0)
        return "Go to " + xanatoo::format_audio_time(frac_in(L.seek, mx) * a.duration);
      return {};
    case MediaPart::Volume:
      return "Volume " + std::to_string(static_cast<int>(std::lround(a.volume * 100))) + "%";
  }
  return {};
}

void draw_media_panel(App& app) {
  namespace th = xanatoo::theme;
  if (!app.media_panel_open) return;
  FloatWindow& w = app.media_win;
  w.scroll = 0;
  if (!draw_floating_frame(app, w, "Media", float_active(app, w))) return;
  const th::Palette& p = th::palette();
  const AudioPlayer& a = app.audio;
  const MediaLayout L = media_layout(app);
  const SDL_Rect& c = L.client;
  xanatoo::sdl::set_clip_rect(app.sdl, &c);
  const int text_w = std::max(0, c.w - 2 * L.pad);
  const float tx = static_cast<float>(c.x + L.pad);

  // The picture, letterboxed in black at its display aspect.
  if (L.video.w > 0 && L.video.h > 0) {
    th::fill(app.sdl, L.video, th::rgb(0, 0, 0));
    if (a.video_tex && a.video_w > 0 && a.video_h > 0) {
      const double s = std::min(double(L.video.w) / a.video_w, double(L.video.h) / a.video_h);
      const int w = std::max(1, int(a.video_w * s)), h = std::max(1, int(a.video_h * s));
      const SDL_Rect dst{L.video.x + (L.video.w - w) / 2, L.video.y + (L.video.h - h) / 2, w, h};
      xanatoo::sdl::copy(app.sdl, a.video_tex, nullptr, &dst);
    }
  }

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
      const int64_t got = a.load ? a.load->got.load() : 0;
      const int64_t total = a.load ? a.load->total.load() : -1;
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
      break;
  }
  app.renderer.draw_text(app.ui_font_bold, tx, static_cast<float>(L.title_y),
                         fit_text(app.ui_font_bold, title, text_w), p.text.r, p.text.g,
                         p.text.b);
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
  draw_bar(app, L.seek, frac, a.load ? p.face_dark : p.accent, have_media(a) && a.duration > 0);
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

  // Buttons and volume.
  const MediaPart hot = media_part_at(app, L, static_cast<int>(app.mouse_x),
                                      static_cast<int>(app.mouse_y));
  const bool have = have_media(a);
  draw_button(app, L.back, MediaPart::Back, hot == MediaPart::Back, have, false);
  draw_button(app, L.toggle, MediaPart::Toggle, hot == MediaPart::Toggle,
              have || a.state == AudioState::Failed, a.state == AudioState::Playing);
  draw_button(app, L.forward, MediaPart::Forward, hot == MediaPart::Forward, have, false);
  draw_button(app, L.stop, MediaPart::Stop, hot == MediaPart::Stop,
              a.state != AudioState::Idle, false);
  if (L.volume.w > 0) {
    const int ty = L.buttons_y + (L.back.h - app.ui_font.line_height()) / 2 +
                   app.ui_font.ascender();
    app.renderer.draw_text(app.ui_font, static_cast<float>(L.volume_label_x),
                           static_cast<float>(ty), "Vol", p.text_dim.r, p.text_dim.g,
                           p.text_dim.b);
    draw_bar(app, L.volume, a.volume, p.accent_soft, true);
  }
  xanatoo::sdl::set_clip_rect(app.sdl, nullptr);
}

}  // namespace xanatoo::app
