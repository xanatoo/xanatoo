// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// The FFmpeg media backend (media.hpp; XANATOO_FFMPEG desktop builds):
// libavformat opens a local file or streams an http(s) address (its own
// TLS), libavcodec decodes, libswscale makes I420 pictures, libswresample
// interleaved float sound (more than two channels mixed down to stereo).

#if defined(XANATOO_FFMPEG) && XANATOO_FFMPEG

#include <algorithm>
#include <cctype>
#include <cstring>

#include "xanatoo/media.hpp"

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

class FfmpegSource final : public MediaSource {
public:
  ~FfmpegSource() override {
    sws_freeContext(sws_);
    swr_free(&swr_);
    av_channel_layout_uninit(&in_layout_);
    av_frame_free(&frame_);
    av_packet_free(&pkt_);
    avcodec_free_context(&vdec_);
    avcodec_free_context(&adec_);
    avformat_close_input(&fmt_);
  }

  bool open(const std::string& url, const MediaOptions& options) {
    cancel_ = options.cancel;
    fmt_ = avformat_alloc_context();
    if (!fmt_) return fail("out of memory");
    fmt_->interrupt_callback.callback = [](void* self) -> int {
      const std::atomic<bool>* c = static_cast<FfmpegSource*>(self)->cancel_;
      return c && c->load() ? 1 : 0;
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
    return true;
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
      const int r = av_read_frame(fmt_, pkt_);
      if (r < 0) {
        if (r != AVERROR_EOF && !avio_feof(fmt_->pb)) {
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
    const int64_t ts = int64_t((seconds + start_) * AV_TIME_BASE);
    const int r = avformat_seek_file(fmt_, -1, INT64_MIN, ts, ts, 0);
    if (vdec_) avcodec_flush_buffers(vdec_);
    if (adec_) avcodec_flush_buffers(adec_);
    flushing_ = vdone_ = adone_ = false;
    return r >= 0;
  }

private:
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
