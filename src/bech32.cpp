// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>

#include "xanatoo/bech32.hpp"

#include <cstring>

namespace xanatoo {
namespace {

constexpr char kBech32Charset[] = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";

int charset_rev(char c) {
  if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  if (c == '\0') return -1;
  const char* p = std::strchr(kBech32Charset, c);
  return p ? static_cast<int>(p - kBech32Charset) : -1;
}

uint32_t polymod(const std::vector<uint8_t>& values) {
  static const uint32_t GEN[] = {0x3b6a57b2, 0x26508e6d, 0x1ea119fa,
                                 0x3d4233dd, 0x2a1462b3};
  uint32_t chk = 1;
  for (uint8_t v : values) {
    uint8_t b = static_cast<uint8_t>(chk >> 25);
    chk = ((chk & 0x1ffffff) << 5) ^ v;
    for (int i = 0; i < 5; ++i)
      if ((b >> i) & 1) chk ^= GEN[i];
  }
  return chk;
}

std::vector<uint8_t> hrp_expand(std::string_view hrp) {
  std::vector<uint8_t> out;
  out.reserve(hrp.size() * 2 + 1);
  for (char c : hrp) out.push_back(static_cast<uint8_t>(c >> 5));
  out.push_back(0);
  for (char c : hrp) out.push_back(static_cast<uint8_t>(c & 31));
  return out;
}

bool convert_bits(const std::vector<uint8_t>& in, int frombits, int tobits,
                  bool pad, std::vector<uint8_t>& out) {
  int acc = 0;
  int bits = 0;
  const int maxv = (1 << tobits) - 1;
  out.clear();
  for (uint8_t value : in) {
    if (value >> frombits) return false;
    acc = ((acc << frombits) | value) & 0xffff;
    bits += frombits;
    while (bits >= tobits) {
      bits -= tobits;
      out.push_back(static_cast<uint8_t>((acc >> bits) & maxv));
    }
  }
  if (pad) {
    if (bits) out.push_back(static_cast<uint8_t>((acc << (tobits - bits)) & maxv));
  } else if (bits >= frombits || ((acc << (tobits - bits)) & maxv)) {
    return false;
  }
  return true;
}

}  // namespace

std::string bech32_encode_bytes(std::string_view hrp,
                                const std::vector<uint8_t>& bytes) {
  std::vector<uint8_t> data5;
  if (!convert_bits(bytes, 8, 5, true, data5)) return {};
  auto values = hrp_expand(hrp);
  values.insert(values.end(), data5.begin(), data5.end());
  for (int i = 0; i < 6; ++i) values.push_back(0);
  const uint32_t mod = polymod(values) ^ 1;
  std::string out(hrp);
  out.push_back('1');
  for (uint8_t d : data5) out.push_back(kBech32Charset[d]);
  for (int i = 0; i < 6; ++i)
    out.push_back(kBech32Charset[(mod >> (5 * (5 - i))) & 31]);
  return out;
}

bool bech32_decode_bytes(std::string_view str, std::string& hrp,
                         std::vector<uint8_t>& bytes) {
  if (str.size() < 8 || str.size() > 200) return false;
  bool lower = false, upper = false;
  for (char c : str) {
    if (c < 33 || c > 126) return false;
    if (c >= 'a' && c <= 'z') lower = true;
    if (c >= 'A' && c <= 'Z') upper = true;
  }
  if (lower && upper) return false;
  std::string s(str);
  for (char& c : s)
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  const auto pos = s.rfind('1');
  if (pos == std::string::npos || pos < 1 || pos + 7 > s.size()) return false;
  hrp = s.substr(0, pos);
  std::vector<uint8_t> data5;
  for (size_t i = pos + 1; i < s.size(); ++i) {
    const int v = charset_rev(s[i]);
    if (v < 0) return false;
    data5.push_back(static_cast<uint8_t>(v));
  }
  auto values = hrp_expand(hrp);
  values.insert(values.end(), data5.begin(), data5.end());
  if (polymod(values) != 1) return false;
  data5.resize(data5.size() - 6);  // drop the checksum
  return convert_bits(data5, 5, 8, false, bytes);
}

}  // namespace xanatoo
