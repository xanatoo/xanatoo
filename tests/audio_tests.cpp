// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Offline tests for audio decoding (audio.hpp): sniffing, the four
// decoders on tiny fixtures in tests/data/audio (0.5 s, 440 Hz, 8 kHz mono,
// title "Test_Tone", made with ffmpeg), seeking, names and times. Part of
// xanatoo_tests (test_audio is called from unit_tests.cpp).

#include "xanatoo/audio.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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

std::vector<uint8_t> fixture(const char* name) {
  std::ifstream in(std::string(XANATOO_TEST_DATA) + "/audio/" + name, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void put16(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x & 0xFF);
  v.push_back((x >> 8) & 0xFF);
}
void put32(std::vector<uint8_t>& v, uint32_t x) {
  put16(v, x & 0xFFFF);
  put16(v, x >> 16);
}

// A 16-bit PCM WAV: `frames` frames of a ramp, stereo at 22050 Hz.
std::vector<uint8_t> make_wav(uint32_t frames) {
  std::vector<uint8_t> v;
  const uint32_t data = frames * 4;
  v.insert(v.end(), {'R', 'I', 'F', 'F'});
  put32(v, 36 + data);
  v.insert(v.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
  put32(v, 16);
  put16(v, 1);          // PCM
  put16(v, 2);          // channels
  put32(v, 22050);      // rate
  put32(v, 22050 * 4);  // byte rate
  put16(v, 4);          // block align
  put16(v, 16);         // bits
  v.insert(v.end(), {'d', 'a', 't', 'a'});
  put32(v, data);
  for (uint32_t i = 0; i < frames; ++i) {
    put16(v, static_cast<uint16_t>(static_cast<int16_t>(i * 8)));
    put16(v, static_cast<uint16_t>(static_cast<int16_t>(-static_cast<int>(i) * 8)));
  }
  return v;
}

// Peak and sign changes of the first channel of everything the decoder gives.
struct Scan {
  size_t frames = 0;
  float peak = 0;
  int crossings = 0;
};
Scan scan(xanatoo::AudioDecoder& d) {
  Scan s;
  const int ch = xanatoo::audio_channels(d);
  std::vector<float> buf(512 * ch);
  float last = 0;
  for (;;) {
    const size_t n = xanatoo::audio_read(d, buf.data(), 512);
    for (size_t i = 0; i < n; ++i) {
      const float x = buf[i * ch];
      s.peak = std::max(s.peak, std::fabs(x));
      if ((x > 0.05f && last < -0.05f) || (x < -0.05f && last > 0.05f)) ++s.crossings;
      if (std::fabs(x) > 0.05f) last = x;
    }
    s.frames += n;
    if (n < 512) break;
  }
  return s;
}
}  // namespace

#define EXPECT(c) expect(!!(c), #c, __FILE__, __LINE__)
#define EXPECT_EQ(a, b) expect((a) == (b), #a " == " #b, __FILE__, __LINE__)

void test_audio(int& passed, int& failed) {
  using namespace xanatoo;

  // Addresses by name.
  EXPECT(is_audio_url("song.mp3"));
  EXPECT(is_audio_url("/music/A B.FLAC"));
  EXPECT(is_audio_url("https://example.org/ep1.mp3?download=1#t"));
  EXPECT(is_audio_url("file:///tmp/x.ogg"));
  EXPECT(is_audio_url("C:/Music/x.wav"));
  EXPECT(is_audio_url("talk.opus"));
  EXPECT(!is_audio_url("song.mp3.md"));
  EXPECT(!is_audio_url("mailto:a.mp3"));
  EXPECT(!is_audio_url("gemini://host/a.mp3"));
  EXPECT(!is_audio_url("video.mp4"));
  EXPECT_EQ(audio_name_of("https://h/pod/My%20Show%2001.mp3?x=1"), "My Show 01.mp3");
  EXPECT_EQ(audio_name_of("/a/b/c.flac"), "c.flac");

  EXPECT_EQ(format_audio_time(0), "0:00");
  EXPECT_EQ(format_audio_time(65.9), "1:05");
  EXPECT_EQ(format_audio_time(754), "12:34");
  EXPECT_EQ(format_audio_time(3723), "1:02:03");
  EXPECT_EQ(format_audio_time(-3), "0:00");

  // Sniffing goes by content.
  {
    const uint8_t sync[] = {0xFF, 0xFB, 0x90, 0x64};
    EXPECT(sniff_audio(sync, sizeof sync) == AudioFormat::Mp3);
    const uint8_t bad[] = {0xFF, 0xFB, 0xF0, 0x64};  // bitrate index 15
    EXPECT(sniff_audio(bad, sizeof bad) == AudioFormat::Unknown);
    const char html[] = "<!doctype html>";
    EXPECT(sniff_audio(reinterpret_cast<const uint8_t*>(html), sizeof html) ==
           AudioFormat::Unknown);
    EXPECT(sniff_audio(nullptr, 0) == AudioFormat::Unknown);
  }
  for (auto [name, fmt] : {std::pair{"tone.mp3", AudioFormat::Mp3},
                           std::pair{"tone.flac", AudioFormat::Flac},
                           std::pair{"tone.ogg", AudioFormat::Vorbis},
                           std::pair{"tone.opus", AudioFormat::Opus}}) {
    const auto b = fixture(name);
    EXPECT(!b.empty());
    EXPECT(sniff_audio(b.data(), b.size()) == fmt);
  }

  // WAV made here: exact samples, length, seek.
  {
    std::string err;
    auto d = open_audio(make_wav(1000), &err);
    EXPECT(d != nullptr);
    if (d) {
      EXPECT(audio_format(*d) == AudioFormat::Wav);
      EXPECT_EQ(audio_channels(*d), 2);
      EXPECT_EQ(audio_rate(*d), 22050);
      EXPECT_EQ(audio_frames(*d), 1000u);
      float f[8];
      EXPECT_EQ(audio_read(*d, f, 4), 4u);
      EXPECT(f[0] == 0.f && f[2] > 0.f && f[3] < 0.f);  // L ramps up, R down
      EXPECT(audio_seek(*d, 990));
      std::vector<float> rest(40);
      EXPECT_EQ(audio_read(*d, rest.data(), 20), 10u);  // only 10 left
      EXPECT(std::fabs(rest[0] - 990 * 8 / 32768.f) < 1e-4f);
    }
  }

  // The fixtures: a 440 Hz tone for 0.5 s, about 220 periods → 440 sign
  // changes (lossy codecs: allow some slack), title from the tags.
  for (const char* name : {"tone.mp3", "tone.flac", "tone.ogg"}) {
    std::string err;
    auto d = open_audio(fixture(name), &err);
    EXPECT(d != nullptr);
    if (!d) {
      std::fprintf(stderr, "  %s: %s\n", name, err.c_str());
      continue;
    }
    EXPECT_EQ(audio_rate(*d), 8000);
    EXPECT_EQ(audio_channels(*d), 1);
    EXPECT_EQ(audio_title(*d), "Test_Tone");
    const uint64_t len = audio_frames(*d);
    EXPECT(len >= 3900 && len <= 5300);  // MP3 adds encoder delay / padding
    const Scan s = scan(*d);
    EXPECT(s.frames == len);
    EXPECT(s.peak > 0.09f && s.peak < 0.2f);  // ffmpeg's sine: amplitude 1/8
    EXPECT(s.crossings > 380 && s.crossings < 480);
    // Seek to the middle: the rest is about half.
    EXPECT(audio_seek(*d, len / 2));
    const Scan half = scan(*d);
    EXPECT(half.frames + 2 >= len - len / 2 && half.frames <= len - len / 2 + 2);
  }

  // Not audio, Opus not yet, empty input.
  {
    std::string err;
    const char text[] = "hello world, no sound here";
    EXPECT(open_audio(std::vector<uint8_t>(text, text + sizeof text), &err) == nullptr);
    EXPECT(!err.empty());
    err.clear();
    EXPECT(open_audio(fixture("tone.opus"), &err) == nullptr);
    EXPECT(err.find("Opus") != std::string::npos);
    EXPECT(open_audio({}, &err) == nullptr);
  }

  passed += g_passed;
  failed += g_failed;
}
