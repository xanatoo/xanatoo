// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Nostr signing keys: BIP-340 Schnorr signatures over secp256k1, NIP-19
// nsec, NIP-49 password-encrypted ncryptsec, and signed NIP-01 events.
// Needs the OpenSSL crypto backend (XANATOO_NOSTR_VERIFY_OPENSSL); other
// builds return failures and nostr_signing_available() is false.
// No files here: the key store is nostr_keys.hpp.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xanatoo {

bool nostr_signing_available();

// A fresh secret key from the system CSPRNG: 64 lower-case hex digits
// (1 ≤ key < curve order). Empty on failure.
std::string nostr_generate_secret();

// x-only public key (64 hex) of a secret key; empty if the key is invalid.
std::string nostr_pubkey_of(std::string_view secret_hex);

// BIP-340 signature (128 hex) of a 32-byte message (64 hex) with 32 bytes of
// auxiliary randomness (64 hex; empty = fresh random bytes). Empty on
// failure; the signature is verified before it is returned.
std::string nostr_sign_bip340(std::string_view secret_hex,
                              std::string_view msg_hex,
                              std::string_view aux_hex = {});

// NIP-19 nsec1… of a secret key, and back (also accepts "nostr:nsec1…").
std::string nostr_nsec(std::string_view secret_hex);
std::optional<std::string> nostr_decode_nsec(std::string_view nsec);

// NIP-49: the secret key encrypted with `password` (scrypt with 2^log_n
// rounds, XChaCha20-Poly1305). key_security: 0 = the key was handled
// insecurely (seen as plain nsec), 1 = not known to have been, 2 = not
// tracked. The password is used as given: NIP-49 asks for NFKC, which is
// the same bytes for ASCII and for the precomposed text keyboards produce.
// Empty on failure.
std::string nostr_ncryptsec_encrypt(std::string_view secret_hex,
                                    std::string_view password,
                                    int log_n = 16, int key_security = 2);
// The secret key (64 hex), or nullopt with *err ("wrong password", …).
std::optional<std::string> nostr_ncryptsec_decrypt(std::string_view ncryptsec,
                                                   std::string_view password,
                                                   std::string* err = nullptr);
bool is_ncryptsec(std::string_view s);

// A complete signed NIP-01 event as JSON ({"id",…,"sig"}). Empty on failure.
std::string nostr_sign_event(std::string_view secret_hex, int kind,
                             std::string_view content,
                             const std::vector<std::vector<std::string>>& tags,
                             int64_t created_at);

// Overwrite a secret held in a string before dropping it.
void nostr_wipe(std::string& secret);

}  // namespace xanatoo
