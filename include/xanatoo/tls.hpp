// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Ingo Ruhnke <grumbel@gmail.com>
#pragma once

// Minimal TLS client stream for the network clients (HTTPS, Gemini, Nostr
// WSS). Backends: tls_openssl.cpp (desktop default) or tls_mbedtls.cpp
// (Android; -DXANATOO_TLS=mbedtls on desktop), chosen by CMake / ndk-build.
//
// TLS 1.2+, SNI = host. Peer certificates and hostnames are verified by
// default. Gemini deliberately opts out of CA verification because it commonly
// uses self-signed certificates; it verifies a stored TOFU fingerprint instead.
// If no system CA store is available, verified connections fail closed.

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace xanatoo {

// Human-readable information from the leaf certificate after a successful
// handshake. Dates are in UTC. `self_issued` only compares subject and issuer;
// `self_signed` additionally verifies the leaf's own signature when supported
// (nullopt means the backend could not determine this).
struct TlsPeerCertificate {
  std::string subject;
  std::string issuer;
  std::string not_before;
  std::string not_after;
  bool self_issued = false;
  std::optional<bool> self_signed;
};

// A client certificate with its private key, both PEM (Gemini identities).
struct TlsClientIdentity {
  std::string cert_pem;
  std::string key_pem;
};

// A new self-signed client certificate: EC P-256 key, SHA-256 signature,
// subject CN = `common_name`, a random serial, valid from now until
// 9999-12-31 (RFC 5280's "no well-defined expiration"). Gemini servers know
// a client by the certificate's fingerprint, so it must not expire.
std::optional<TlsClientIdentity> tls_make_client_identity(
    const std::string& common_name, std::string* err = nullptr);

// SHA-256 (lowercase hex) of a PEM certificate's DER encoding: the
// fingerprint a server sees. nullopt when it does not parse.
std::optional<std::string> tls_certificate_sha256(const std::string& cert_pem);

// Subject, issuer and validity of a PEM certificate.
std::optional<TlsPeerCertificate> tls_certificate_details(
    const std::string& cert_pem);

// True when `key_pem` is the private key of `cert_pem` (an import check).
bool tls_identity_matches(const TlsClientIdentity& identity,
                          std::string* err = nullptr);

class TlsStream {
 public:
  TlsStream();
  ~TlsStream();
  TlsStream(const TlsStream&) = delete;
  TlsStream& operator=(const TlsStream&) = delete;

  // Handshake over the connected TCP socket `fd`. The stream owns `fd` from
  // here on (closed by close() / the destructor, also when this fails).
  bool connect(int fd, const std::string& host, std::string* err = nullptr,
               bool verify_peer = true);
  bool connected() const;

  // Present this client certificate when the server asks for one. Call
  // before connect(); it applies to the next handshakes of this stream. A
  // certificate or key that does not load fails connect().
  void set_client_identity(TlsClientIdentity identity);
  // Unverified connections only (verify_peer=false, Gemini): called with the
  // server's leaf fingerprint during the handshake, before a client
  // certificate goes out; false aborts the handshake (peer_guard_refused()).
  // This keeps an identity from reaching a server whose pin changed.
  void set_peer_guard(std::function<bool(const std::string& sha256)> guard);
  bool peer_guard_refused() const;

  // SHA-256 of the peer's full DER leaf certificate (lowercase hex), after
  // a successful handshake. No certificate or hash error => nullopt.
  // Gemini checks this before sending any application data.
  std::optional<std::string> peer_certificate_sha256() const;

  // Certificate identity and validity for Gemini's explicit trust review.
  // This is descriptive information, not proof of CA or hostname trust.
  std::optional<TlsPeerCertificate> peer_certificate_details() const;

  // Whole buffer; false on error.
  bool write_all(const void* data, size_t n);
  // Up to n bytes: > 0 read, 0 = closed by peer, < 0 = error / timeout
  // (the socket's SO_RCVTIMEO).
  int read(void* buf, size_t n);
  // Why the last read() / write_all() failed (library error string, errno
  // text, "timed out", "connection reset" ...); empty after success.
  const std::string& last_error() const;
  // Decrypted bytes buffered inside TLS (readable without blocking).
  bool pending() const;

  void close();

  // Name of the compiled-in backend ("OpenSSL", "Mbed TLS").
  static const char* backend();
  // Library version at runtime, plus the built-against one where they can
  // differ ("OpenSSL 3.4.1 11 Feb 2025 (built against …)").
  static std::string version();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xanatoo
