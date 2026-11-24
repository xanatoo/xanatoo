// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
// BIP-340 signing, NIP-19 nsec, NIP-49 ncryptsec and signed NIP-01 events
// on OpenSSL's secp256k1, scrypt and ChaCha20-Poly1305.

#include "xanatoo/nostr_sign.hpp"
#include "xanatoo/bech32.hpp"
#include "xanatoo/nostr_verify.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>

#ifndef XANATOO_NOSTR_VERIFY_OPENSSL
#define XANATOO_NOSTR_VERIFY_OPENSSL 0
#endif
#if XANATOO_NOSTR_VERIFY_OPENSSL
#include <openssl/bn.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/kdf.h>
#include <openssl/obj_mac.h>
#include <openssl/rand.h>
#endif

namespace xanatoo {
namespace {

using Bytes = std::vector<unsigned char>;

int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Exactly `n` bytes of hex, or nullopt.
std::optional<Bytes> from_hex(std::string_view h, size_t n) {
  if (h.size() != n * 2) return std::nullopt;
  Bytes out(n);
  for (size_t i = 0; i < n; ++i) {
    const int hi = hex_value(h[2 * i]), lo = hex_value(h[2 * i + 1]);
    if (hi < 0 || lo < 0) return std::nullopt;
    out[i] = static_cast<unsigned char>(hi << 4 | lo);
  }
  return out;
}

std::string to_hex(const unsigned char* p, size_t n) {
  constexpr char hexd[] = "0123456789abcdef";
  std::string out(n * 2, '0');
  for (size_t i = 0; i < n; ++i) {
    out[2 * i] = hexd[p[i] >> 4];
    out[2 * i + 1] = hexd[p[i] & 15];
  }
  return out;
}

void wipe(Bytes& b) {
  volatile unsigned char* p = b.data();
  for (size_t i = 0; i < b.size(); ++i) p[i] = 0;
  b.clear();
}

std::string_view strip_scheme(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                        s.back() == '\n' || s.back() == '\r'))
    s.remove_suffix(1);
  if (s.size() > 6 && (s.substr(0, 6) == "nostr:" || s.substr(0, 6) == "NOSTR:"))
    s.remove_prefix(6);
  return s;
}

#if XANATOO_NOSTR_VERIFY_OPENSSL

struct BnFree { void operator()(BIGNUM* b) const { BN_clear_free(b); } };
struct PointFree { void operator()(EC_POINT* p) const { EC_POINT_clear_free(p); } };
using Bn = std::unique_ptr<BIGNUM, BnFree>;
using Point = std::unique_ptr<EC_POINT, PointFree>;

struct Curve {
  std::unique_ptr<EC_GROUP, decltype(&EC_GROUP_free)> group{
      EC_GROUP_new_by_curve_name(NID_secp256k1), EC_GROUP_free};
  std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> ctx{BN_CTX_secure_new(),
                                                      BN_CTX_free};
  Bn order{BN_new()};
  bool ok() {
    return group && ctx && order &&
           EC_GROUP_get_order(group.get(), order.get(), ctx.get());
  }
  Point point() { return Point(EC_POINT_new(group.get())); }
};

Bn secret_bn() {
  Bn b(BN_secure_new());
  if (b) BN_set_flags(b.get(), BN_FLG_CONSTTIME);
  return b;
}

bool sha256(const unsigned char* data, size_t n, unsigned char out[32]) {
  unsigned int len = 0;
  return EVP_Digest(data, n, out, &len, EVP_sha256(), nullptr) == 1 && len == 32;
}

// BIP-340 tagged hash: SHA256(SHA256(tag) || SHA256(tag) || msg).
std::array<unsigned char, 32> tagged_hash(std::string_view tag, const Bytes& msg) {
  std::array<unsigned char, 32> t{}, out{};
  sha256(reinterpret_cast<const unsigned char*>(tag.data()), tag.size(), t.data());
  Bytes buf;
  buf.reserve(64 + msg.size());
  buf.insert(buf.end(), t.begin(), t.end());
  buf.insert(buf.end(), t.begin(), t.end());
  buf.insert(buf.end(), msg.begin(), msg.end());
  sha256(buf.data(), buf.size(), out.data());
  wipe(buf);
  return out;
}

bool bn_bytes32(const BIGNUM* b, unsigned char out[32]) {
  return BN_bn2binpad(b, out, 32) == 32;
}

// Affine x (32 bytes) of `p` and whether its y is even.
bool point_xy(Curve& c, const EC_POINT* p, unsigned char x[32], bool& even_y) {
  Bn bx(BN_new()), by(BN_new());
  if (!bx || !by ||
      !EC_POINT_get_affine_coordinates(c.group.get(), p, bx.get(), by.get(),
                                       c.ctx.get()))
    return false;
  even_y = !BN_is_odd(by.get());
  return bn_bytes32(bx.get(), x);
}

// The secret as a scalar in [1, n-1], or null.
Bn secret_scalar(Curve& c, std::string_view secret_hex) {
  auto sk = from_hex(secret_hex, 32);
  if (!sk) return nullptr;
  Bn d = secret_bn();
  const bool ok = d && BN_bin2bn(sk->data(), 32, d.get()) &&
                  !BN_is_zero(d.get()) && BN_cmp(d.get(), c.order.get()) < 0;
  wipe(*sk);
  return ok ? std::move(d) : nullptr;
}

// ---- XChaCha20-Poly1305 (draft-irtf-cfrg-xchacha) ------------------------

uint32_t rotl(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }

uint32_t load_le(const unsigned char* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
         uint32_t(p[3]) << 24;
}

void store_le(unsigned char* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<unsigned char>(v >> (8 * i));
}

// HChaCha20: the ChaCha20 block function without the final addition; words
// 0..3 and 12..15 are the subkey.
void hchacha20(const unsigned char key[32], const unsigned char nonce[16],
               unsigned char out[32]) {
  uint32_t x[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
  for (int i = 0; i < 8; ++i) x[4 + i] = load_le(key + 4 * i);
  for (int i = 0; i < 4; ++i) x[12 + i] = load_le(nonce + 4 * i);
  auto qr = [&](int a, int b, int c, int d) {
    x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 16);
    x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 12);
    x[a] += x[b]; x[d] = rotl(x[d] ^ x[a], 8);
    x[c] += x[d]; x[b] = rotl(x[b] ^ x[c], 7);
  };
  for (int i = 0; i < 10; ++i) {
    qr(0, 4, 8, 12); qr(1, 5, 9, 13); qr(2, 6, 10, 14); qr(3, 7, 11, 15);
    qr(0, 5, 10, 15); qr(1, 6, 11, 12); qr(2, 7, 8, 13); qr(3, 4, 9, 14);
  }
  for (int i = 0; i < 4; ++i) store_le(out + 4 * i, x[i]);
  for (int i = 0; i < 4; ++i) store_le(out + 16 + 4 * i, x[12 + i]);
  volatile uint32_t* v = x;
  for (int i = 0; i < 16; ++i) v[i] = 0;
}

// Encrypts (or decrypts and checks `tag`) `in` in place into `out`.
bool xchacha20_poly1305(bool encrypt, const unsigned char key[32],
                        const unsigned char nonce[24], const unsigned char* aad,
                        size_t aad_len, const unsigned char* in, size_t n,
                        unsigned char* out, unsigned char tag[16]) {
  unsigned char subkey[32];
  hchacha20(key, nonce, subkey);
  unsigned char iv[12] = {0, 0, 0, 0};
  std::memcpy(iv + 4, nonce + 16, 8);
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(
      EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  int len = 0;
  bool ok = ctx &&
      EVP_CipherInit_ex(ctx.get(), EVP_chacha20_poly1305(), nullptr, nullptr,
                        nullptr, encrypt ? 1 : 0) == 1 &&
      EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_IVLEN, 12, nullptr) == 1 &&
      EVP_CipherInit_ex(ctx.get(), nullptr, nullptr, subkey, iv, -1) == 1 &&
      (encrypt || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_SET_TAG, 16,
                                      tag) == 1) &&
      EVP_CipherUpdate(ctx.get(), nullptr, &len, aad, int(aad_len)) == 1 &&
      EVP_CipherUpdate(ctx.get(), out, &len, in, int(n)) == 1 &&
      size_t(len) == n &&
      EVP_CipherFinal_ex(ctx.get(), out + len, &len) == 1 &&
      (!encrypt || EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_AEAD_GET_TAG, 16,
                                       tag) == 1);
  OPENSSL_cleanse(subkey, sizeof subkey);
  return ok;
}

constexpr int kMaxLogN = 20;  // 2^20 scrypt rounds need 1 GiB

bool scrypt_key(std::string_view password, const unsigned char salt[16],
                int log_n, unsigned char key[32]) {
  if (log_n < 1 || log_n > kMaxLogN) return false;
  const uint64_t n = uint64_t(1) << log_n;
  const uint64_t maxmem = 1025 * n + (uint64_t(1) << 20);  // 128·r·N, r = 8
  return EVP_PBE_scrypt(password.data(), password.size(), salt, 16, n, 8, 1,
                        maxmem, key, 32) == 1;
}

#endif  // XANATOO_NOSTR_VERIFY_OPENSSL

}  // namespace

void nostr_wipe(std::string& secret) {
  volatile char* p = secret.data();
  for (size_t i = 0; i < secret.size(); ++i) p[i] = 0;
  secret.clear();
}

bool nostr_signing_available() { return XANATOO_NOSTR_VERIFY_OPENSSL != 0; }

std::string nostr_generate_secret() {
#if XANATOO_NOSTR_VERIFY_OPENSSL
  for (int attempt = 0; attempt < 16; ++attempt) {
    Bytes sk(32);
    if (RAND_priv_bytes(sk.data(), 32) != 1) return {};
    std::string hex = to_hex(sk.data(), 32);
    wipe(sk);
    if (!nostr_pubkey_of(hex).empty()) return hex;  // 0 < key < n
    nostr_wipe(hex);
  }
#endif
  return {};
}

std::string nostr_pubkey_of(std::string_view secret_hex) {
#if XANATOO_NOSTR_VERIFY_OPENSSL
  Curve c;
  if (!c.ok()) return {};
  Bn d = secret_scalar(c, secret_hex);
  Point p = c.point();
  unsigned char x[32];
  bool even = false;
  if (!d || !p ||
      !EC_POINT_mul(c.group.get(), p.get(), d.get(), nullptr, nullptr, c.ctx.get()) ||
      !point_xy(c, p.get(), x, even))
    return {};
  return to_hex(x, 32);
#else
  (void)secret_hex;
  return {};
#endif
}

std::string nostr_sign_bip340(std::string_view secret_hex,
                              std::string_view msg_hex,
                              std::string_view aux_hex) {
#if XANATOO_NOSTR_VERIFY_OPENSSL
  auto msg = from_hex(msg_hex, 32);
  std::optional<Bytes> aux;
  if (aux_hex.empty()) {
    aux = Bytes(32);
    if (RAND_bytes(aux->data(), 32) != 1) return {};
  } else {
    aux = from_hex(aux_hex, 32);
  }
  Curve c;
  if (!msg || !aux || !c.ok()) return {};
  Bn d = secret_scalar(c, secret_hex);
  Point P = c.point(), R = c.point();
  Bn k = secret_bn(), e(BN_new()), s = secret_bn();
  if (!d || !P || !R || !k || !e || !s) return {};
  unsigned char px[32], rx[32];
  bool even = false;
  if (!EC_POINT_mul(c.group.get(), P.get(), d.get(), nullptr, nullptr, c.ctx.get()) ||
      !point_xy(c, P.get(), px, even))
    return {};
  // d = d' if P has an even y, else n - d'.
  if (!even && !BN_sub(d.get(), c.order.get(), d.get())) return {};

  // t = bytes(d) xor hash_aux(a); k' = hash_nonce(t || P.x || m) mod n
  Bytes nonce_in(96);
  if (!bn_bytes32(d.get(), nonce_in.data())) return {};
  const auto aux_hash = tagged_hash("BIP0340/aux", *aux);
  for (int i = 0; i < 32; ++i) nonce_in[i] ^= aux_hash[i];
  std::memcpy(nonce_in.data() + 32, px, 32);
  std::memcpy(nonce_in.data() + 64, msg->data(), 32);
  auto rand = tagged_hash("BIP0340/nonce", nonce_in);
  wipe(nonce_in);
  const bool k_ok = BN_bin2bn(rand.data(), 32, k.get()) &&
                    BN_nnmod(k.get(), k.get(), c.order.get(), c.ctx.get());
  OPENSSL_cleanse(rand.data(), rand.size());
  if (!k_ok || BN_is_zero(k.get())) return {};
  if (!EC_POINT_mul(c.group.get(), R.get(), k.get(), nullptr, nullptr, c.ctx.get()) ||
      !point_xy(c, R.get(), rx, even))
    return {};
  if (!even && !BN_sub(k.get(), c.order.get(), k.get())) return {};

  // e = hash_challenge(R.x || P.x || m) mod n; s = k + e·d mod n
  Bytes chal(96);
  std::memcpy(chal.data(), rx, 32);
  std::memcpy(chal.data() + 32, px, 32);
  std::memcpy(chal.data() + 64, msg->data(), 32);
  const auto eh = tagged_hash("BIP0340/challenge", chal);
  if (!BN_bin2bn(eh.data(), 32, e.get()) ||
      !BN_nnmod(e.get(), e.get(), c.order.get(), c.ctx.get()) ||
      !BN_mod_mul(s.get(), e.get(), d.get(), c.order.get(), c.ctx.get()) ||
      !BN_mod_add(s.get(), s.get(), k.get(), c.order.get(), c.ctx.get()))
    return {};
  unsigned char sig[64];
  std::memcpy(sig, rx, 32);
  if (!bn_bytes32(s.get(), sig + 32)) return {};
  std::string out = to_hex(sig, 64);
  if (!nostr_verify_bip340(to_hex(px, 32), msg_hex, out)) return {};
  return out;
#else
  (void)secret_hex; (void)msg_hex; (void)aux_hex;
  return {};
#endif
}

std::string nostr_nsec(std::string_view secret_hex) {
  auto sk = from_hex(secret_hex, 32);
  if (!sk) return {};
  std::string out = bech32_encode_bytes("nsec", *sk);
  wipe(*sk);
  return out;
}

std::optional<std::string> nostr_decode_nsec(std::string_view nsec) {
  std::string hrp;
  Bytes data;
  if (!bech32_decode_bytes(strip_scheme(nsec), hrp, data) || hrp != "nsec" ||
      data.size() != 32) {
    wipe(data);
    return std::nullopt;
  }
  std::string hex = to_hex(data.data(), 32);
  wipe(data);
  return hex;
}

bool is_ncryptsec(std::string_view s) {
  s = strip_scheme(s);
  return s.size() > 10 && (s.substr(0, 10) == "ncryptsec1" ||
                           s.substr(0, 10) == "NCRYPTSEC1");
}

// NIP-49 payload: version 0x02, log_n, salt[16], nonce[24], key security
// byte (also the associated data), ciphertext[32] + tag[16]: 91 bytes.
std::string nostr_ncryptsec_encrypt(std::string_view secret_hex,
                                    std::string_view password, int log_n,
                                    int key_security) {
#if XANATOO_NOSTR_VERIFY_OPENSSL
  auto sk = from_hex(secret_hex, 32);
  if (!sk || key_security < 0 || key_security > 2 || password.empty())
    return {};
  Bytes out(91);
  out[0] = 0x02;
  out[1] = static_cast<unsigned char>(log_n);
  unsigned char* salt = out.data() + 2;
  unsigned char* nonce = out.data() + 18;
  out[42] = static_cast<unsigned char>(key_security);
  unsigned char key[32];
  bool ok = RAND_bytes(salt, 16) == 1 && RAND_bytes(nonce, 24) == 1 &&
            scrypt_key(password, salt, log_n, key) &&
            xchacha20_poly1305(true, key, nonce, out.data() + 42, 1, sk->data(),
                               32, out.data() + 43, out.data() + 75);
  OPENSSL_cleanse(key, sizeof key);
  wipe(*sk);
  return ok ? bech32_encode_bytes("ncryptsec", out) : std::string{};
#else
  (void)secret_hex; (void)password; (void)log_n; (void)key_security;
  return {};
#endif
}

std::optional<std::string> nostr_ncryptsec_decrypt(std::string_view ncryptsec,
                                                   std::string_view password,
                                                   std::string* err) {
  auto fail = [&](const char* why) -> std::optional<std::string> {
    if (err) *err = why;
    return std::nullopt;
  };
  std::string hrp;
  Bytes data;
  if (!bech32_decode_bytes(strip_scheme(ncryptsec), hrp, data) ||
      hrp != "ncryptsec")
    return fail("not an ncryptsec1… key");
  if (data.size() != 91 || data[0] != 0x02)
    return fail("unknown ncryptsec version");
#if XANATOO_NOSTR_VERIFY_OPENSSL
  const int log_n = data[1];
  if (log_n > kMaxLogN) return fail("the key's scrypt cost is too high to unlock here");
  unsigned char key[32];
  Bytes sk(32);
  unsigned char tag[16];
  std::memcpy(tag, data.data() + 75, 16);
  if (!scrypt_key(password, data.data() + 2, log_n, key))
    return fail("scrypt failed");
  const bool ok = xchacha20_poly1305(false, key, data.data() + 18,
                                     data.data() + 42, 1, data.data() + 43, 32,
                                     sk.data(), tag);
  OPENSSL_cleanse(key, sizeof key);
  if (!ok) {
    wipe(sk);
    return fail("wrong password");
  }
  std::string hex = to_hex(sk.data(), 32);
  wipe(sk);
  if (nostr_pubkey_of(hex).empty()) {
    nostr_wipe(hex);
    return fail("the decrypted key is not a valid secp256k1 key");
  }
  return hex;
#else
  (void)password;
  return fail("this build cannot decrypt keys (needs OpenSSL)");
#endif
}

std::string nostr_sign_event(std::string_view secret_hex, int kind,
                             std::string_view content,
                             const std::vector<std::vector<std::string>>& tags,
                             int64_t created_at) {
  const std::string pubkey = nostr_pubkey_of(secret_hex);
  if (pubkey.empty() || kind < 0 || kind > 65535 || created_at < 0) return {};
  std::string tags_json = "[";
  for (size_t i = 0; i < tags.size(); ++i) {
    if (i) tags_json += ',';
    tags_json += '[';
    for (size_t j = 0; j < tags[i].size(); ++j) {
      if (j) tags_json += ',';
      tags_json += nostr_json_string(tags[i][j]);
    }
    tags_json += ']';
  }
  tags_json += ']';
  const std::string body = "\"pubkey\":\"" + pubkey + "\",\"created_at\":" +
                           std::to_string(created_at) + ",\"kind\":" +
                           std::to_string(kind) + ",\"tags\":" + tags_json +
                           ",\"content\":" + nostr_json_string(content);
  const auto id = nostr_event_id("{" + body + "}");
  if (!id) return {};
  const std::string sig = nostr_sign_bip340(secret_hex, *id);
  if (sig.empty()) return {};
  return "{\"id\":\"" + *id + "\"," + body + ",\"sig\":\"" + sig + "\"}";
}

}  // namespace xanatoo
