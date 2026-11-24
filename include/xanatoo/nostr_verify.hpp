// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

#include <optional>
#include <string>
#include <string_view>

// secp256k1 crypto backend, chosen at build time (one of):
//   XANATOO_NOSTR_SECP256K1=1      libsecp256k1 + Mbed TLS (Android, mbedtls builds)
//   XANATOO_NOSTR_VERIFY_OPENSSL=1 OpenSSL's libcrypto (desktop default)
// XANATOO_NOSTR_CRYPTO is 1 when either is present; without one, events are
// only hash-checked and nothing can be signed.
#ifndef XANATOO_NOSTR_SECP256K1
#define XANATOO_NOSTR_SECP256K1 0
#endif
#ifndef XANATOO_NOSTR_VERIFY_OPENSSL
#define XANATOO_NOSTR_VERIFY_OPENSSL 0
#endif
#if XANATOO_NOSTR_SECP256K1 || XANATOO_NOSTR_VERIFY_OPENSSL
#define XANATOO_NOSTR_CRYPTO 1
#else
#define XANATOO_NOSTR_CRYPTO 0
#endif

namespace xanatoo {

// NIP-01 event integrity. HashOnly is intentionally NOT an authenticated
// identity: a relay can invent any content/key pair and recompute its hash.
enum class NostrVerification { Invalid, HashOnly, SignatureValid };

// Reconstruct the NIP-01 canonical [0,pubkey,created_at,kind,tags,content]
// serialization and SHA-256. Returns nullopt for invalid event JSON.
std::optional<std::string> nostr_event_id(std::string_view event_json);
NostrVerification nostr_verify_event(std::string_view event_json);

// `value` as a quoted JSON string with NIP-01's escapes (the canonical form
// event ids are hashed over).
std::string nostr_json_string(std::string_view value);

// BIP-340 testable separately against the upstream official vectors.
// Returns false on builds without an approved secp256k1 crypto backend.
bool nostr_verify_bip340(std::string_view pubkey_hex,
                         std::string_view msg_hash_hex,
                         std::string_view signature_hex);

}  // namespace xanatoo
