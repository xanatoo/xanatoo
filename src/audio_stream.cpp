// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

// The built-in decoders (audio.hpp) as a MediaSource for the playback pipe
// (media.hpp), reading a ByteFeed while it downloads: remote MP3 / FLAC /
// WAV play as they arrive. Reads that run ahead of the download wait on the
// worker thread, never in the sound callback. Once the download is
// complete, the in-memory decoder (MP3 with its seek table and exact
// length) takes over at the same frame. docs/dev/CONTENT.md "Audio".

#include <algorithm>

#include "xanatoo/audio.hpp"
#include "xanatoo/media.hpp"

namespace xanatoo {

namespace {

// Per next(): a read waits until all of it has arrived, so small.
constexpr size_t kChunkFrames = 1024;

class AudioFeedSource final : public MediaSource {
public:
  AudioFeedSource(std::shared_ptr<ByteFeed> feed, const MediaOptions& options)
      : feed_(std::move(feed)) {
    wait_.cancel = options.cancel;
    wait_.seeks = options.seeks;
  }

  bool open(std::string* error) {
    dec_ = open_audio_feed(feed_, wait_, error);
    if (!dec_) return false;
    info_.has_audio = true;
    info_.rate = audio_rate(*dec_);
    info_.channels = audio_channels(*dec_);
    info_.title = audio_title(*dec_);
    info_.format = audio_describe(*dec_);
    set_duration();
    return true;
  }

  Got next(std::vector<float>& audio, double& pts, VideoFrame&) override {
    audio_rearm(*dec_);
    if (audio_streaming(*dec_) && feed_complete(*feed_)) take_over();
    const size_t ch = size_t(info_.channels);
    audio.resize(kChunkFrames * ch);
    const size_t n = audio_read(*dec_, audio.data(), kChunkFrames);
    audio.resize(n * ch);
    pts = double(pos_) / info_.rate;
    pos_ += n;
    if (n > 0 || audio_interrupted(*dec_)) return Got::Audio;  // (a seek is due)
    std::lock_guard lock(feed_->m);
    if (feed_->done && !feed_->error.empty()) {
      error_ = feed_->error;
      return Got::Error;
    }
    return Got::End;
  }

  bool seek(double seconds) override {
    audio_rearm(*dec_);
    if (audio_streaming(*dec_) && feed_complete(*feed_)) take_over();
    uint64_t frame = uint64_t(std::max(0.0, seconds) * info_.rate);
    const uint64_t frames = audio_frames(*dec_);
    if (frames > 0 && !audio_streaming(*dec_)) frame = std::min(frame, frames);
    if (!audio_seek(*dec_, frame)) return false;
    pos_ = frame;
    return true;
  }

private:
  void set_duration() {
    const uint64_t frames = audio_frames(*dec_);
    info_.duration = frames > 0 ? double(frames) / info_.rate : 0;
  }

  // The whole file is here: the in-memory decoder, at the same frame.
  void take_over() {
    AudioDecoderPtr whole = open_audio_feed(feed_, wait_, nullptr);
    if (!whole || audio_rate(*whole) != info_.rate || audio_channels(*whole) != info_.channels)
      return;  // keep streaming
    if (!audio_seek(*whole, pos_)) return;
    dec_ = std::move(whole);
    set_duration();
  }

  std::shared_ptr<ByteFeed> feed_;
  FeedWait wait_;
  AudioDecoderPtr dec_;
  uint64_t pos_ = 0;  // frame next() gives next
};

}  // namespace

std::unique_ptr<MediaSource> open_audio_stream(std::shared_ptr<ByteFeed> feed,
                                               const MediaOptions& options, std::string* error) {
  auto src = std::make_unique<AudioFeedSource>(std::move(feed), options);
  if (!src->open(error)) return nullptr;
  return src;
}

}  // namespace xanatoo
