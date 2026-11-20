// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
//
// Offline tests for audio decoding (audio.hpp): sniffing, the four
// decoders on tiny fixtures in tests/data/audio (0.5 s, 440 Hz, 8 kHz mono,
// title "Test_Tone", made with ffmpeg), seeking, names and times, and
// decoding from a ByteFeed while it arrives (streaming). Part of
// xanatoo_tests (test_audio is called from unit_tests.cpp).

#include "xanatoo/audio.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
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

// Every sample the decoder gives.
std::vector<float> decode_all(xanatoo::AudioDecoder& d) {
  const int ch = xanatoo::audio_channels(d);
  std::vector<float> out, buf(256 * ch);
  for (;;) {
    const size_t n = xanatoo::audio_read(d, buf.data(), 256);
    out.insert(out.end(), buf.begin(), buf.begin() + n * ch);
    if (n < 256) return out;
  }
}

float max_diff(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return 1e9f;
  float m = 0;
  for (size_t i = 0; i < a.size(); ++i) m = std::max(m, std::fabs(a[i] - b[i]));
  return m;
}

void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// The rest of `whole` from `from` into `feed`, in small pieces, after a pause.
std::thread trickle(std::shared_ptr<xanatoo::ByteFeed> feed, std::vector<uint8_t> whole,
                    size_t from) {
  return std::thread([feed, whole = std::move(whole), from] {
    sleep_ms(30);
    for (size_t at = from; at < whole.size(); at += 97) {
      const size_t n = std::min<size_t>(97, whole.size() - at);
      xanatoo::feed_append(*feed, {reinterpret_cast<const char*>(whole.data() + at), n});
      if ((at / 97) % 8 == 0) sleep_ms(1);
    }
    xanatoo::feed_end(*feed);
  });
}

// A pretend server for feed_download: `whole` from the asked byte on (all of
// it without `ranges`), 4 KiB every millisecond; the request starting at
// `break_at` breaks off after 50000 bytes. Notes where each request started.
struct FakeServer {
  std::vector<uint8_t> whole;
  bool ranges = true;
  int64_t break_at = -1;
  std::string etag;
  std::mutex m;
  std::vector<int64_t> asked;

  std::string get(int64_t from, const std::function<bool(const xanatoo::FeedHead&)>& head,
                  const std::function<bool(std::string_view)>& body) {
    {
      std::lock_guard lock(m);
      asked.push_back(from);
    }
    if (!ranges) from = 0;
    xanatoo::FeedHead h;
    h.start = from;
    h.total = int64_t(whole.size());
    h.ranged = ranges;
    h.etag = etag;
    if (!head(h)) return {};
    const bool breaks = from == break_at;
    if (breaks) break_at = -1;
    for (int64_t at = from; at < int64_t(whole.size()); at += 4096) {
      if (breaks && at - from >= 50000) return "connection reset";
      const size_t n = std::min<size_t>(4096, whole.size() - size_t(at));
      if (!body({reinterpret_cast<const char*>(whole.data() + at), n})) return {};
      sleep_ms(1);
    }
    return {};
  }
  bool asked_near(int64_t pos) {
    std::lock_guard lock(m);
    for (int64_t a : asked)
      if (a <= pos && pos - a < 300000) return true;
    return false;
  }
};

std::thread serve(std::shared_ptr<xanatoo::ByteFeed> feed, FakeServer& server) {
  return std::thread([feed, &server] {
    xanatoo::feed_download(*feed, [&](int64_t from, const auto& head, const auto& body) {
      return server.get(from, head, body);
    });
  });
}

std::shared_ptr<xanatoo::ByteFeed> feed_of(const std::vector<uint8_t>& whole, size_t first) {
  auto feed = std::make_shared<xanatoo::ByteFeed>();
  feed->total.store(static_cast<int64_t>(whole.size()));
  xanatoo::feed_append(*feed, {reinterpret_cast<const char*>(whole.data()), first});
  return feed;
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

  // Streaming: a feed that arrives in pieces decodes exactly like the whole
  // file; once complete the feed opens as the in-memory decoder.
  for (const char* name : {"tone.mp3", "tone.flac", "ramp.wav"}) {
    const std::vector<uint8_t> whole =
        std::string(name) == "ramp.wav" ? make_wav(22050) : fixture(name);
    const auto ref_dec = open_audio(whole, nullptr);
    EXPECT(ref_dec != nullptr);
    if (!ref_dec) continue;
    const std::vector<float> ref = decode_all(*ref_dec);
    auto feed = feed_of(whole, std::min<size_t>(whole.size() / 2, 1000));
    std::atomic<bool> cancel{false};
    FeedWait w;
    w.cancel = &cancel;
    std::thread more = trickle(feed, whole, std::min<size_t>(whole.size() / 2, 1000));
    std::string err;
    AudioDecoderPtr d = open_audio_feed(feed, w, &err);
    EXPECT(d != nullptr);
    if (d) {
      EXPECT(audio_streaming(*d));
      EXPECT(audio_format(*d) == audio_format(*ref_dec));
      EXPECT_EQ(audio_rate(*d), audio_rate(*ref_dec));
      EXPECT_EQ(audio_title(*d), audio_title(*ref_dec));
      EXPECT_EQ(audio_frames(*d), audio_frames(*ref_dec));
      const std::vector<float> got = decode_all(*d);
      EXPECT_EQ(got.size(), ref.size());
      EXPECT(max_diff(got, ref) < 1e-6f);
      EXPECT(!audio_interrupted(*d));
    }
    more.join();
    AudioDecoderPtr m = open_audio_feed(feed, w, &err);
    EXPECT(m != nullptr && !audio_streaming(*m));
    if (m) EXPECT(max_diff(decode_all(*m), ref) < 1e-6f);
  }

  // Ogg Vorbis cannot stream: the open waits for all of it.
  {
    const std::vector<uint8_t> whole = fixture("tone.ogg");
    auto feed = feed_of(whole, whole.size() / 2);
    std::thread more = trickle(feed, whole, whole.size() / 2);
    std::string err;
    AudioDecoderPtr d = open_audio_feed(feed, FeedWait{}, &err);
    more.join();
    EXPECT(d != nullptr && !audio_streaming(*d));
    EXPECT(feed_sniff(*feed, FeedWait{}) == AudioFormat::Vorbis);
    EXPECT(!audio_streamable(AudioFormat::Vorbis) && audio_streamable(AudioFormat::Mp3));
  }

  // A waiting open gives up when cancelled.
  {
    const std::vector<uint8_t> whole = fixture("tone.mp3");
    auto feed = feed_of(whole, 10);
    std::atomic<bool> cancel{false};
    FeedWait w;
    w.cancel = &cancel;
    std::thread stop([&] {
      sleep_ms(30);
      cancel.store(true);
    });
    std::string err;
    EXPECT(open_audio_feed(feed, w, &err) == nullptr);
    EXPECT_EQ(err, "stopped");
    stop.join();
  }

  // A read waiting for bytes gives up for a newer seek (and only then).
  {
    const std::vector<uint8_t> whole = make_wav(22050);
    auto feed = feed_of(whole, 1000);
    std::atomic<uint64_t> seeks{0};
    FeedWait w;
    w.seeks = &seeks;
    AudioDecoderPtr d = open_audio_feed(feed, w, nullptr);
    EXPECT(d != nullptr && audio_streaming(*d));
    if (d) {
      std::thread seek([&] {
        sleep_ms(30);
        seeks.fetch_add(1);
      });
      audio_rearm(*d);
      std::vector<float> buf(22050 * 2);
      const size_t n = audio_read(*d, buf.data(), 22050);
      seek.join();
      EXPECT_EQ(n, size_t((1000 - 44) / 4));  // the frames that had arrived
      EXPECT(audio_interrupted(*d));
      audio_rearm(*d);
      EXPECT(!audio_interrupted(*d));
    }
    // A download that fails: what came plays, then the end.
    feed_end(*feed, "HTTP 500");
    EXPECT(!feed_complete(*feed));
  }

  // A sparse feed keeps what arrived in order, however it came.
  {
    ByteFeed f;
    feed_begin_sparse(f, 100);
    const std::string ten(10, 'x');
    feed_write(f, 10, ten);
    feed_write(f, 30, ten);
    EXPECT_EQ(feed_reach(f, 0), 0);
    EXPECT_EQ(feed_reach(f, 12), 20);
    EXPECT_EQ(feed_reach(f, 25), 25);
    EXPECT_EQ(f.got.load(), 20);
    feed_write(f, 20, ten);
    EXPECT_EQ(feed_reach(f, 10), 40);
    feed_write(f, 95, ten);  // cut at the end
    EXPECT_EQ(feed_reach(f, 95), 100);
    EXPECT_EQ(f.got.load(), 35);
  }

  // A ranged download: a seek far ahead jumps there and plays at once, the
  // gaps fill after, and the file comes out whole.
  {
    FakeServer server;
    server.whole = make_wav(220500);  // 10 s, 882 KB
    const auto ref = open_audio(server.whole, nullptr);
    std::vector<float> want(1024 * 2);
    EXPECT(ref && audio_seek(*ref, 22050 * 8) && audio_read(*ref, want.data(), 1024) == 1024);
    auto feed = std::make_shared<ByteFeed>();
    std::thread download = serve(feed, server);
    AudioDecoderPtr d = open_audio_feed(feed, FeedWait{}, nullptr);
    EXPECT(d != nullptr && audio_streaming(*d));
    if (d) {
      EXPECT(audio_seek(*d, 22050 * 8));
      std::vector<float> got(1024 * 2);
      EXPECT_EQ(audio_read(*d, got.data(), 1024), size_t(1024));
      EXPECT(max_diff(got, want) < 1e-6f);
      EXPECT(feed->got.load() < int64_t(server.whole.size()));  // came before the rest
      EXPECT(server.asked_near(44 + 22050 * 8 * 4));
    }
    download.join();
    EXPECT(feed_complete(*feed));
    EXPECT(feed->bytes == server.whole);
  }

  // A server without ranges: one download from the start.
  {
    FakeServer server;
    server.whole = make_wav(22050);
    server.ranges = false;
    auto feed = std::make_shared<ByteFeed>();
    std::thread download = serve(feed, server);
    download.join();
    EXPECT(feed_complete(*feed) && !feed->sparse);
    EXPECT(feed->bytes == server.whole);
    EXPECT_EQ(server.asked.size(), size_t(1));
  }

  // A broken connection goes on where it broke.
  {
    FakeServer server;
    server.whole = make_wav(44100);
    server.break_at = 0;
    auto feed = std::make_shared<ByteFeed>();
    std::thread download = serve(feed, server);
    download.join();
    EXPECT(feed_complete(*feed));
    EXPECT(feed->bytes == server.whole);
    EXPECT(server.asked.size() == 2 && server.asked[1] >= 50000);
  }

  // A streaming MP3 seeks by byte position: it plays on from there to the end.
  {
    const std::vector<uint8_t> whole = fixture("tone.mp3");
    const auto ref = open_audio(whole, nullptr);
    auto feed = feed_of(whole, 1000);
    std::thread more = trickle(feed, whole, 1000);
    AudioDecoderPtr d = open_audio_feed(feed, FeedWait{}, nullptr);
    EXPECT(d != nullptr && ref != nullptr);
    if (d && ref) {
      const uint64_t half = audio_frames(*ref) / 2;
      EXPECT(audio_seek(*d, half));
      const size_t rest = decode_all(*d).size() / size_t(audio_channels(*d));
      EXPECT(rest + 2 * 1152 >= audio_frames(*ref) - half &&
             rest <= audio_frames(*ref) - half + 2 * 1152);
    }
    more.join();
  }

  // The media cache: what arrived stays for the next feed of the address.
  {
    namespace fs = std::filesystem;
    const std::string dir =
        (fs::temp_directory_path() /
         ("xanatoo-media-cache-" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
            .string();
    const std::string url = "http://example.test/a.wav";
    const int64_t budget = int64_t(1) << 30;
    FakeServer server;
    server.whole = make_wav(44100);  // 176 KB
    server.etag = "\"1\"";
    auto same = [](ByteFeed& f, const std::vector<uint8_t>& whole) {
      const uint8_t* p = nullptr;
      size_t n = 0;
      const std::vector<uint8_t> keep = feed_whole(f, &p, &n);
      return n == whole.size() && std::memcmp(p, whole.data(), n) == 0;
    };
    {  // A part of it, then stopped.
      auto feed = feed_open_cached(dir, url, budget);
      EXPECT(feed != nullptr);
      if (!feed) return;
      std::thread download = serve(feed, server);
      while (feed->got.load() < 60000) sleep_ms(1);
      feed->cancel.store(true);
      download.join();
      EXPECT(!feed_complete(*feed));
      // One file, one feed at a time.
      EXPECT(feed_open_cached(dir, url, budget, 0) == nullptr);
    }
    {  // Next time on from there.
      server.asked.clear();
      auto feed = feed_open_cached(dir, url, budget);
      EXPECT(feed && feed->sparse && feed->got.load() >= 60000);
      std::thread download = serve(feed, server);
      download.join();
      EXPECT(feed_complete(*feed) && same(*feed, server.whole));
      EXPECT(server.asked.size() == 1 && server.asked[0] >= 60000);
    }
    {  // Complete: no network; decodes as the file does.
      server.asked.clear();
      auto feed = feed_open_cached(dir, url, budget);
      std::thread download = serve(feed, server);
      download.join();
      // (Then one request stopped at the head, to see that it did not change.)
      EXPECT(feed_complete(*feed) && server.asked.size() == 1 && server.asked[0] == 0);
      EXPECT(feed->got.load() == int64_t(server.whole.size()));
      EXPECT(!feed->stale);
      AudioDecoderPtr d = open_audio_feed(feed, FeedWait{}, nullptr);
      const auto ref = open_audio(server.whole, nullptr);
      EXPECT(d && ref && !audio_streaming(*d));
      if (d && ref) EXPECT(max_diff(decode_all(*d), decode_all(*ref)) < 1e-6f);
    }
    {  // Changed on the server since it was complete: plays what it has once,
       // then the next play fetches the new file.
      const std::vector<uint8_t> old = server.whole;
      server.whole = make_wav(44100);
      for (size_t i = 44; i < server.whole.size(); ++i) server.whole[i] ^= 0x33;
      server.etag = "\"1b\"";
      server.asked.clear();
      auto feed = feed_open_cached(dir, url, budget);
      std::thread download = serve(feed, server);
      download.join();
      EXPECT(feed_complete(*feed) && same(*feed, old) && feed->stale);
      feed.reset();
      server.asked.clear();
      feed = feed_open_cached(dir, url, budget);
      EXPECT(feed && feed->got.load() == 0);
      download = serve(feed, server);
      download.join();
      EXPECT(feed_complete(*feed) && same(*feed, server.whole));
      EXPECT(server.asked.size() == 1 && server.asked[0] == 0);
    }
    {  // No answer (offline): the cached file stays.
      server.asked.clear();
      auto feed = feed_open_cached(dir, url, budget);
      std::thread download([feed] {
        feed_download(*feed, [](int64_t, const auto&, const auto&) {
          return std::string("network unreachable");
        });
      });
      download.join();
      EXPECT(feed_complete(*feed) && !feed->stale);
      feed.reset();
      feed = feed_open_cached(dir, url, budget);
      EXPECT(feed && feed->got.load() == int64_t(server.whole.size()));
    }
    {  // Changed on the server (another ETag) while half cached: starts over.
      const std::string url2 = "http://example.test/b.wav";
      auto feed = feed_open_cached(dir, url2, budget);
      std::thread download = serve(feed, server);
      while (feed->got.load() < 60000) sleep_ms(1);
      feed->cancel.store(true);
      download.join();
      feed.reset();
      server.whole = make_wav(44100);
      for (size_t i = 44; i < server.whole.size(); ++i) server.whole[i] ^= 0x55;
      server.etag = "\"2\"";
      feed = feed_open_cached(dir, url2, budget);
      download = serve(feed, server);
      download.join();
      EXPECT(feed_complete(*feed) && same(*feed, server.whole));
    }
    {  // Past the budget the older files go.
      auto feed = feed_open_cached(dir, "http://example.test/c.wav", 1000);
      EXPECT(feed != nullptr);
      std::error_code ec;
      int files = 0;
      for (const auto& e : fs::directory_iterator(dir, ec))
        if (!e.path().has_extension()) ++files;
      EXPECT_EQ(files, 1);  // only the live one
    }
    std::error_code ec;
    fs::remove_all(dir, ec);
  }

  // The download stays near the reader in a large file.
  {
    FakeServer server;
    server.whole = make_wav(220500);  // 882 KB
    auto feed = std::make_shared<ByteFeed>();
    feed->ahead = 100000;
    feed->fill_whole = 0;
    std::thread download = serve(feed, server);
    sleep_ms(400);  // (all of it would take ~220 ms)
    EXPECT(feed->got.load() <= 100000 + 8192);
    // A read far ahead: fetched from there.
    EXPECT(feed_wait(*feed, 500000, 501000, nullptr) >= 501000);
    std::vector<uint8_t> b(1000);
    EXPECT_EQ(feed_copy(*feed, 500000, b.data(), b.size()), size_t(1000));
    EXPECT(std::memcmp(b.data(), server.whole.data() + 500000, 1000) == 0);
    feed->cancel.store(true);
    download.join();
    EXPECT(!feed_complete(*feed));
  }

  passed += g_passed;
  failed += g_failed;
}
