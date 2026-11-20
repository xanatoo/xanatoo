// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// The FFmpeg media backend (media.hpp; XANATOO_FFMPEG desktop builds):
// libavformat opens a local file or streams an http(s) address (its own
// TLS), libavcodec decodes, libswscale makes I420 pictures, libswresample
// interleaved float sound (more than two channels mixed down to stereo).
// A stream gets a reader thread that demuxes up to kReadAhead seconds of
// packets ahead of the decoder, so a slow or stalling server does not stop
// the picture at once; seeks go through it. A single http(s) file whose
// server answers byte ranges is read through MediaOptions::feed (the media
// cache, byte_feed.hpp) with an AVIOContext of ours: what arrived stays, a
// seek back needs no network. Playlists (HLS / DASH) and servers without
// ranges keep FFmpeg's own http.

#if defined(XANATOO_FFMPEG) && XANATOO_FFMPEG

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "xanatoo/byte_feed.hpp"
#include "xanatoo/media.hpp"
#include "xanatoo/util.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace xanatoo {

std::string media_backend_name() {
  std::string v = av_version_info();
  if (!v.empty() && v[0] == 'n') v.erase(0, 1);
  return "FFmpeg " + v;
}

bool media_backend_reads_feeds() { return true; }
bool media_backend_plays_playlists() { return true; }

namespace {

std::string av_error(int err) {
  char buf[AV_ERROR_MAX_STRING_SIZE] = {};
  av_strerror(err, buf, sizeof buf);
  return buf;
}

std::string codec_label(AVCodecID id) {
  switch (id) {
    case AV_CODEC_ID_H264: return "H.264";
    case AV_CODEC_ID_HEVC: return "H.265";
    case AV_CODEC_ID_MPEG4: return "MPEG-4";
    case AV_CODEC_ID_MPEG2VIDEO: return "MPEG-2";
    case AV_CODEC_ID_VORBIS: return "Vorbis";
    case AV_CODEC_ID_OPUS: return "Opus";
    case AV_CODEC_ID_THEORA: return "Theora";
    default: break;
  }
  std::string s = avcodec_get_name(id);
  for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

bool remote(const std::string& url) {
  return url.rfind("http://", 0) == 0 || url.rfind("https://", 0) == 0;
}

// An HLS / DASH playlist: its pieces are other addresses, FFmpeg fetches them.
bool playlist(const std::string& url, std::string_view content_type) {
  const std::string type = ascii_lower(content_type);
  if (type.find("mpegurl") != std::string::npos || type.find("dash+xml") != std::string::npos)
    return true;
  const std::string path = ascii_lower(url.substr(0, url.find_first_of("?#")));
  return ends_with(path, ".m3u8") || ends_with(path, ".m3u") || ends_with(path, ".mpd");
}

constexpr int kIoBuffer = 64 * 1024;

// Read ahead of the decoder (streams): this much time, at most this many bytes.
constexpr double kReadAhead = 30.0;
constexpr size_t kReadAheadBytes = size_t(96) << 20;

class FfmpegSource final : public MediaSource {
public:
  ~FfmpegSource() override {
    if (reader_.joinable()) {
      {
        std::lock_guard lock(qm_);
        stop_.store(true);
      }
      qcv_.notify_all();
      reader_.join();
    }
    for (AVPacket*& p : q_) av_packet_free(&p);
    sws_freeContext(sws_);
    swr_free(&swr_);
    av_channel_layout_uninit(&in_layout_);
    av_frame_free(&frame_);
    av_packet_free(&pkt_);
    avcodec_free_context(&vdec_);
    avcodec_free_context(&adec_);
    avformat_close_input(&fmt_);
    if (pb_) av_freep(&pb_->buffer);
    avio_context_free(&pb_);
  }

  bool open(const std::string& url, const MediaOptions& options) {
    cancel_ = options.cancel;
    fmt_ = avformat_alloc_context();
    if (!fmt_) return fail("out of memory");
    if (remote(url) && options.feed && !read_from_feed(url, options.feed)) return false;
    seeks_ = options.seeks;
    fmt_->interrupt_callback.callback = [](void* self) -> int {
      return static_cast<FfmpegSource*>(self)->interrupted() ? 1 : 0;
    };
    fmt_->interrupt_callback.opaque = this;
    AVDictionary* opts = nullptr;
    if (remote(url)) {
      set_agent(&opts, options);
      av_dict_set(&opts, "rw_timeout", "20000000", 0);  // µs
      av_dict_set(&opts, "reconnect", "1", 0);
    }
    // A local path with a ':' must not read as a protocol.
    const std::string name = remote(url) ? url : "file:" + url;
    int r = avformat_open_input(&fmt_, name.c_str(), nullptr, &opts);
    av_dict_free(&opts);
    if (r < 0) return fail(av_error(r));
    if ((r = avformat_find_stream_info(fmt_, nullptr)) < 0) return fail(av_error(r));

    vs_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vs_ >= 0 && (fmt_->streams[vs_]->disposition & AV_DISPOSITION_ATTACHED_PIC))
      vs_ = -1;  // cover art, not a video
    as_ = av_find_best_stream(fmt_, AVMEDIA_TYPE_AUDIO, -1, vs_, nullptr, 0);
    std::string why;
    if (vs_ >= 0 && !open_decoder(vs_, vdec_, why)) vs_ = -1;
    if (as_ >= 0 && !open_decoder(as_, adec_, why)) as_ = -1;
    if (vs_ < 0 && as_ < 0) return fail(why.empty() ? "no sound or video in it" : why);
    for (unsigned i = 0; i < fmt_->nb_streams; ++i)
      if (int(i) != vs_ && int(i) != as_) fmt_->streams[i]->discard = AVDISCARD_ALL;

    pkt_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    if (!pkt_ || !frame_) return fail("out of memory");
    start_ = fmt_->start_time != AV_NOPTS_VALUE ? double(fmt_->start_time) / AV_TIME_BASE : 0;

    info_.duration = fmt_->duration > 0 ? double(fmt_->duration) / AV_TIME_BASE : 0;
    if (const AVDictionaryEntry* t = av_dict_get(fmt_->metadata, "title", nullptr, 0))
      info_.title = t->value;
    std::string vname, aname;
    if (vs_ >= 0) {
      AVStream* st = fmt_->streams[vs_];
      info_.has_video = true;
      info_.width = vdec_->width;
      info_.height = vdec_->height;
      const AVRational sar = av_guess_sample_aspect_ratio(fmt_, st, nullptr);
      if (sar.num > 0 && sar.den > 0 && sar.num != sar.den)
        info_.width = int(int64_t(info_.width) * sar.num / sar.den);
      const AVRational fr = av_guess_frame_rate(fmt_, st, nullptr);
      if (fr.num > 0 && fr.den > 0) info_.frame_rate = av_q2d(fr);
      vname = codec_label(vdec_->codec_id);
    }
    if (as_ >= 0) {
      info_.has_audio = true;
      info_.rate = adec_->sample_rate;
      info_.channels = std::min(2, std::max(1, adec_->ch_layout.nb_channels));
      aname = codec_label(adec_->codec_id);
      if (info_.rate <= 0) {
        info_.has_audio = false;
        avcodec_free_context(&adec_);
        as_ = -1;
        aname.clear();
      }
    }
    info_.format = media_describe(vname, info_.width, info_.height, aname, info_.rate,
                                  info_.channels);
    if (options.read_ahead > 0 || (options.read_ahead < 0 && remote(url)))
      reader_ = std::thread([this] { read_ahead(); });
    return true;
  }

  double buffered() const override { return reader_.joinable() ? buffered_.load() : -1; }

  bool interrupted() const {
    return (cancel_ && cancel_->load()) || stop_.load() || interrupt_.load();
  }

  Got next(std::vector<float>& audio, double& pts, VideoFrame& frame) override {
    for (;;) {
      if (vdec_ && !vdone_) {
        const int r = avcodec_receive_frame(vdec_, frame_);
        if (r == 0) return picture(frame) ? Got::Video : Got::Error;
        if (r == AVERROR_EOF) vdone_ = true;
      }
      if (adec_ && !adone_) {
        const int r = avcodec_receive_frame(adec_, frame_);
        if (r == 0) return sound(audio, pts) ? Got::Audio : Got::Error;
        if (r == AVERROR_EOF) adone_ = true;
      }
      if (flushing_) {
        if ((!vdec_ || vdone_) && (!adec_ || adone_)) return Got::End;
        // A decoder still owes frames: receive again.
        if (++flush_spins_ > 64) return Got::End;
        continue;
      }
      const int r = read_packet();
      if (r == kInterrupted) return Got::Audio;  // nothing: a seek is due
      if (r < 0) {
        if (r != AVERROR_EOF) {
          error_ = cancel_ && cancel_->load() ? "cancelled" : av_error(r);
          return Got::Error;
        }
        if (vdec_) avcodec_send_packet(vdec_, nullptr);
        if (adec_) avcodec_send_packet(adec_, nullptr);
        flushing_ = true;
        flush_spins_ = 0;
        continue;
      }
      if (pkt_->stream_index == vs_ && vdec_) avcodec_send_packet(vdec_, pkt_);
      else if (pkt_->stream_index == as_ && adec_) avcodec_send_packet(adec_, pkt_);
      av_packet_unref(pkt_);
    }
  }

  bool seek(double seconds) override {
    const int r = reader_.joinable() ? seek_reader(seconds) : seek_file(seconds);
    if (vdec_) avcodec_flush_buffers(vdec_);
    if (adec_) avcodec_flush_buffers(adec_);
    flushing_ = vdone_ = adone_ = false;
    return r >= 0;
  }

private:
  static constexpr int kInterrupted = -0x7A7A;  // read_packet: a newer seek came

  // Reads `url` from `feed` once its server answered byte ranges and it is
  // no playlist; else FFmpeg fetches the address itself and the feed stops.
  // False: cancelled while waiting for the answer.
  bool read_from_feed(const std::string& url, const std::shared_ptr<ByteFeed>& feed) {
    if (!feed_wait_answer(*feed, [this] { return cancel_ && cancel_->load(); }))
      return fail("cancelled");
    bool use;
    {
      std::lock_guard lock(feed->m);
      use = feed->sparse && !playlist(url, feed->content_type);
    }
    if (!use) {
      feed->cancel.store(true);
      return true;
    }
    auto* buf = static_cast<uint8_t*>(av_malloc(kIoBuffer));
    if (!buf) return fail("out of memory");
    pb_ = avio_alloc_context(buf, kIoBuffer, 0, this, &FfmpegSource::io_read, nullptr,
                             &FfmpegSource::io_seek);
    if (!pb_) {
      av_free(buf);
      return fail("out of memory");
    }
    feed_ = feed;
    fmt_->pb = pb_;
    fmt_->flags |= AVFMT_FLAG_CUSTOM_IO;
    return true;
  }

  // The AVIOContext over feed_: reads wait for the bytes (and give up like
  // FFmpeg's own on cancel / stop / a seek), a far read makes the download
  // jump there.
  static int io_read(void* opaque, uint8_t* buf, int size) {
    auto* s = static_cast<FfmpegSource*>(opaque);
    ByteFeed& f = *s->feed_;
    const int64_t total = f.total.load();
    if (size <= 0 || (total >= 0 && s->io_pos_ >= total)) return AVERROR_EOF;
    if (feed_wait(f, s->io_pos_, s->io_pos_ + 1, [s] { return s->interrupted(); }) < 0)
      return AVERROR_EXIT;
    const size_t n = feed_copy(f, s->io_pos_, buf, size_t(size));
    if (n == 0) {
      std::lock_guard lock(f.m);
      return f.error.empty() ? AVERROR_EOF : AVERROR(EIO);
    }
    s->io_pos_ += int64_t(n);
    return int(n);
  }

  static int64_t io_seek(void* opaque, int64_t offset, int whence) {
    auto* s = static_cast<FfmpegSource*>(opaque);
    const int64_t total = s->feed_->total.load();
    int64_t to;
    switch (whence & ~AVSEEK_FORCE) {
      case AVSEEK_SIZE: return total >= 0 ? total : AVERROR(ENOSYS);
      case SEEK_SET: to = offset; break;
      case SEEK_CUR: to = s->io_pos_ + offset; break;
      case SEEK_END:
        if (total < 0) return AVERROR(ENOSYS);
        to = total + offset;
        break;
      default: return AVERROR(EINVAL);
    }
    if (to < 0) return AVERROR(EINVAL);
    s->io_pos_ = to;
    return to;
  }

  int seek_file(double seconds) {
    const int64_t ts = int64_t((seconds + start_) * AV_TIME_BASE);
    return avformat_seek_file(fmt_, -1, INT64_MIN, ts, ts, 0);
  }

  bool given_up(uint64_t seeks_seen) const {
    return (cancel_ && cancel_->load()) || (seeks_ && seeks_->load() != seeks_seen);
  }

  // The next packet into pkt_: read here (a local file), or from the
  // reader's queue. 0, AVERROR_EOF, another error, or kInterrupted.
  int read_packet() {
    if (!reader_.joinable()) {
      const int r = av_read_frame(fmt_, pkt_);
      return r < 0 && avio_feof(fmt_->pb) ? AVERROR_EOF : r;
    }
    const uint64_t seen = seeks_ ? seeks_->load() : 0;
    std::unique_lock lock(qm_);
    while (q_.empty() && !eof_ && read_err_ == 0) {
      if (given_up(seen)) return kInterrupted;
      // Timed: the give-up flags belong to others and do not signal us.
      qcv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    if (q_.empty()) return eof_ ? AVERROR_EOF : read_err_;
    AVPacket* p = q_.front();
    q_.pop_front();
    q_bytes_ -= size_t(p->size);
    qcv_.notify_all();
    lock.unlock();
    av_packet_move_ref(pkt_, p);
    av_packet_free(&p);
    return 0;
  }

  // Seconds of the packets queued, first to last.
  double queued_span() const {
    if (q_.empty()) return 0;
    const double first = packet_time(q_.front());
    return first < 0 ? 0 : q_end_ - first;  // (no time: the byte cap rules)
  }

  double packet_time(const AVPacket* p) const {
    const int64_t ts = p->pts != AV_NOPTS_VALUE ? p->pts : p->dts;
    return seconds_of(p->stream_index, ts);
  }

  // The reader thread: demux ahead into q_ until full, the end or an error;
  // seeks on request (the interrupt callback abandons a read for them).
  void read_ahead() {
    for (;;) {
      std::unique_lock lock(qm_);
      qcv_.wait(lock, [&] {
        return stop_.load() || seek_req_ ||
               (!eof_ && read_err_ == 0 && queued_span() < kReadAhead &&
                q_bytes_ < kReadAheadBytes);
      });
      if (stop_.load()) return;
      if (seek_req_) {
        seek_req_ = false;
        interrupt_.store(false);
        const double to = seek_to_;
        const uint64_t id = seek_asked_;
        lock.unlock();
        const int r = seek_file(to);
        lock.lock();
        for (AVPacket*& p : q_) av_packet_free(&p);
        q_.clear();
        q_bytes_ = 0;
        q_end_ = to;
        buffered_.store(to);
        eof_ = false;
        read_err_ = 0;
        seek_result_ = r;
        seek_done_ = id;
        qcv_.notify_all();
        continue;
      }
      lock.unlock();
      AVPacket* p = av_packet_alloc();
      const int r = p ? av_read_frame(fmt_, p) : AVERROR(ENOMEM);
      const bool at_end = r < 0 && (r == AVERROR_EOF || avio_feof(fmt_->pb));
      lock.lock();
      if (r < 0 || seek_req_ || stop_.load() ||
          (p->stream_index != vs_ && p->stream_index != as_)) {
        av_packet_free(&p);
        if (r < 0 && !seek_req_ && !stop_.load()) {
          if (at_end) {
            eof_ = true;
            buffered_.store(info_.duration > 0 ? info_.duration : q_end_);
          } else {
            read_err_ = r;
          }
          qcv_.notify_all();
        }
        continue;
      }
      double t = packet_time(p);
      if (p->duration > 0) t += double(p->duration) * av_q2d(fmt_->streams[p->stream_index]->time_base);
      if (t > q_end_) {
        q_end_ = t;
        buffered_.store(t);
      }
      q_bytes_ += size_t(p->size);
      q_.push_back(p);
      qcv_.notify_all();
    }
  }

  // Asks the reader to seek and waits for it (or a newer seek / cancel).
  int seek_reader(double seconds) {
    const uint64_t seen = seeks_ ? seeks_->load() : 0;
    std::unique_lock lock(qm_);
    seek_req_ = true;
    seek_to_ = seconds;
    const uint64_t id = ++seek_asked_;
    interrupt_.store(true);  // a read in progress gives up
    qcv_.notify_all();
    while (seek_done_ < id) {
      if (given_up(seen)) return -1;
      qcv_.wait_for(lock, std::chrono::milliseconds(50));
    }
    return seek_result_;
  }

  bool fail(std::string why) {
    error_ = std::move(why);
    return false;
  }

  void set_agent(AVDictionary** opts, const MediaOptions& options);

  bool open_decoder(int index, AVCodecContext*& ctx, std::string& why) {
    AVStream* st = fmt_->streams[index];
    const AVCodec* codec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!codec) {
      why = "no decoder for " + codec_label(st->codecpar->codec_id);
      return false;
    }
    ctx = avcodec_alloc_context3(codec);
    if (!ctx || avcodec_parameters_to_context(ctx, st->codecpar) < 0) {
      avcodec_free_context(&ctx);
      why = "out of memory";
      return false;
    }
    ctx->pkt_timebase = st->time_base;
    ctx->thread_count = 0;  // as many as useful
    if (const int r = avcodec_open2(ctx, codec, nullptr); r < 0) {
      why = codec_label(st->codecpar->codec_id) + ": " + av_error(r);
      avcodec_free_context(&ctx);
      return false;
    }
    return true;
  }

  double seconds_of(int stream, int64_t ts) const {
    if (ts == AV_NOPTS_VALUE) return -1;
    return double(ts) * av_q2d(fmt_->streams[stream]->time_base) - start_;
  }

  bool picture(VideoFrame& out) {
    const int w = frame_->width, h = frame_->height;
    double t = seconds_of(vs_, frame_->best_effort_timestamp);
    if (t < 0) t = last_video_ + 1.0 / 30;
    last_video_ = t;
    out.pts = t;
    out.alloc(w, h);
    const auto fmt = static_cast<AVPixelFormat>(frame_->format);
    if (fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P) {
      av_image_copy_plane(out.y(), w, frame_->data[0], frame_->linesize[0], w, h);
      av_image_copy_plane(out.u(), out.cw(), frame_->data[1], frame_->linesize[1], out.cw(),
                          out.ch());
      av_image_copy_plane(out.v(), out.cw(), frame_->data[2], frame_->linesize[2], out.cw(),
                          out.ch());
    } else {
      sws_ = sws_getCachedContext(sws_, w, h, fmt, w, h, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                                  nullptr, nullptr, nullptr);
      if (!sws_) {
        error_ = std::string("cannot convert pictures from ") +
                 (av_get_pix_fmt_name(fmt) ? av_get_pix_fmt_name(fmt) : "?");
        av_frame_unref(frame_);
        return false;
      }
      uint8_t* planes[4] = {out.y(), out.u(), out.v(), nullptr};
      const int strides[4] = {w, out.cw(), out.cw(), 0};
      sws_scale(sws_, frame_->data, frame_->linesize, 0, h, planes, strides);
    }
    av_frame_unref(frame_);
    return true;
  }

  bool sound(std::vector<float>& audio, double& pts) {
    const int ch = info_.channels;
    if (!swr_ || frame_->sample_rate != in_rate_ || frame_->format != in_fmt_ ||
        av_channel_layout_compare(&frame_->ch_layout, &in_layout_) != 0) {
      swr_free(&swr_);
      av_channel_layout_uninit(&in_layout_);
      if (frame_->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
        av_channel_layout_default(&in_layout_, frame_->ch_layout.nb_channels);
      else
        av_channel_layout_copy(&in_layout_, &frame_->ch_layout);
      in_rate_ = frame_->sample_rate;
      in_fmt_ = frame_->format;
      AVChannelLayout out_layout;
      av_channel_layout_default(&out_layout, ch);
      if (swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_FLT, info_.rate, &in_layout_,
                              static_cast<AVSampleFormat>(in_fmt_), in_rate_, 0,
                              nullptr) < 0 ||
          swr_init(swr_) < 0) {
        error_ = "cannot convert the sound";
        av_frame_unref(frame_);
        return false;
      }
    }
    const double t = seconds_of(as_, frame_->best_effort_timestamp);
    pts = t >= 0 ? t : last_audio_;
    const int room = swr_get_out_samples(swr_, frame_->nb_samples);
    const size_t old = audio.size();
    audio.resize(old + size_t(std::max(0, room)) * ch);
    auto* out = reinterpret_cast<uint8_t*>(audio.data() + old);
    const int n = swr_convert(swr_, &out, room, const_cast<const uint8_t**>(frame_->extended_data),
                              frame_->nb_samples);
    audio.resize(old + size_t(std::max(0, n)) * ch);
    last_audio_ = pts + double(std::max(0, n)) / info_.rate;
    av_frame_unref(frame_);
    return true;
  }

  const std::atomic<bool>* cancel_ = nullptr;
  const std::atomic<uint64_t>* seeks_ = nullptr;
  // Reading through the feed (read_from_feed).
  std::shared_ptr<ByteFeed> feed_;
  AVIOContext* pb_ = nullptr;
  int64_t io_pos_ = 0;
  // The reader (streams): packets read ahead, under qm_.
  std::thread reader_;
  std::mutex qm_;
  std::condition_variable qcv_;
  std::deque<AVPacket*> q_;
  size_t q_bytes_ = 0;
  double q_end_ = 0;   // seconds: end of the latest packet read
  bool eof_ = false;
  int read_err_ = 0;
  bool seek_req_ = false;
  double seek_to_ = 0;
  uint64_t seek_asked_ = 0, seek_done_ = 0;
  int seek_result_ = 0;
  std::atomic<bool> stop_{false};       // the source is going away
  std::atomic<bool> interrupt_{false};  // a seek waits: abandon a read
  std::atomic<double> buffered_{0};
  AVFormatContext* fmt_ = nullptr;
  AVCodecContext* vdec_ = nullptr;
  AVCodecContext* adec_ = nullptr;
  int vs_ = -1, as_ = -1;
  AVPacket* pkt_ = nullptr;
  AVFrame* frame_ = nullptr;
  SwsContext* sws_ = nullptr;
  SwrContext* swr_ = nullptr;
  AVChannelLayout in_layout_{};
  int in_rate_ = 0, in_fmt_ = -1;
  double start_ = 0, last_video_ = 0, last_audio_ = 0;
  bool flushing_ = false, vdone_ = false, adone_ = false;
  int flush_spins_ = 0;
};

void FfmpegSource::set_agent(AVDictionary** opts, const MediaOptions& options) {
  if (!options.user_agent.empty()) av_dict_set(opts, "user_agent", options.user_agent.c_str(), 0);
}

}  // namespace

std::unique_ptr<MediaSource> open_media(const std::string& url, const MediaOptions& options,
                                        std::string* error) {
  auto src = std::make_unique<FfmpegSource>();
  if (!src->open(url, options)) {
    if (error) *error = src->error();
    return nullptr;
  }
  return src;
}

}  // namespace xanatoo

#endif  // XANATOO_FFMPEG
