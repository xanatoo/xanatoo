// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Offline tests for media.hpp: address tests, the playback pipe with a fake
// source (read-ahead, audio clock, frame picking, seek trimming, the wall
// clock without sound, the end) and, in FFmpeg builds, the fixtures in
// tests/data/video (0.5 s, 64×48 at 10 fps: red H.264 + AAC 8 kHz mono in
// MP4, blue VP9 + Opus in WebM; made with ffmpeg). Part of xanatoo_tests.

#include "xanatoo/media.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <functional>
#include <thread>

namespace {
int g_passed = 0;
int g_failed = 0;
void expect(bool cond, const char* expr, const char* file, int line) {
  if (cond) {
    ++g_passed;
    return;
  }
  ++g_failed;
  std::fprintf(stderr, "FAIL %s:%d: %s\n", file, line, expr);
}

bool wait_for(const std::function<bool()>& ok) {
  for (int i = 0; i < 400; ++i) {
    if (ok()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return ok();
}

// 1 s at 1000 Hz mono: 100-frame sound chunks whose samples are their own
// time, and a 2×2 picture every 0.1 s whose first byte is its number. Seeks
// land on the 0.1 s before (a "key frame").
class FakeSource final : public xanatoo::MediaSource {
public:
  FakeSource(bool audio, bool video) {
    info_.has_audio = audio;
    info_.has_video = video;
    info_.rate = 1000;
    info_.channels = 1;
    info_.duration = 1.0;
  }
  Got next(std::vector<float>& audio, double& pts, xanatoo::VideoFrame& frame) override {
    const bool a = info_.has_audio && ia_ < 10, v = info_.has_video && iv_ < 10;
    if (a && (!v || ia_ <= iv_)) {
      pts = ia_ / 10.0;
      for (int i = 0; i < 100; ++i) audio.push_back(float(ia_ * 100 + i) / 1000.f);
      ++ia_;
      return Got::Audio;
    }
    if (v) {
      frame.alloc(2, 2);
      frame.pts = iv_ / 10.0;
      frame.data[0] = uint8_t(iv_);
      ++iv_;
      return Got::Video;
    }
    return Got::End;
  }
  bool seek(double s) override {
    ia_ = iv_ = int(std::floor(s * 10 + 1e-9));
    return true;
  }

private:
  int ia_ = 0, iv_ = 0;
};

// 3 s at 1000 Hz mono with 25 fps pictures that come out of the decoder
// half a second after the sound of their time (as H.264 in MP4 does).
class LaggingSource final : public xanatoo::MediaSource {
public:
  LaggingSource() {
    info_.has_audio = info_.has_video = true;
    info_.rate = 1000;
    info_.channels = 1;
    info_.duration = 3.0;
  }
  Got next(std::vector<float>& audio, double& pts, xanatoo::VideoFrame& frame) override {
    const double ta = ia_ * 0.02, tv = iv_ * 0.04;
    if (iv_ < 75 && (ia_ >= 150 || tv + 0.5 <= ta)) {
      frame.alloc(2, 2);
      frame.pts = tv;
      ++iv_;
      return Got::Video;
    }
    if (ia_ < 150) {
      pts = ta;
      audio.insert(audio.end(), 20, 0.f);
      ++ia_;
      return Got::Audio;
    }
    return Got::End;
  }
  bool seek(double) override { return false; }

private:
  int ia_ = 0, iv_ = 0;
};

size_t queued(xanatoo::MediaPlayback& pb) {
  std::lock_guard lock(pb.m);
  return pb.audio.size();
}
bool settled(xanatoo::MediaPlayback& pb) {
  std::lock_guard lock(pb.m);
  return !pb.seek_pending && (pb.source_end || !pb.audio.empty() || !pb.frames.empty());
}
}  // namespace

#define EXPECT(c) expect(!!(c), #c, __FILE__, __LINE__)
#define EXPECT_EQ(a, b) expect((a) == (b), #a " == " #b, __FILE__, __LINE__)
#define EXPECT_NEAR(a, b, e) expect(std::fabs((a) - (b)) <= (e), #a " ~ " #b, __FILE__, __LINE__)

void test_media(int& passed, int& failed) {
  using namespace xanatoo;

  EXPECT(is_video_url("clip.mp4"));
  EXPECT(is_video_url("https://example.org/v/Talk.WebM?x=1#t=3"));
  EXPECT(is_video_url("file:///tmp/a.mkv"));
  EXPECT(!is_video_url("clip.mp4.md"));
  EXPECT(!is_video_url("song.mp3"));
  EXPECT(!is_video_url("mailto:a.mp4"));
  EXPECT(needs_media_backend("talk.opus"));
  EXPECT(needs_media_backend("song.m4a"));
  EXPECT(!needs_media_backend("song.mp3"));
  EXPECT(is_playable_url("song.mp3"));
  EXPECT_EQ(is_playable_url("clip.mp4"), media_backend_available());
  EXPECT_EQ(media_describe("H.264", 640, 360, "AAC", 44100, 2),
            "H.264 640\xC3\x97" "360 \xC2\xB7 AAC 44100 Hz stereo");
  EXPECT_EQ(media_describe("", 0, 0, "Opus", 48000, 1), "Opus 48000 Hz mono");

  // Media Fragments: the start of a temporal fragment.
  EXPECT(parse_media_time("t=10") == 10.0);
  EXPECT(parse_media_time("t=12.5") == 12.5);
  EXPECT(parse_media_time("t=npt:1:02.5") == 62.5);
  EXPECT(parse_media_time("t=1:00:03") == 3603.0);
  EXPECT(parse_media_time("t=10,20") == 10.0);
  EXPECT(parse_media_time("t=,20") == 0.0);
  EXPECT(parse_media_time("xywh=0,0,10,10&t=7") == 7.0);
  EXPECT(parse_media_time("t%3D30") == 30.0);
  EXPECT(!parse_media_time("t=abc"));
  EXPECT(!parse_media_time("t=1..2"));
  EXPECT(!parse_media_time("intro"));
  EXPECT(!parse_media_time(""));
  {
    const MediaAddress a = split_media_address("file:///m/song.mp3#t=30");
    EXPECT_EQ(a.base, "file:///m/song.mp3");
    EXPECT(a.start == 30.0);
    const MediaAddress b = split_media_address("song.mp3#intro");
    EXPECT_EQ(b.base, "song.mp3#intro");
    EXPECT(!b.start);
    EXPECT_EQ(split_media_address("song.mp3").base, "song.mp3");
  }
  EXPECT_EQ(media_time_address("a.mp4", 12.0), "a.mp4#t=12");
  EXPECT_EQ(media_time_address("a.mp4", 12.34), "a.mp4#t=12.3");
  EXPECT_EQ(media_time_address("a.mp4", 59.96), "a.mp4#t=60");
  EXPECT_EQ(media_time_address("a.mp4", -1), "a.mp4#t=0");

  // The pipe with sound and pictures.
  {
    auto pb = std::make_shared<MediaPlayback>();
    std::thread worker(media_worker, pb, std::string(), MediaOptions{},
                       std::make_unique<FakeSource>(true, true));
    EXPECT(wait_for([&] { return pb->opened.load(); }));
    EXPECT(pb->info.has_audio && pb->info.has_video);
    EXPECT(wait_for([&] { return queued(*pb) >= 1000; }));  // all of it: 1 s ahead
    float out[250];
    EXPECT_EQ(media_pull_audio(*pb, out, 250), 250u);
    EXPECT_NEAR(out[0], 0.f, 1e-6f);
    EXPECT_NEAR(out[249], 0.249f, 1e-6f);
    EXPECT_NEAR(media_clock(*pb, 0), 0.25, 1e-9);
    VideoFrame f;
    EXPECT(media_take_frame(*pb, 0.25, f));
    EXPECT_EQ(int(f.data[0]), 2);  // 0.0 and 0.1 passed, 0.2 shown
    EXPECT(!media_take_frame(*pb, 0.25, f));

    // Seek between key frames: sound starts at the target exactly, the
    // picture before it is dropped and the next one comes at once.
    media_seek(*pb, 0.55, 0);
    EXPECT(wait_for([&] { return settled(*pb) && queued(*pb) > 0; }));
    EXPECT_EQ(media_pull_audio(*pb, out, 1), 1u);
    EXPECT_NEAR(out[0], 0.55f, 1.5e-3f);
    EXPECT_NEAR(media_clock(*pb, 0), 0.551, 1e-9);
    EXPECT(wait_for([&] {
      std::lock_guard lock(pb->m);
      return !pb->frames.empty();
    }));
    EXPECT(media_take_frame(*pb, 0.551, f));
    EXPECT_EQ(int(f.data[0]), 6);

    // Play out.
    EXPECT(!media_finished(*pb));
    EXPECT(wait_for([&] {
      media_pull_audio(*pb, out, 250);
      media_take_frame(*pb, 10, f);
      return media_finished(*pb);
    }));
    EXPECT_EQ(int(f.data[0]), 9);
    media_stop(*pb);
    worker.join();
  }

  // Pictures decoded behind their sound still show on time (they once were
  // dropped from a full queue just before they were due).
  {
    auto pb = std::make_shared<MediaPlayback>();
    std::thread worker(media_worker, pb, std::string(), MediaOptions{},
                       std::make_unique<LaggingSource>());
    EXPECT(wait_for([&] { return pb->opened.load(); }));
    float out[10];
    VideoFrame f;
    int shown = 0;
    double worst = 0;
    for (int step = 0; step < 280; ++step) {
      // As a sound device would: the worker keeps its lead (more than the
      // pictures' half second) however slow the machine.
      wait_for([&] {
        std::lock_guard lock(pb->m);
        return pb->audio.size() >= 700 || pb->source_end;
      });
      media_pull_audio(*pb, out, 10);
      const double clock = media_clock(*pb, 0);
      if (media_take_frame(*pb, clock, f)) {
        ++shown;
        if (clock > 0.1) worst = std::max(worst, clock - f.pts);
      }
    }
    EXPECT(shown >= 60);
    EXPECT(worst < 0.1);
    media_stop(*pb);
    worker.join();
  }

  // Pictures alone run on the wall clock.
  {
    auto pb = std::make_shared<MediaPlayback>();
    std::thread worker(media_worker, pb, std::string(), MediaOptions{},
                       std::make_unique<FakeSource>(false, true));
    EXPECT(wait_for([&] { return pb->opened.load(); }));
    EXPECT_NEAR(media_clock(*pb, 5), 0.0, 1e-9);  // paused until started
    media_set_paused(*pb, false, 10.0);
    EXPECT_NEAR(media_clock(*pb, 10.3), 0.3, 1e-9);
    media_set_paused(*pb, true, 10.5);
    EXPECT_NEAR(media_clock(*pb, 20), 0.5, 1e-9);
    media_seek(*pb, 0.2, 21);
    EXPECT_NEAR(media_clock(*pb, 22), 0.2, 1e-9);
    media_stop(*pb);
    worker.join();
  }

  // Open failures.
  {
    std::string err;
    EXPECT(open_media("/nonexistent/clip.mp4", MediaOptions{}, &err) == nullptr);
    EXPECT(!err.empty());
  }

#if defined(XANATOO_FFMPEG) && XANATOO_FFMPEG
  EXPECT(media_backend_name().rfind("FFmpeg ", 0) == 0);
  struct Fixture {
    const char* name;
    int rate, y;  // sample rate, luma of the colour
  };
  for (const Fixture& fx : {Fixture{"red.mp4", 8000, 81}, Fixture{"blue.webm", 48000, 41}}) {
    std::string err;
    auto src = open_media(std::string(XANATOO_TEST_DATA) + "/video/" + fx.name, MediaOptions{},
                          &err);
    EXPECT(src != nullptr);
    if (!src) {
      std::fprintf(stderr, "  %s: %s\n", fx.name, err.c_str());
      continue;
    }
    const MediaInfo& i = src->info();
    EXPECT(i.has_audio && i.has_video);
    EXPECT_EQ(i.width, 64);
    EXPECT_EQ(i.height, 48);
    EXPECT_EQ(i.rate, fx.rate);
    EXPECT_EQ(i.channels, 1);
    EXPECT_EQ(i.title, "Test_Clip");
    EXPECT_NEAR(i.duration, 0.5, 0.05);
    EXPECT(!i.format.empty());
    std::vector<float> audio;
    VideoFrame f, first;
    int pictures = 0;
    double pts = 0;
    for (;;) {
      const MediaSource::Got g = src->next(audio, pts, f);
      if (g == MediaSource::Got::Video && pictures++ == 0) first = f;
      if (g == MediaSource::Got::End || g == MediaSource::Got::Error) {
        EXPECT(g == MediaSource::Got::End);
        break;
      }
    }
    EXPECT_EQ(pictures, 5);
    EXPECT(first.w == 64 && first.h == 48);
    if (!first.data.empty()) EXPECT_NEAR(int(first.y()[64 * 24 + 32]), fx.y, 4);
    EXPECT(audio.size() > size_t(fx.rate) * 4 / 10 && audio.size() < size_t(fx.rate) * 6 / 10);
    float peak = 0;
    for (float s : audio) peak = std::max(peak, std::fabs(s));
    EXPECT(peak > 0.05f && peak < 0.3f);
    // Back to the key frame at 0.3 s (one every three pictures).
    EXPECT(src->seek(0.3));
    MediaSource::Got g;
    do g = src->next(audio, pts, f);
    while (g == MediaSource::Got::Audio);
    EXPECT(g == MediaSource::Got::Video);
    EXPECT(f.pts > 0.25 && f.pts < 0.35);
  }
#endif

  passed += g_passed;
  failed += g_failed;
}
