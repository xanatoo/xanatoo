// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// The Android media backend (media.hpp): the NDK's AMediaExtractor reads a
// local file (by descriptor) or streams an http(s) address with the
// system's networking; AMediaCodec decodes in ByteBuffer mode (no surface),
// so pictures come out as YUV in the decoder's layout (I420 or NV12-like,
// with stride, slice height and crop from its output format) and are
// copied to I420; sound is 16-bit or float PCM. Needs API 21 (libmediandk).

#if defined(__ANDROID__)

#include <android/log.h>
#include <fcntl.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaFormat.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <deque>

#include "xanatoo/media.hpp"
#include "xanatoo/sdl.hpp"

namespace xanatoo {

std::string media_backend_name() { return "Android MediaCodec"; }
bool media_backend_reads_feeds() { return false; }
bool media_backend_plays_dash() { return false; }

#if defined(XANATOO_FFMPEG) && XANATOO_FFMPEG
bool media_backend_plays_hls() { return true; }
std::unique_ptr<MediaSource> open_ffmpeg_media(
    const std::string& url, const MediaOptions& options, std::string* error);
#else
bool media_backend_plays_hls() { return false; }
#endif

namespace {

constexpr int kColorI420 = 19;          // COLOR_FormatYUV420Planar
constexpr int kColorNV12 = 21;          // COLOR_FormatYUV420SemiPlanar
constexpr int kEncodingPcm16 = 2;       // AudioFormat.ENCODING_PCM_16BIT
constexpr int kEncodingPcmFloat = 4;    // AudioFormat.ENCODING_PCM_FLOAT

std::string codec_label(const std::string& mime) {
  struct Name { const char* mime; const char* label; };
  static const Name names[] = {
      {"video/avc", "H.264"},  {"video/hevc", "H.265"},      {"video/x-vnd.on2.vp8", "VP8"},
      {"video/x-vnd.on2.vp9", "VP9"}, {"video/av01", "AV1"}, {"video/mp4v-es", "MPEG-4"},
      {"video/3gpp", "H.263"}, {"audio/mp4a-latm", "AAC"},   {"audio/opus", "Opus"},
      {"audio/vorbis", "Vorbis"}, {"audio/mpeg", "MP3"},     {"audio/flac", "FLAC"},
      {"audio/raw", "PCM"},    {"audio/3gpp", "AMR"},
  };
  for (const Name& n : names)
    if (mime == n.mime) return n.label;
  const size_t slash = mime.find('/');
  return slash == std::string::npos ? mime : mime.substr(slash + 1);
}

int32_t get_int(AMediaFormat* f, const char* key, int32_t fallback) {
  int32_t v = 0;
  return AMediaFormat_getInt32(f, key, &v) ? v : fallback;
}

class AndroidSource final : public MediaSource {
public:
  ~AndroidSource() override {
    for (Track* t : {&video_, &audio_})
      if (t->codec) {
        AMediaCodec_stop(t->codec);
        AMediaCodec_delete(t->codec);
      }
    if (ex_) AMediaExtractor_delete(ex_);
    if (fd_ >= 0) close(fd_);
  }

  bool open(const std::string& url) {
    ex_ = AMediaExtractor_new();
    if (!ex_) return fail("out of memory");
    media_status_t st;
    if (url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0) {
      // A path or address goes through Java (MediaHTTPService): the
      // calling thread must be attached to the VM. The media worker is a
      // plain std::thread; SDL's getter attaches it (and detaches it when
      // the thread ends). Without that, Android 5's extractor dereferences
      // the missing JNIEnv and crashes instead of returning an error.
#if defined(XANATOO_SDL3) && XANATOO_SDL3
      if (!SDL_GetAndroidJNIEnv()) return fail("cannot attach to the Java VM");
#else
      if (!SDL_AndroidGetJNIEnv()) return fail("cannot attach to the Java VM");
#endif
      st = AMediaExtractor_setDataSource(ex_, url.c_str());
    } else {
      fd_ = ::open(url.c_str(), O_RDONLY | O_CLOEXEC);
      if (fd_ < 0) return fail(std::string("cannot open: ") + std::strerror(errno));
      struct stat sb {};
      fstat(fd_, &sb);
      st = AMediaExtractor_setDataSourceFd(ex_, fd_, 0, sb.st_size);
    }
    if (st != AMEDIA_OK) return fail("cannot read it (media error " + std::to_string(st) + ")");

    std::string vname, aname, why;
    int64_t duration_us = 0;
    const size_t n = AMediaExtractor_getTrackCount(ex_);
    for (size_t i = 0; i < n; ++i) {
      AMediaFormat* fmt = AMediaExtractor_getTrackFormat(ex_, i);
      const char* m = nullptr;
      std::string mime = AMediaFormat_getString(fmt, AMEDIAFORMAT_KEY_MIME, &m) && m ? m : "";
      int64_t us = 0;
      Track* t = nullptr;
      if (mime.rfind("video/", 0) == 0 && !video_.codec) t = &video_;
      else if (mime.rfind("audio/", 0) == 0 && !audio_.codec) t = &audio_;
      if (t && start(*t, int(i), mime, fmt, why)) {
        if (AMediaFormat_getInt64(fmt, AMEDIAFORMAT_KEY_DURATION, &us))
          duration_us = std::max(duration_us, us);
        if (t == &video_) {
          info_.has_video = true;
          info_.width = width_ = get_int(fmt, AMEDIAFORMAT_KEY_WIDTH, 0);
          info_.height = height_ = get_int(fmt, AMEDIAFORMAT_KEY_HEIGHT, 0);
          info_.frame_rate = get_int(fmt, AMEDIAFORMAT_KEY_FRAME_RATE, 0);
          stride_ = width_;  // until the decoder's output format says
          slice_ = height_;
          vname = codec_label(mime);
        } else {
          info_.has_audio = true;
          rate_ = get_int(fmt, AMEDIAFORMAT_KEY_SAMPLE_RATE, 0);
          in_channels_ = get_int(fmt, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 0);
          aname = codec_label(mime);
        }
      }
      AMediaFormat_delete(fmt);
    }
    if (!video_.codec && !audio_.codec) return fail(why.empty() ? "no sound or video in it" : why);
    info_.duration = double(duration_us) / 1e6;

    // The decoder's own sound format may differ from the track's (HE-AAC
    // doubles the rate): decode until it says, keeping what comes out.
    for (int spins = 0; audio_.codec && !audio_format_known_ && !audio_.out_eos && spins < 4000;
         ++spins)
      if (!step(spins > 50)) break;
    if (info_.has_audio) {
      if (rate_ <= 0) {
        info_.has_audio = false;
        aname.clear();
      }
      info_.rate = rate_;
      info_.channels = std::min(2, std::max(1, in_channels_));
    }
    if (info_.has_video) {
      info_.width = crop_w();
      info_.height = crop_h();
    }
    info_.format = media_describe(vname, info_.width, info_.height, aname, info_.rate,
                                  info_.channels);
    return true;
  }

  Got next(std::vector<float>& audio, double& pts, VideoFrame& frame) override {
    for (int idle = 0;;) {
      if (!pending_.empty()) {
        Piece& p = pending_.front();
        Got g;
        if (p.video) {
          frame = std::move(p.frame);
          g = Got::Video;
        } else {
          audio.insert(audio.end(), p.audio.begin(), p.audio.end());
          pts = p.pts;
          g = Got::Audio;
        }
        pending_.pop_front();
        return g;
      }
      if ((!video_.codec || video_.out_eos) && (!audio_.codec || audio_.out_eos)) return Got::End;
      if (!error_.empty()) return Got::Error;
      idle = step(idle > 2) ? 0 : idle + 1;
      if (idle > 5000) return Got::End;  // a decoder that never finishes
    }
  }

  bool seek(double seconds) override {
    const media_status_t st = AMediaExtractor_seekTo(ex_, int64_t(seconds * 1e6),
                                                     AMEDIAEXTRACTOR_SEEK_PREVIOUS_SYNC);
    for (Track* t : {&video_, &audio_})
      if (t->codec) {
        AMediaCodec_flush(t->codec);
        t->in_eos = t->out_eos = false;
      }
    pending_.clear();
    return st == AMEDIA_OK;
  }

private:
  struct Track {
    int index = -1;
    AMediaCodec* codec = nullptr;
    bool in_eos = false, out_eos = false;
  };
  struct Piece {
    bool video = false;
    VideoFrame frame;
    std::vector<float> audio;
    double pts = 0;
  };

  bool fail(std::string why) {
    error_ = std::move(why);
    return false;
  }

  bool start(Track& t, int index, const std::string& mime, AMediaFormat* fmt, std::string& why) {
    t.codec = AMediaCodec_createDecoderByType(mime.c_str());
    if (!t.codec) {
      why = "this device has no decoder for " + codec_label(mime);
      return false;
    }
    if (AMediaCodec_configure(t.codec, fmt, nullptr, nullptr, 0) != AMEDIA_OK ||
        AMediaCodec_start(t.codec) != AMEDIA_OK) {
      AMediaCodec_delete(t.codec);
      t.codec = nullptr;
      why = "cannot start the " + codec_label(mime) + " decoder";
      return false;
    }
    t.index = index;
    AMediaExtractor_selectTrack(ex_, size_t(index));
    return true;
  }

  int crop_w() const { return crop_r_ >= crop_l_ ? crop_r_ - crop_l_ + 1 : width_; }
  int crop_h() const { return crop_b_ >= crop_t_ ? crop_b_ - crop_t_ + 1 : height_; }

  void output_format(Track& t) {
    AMediaFormat* f = AMediaCodec_getOutputFormat(t.codec);
    if (!f) return;
    if (&t == &video_) {
      width_ = get_int(f, AMEDIAFORMAT_KEY_WIDTH, width_);
      height_ = get_int(f, AMEDIAFORMAT_KEY_HEIGHT, height_);
      stride_ = get_int(f, "stride", width_);
      slice_ = get_int(f, "slice-height", height_);
      crop_l_ = get_int(f, "crop-left", 0);
      crop_t_ = get_int(f, "crop-top", 0);
      crop_r_ = get_int(f, "crop-right", width_ - 1);
      crop_b_ = get_int(f, "crop-bottom", height_ - 1);
      color_ = get_int(f, AMEDIAFORMAT_KEY_COLOR_FORMAT, kColorNV12);
      if (stride_ < width_) stride_ = width_;
      if (slice_ < height_) slice_ = height_;
      __android_log_print(ANDROID_LOG_INFO, "xanatoo",
                          "video decoder output: %dx%d stride %d slice %d color 0x%x", width_,
                          height_, stride_, slice_, color_);
    } else {
      const int rate = get_int(f, AMEDIAFORMAT_KEY_SAMPLE_RATE, rate_);
      const int ch = get_int(f, AMEDIAFORMAT_KEY_CHANNEL_COUNT, in_channels_);
      encoding_ = get_int(f, "pcm-encoding", kEncodingPcm16);
      if (!audio_format_known_) {  // before info_ is published
        rate_ = rate;
        in_channels_ = ch;
      } else if (rate != rate_ || ch != in_channels_) {
        __android_log_print(ANDROID_LOG_WARN, "xanatoo",
                            "sound format changed while playing: %d Hz %d ch", rate, ch);
        in_channels_ = ch;  // mixed to the opened channels; the rate stays
      }
      audio_format_known_ = true;
    }
    AMediaFormat_delete(f);
  }

  // One picture from the decoder's buffer, cropped, into I420.
  bool picture(const uint8_t* buf, size_t size, double pts) {
    Piece p;
    p.video = true;
    VideoFrame& f = p.frame;
    const int w = crop_w() & ~1, h = crop_h() & ~1;
    if (w <= 0 || h <= 0) return true;
    // A slice height the buffer cannot hold is a lie: fall back to the height.
    int slice = slice_;
    if (size_t(stride_) * slice * 3 / 2 > size) slice = height_;
    if (size_t(stride_) * slice * 3 / 2 > size) return true;  // drop it
    f.alloc(w, h);
    f.pts = pts;
    const uint8_t* y = buf + size_t(crop_t_) * stride_ + crop_l_;
    for (int r = 0; r < h; ++r) std::memcpy(f.y() + size_t(r) * w, y + size_t(r) * stride_, w);
    const int cw = f.cw(), ch = f.ch();
    const uint8_t* chroma = buf + size_t(stride_) * slice;
    if (color_ == kColorI420) {
      const int cs = stride_ / 2;
      const uint8_t* u = chroma + size_t(crop_t_ / 2) * cs + crop_l_ / 2;
      const uint8_t* v = u + size_t(cs) * (slice / 2);
      for (int r = 0; r < ch; ++r) {
        std::memcpy(f.u() + size_t(r) * cw, u + size_t(r) * cs, cw);
        std::memcpy(f.v() + size_t(r) * cw, v + size_t(r) * cs, cw);
      }
    } else {  // NV12 and vendor variants of it
      const uint8_t* uv = chroma + size_t(crop_t_ / 2) * stride_ + (crop_l_ & ~1);
      for (int r = 0; r < ch; ++r) {
        const uint8_t* s = uv + size_t(r) * stride_;
        uint8_t* u = f.u() + size_t(r) * cw;
        uint8_t* v = f.v() + size_t(r) * cw;
        for (int c = 0; c < cw; ++c) {
          u[c] = s[2 * c];
          v[c] = s[2 * c + 1];
        }
      }
    }
    pending_.push_back(std::move(p));
    return true;
  }

  void sound(const uint8_t* buf, size_t size, double pts) {
    const int in = std::max(1, in_channels_);
    const int out = info_.channels > 0 ? info_.channels : std::min(2, in);
    const size_t bytes = encoding_ == kEncodingPcmFloat ? 4 : 2;
    const size_t frames = size / (bytes * size_t(in));
    Piece p;
    p.pts = pts;
    p.audio.resize(frames * size_t(out));
    for (size_t i = 0; i < frames; ++i)
      for (int c = 0; c < out; ++c) {
        const size_t k = i * size_t(in) + size_t(std::min(c, in - 1));
        float s;
        if (bytes == 4) {
          std::memcpy(&s, buf + k * 4, 4);
        } else {
          int16_t v;
          std::memcpy(&v, buf + k * 2, 2);
          s = float(v) / 32768.f;
        }
        p.audio[i * size_t(out) + size_t(c)] = s;
      }
    pending_.push_back(std::move(p));
  }

  // Takes one finished buffer from a decoder or gives one sample to one;
  // false when nothing happened (`wait`: block a few ms for output).
  bool step(bool wait) {
    for (Track* t : {&video_, &audio_}) {
      if (!t->codec || t->out_eos) continue;
      AMediaCodecBufferInfo bi{};
      const ssize_t idx = AMediaCodec_dequeueOutputBuffer(t->codec, &bi, wait ? 2000 : 0);
      if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
        output_format(*t);
        return true;
      }
      if (idx < 0) continue;  // try later / buffers changed
      if (bi.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) t->out_eos = true;
      size_t cap = 0;
      const uint8_t* buf = AMediaCodec_getOutputBuffer(t->codec, size_t(idx), &cap);
      if (buf && bi.size > 0 && size_t(bi.offset) + size_t(bi.size) <= cap) {
        const double pts = double(bi.presentationTimeUs) / 1e6;
        if (t == &video_) picture(buf + bi.offset, size_t(bi.size), pts);
        else sound(buf + bi.offset, size_t(bi.size), pts);
      }
      AMediaCodec_releaseOutputBuffer(t->codec, size_t(idx), false);
      return true;
    }
    // Input: the extractor's next sample to its track's decoder, or the end
    // to every decoder that has not had it.
    const ssize_t ti = AMediaExtractor_getSampleTrackIndex(ex_);
    for (Track* t : {&video_, &audio_}) {
      if (!t->codec || t->in_eos) continue;
      if (ti >= 0 && ti != t->index) continue;
      const ssize_t ib = AMediaCodec_dequeueInputBuffer(t->codec, wait ? 2000 : 0);
      if (ib < 0) return false;
      size_t cap = 0;
      uint8_t* buf = AMediaCodec_getInputBuffer(t->codec, size_t(ib), &cap);
      if (ti < 0) {
        AMediaCodec_queueInputBuffer(t->codec, size_t(ib), 0, 0, 0,
                                     AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
        t->in_eos = true;
        return true;
      }
      const ssize_t n = AMediaExtractor_readSampleData(ex_, buf, cap);
      const int64_t us = AMediaExtractor_getSampleTime(ex_);
      AMediaCodec_queueInputBuffer(t->codec, size_t(ib), 0, size_t(std::max<ssize_t>(0, n)),
                                   uint64_t(std::max<int64_t>(0, us)), 0);
      AMediaExtractor_advance(ex_);
      return true;
    }
    if (ti >= 0 && ti != video_.index && ti != audio_.index) {
      AMediaExtractor_advance(ex_);  // a track we did not select
      return true;
    }
    return false;
  }

  AMediaExtractor* ex_ = nullptr;
  int fd_ = -1;
  Track video_, audio_;
  std::deque<Piece> pending_;
  int width_ = 0, height_ = 0, stride_ = 0, slice_ = 0, color_ = kColorNV12;
  int crop_l_ = 0, crop_t_ = 0, crop_r_ = -1, crop_b_ = -1;
  int rate_ = 0, in_channels_ = 0, encoding_ = kEncodingPcm16;
  bool audio_format_known_ = false;
};

}  // namespace

std::unique_ptr<MediaSource> open_media(const std::string& url, const MediaOptions& options,
                                        std::string* error) {
#if defined(XANATOO_FFMPEG) && XANATOO_FFMPEG
  if (options.hls || is_hls_url(url)) return open_ffmpeg_media(url, options, error);
#else
  (void)options;
#endif
  auto src = std::make_unique<AndroidSource>();
  if (!src->open(url)) {
    if (error) *error = src->error();
    return nullptr;
  }
  return src;
}

}  // namespace xanatoo

#endif  // __ANDROID__
