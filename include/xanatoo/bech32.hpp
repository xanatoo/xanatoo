// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Bech32 (BIP-173, not bech32m) as NIP-19 / NIP-49 use it: npub, nsec,
// note, nevent, nprofile, ncryptsec. Dependency-free.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

// hrp + "1" + data + checksum over 8-bit `bytes` (regrouped to 5 bits).
std::string bech32_encode_bytes(std::string_view hrp,
                                const std::vector<uint8_t>& bytes);

// Checks the checksum and returns the lower-case hrp and the 8-bit payload.
// Rejects mixed case and inputs longer than max_length. The default
// accommodates NIP-49 ncryptsec, which exceeds the BIP-173 limit.
bool bech32_decode_bytes(std::string_view s, std::string& hrp,
                         std::vector<uint8_t>& bytes, std::size_t max_length = 200);

}  // namespace xanatoo
