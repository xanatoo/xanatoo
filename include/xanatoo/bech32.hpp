// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Bech32 (BIP-173, not bech32m) as NIP-19 / NIP-49 use it: npub, nsec,
// note, nevent, nprofile, ncryptsec. Dependency-free.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

// hrp + "1" + data + checksum over 8-bit `bytes` (regrouped to 5 bits).
std::string bech32_encode_bytes(std::string_view hrp,
                                const std::vector<uint8_t>& bytes);

// Checks the checksum and returns the lower-case hrp and the 8-bit payload.
// Accepts up to 200 characters (NIP-49 ncryptsec is longer than BIP-173's
// 90); mixed case is rejected.
bool bech32_decode_bytes(std::string_view s, std::string& hrp,
                         std::vector<uint8_t>& bytes);

}  // namespace xanatoo
