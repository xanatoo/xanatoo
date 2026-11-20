// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <fstream>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace xanatoo {

enum class DownloadPhase : uint8_t {
  Pending, Skipped, Active, Complete, Failed, Cancelled
};

// Worker publishes name/MIME before Active, path/error before its terminal
// phase. Only counters and cancellation change while Active.
struct DownloadProgress {
  std::atomic<DownloadPhase> phase{DownloadPhase::Pending};
  std::atomic<uint64_t> received{0};
  std::atomic<int64_t> total{-1};
  std::atomic<bool> cancel{false};
  std::atomic<bool> announced{false};
  std::atomic<bool> recorded{false};
  std::string url;
  std::string name;
  std::string mime;
  std::string path;
  std::string final_name;
  std::string error;
  std::string dest_dir;  // empty: default downloads_dir()
  // Response was a downloadable attachment during Navigate/Tab:
  // do not save until the user confirms (start_download dialog).
  std::atomic<bool> offered{false};
  // The response is sound or video the player takes (is_playable_type), in
  // a Navigate/Tab: not saved, not shown as a page; `mime` holds its type.
  std::atomic<bool> playable{false};
};

// Dispatch based on the response headers, not the URL extension. A forced
// attachment always downloads, including text that could otherwise render.
bool download_response(std::string_view url, std::string_view content_type,
                       std::string_view disposition);
std::string download_filename(std::string_view url,
                              std::string_view disposition);

// Stream into a unique temporary file in the user's Downloads directory.
// finish() never replaces an existing file; a failed/aborted transfer removes
// the partial file. The saved path is valid only after finish() succeeds.
class DownloadFile {
public:
  DownloadFile() = default;
  DownloadFile(const DownloadFile&) = delete;
  DownloadFile& operator=(const DownloadFile&) = delete;
  ~DownloadFile();

  // `directory` empty → downloads_dir(); otherwise that folder is used.
  bool begin(std::string filename, std::string* error,
             std::string_view directory = {});
  bool append(std::string_view bytes, std::string* error);
  bool finish(std::string* error);
  void abort();
  const std::string& filename() const { return filename_; }
  const std::string& path() const { return path_; }

private:
  std::string filename_;
  std::string temporary_;
  std::string path_;
  std::ofstream file_;
};

}  // namespace xanatoo
