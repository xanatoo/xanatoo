// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Bytes a page fetch has received so far, for the loading indicator.
// The fetch worker installs a meter for its thread (TransferMeterScope);
// the network readers (https.cpp, gemini.cpp) feed whatever meter the
// current thread has, so service clients need no extra parameter. The main
// thread only reads the atomics. Header-only; the formatting is pure.
// The same meter carries the job's cancel flag: readers check
// transfer_cancelled() per block, so Stop ends any service's transfer.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>

namespace xanatoo {

struct TransferMeter {
  // Bytes read from the network by every request of the job (headers too).
  std::atomic<uint64_t> received{0};
  // Value of `received` at which the current response is complete; -1 while
  // unknown (no Content-Length, chunked, before the headers).
  std::atomic<int64_t> expected{-1};
  // Steady-clock milliseconds of the first byte; -1 before it.
  std::atomic<int64_t> first_ms{-1};
  // Set by the main thread when the job is dropped (toolbar Stop, Esc,
  // navigating elsewhere); null: never cancelled. Set before the worker
  // starts.
  const std::atomic<bool>* cancel = nullptr;
};

inline int64_t transfer_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

inline TransferMeter*& thread_transfer_meter() {
  static thread_local TransferMeter* meter = nullptr;
  return meter;
}

class TransferMeterScope {
public:
  explicit TransferMeterScope(TransferMeter* m) : prev_(thread_transfer_meter()) {
    thread_transfer_meter() = m;
  }
  ~TransferMeterScope() { thread_transfer_meter() = prev_; }
  TransferMeterScope(const TransferMeterScope&) = delete;
  TransferMeterScope& operator=(const TransferMeterScope&) = delete;

private:
  TransferMeter* prev_;
};

// Worker side: the job this thread fetches for was cancelled; a reader
// stops and fails with transfer_cancelled_error().
inline bool transfer_cancelled() {
  const TransferMeter* m = thread_transfer_meter();
  return m && m->cancel && m->cancel->load(std::memory_order_relaxed);
}

inline const char* transfer_cancelled_error() { return "cancelled"; }

// Worker side: `n` more bytes arrived on this thread's request.
inline void meter_bytes(size_t n) {
  TransferMeter* m = thread_transfer_meter();
  if (!m || n == 0) return;
  int64_t unset = -1;
  m->first_ms.compare_exchange_strong(unset, transfer_now_ms(),
                                      std::memory_order_relaxed);
  m->received.fetch_add(n, std::memory_order_relaxed);
}

// Worker side: the current response needs `remaining` more bytes (-1 if
// that is unknown, also when a new request starts).
inline void meter_expect(int64_t remaining) {
  TransferMeter* m = thread_transfer_meter();
  if (!m) return;
  m->expected.store(
      remaining < 0 ? -1
                    : static_cast<int64_t>(
                          m->received.load(std::memory_order_relaxed)) +
                          remaining,
      std::memory_order_relaxed);
}

inline std::string transfer_bytes_label(uint64_t n) {
  char s[48];
  if (n >= 1048576)
    std::snprintf(s, sizeof(s), "%.1f MiB", static_cast<double>(n) / 1048576.0);
  else if (n >= 1024)
    std::snprintf(s, sizeof(s), "%.1f KiB", static_cast<double>(n) / 1024.0);
  else
    std::snprintf(s, sizeof(s), "%llu B", static_cast<unsigned long long>(n));
  return s;
}

inline std::string transfer_eta_label(double secs) {
  char s[32];
  const long long t = static_cast<long long>(secs + 0.5);
  if (t >= 3600)
    std::snprintf(s, sizeof(s), "%lld h %02lld min", t / 3600, t / 60 % 60);
  else if (t >= 60)
    std::snprintf(s, sizeof(s), "%lld min %02lld s", t / 60, t % 60);
  else
    std::snprintf(s, sizeof(s), "%lld s", t);
  return s;
}

// "12.0 KiB", "12.0 KiB / 1.5 MiB (1%) · 2 min 03 s left". `active_ms` is
// the time since the first byte; the rate is the average over it. No ETA
// before half a second of data or without a known size. Empty before any
// byte arrived.
inline std::string transfer_label(uint64_t received, int64_t expected,
                                  int64_t active_ms) {
  if (received == 0) return {};
  std::string s = transfer_bytes_label(received);
  if (expected < 0 || static_cast<uint64_t>(expected) < received) return s;
  const uint64_t total = static_cast<uint64_t>(expected);
  char pct[16];
  std::snprintf(pct, sizeof(pct), " (%d%%)",
                total ? static_cast<int>(received * 100 / total) : 100);
  s += " / " + transfer_bytes_label(total) + pct;
  if (active_ms >= 500 && received < total) {
    const double rate = static_cast<double>(received) * 1000.0 /
                        static_cast<double>(active_ms);
    if (rate > 0)
      s += " · " + transfer_eta_label(static_cast<double>(total - received) /
                                      rate) +
           " left";
  }
  return s;
}

// Main-thread side: label for a running meter.
inline std::string transfer_label(const TransferMeter& m) {
  const int64_t first = m.first_ms.load(std::memory_order_relaxed);
  return transfer_label(m.received.load(std::memory_order_relaxed),
                        m.expected.load(std::memory_order_relaxed),
                        first < 0 ? 0 : transfer_now_ms() - first);
}

}  // namespace xanatoo
