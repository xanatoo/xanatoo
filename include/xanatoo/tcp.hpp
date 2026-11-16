// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Plain TCP connect helper shared by small-net clients (Gopher, Spartan, Nex,
// Finger) and the HTTPS/TLS stack. No TLS dependency.

#include <string>

namespace xanatoo {

// Connected SOCK_STREAM fd, or -1. Caller owns the fd (close with ::close).
// `public_only`: skip private / loopback addresses (untrusted discovery).
int tcp_connect(const std::string& host, int port, std::string* err = nullptr,
                bool public_only = false);

// Same as tcp_connect(..., public_only=false). Kept for existing call sites.
inline int tcp_connect_host(const std::string& host, int port,
                            std::string* err = nullptr) {
  return tcp_connect(host, port, err, false);
}

}  // namespace xanatoo
